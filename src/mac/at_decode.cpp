#include "../aac_config.h"
#include "../audio_codec.h"
#include <AudioToolbox/AudioToolbox.h>
#include <algorithm>
#include <alloca.h>
#include <cstdlib>

namespace absent {

namespace {

const UInt32 kPull = 4096;
const OSStatus kStarved = 'more';

struct SystemAudio : PacketDecoder {
    AudioConverterRef converter = nullptr;
    AudioStreamBasicDescription from{};
    AudioStreamBasicDescription to{};
    std::vector<uint8_t> cookie;
    AudioChannelLayoutTag sourceLayout = 0;
    AudioChannelLayoutTag order = 0;
    const std::vector<uint8_t>* waiting = nullptr;
    float* scratch = nullptr;
    bool ending = false;
    AudioStreamPacketDescription description{};

    ~SystemAudio() override {
        if (converter) AudioConverterDispose(converter);
        free(scratch);
    }

    static OSStatus input(AudioConverterRef, UInt32* count, AudioBufferList* list, AudioStreamPacketDescription** descriptions, void* self) {
        auto* a = static_cast<SystemAudio*>(self);
        if (!a->waiting) {
            *count = 0;
            list->mBuffers[0].mData = nullptr;
            list->mBuffers[0].mDataByteSize = 0;
            return a->ending ? noErr : kStarved;
        }
        const std::vector<uint8_t>& packet = *a->waiting;
        a->waiting = nullptr;
        a->description.mStartOffset = 0;
        a->description.mVariableFramesInPacket = 0;
        a->description.mDataByteSize = UInt32(packet.size());
        list->mNumberBuffers = 1;
        list->mBuffers[0].mNumberChannels = UInt32(a->channels);
        list->mBuffers[0].mData = const_cast<uint8_t*>(packet.data());
        list->mBuffers[0].mDataByteSize = UInt32(packet.size());
        if (descriptions) *descriptions = &a->description;
        *count = 1;
        return noErr;
    }

    void pull(std::vector<float>& out) {
        uintptr_t frame = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
        volatile char* settle = static_cast<char*>(alloca(16 + (frame & 4095)));
        settle[0] = 0;
        while (converter) {
            AudioBufferList list;
            list.mNumberBuffers = 1;
            list.mBuffers[0].mNumberChannels = UInt32(channels);
            list.mBuffers[0].mDataByteSize = kPull * UInt32(channels) * 4;
            list.mBuffers[0].mData = scratch;
            UInt32 frames = kPull;
            OSStatus status = AudioConverterFillComplexBuffer(converter, input, this, &frames, &list, nullptr);
            out.insert(out.end(), scratch, scratch + size_t(frames) * size_t(channels));
            if (status == kStarved) break;
            if (status != noErr) {
                build();
                break;
            }
            if (!frames) break;
        }
        waiting = nullptr;
    }

    bool build() {
        if (converter) AudioConverterDispose(converter);
        converter = nullptr;
        ending = false;
        waiting = nullptr;
        if (AudioConverterNew(&from, &to, &converter) != noErr) return false;
        if (!cookie.empty() && AudioConverterSetProperty(converter, kAudioConverterDecompressionMagicCookie, UInt32(cookie.size()), cookie.data()) != noErr) return false;
        if (order) {
            AudioChannelLayout layout{};
            if (sourceLayout) {
                layout.mChannelLayoutTag = sourceLayout;
                AudioConverterSetProperty(converter, kAudioConverterInputChannelLayout, sizeof layout, &layout);
            }
            layout.mChannelLayoutTag = order;
            AudioConverterSetProperty(converter, kAudioConverterOutputChannelLayout, sizeof layout, &layout);
        }
        return true;
    }

    void reset() override { build(); }

    void feed(const std::vector<uint8_t>& packet, std::vector<float>& out) override {
        waiting = &packet;
        pull(out);
    }

