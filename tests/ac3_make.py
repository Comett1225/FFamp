import os
import random
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = open(os.path.join(HERE, "..", "src", "ac3_tables.cpp")).read()


def table(name):
    body = re.search(r"\b%s\[[^=]*=\s*\{(.*?)\};" % name, SOURCE, re.S).group(1)
    return [int(x, 0) for x in re.findall(r"-?0x[0-9a-fA-F]+|-?\d+", body)]


FRAME_SIZES = table("kFrameSizes")
CHANNELS = table("kChannels")
BAND_START = table("kBandStart")
BIN_TO_BAND = table("kBinToBand")
LOG_ADD = table("kLogAdd")
HEARING = table("kHearingThreshold")
BAP_TABLE = table("kBapTable")
HIGH_BAP = table("kHighBapTable")
SLOW_DECAY = table("kSlowDecay")
FAST_DECAY = table("kFastDecay")
SLOW_GAIN = table("kSlowGain")
DB_PER_BIT = table("kDbPerBit")
FLOORS = table("kFloors")
FAST_GAIN = table("kFastGain")
HEBAP_BITS = table("kBitsForHighBap")
QUANT_BITS = table("kQuantizationBits")
FRAME_STRATEGIES = [table("kFrameExponentStrategies")[i * 6:i * 6 + 6] for i in range(32)]
DEFAULT_CPL = table("kDefaultCouplingBands")
DEFAULT_SPX = table("kDefaultSpxBands")


class Bits:
    def __init__(self):
        self.out = []

    def put(self, n, v):
        for i in range(n - 1, -1, -1):
            self.out.append((v >> i) & 1)

    def flag(self, v):
        self.out.append(1 if v else 0)
        return bool(v)

    def size(self):
        return len(self.out)

    def data(self):
        bits = self.out + [0] * (-len(self.out) % 8)
        return bytes(int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))


def crc16(data):
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x8005) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def lowcomp1(a, b0, b1, c):
    if b0 + 256 == b1:
        return c
    if b0 > b1:
        return max(a - 64, 0)
    return a


def lowcomp(a, b0, b1, band):
    if band < 7:
        return lowcomp1(a, b0, b1, 384)
    if band < 20:
        return lowcomp1(a, b0, b1, 320)
    return max(a - 128, 0)


def wrap16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def allocate(p, exps, start, end, fast_gain, lfe, dba, snr, aht):
    psd = [0] * 256
    band_psd = [0] * 50
    for b in range(start, end):
        psd[b] = 3072 - (exps[b] << 7)
    b = start
    while b < min(end, 28):
        band_psd[b] = psd[b]
        b += 1
    if b < end:
        band = BIN_TO_BAND[b]
        while True:
            v = psd[b]
            b += 1
            band_end = min(BAND_START[band + 1], end)
            while b < band_end:
                most = max(v, psd[b])
                v = most + LOG_ADD[min(most - ((v + psd[b] + 1) >> 1), 255)]
                b += 1
            band_psd[band] = wrap16(v)
            band += 1
            if not end > BAND_START[band]:
                break
    excite = [0] * 50
    mask = [0] * 50
    band_start = BIN_TO_BAND[start]
    band_end = BIN_TO_BAND[end - 1] + 1
    fast = slow = 0
    if band_start == 0:
        low = lowcomp1(0, band_psd[0], band_psd[1], 384)
        excite[0] = wrap16(band_psd[0] - fast_gain - low)
        low = lowcomp1(low, band_psd[1], band_psd[2], 384)
        excite[1] = wrap16(band_psd[1] - fast_gain - low)
        begin = 7
        for band in range(2, 7):
            if not (lfe and band == 6):
                low = lowcomp1(low, band_psd[band], band_psd[band + 1], 384)
            fast = band_psd[band] - fast_gain
            slow = band_psd[band] - p["slow_gain"]
            excite[band] = wrap16(fast - low)
            if not (lfe and band == 6) and band_psd[band] <= band_psd[band + 1]:
                begin = band + 1
                break
        for band in range(begin, min(band_end, 22)):
            if not (lfe and band == 6):
                low = lowcomp(low, band_psd[band], band_psd[band + 1], band)
            fast = max(fast - p["fast_decay"], band_psd[band] - fast_gain)
            slow = max(slow - p["slow_decay"], band_psd[band] - p["slow_gain"])
            excite[band] = wrap16(max(fast - low, slow))
        begin = 22
    else:
        begin = band_start
        fast = (p["cpl_fast"] << 8) + 768
        slow = (p["cpl_slow"] << 8) + 768
    for band in range(begin, band_end):
        fast = max(fast - p["fast_decay"], band_psd[band] - fast_gain)
        slow = max(slow - p["slow_decay"], band_psd[band] - p["slow_gain"])
        excite[band] = wrap16(max(fast, slow))
    for band in range(band_start, band_end):
        tmp = p["db_per_bit"] - band_psd[band]
        if tmp > 0:
            excite[band] = wrap16(excite[band] + (tmp >> 2))
        mask[band] = max(HEARING[(band >> p["shift"]) * 3 + p["sr"]], excite[band])
    if dba:
        band = band_start
        for offset, length, value in dba:
            band += offset
            delta = (value - 3) * 128 if value >= 4 else (value - 4) * 128
            for _ in range(length):
                mask[band] = wrap16(mask[band] + delta)
                band += 1
    bap = [0] * 256
    if snr == -960:
        return bap
    tab = HIGH_BAP if aht else BAP_TABLE
    floor = p["floor"]
    b = start
    while b < min(end, 28):
        m = (max(mask[b] - snr - floor, 0) & 0x1FE0) + floor
        bap[b] = tab[min(max((psd[b] - m) >> 5, 0), 63)]
        b += 1
    if b >= end:
        return bap
    band = BIN_TO_BAND[b]
    while True:
        m = (max(mask[band] - snr - floor, 0) & 0x1FE0) + floor
        band += 1
        band_end2 = min(BAND_START[band], end)
        while b < band_end2:
            bap[b] = tab[min(max((psd[b] - m) >> 5, 0), 63)]
            b += 1
        if not end > band_end2:
            break
    return bap