    void finish(std::vector<float>& out) override {
        ending = true;
        pull(out);
    }
};

const int kMpegRates[4][3] = {{11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}, {44100, 48000, 32000}};

bool describeMpeg(const std::vector<uint8_t>& p, AudioStreamBasicDescription& d) {
    if (p.size() < 4 || p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
    int version = (p[1] >> 3) & 3;
    int layer = 4 - ((p[1] >> 1) & 3);
    int rate = (p[2] >> 2) & 3;
    if (version == 1 || layer == 4 || rate == 3) return false;
    d.mSampleRate = kMpegRates[version][rate];
    d.mChannelsPerFrame = (p[3] >> 6) == 3 ? 1 : 2;
    d.mFormatID = layer == 3 ? kAudioFormatMPEGLayer3 : layer == 2 ? kAudioFormatMPEGLayer2 : kAudioFormatMPEGLayer1;
    d.mFramesPerPacket = layer == 1 ? 384 : (layer == 3 && version != 3) ? 576 : 1152;
    return true;
}

UInt32 losslessFlag(int bits) {
    return bits == 16 ? kAppleLosslessFormatFlag_16BitSourceData : bits == 20 ? kAppleLosslessFormatFlag_20BitSourceData
        : bits == 24 ? kAppleLosslessFormatFlag_24BitSourceData : bits == 32 ? kAppleLosslessFormatFlag_32BitSourceData : 0;
}

std::vector<uint8_t> elementaryStreamOf(const std::vector<uint8_t>& specific) {
    Bytes b;
    b.u8(0x03);
    b.u8(uint32_t(23 + specific.size()));
    b.u16(0);
    b.u8(0);
    b.u8(0x04);
    b.u8(uint32_t(15 + specific.size()));
    b.u8(0x40);
    b.u8(0x15);
    b.u24(0);
    b.u32(0);
    b.u32(0);
    b.u8(0x05);
    b.u8(uint32_t(specific.size()));
    b.raw(specific.data(), specific.size());
    b.u8(0x06);
    b.u8(1);
    b.u8(0x02);
    return b.d;
}

AudioChannelLayoutTag orderOf(int channels) {
    switch (channels) {
        case 3: return kAudioChannelLayoutTag_MPEG_3_0_A;
        case 4: return kAudioChannelLayoutTag_MPEG_4_0_A;
        case 5: return kAudioChannelLayoutTag_MPEG_5_0_A;
        case 6: return kAudioChannelLayoutTag_MPEG_5_1_A;
        case 7: return kAudioChannelLayoutTag_MPEG_6_1_A;
        case 8: return kAudioChannelLayoutTag_MPEG_7_1_C;
    }
    return 0;
}

}

std::unique_ptr<PacketDecoder> openSystemAudio(const Track& track, const std::vector<uint8_t>& firstPacket, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    auto decoder = std::make_unique<SystemAudio>();
    AudioStreamBasicDescription& from = decoder->from;
    std::vector<uint8_t>& cookie = decoder->cookie;
    from.mSampleRate = track.sampleRate;
    from.mChannelsPerFrame = UInt32(track.channels);
    if (track.codec == fourcc("mp4a")) {
        if (track.configType == fourcc("esds") && track.config.size() > 4) cookie.assign(track.config.begin() + 4, track.config.end());
        else if (track.configType == fourcc("ASC ") && !track.config.empty() && track.config.size() < 100) cookie = elementaryStreamOf(track.config);
        else return fail("AAC track without a decoder configuration");
        AudioFormatInfo info{};
        info.mASBD.mFormatID = kAudioFormatMPEG4AAC;
        info.mMagicCookie = cookie.data();
        info.mMagicCookieSize = UInt32(cookie.size());
        UInt32 size = 0;
        if (AudioFormatGetPropertyInfo(kAudioFormatProperty_FormatList, sizeof info, &info, &size) != noErr || size < sizeof(AudioFormatListItem)) return fail("AAC configuration not understood");
        std::vector<AudioFormatListItem> items(size / sizeof(AudioFormatListItem));
        if (AudioFormatGetProperty(kAudioFormatProperty_FormatList, sizeof info, &info, &size, items.data()) != noErr) return fail("AAC configuration not understood");
        from = items[0].mASBD;
        decoder->sourceLayout = items[0].mChannelLayoutTag;
        decoder->prerollPackets = from.mFormatID == kAudioFormatMPEG4AAC ? 4 : 32;
    } else if (track.codec == fourcc(".mp3") || track.codec == fourcc(".mp2")) {
        if (!describeMpeg(firstPacket, from)) return fail("MPEG audio header not understood");
        decoder->prerollPackets = 12;
        decoder->lead = from.mFormatID == kAudioFormatMPEGLayer3 ? -529 : -241;
    } else if (track.codec == fourcc("alac")) {
        if (track.config.size() != 24) return fail("ALAC track without its configuration");
        cookie = track.config;
        from.mFormatID = kAudioFormatAppleLossless;
        from.mFramesPerPacket = View{cookie.data(), 4}.u32();
        from.mFormatFlags = losslessFlag(cookie[5]);
        from.mChannelsPerFrame = cookie[9];
        from.mSampleRate = View{cookie.data() + 20, 4}.u32();
        decoder->stateless = true;
    } else if (track.codec == fourcc("fLaC")) {
        if (track.config.size() != 34) return fail("FLAC track without its stream information");
        const uint8_t* info = track.config.data();
        cookie.assign(info, info + 34);
        from.mFormatID = kAudioFormatFLAC;
        from.mFramesPerPacket = View{info, 2}.u16() == View{info + 2, 2}.u16() ? View{info, 2}.u16() : 0;
        decoder->longestPacket = std::max<int>(16, View{info + 2, 2}.u16());
        from.mSampleRate = (uint32_t(info[10]) << 12) | (uint32_t(info[11]) << 4) | (info[12] >> 4);
        from.mChannelsPerFrame = ((info[12] >> 1) & 7) + 1;
        from.mFormatFlags = losslessFlag((((info[12] & 1) << 4) | (info[13] >> 4)) + 1);
        decoder->stateless = true;
    } else {
        return fail("audio codec not supported");
    }
    if (from.mSampleRate < 1 || from.mChannelsPerFrame < 1 || from.mChannelsPerFrame > 8) return fail("audio layout not supported");
    AudioStreamBasicDescription& to = decoder->to;
    to.mSampleRate = from.mSampleRate;
    to.mFormatID = kAudioFormatLinearPCM;
    to.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
    to.mBitsPerChannel = 32;
    to.mChannelsPerFrame = from.mChannelsPerFrame;
    to.mBytesPerFrame = to.mChannelsPerFrame * 4;
    to.mBytesPerPacket = to.mBytesPerFrame;
    to.mFramesPerPacket = 1;
    if (from.mFormatID == kAudioFormatFLAC) {
        Bytes box;
        size_t at = box.open(fourcc("dfLa"), 0, 0);
        box.u8(0x80);
        box.u24(34);
        box.raw(cookie.data(), 34);
        box.close(at);
        cookie = box.d;
    }
    decoder->order = orderOf(int(from.mChannelsPerFrame));
    decoder->scratch = static_cast<float*>(aligned_alloc(16384, size_t(kPull) * 64 * sizeof(float)));
    if (!decoder->scratch || !decoder->build()) return fail("the system has no decoder for this audio");
    decoder->rate = int(from.mSampleRate);
    decoder->channels = int(from.mChannelsPerFrame);
    decoder->packetFrames = int(from.mFramesPerPacket);
    return decoder;
}

}