def exponents(rng, w, strategy, groups, first, top=24):
    size = strategy + (strategy == 3)
    out = [first]
    prev = first
    for _ in range(groups):
        code = []
        for _ in range(3):
            d = rng.choice([-2, -1, 0, 1, 2]) if rng.random() < 0.6 else 0
            if prev + d < 4 or prev + d > top:
                d = -d if 4 <= prev - d <= top else 0
            prev += d
            code.append(d + 2)
            out += [prev] * size
        w.put(7, code[0] * 25 + code[1] * 5 + code[2])
    return out


def bands_of(struct, start, end, enhanced=False):
    sizes = [6 if enhanced else 12]
    for sub in range(1, end - start):
        size = 6 if enhanced and sub < 4 else 12
        if struct[start + sub]:
            sizes[-1] += size
        else:
            sizes.append(size)
    return sizes


class Mantissas:
    def __init__(self, rng, w):
        self.rng, self.w = rng, w
        self.b1 = self.b2 = self.b4 = 0

    def plain(self, bap, start, end):
        rng, w = self.rng, self.w
        for b in range(start, end):
            v = bap[b]
            if v == 1:
                if self.b1:
                    self.b1 -= 1
                else:
                    w.put(5, rng.randrange(27))
                    self.b1 = 2
            elif v == 2:
                if self.b2:
                    self.b2 -= 1
                else:
                    w.put(7, rng.randrange(125))
                    self.b2 = 2
            elif v == 3:
                w.put(3, rng.randrange(7))
            elif v == 4:
                if self.b4:
                    self.b4 = 0
                else:
                    w.put(7, rng.randrange(121))
                    self.b4 = 1
            elif v == 5:
                w.put(4, rng.randrange(15))
            elif v >= 6:
                q = QUANT_BITS[v]
                w.put(q, rng.randrange(1 << q) if rng.random() < 0.3 else rng.randrange(1 << (q - 3)) - (1 << (q - 4)))

    def aht(self, bap, start, end):
        rng, w = self.rng, self.w
        mode = rng.randrange(4)
        w.put(2, mode)
        end_bap = 12 if mode < 2 else 17
        gains = []
        if mode in (1, 2):
            for b in range(start, end):
                if 7 < bap[b] < end_bap:
                    g = rng.randrange(2)
                    w.put(1, g)
                    gains.append(g << (mode - 1))
        elif mode == 3:
            pending = 0
            for b in range(start, end):
                if 7 < bap[b] < 17:
                    if pending == 0:
                        code = rng.randrange(27)
                        w.put(5, code)
                        gains += [code // 9, (code % 9) // 3, code % 3]
                        pending = 3
                    pending -= 1
        at = 0
        for b in range(start, end):
            h = bap[b]
            bits = HEBAP_BITS[h]
            if h == 0:
                continue
            if h < 8:
                w.put(bits, rng.randrange(1 << bits) if h < 7 else rng.randrange(512))
                continue
            gain = 0
            if mode != 0 and h < end_bap:
                gain = gains[at]
                at += 1
            gbits = bits - gain
            for _ in range(6):
                if gain and rng.random() < 0.15:
                    w.put(gbits, 1 << (gbits - 1))
                    mbits = bits - (2 - gain)
                    w.put(mbits, rng.randrange(1 << mbits))
                else:
                    v = rng.randrange(1 << gbits)
                    if gain and v == 1 << (gbits - 1):
                        v = 0
                    w.put(gbits, v)


def make_frame(rng, cfg):
    eac3 = cfg["eac3"]
    mode, lfe, sr = cfg["mode"], cfg["lfe"], cfg["sr"]
    blocks = cfg.get("blocks", 6)
    dependent = cfg.get("dependent", False)
    fbw = CHANNELS[mode]
    channels = fbw + lfe
    lfe_ch = fbw + 1
    w = Bits()
    w.put(16, 0x0B77)
    if not eac3:
        code = cfg.get("size_code", 36)
        frame_bytes = FRAME_SIZES[code * 3 + sr] * 2
        w.put(16, 0)
        w.put(2, sr)
        w.put(6, code)
        w.put(5, cfg.get("bsid", 8))
        w.put(3, 0)
        w.put(3, mode)
        if mode == 2:
            w.put(2, 0)
        else:
            if (mode & 1) and mode != 1:
                w.put(2, 0)
            if mode & 4:
                w.put(2, 0)
        w.put(1, lfe)
        for _ in range(1 if mode else 2):
            w.put(5, 27)
            if w.flag(rng.random() < 0.3):
                w.put(8, rng.randrange(256))
            if w.flag(rng.random() < 0.3):
                w.put(8, 0)
            if w.flag(rng.random() < 0.3):
                w.put(7, 0)
        w.put(2, 0)
        if w.flag(rng.random() < 0.3):
            w.put(14, 0)
        if w.flag(rng.random() < 0.3):
            w.put(14, 0)
        if w.flag(rng.random() < 0.2):
            n = rng.randrange(3)
            w.put(6, n)
            w.put(8 * (n + 1), 0)
    else:
        w.put(2, 1 if dependent else 0)
        w.put(3, 0)
        size_at = w.size()
        w.put(11, 0)
        w.put(2, sr)
        w.put(2, {1: 0, 2: 1, 3: 2, 6: 3}[blocks])
        w.put(3, mode)
        w.put(1, lfe)
        w.put(5, 16)
        for _ in range(1 if mode else 2):
            w.put(5, 27)
            if w.flag(rng.random() < 0.3):
                w.put(8, rng.randrange(256))
        if dependent:
            w.put(1, 1)
            w.put(16, cfg["chanmap"])
        if w.flag(rng.random() < 0.5):
            if mode > 2:
                w.put(2, 0)
                if mode & 1:
                    w.put(6, 0)
                if mode & 4:
                    w.put(6, 0)
            if lfe and w.flag(rng.random() < 0.5):
                w.put(5, 10)
            if not dependent:
                for _ in range(1 if mode else 2):
                    if w.flag(rng.random() < 0.3):
                        w.put(6, 0)
                if w.flag(rng.random() < 0.3):
                    w.put(6, 0)
                kind = rng.randrange(4)
                w.put(2, kind)
                if kind == 1:
                    w.put(5, 0)
                elif kind == 2:
                    w.put(12, 0)
                elif kind == 3:
                    n = rng.randrange(3)
                    w.put(5, n)
                    w.put((n + 2) * 8, 0)
                if mode < 2:
                    for _ in range(1 if mode else 2):
                        if w.flag(rng.random() < 0.5):
                            w.put(14, 0)
                if w.flag(rng.random() < 0.3):
                    for _ in range(blocks):
                        if blocks == 1 or w.flag(rng.random() < 0.5):
                            w.put(5, 0)
        if w.flag(rng.random() < 0.5):
            w.put(5, 0)
            if mode == 2:
                w.put(4, 0)
            if mode >= 6:
                w.put(2, 0)
            for _ in range(1 if mode else 2):
                if w.flag(rng.random() < 0.3):
                    w.put(8, 0)
            w.put(1, 0)
        if not dependent and blocks != 6:
            w.put(1, 0)
        if w.flag(rng.random() < 0.2):
            w.put(6, 0)
            w.put(8, 0)
    shift = 0 if eac3 else max(cfg.get("bsid", 8), 8) - 8
    p = {"sr": sr, "shift": shift}
    full = False
    state = {"cpl": [0] * 6, "cpl_exists": [0] * 6, "exp": [[0] * 7 for _ in range(6)], "aht": [0] * 7}
    if eac3:
        six = blocks == 6
        ac3_style = w.flag(rng.random() < 0.5) if six else True
        aht_info = w.flag(cfg.get("aht", False)) if six else False
        snr_strategy = rng.randrange(3)
        w.put(2, snr_strategy)
        transient = w.flag(rng.random() < 0.3)
        block_switch = w.flag(cfg.get("switch", True))
        dither_syntax = w.flag(rng.random() < 0.5)
        ba_syntax = w.flag(rng.random() < 0.5)
        fgain_syntax = w.flag(rng.random() < 0.5)
        dba_syntax = w.flag(rng.random() < 0.5)
        skip_syntax = w.flag(rng.random() < 0.5)
        atten_syntax = w.flag(cfg.get("spx", False))
        cpl_blocks = 0
        if mode > 1:
            use = 1 if cfg.get("cpl", True) and rng.random() < 0.8 else 0
            for blk in range(blocks):
                exists = blk == 0 or w.flag(False)
                state["cpl_exists"][blk] = exists
                if exists:
                    w.put(1, use)
                state["cpl"][blk] = use
                cpl_blocks += use
        if ac3_style:
            for blk in range(blocks):
                for ch in range(0 if state["cpl"][blk] else 1, fbw + 1):
                    s = rng.randrange(1, 4) if blk == 0 else (0 if (aht_info and rng.random() < 0.7) else rng.randrange(4))
                    state["exp"][blk][ch] = s
                    w.put(2, s)
        else:
            for ch in range(0 if (mode > 1 and cpl_blocks) else 1, fbw + 1):
                c = 0 if (aht_info and rng.random() < 0.7) else rng.randrange(32)
                w.put(5, c)
                for blk in range(6):
                    state["exp"][blk][ch] = FRAME_STRATEGIES[c][blk]
        if lfe:
            for blk in range(blocks):
                s = 1 if blk == 0 else rng.randrange(2)
                state["exp"][blk][lfe_ch] = s
                w.put(1, s)
        if not dependent and (blocks == 6 or w.flag(rng.random() < 0.5)):
            w.put(5 * fbw, 0)
        if aht_info:
            for ch in range(0 if cpl_blocks == 6 else 1, channels + 1):
                ok = all(state["exp"][blk][ch] == 0 and not (ch == 0 and state["cpl_exists"][blk]) for blk in range(1, 6))
                if ok:
                    state["aht"][ch] = w.flag(rng.random() < 0.8)
        if snr_strategy == 0:
            coarse = rng.randrange(20, 50)
            fine = rng.randrange(16)
            w.put(6, coarse)
            w.put(4, fine)
            frame_snr = (((coarse - 15) << 4) + fine) << 2
        if transient:
            for ch in range(fbw):
                if w.flag(rng.random() < 0.5):
                    w.put(18, 0)
        atten = [-1] * 7
        for ch in range(1, fbw + 1):
            if atten_syntax and w.flag(rng.random() < 0.6):
                atten[ch] = rng.randrange(32)
                w.put(5, atten[ch])
        if blocks > 1 and w.flag(rng.random() < 0.2):
            full = True
            w.put((blocks - 1) * (4 + (2048 * 2 - 2).bit_length() - 1), 0)
        if not ba_syntax:
            p.update(slow_decay=SLOW_DECAY[2], fast_decay=FAST_DECAY[1], slow_gain=SLOW_GAIN[1], db_per_bit=DB_PER_BIT[2], floor=FLOORS[7])
    exps = [[0] * 256 for _ in range(7)]
    end = [0] * 7
    start = [0] * 7
    groups = [0] * 7
    snr = [0] * 7
    fgain = [0] * 7
    dba = [None] * 7
    in_cpl = [0] * 7
    cpl_struct = list(DEFAULT_CPL)
    cpl_sizes = []
    spx_on = False
    uses_spx = [0] * 7
    spx_src = 0
    spx_sizes = []
    first_spx = [1] * 7
    first_cpl = [1] * 7
    first_leak = 1 if eac3 else 0
    pending_aht = {}
    for blk in range(blocks):
        if not eac3 or block_switch:
            for ch in range(1, fbw + 1):
                w.put(1, 1 if rng.random() < cfg.get("switch_rate", 0.3) else 0)
        if not eac3 or dither_syntax:
            for ch in range(1, fbw + 1):
                w.put(1, 1 if rng.random() < 0.8 else 0)
        for _ in range(2 if mode == 0 else 1):
            if w.flag(rng.random() < 0.4):
                w.put(8, rng.choice([0, rng.randrange(256)]))
        if eac3:
            if blk == 0 or w.flag(False):
                spx_on = w.flag(cfg.get("spx", False) and rng.random() < 0.9)
                if spx_on:
                    if mode == 1:
                        uses_spx[1] = 1
                    else:
                        for ch in range(1, fbw + 1):
                            uses_spx[ch] = 1 if rng.random() < 0.7 else 0
                            w.put(1, uses_spx[ch])
                    dst = rng.randrange(2)
                    begin = rng.randrange(2, 6)
                    stop = rng.randrange(max(begin + 1, 3), 8)
                    w.put(2, dst)
                    w.put(3, begin)
                    w.put(3, stop)
                    sb = begin + 2
                    sb = sb + sb - 7 if sb > 7 else sb
                    eb = stop + 5
                    eb = eb + eb - 7 if eb > 7 else eb
                    spx_src = sb * 12 + 25
                    spx_struct = list(DEFAULT_SPX)
                    if w.flag(rng.random() < 0.5):
                        for sub in range(eb - sb - 1):
                            spx_struct[sb + 1 + sub] = w.flag(rng.random() < 0.5)
                    spx_sizes = bands_of(spx_struct, sb, eb)
                else:
                    uses_spx = [0] * 7
            if not spx_on:
                uses_spx = [0] * 7
                first_spx = [1] * 7
            if spx_on:
                for ch in range(1, fbw + 1):
                    if uses_spx[ch]:
                        if first_spx[ch] or w.flag(rng.random() < 0.3):
                            first_spx[ch] = 0
                            w.put(5, rng.randrange(32))
                            w.put(2, rng.randrange(4))
                            for _ in spx_sizes:
                                w.put(4, rng.randrange(16))
                                w.put(2, rng.randrange(4))
                    else:
                        first_spx[ch] = 1
        new_cpl = False
        if eac3:
            strategy_now = state["cpl_exists"][blk] if mode > 1 else False
        else:
            strategy_now = w.flag(blk == 0 or rng.random() < 0.2)
            if strategy_now:
                state["cpl"][blk] = 1 if mode >= 2 and cfg.get("cpl", True) and rng.random() < 0.7 else 0
            else:
                state["cpl"][blk] = state["cpl"][blk - 1]
        if strategy_now:
            new_cpl = True
            if not eac3:
                w.put(1, state["cpl"][blk])
            if state["cpl"][blk]:
                if eac3:
                    enhanced = cfg.get("enhanced", False) and rng.random() < 0.3
                    w.put(1, 1 if enhanced else 0)
                    if enhanced:
                        break
                if eac3 and mode == 2:
                    in_cpl[1] = in_cpl[2] = 1
                else:
                    while True:
                        pick = [0] + [1 if rng.random() < 0.8 else 0 for _ in range(fbw)]
                        if sum(pick) >= 1:
                            break
                    for ch in range(1, fbw + 1):
                        in_cpl[ch] = pick[ch]
                        w.put(1, pick[ch])
                if mode == 2:
                    state["phase"] = w.flag(rng.random() < 0.5)
                if spx_on:
                    cpl_end = (spx_src - 37) // 12
                    cpl_begin = rng.randrange(0, cpl_end)
                    w.put(4, cpl_begin)
                else:
                    cpl_begin = rng.randrange(0, 8)
                    stop = rng.randrange(max(0, cpl_begin - 2), 16)
                    w.put(4, cpl_begin)
                    w.put(4, stop)
                    cpl_end = stop + 3
                start[0] = cpl_begin * 12 + 37
                end[0] = cpl_end * 12 + 37
                if blk == 0:
                    cpl_struct = list(DEFAULT_CPL)
                if not eac3 or w.flag(rng.random() < 0.5):
                    for sub in range(cpl_end - cpl_begin - 1):
                        cpl_struct[cpl_begin + 1 + sub] = w.flag(rng.random() < 0.5)
                cpl_sizes = bands_of(cpl_struct, cpl_begin, cpl_end)
            else:
                for ch in range(1, fbw + 1):
                    in_cpl[ch] = 0
                    first_cpl[ch] = 1
                first_leak = 1 if eac3 else 0
                state["phase"] = 0
        coupled = state["cpl"][blk]
        if coupled:
            exist = False
            for ch in range(1, fbw + 1):
                if in_cpl[ch]:
                    if (eac3 and first_cpl[ch]) or w.flag(blk == 0 or new_cpl or rng.random() < 0.5):
                        first_cpl[ch] = 0
                        exist = True
                        w.put(2, rng.randrange(4))
                        for _ in cpl_sizes:
                            w.put(4, rng.randrange(16))
                            w.put(4, rng.randrange(16))
                else:
                    first_cpl[ch] = 1
            if mode == 2 and exist:
                for _ in cpl_sizes:
                    if state.get("phase"):
                        w.put(1, rng.randrange(2))
        if mode == 2:
            if (eac3 and blk == 0) or w.flag(blk == 0 or rng.random() < 0.5):
                n = 4
                if coupled and start[0] <= 61:
                    n -= 1 + (start[0] == 37)
                elif spx_on and spx_src <= 61:
                    n -= 1
                for _ in range(n):
                    w.put(1, rng.randrange(2))
        stages_new = new_cpl or blk == 0
        for ch in range(0 if coupled else 1, channels + 1):
            if not eac3:
                if ch == lfe_ch:
                    s = 1 if (stages_new or rng.random() < 0.5) else 0
                    w.put(1, s)
                else:
                    s = rng.randrange(1, 4) if stages_new else rng.randrange(4)
                    w.put(2, s)
                state["exp"][blk][ch] = s
        for ch in range(1, fbw + 1):
            start[ch] = 0
            if state["exp"][blk][ch]:
                if in_cpl[ch]:
                    end[ch] = start[0]
                elif uses_spx[ch]:
                    end[ch] = spx_src
                else:
                    code = rng.randrange(61)
                    w.put(6, code)
                    end[ch] = code * 3 + 73
                g = 3 << (state["exp"][blk][ch] - 1)
                groups[ch] = (end[ch] + g - 4) // g
        if coupled and state["exp"][blk][0]:
            groups[0] = (end[0] - start[0]) // (3 << (state["exp"][blk][0] - 1))
        if lfe:
            start[lfe_ch], end[lfe_ch], groups[lfe_ch] = 0, 7, 2
        for ch in range(0 if coupled else 1, channels + 1):
            s = state["exp"][blk][ch]
            if s:
                first = rng.randrange(4, 13) * (2 if ch == 0 else 1) if ch == 0 else rng.randrange(4, 16)
                w.put(4, first >> 1 if ch == 0 else first)
                values = exponents(rng, w, s, groups[ch], first)
                at = start[ch] + (1 if ch else 0)
                exps[ch][at:at + len(values) - 1] = values[1:]
                if ch:
                    exps[ch][0] = first
                if ch != 0 and ch != lfe_ch:
                    w.put(2, 0)
        if not eac3 or ba_syntax:
            if w.flag(blk == 0 or rng.random() < 0.3):
                sd, fd, sg, dbb, fl = rng.randrange(4), rng.randrange(4), rng.randrange(4), rng.randrange(4), rng.randrange(8)
                for n, v in ((2, sd), (2, fd), (2, sg), (2, dbb), (3, fl)):
                    w.put(n, v)
                p.update(slow_decay=SLOW_DECAY[sd] >> shift, fast_decay=FAST_DECAY[fd] >> shift, slow_gain=SLOW_GAIN[sg], db_per_bit=DB_PER_BIT[dbb], floor=FLOORS[fl])
        bias = cfg.get("snr", 30)
        if not eac3 or blk == 0:
            strategy = 2 if not eac3 else snr_strategy
            if strategy and w.flag(blk == 0 or new_cpl or rng.random() < 0.3):
                coarse = rng.randrange(bias - 6, bias + 6)
                w.put(6, coarse)
                first = 0 if coupled else 1
                value = 0
                for ch in range(first, channels + 1):
                    if ch == first or strategy == 2:
                        value = (((coarse - 15) << 4) + rng.randrange(16)) << 2
                        w.put(4, ((value >> 2) - ((coarse - 15) << 4)))
                    snr[ch] = value
                    if not eac3:
                        fgain[ch] = FAST_GAIN[rng.randrange(8)]
                        w.put(3, FAST_GAIN.index(fgain[ch]))
            elif eac3 and blk == 0 and strategy == 0:
                for ch in range(channels + 1):
                    snr[ch] = frame_snr
        if eac3 and fgain_syntax and w.flag(rng.random() < 0.5):
            for ch in range(0 if coupled else 1, channels + 1):
                fgain[ch] = FAST_GAIN[rng.randrange(8)]
                w.put(3, FAST_GAIN.index(fgain[ch]))
        elif eac3 and blk == 0:
            for ch in range(0 if coupled else 1, channels + 1):
                fgain[ch] = FAST_GAIN[4]
        if eac3 and not dependent and w.flag(rng.random() < 0.3):
            w.put(10, 0)
        if coupled:
            if first_leak or w.flag(blk == 0 or new_cpl or rng.random() < 0.3):
                p["cpl_fast"], p["cpl_slow"] = rng.randrange(8), rng.randrange(8)
                w.put(3, p["cpl_fast"])
                w.put(3, p["cpl_slow"])
            first_leak = 0
        if not eac3 or dba_syntax:
            if w.flag(rng.random() < 0.3 or (blk and new_cpl and coupled)):
                modes = {}
                for ch in range(0 if coupled else 1, fbw + 1):
                    modes[ch] = rng.choice([1, 2] if blk == 0 or dba[ch] is None else [0, 1, 2])
                    w.put(2, modes[ch])
                for ch in range(0 if coupled else 1, fbw + 1):
                    if modes[ch] == 1:
                        segs = []
                        n = rng.randrange(1, 4)
                        w.put(3, n - 1)
                        roomy = ch != 0
                        for _ in range(n):
                            segs.append((rng.randrange(4) if roomy else 0, rng.randrange(1, 4) if roomy else 1, rng.randrange(8)))
                            w.put(5, segs[-1][0])
                            w.put(4, segs[-1][1])
                            w.put(3, segs[-1][2])
                        dba[ch] = segs
                    elif modes[ch] == 2:
                        dba[ch] = None
            elif blk == 0:
                dba = [None] * 7
        baps = {}
        for ch in range(0 if coupled else 1, channels + 1):
            baps[ch] = allocate(p, exps[ch], start[ch], end[ch], fgain[ch], ch == lfe_ch and lfe, dba[ch] if ch <= fbw else None, snr[ch], state["aht"][ch])
        if (not eac3 or skip_syntax) and w.flag(rng.random() < 0.2):
            n = rng.randrange(4)
            w.put(9, n)
            w.put(8 * n, 0)
        m = Mantissas(rng, w)
        got = False
        for ch in range(1, channels + 1):
            if state["aht"][ch]:
                if blk == 0:
                    m.aht(baps[ch], start[ch], end[ch])
            else:
                m.plain(baps[ch], start[ch], end[ch])
            if in_cpl[ch] and coupled and not got:
                got = True
                if state["aht"][0]:
                    if blk == 0:
                        m.aht(baps[0], start[0], end[0])
                else:
                    m.plain(baps[0], start[0], end[0])
    tail = 17 if not eac3 else 17
    if not eac3:
        limit = frame_bytes * 8
        if w.size() + tail + 1 > limit:
            return False
        w.put(limit - tail - 1 - w.size(), 0)
        w.put(1, 0)
        w.put(1, 0)
        w.put(16, 0)
        data = bytearray(w.data())
    else:
        total = w.size() + tail + 1
        words = 2048 if full else (total + 15) // 16
        if total > words * 16 or words > 2048:
            return False
        w.put(words * 16 - tail - 1 - w.size(), 0)
        w.put(1, 0)
        w.put(1, 0)
        w.put(16, 0)
        data = bytearray(w.data())
        v = words - 1
        data[2] = (data[2] & 0xF8) | (v >> 8)
        data[3] = v & 0xFF
    crc = crc16(bytes(data[2:-2]))
    data[-2], data[-1] = crc >> 8, crc & 0xFF
    return bytes(data)


def stream(path, frames, seed, configs):
    rng = random.Random(seed)
    out = bytearray()
    for i in range(frames):
        packet = bytearray()
        for cfg in configs:
            for attempt in range(40):
                c = dict(cfg)
                c["snr"] = max(18, cfg.get("snr", 30) - attempt)
                frame = make_frame(rng, c)
                if frame:
                    packet += frame
                    break
            else:
                raise SystemExit("no frame fits: " + repr(cfg))
        out += packet
    open(path, "wb").write(out)


KINDS = {
    "ac3_dualmono": [dict(eac3=False, mode=0, lfe=0, sr=0)],
    "ac3_dualmono_lfe": [dict(eac3=False, mode=0, lfe=1, sr=1)],
    "ac3_22_lfe": [dict(eac3=False, mode=6, lfe=1, sr=2)],
    "ac3_51_switch": [dict(eac3=False, mode=7, lfe=1, sr=0, switch_rate=0.5)],
    "ac3_stereo_switch": [dict(eac3=False, mode=2, lfe=0, sr=0, switch_rate=0.5)],
    "ac3_half_rate": [dict(eac3=False, mode=2, lfe=0, sr=0, bsid=9, size_code=30)],
    "eac3_aht": [dict(eac3=True, mode=7, lfe=1, sr=0, aht=True, spx=False)],
    "eac3_aht_stereo": [dict(eac3=True, mode=2, lfe=0, sr=1, aht=True, cpl=True)],
    "eac3_spx": [dict(eac3=True, mode=7, lfe=1, sr=0, spx=True)],
    "eac3_spx_mono": [dict(eac3=True, mode=1, lfe=0, sr=2, spx=True)],
    "eac3_spx_aht": [dict(eac3=True, mode=2, lfe=1, sr=0, spx=True, aht=True)],
    "eac3_blocks1": [dict(eac3=True, mode=2, lfe=0, sr=0, blocks=1)],
    "eac3_blocks2": [dict(eac3=True, mode=5, lfe=1, sr=1, blocks=2, spx=True)],
    "eac3_blocks3": [dict(eac3=True, mode=0, lfe=0, sr=0, blocks=3)],
    "eac3_dualmono": [dict(eac3=True, mode=0, lfe=1, sr=2)],
    "eac3_22_lfe": [dict(eac3=True, mode=6, lfe=1, sr=0, spx=True, aht=True)],
    "eac3_71": [dict(eac3=True, mode=7, lfe=1, sr=0), dict(eac3=True, mode=6, lfe=0, sr=0, dependent=True, chanmap=0x1A00, cpl=False)],
    "eac3_71_backonly": [dict(eac3=True, mode=7, lfe=1, sr=0), dict(eac3=True, mode=2, lfe=0, sr=0, dependent=True, chanmap=0x0200)],
    "eac3_61": [dict(eac3=True, mode=7, lfe=1, sr=1, spx=True), dict(eac3=True, mode=1, lfe=0, sr=1, dependent=True, chanmap=0x0100)],
    "eac3_enhanced": [dict(eac3=True, mode=2, lfe=0, sr=0, enhanced=True)],
}

if __name__ == "__main__":
    folder = sys.argv[1]
    frames = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    names = sys.argv[3:] or sorted(KINDS)
    for i, name in enumerate(names):
        kind = KINDS[name]
        stream(os.path.join(folder, name + (".eac3" if kind[0]["eac3"] else ".ac3")), frames, 1000 + i, kind)
        first = kind[0]
        print(name + (".eac3" if first["eac3"] else ".ac3"), "remix" if len(kind) == 1 and ((first["lfe"] and first["mode"] != 7) or first["mode"] in (4, 6)) else "-")
