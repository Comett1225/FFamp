#include "absent/media.h"
#include "../bytes.h"
#include <AudioToolbox/AudioToolbox.h>
#include <algorithm>

namespace absent {

namespace {

const OSStatus kStarved = 'more';
const int kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};

}

struct AacEncoder::Impl {
    AudioConverterRef converter = nullptr;
    int rate = 0;
    int channels = 0;
    int bitrate = 0;
    int priming = 0;
    std::vector<int16_t> pending;
    size_t head = 0;
    bool ending = false;
    std::vector<uint8_t> packet;

    ~Impl() { if (converter) AudioConverterDispose(converter); }

    static OSStatus input(AudioConverterRef, UInt32* count, AudioBufferList* list, AudioStreamPacketDescription**, void* self) {
        auto* a = static_cast<Impl*>(self);
        size_t available = (a->pending.size() - a->head) / size_t(a->channels);
        if (!available) {
            *count = 0;
            list->mBuffers[0].mData = nullptr;
            list->mBuffers[0].mDataByteSize = 0;
            return a->ending ? noErr : kStarved;
        }
        UInt32 frames = UInt32(std::min<size_t>(*count, available));
        list->mNumberBuffers = 1;
        list->mBuffers[0].mNumberChannels = UInt32(a->channels);
        list->mBuffers[0].mData = a->pending.data() + a->head;
        list->mBuffers[0].mDataByteSize = frames * UInt32(a->channels) * 2;
        a->head += frames * size_t(a->channels);
        *count = frames;
        return noErr;
    }

    bool drain(const AudioSink& sink) {
        for (;;) {
            AudioBufferList list;
            list.mNumberBuffers = 1;
            list.mBuffers[0].mNumberChannels = UInt32(channels);
            list.mBuffers[0].mDataByteSize = UInt32(packet.size());
            list.mBuffers[0].mData = packet.data();
            UInt32 count = 1;
            AudioStreamPacketDescription description{};
            OSStatus status = AudioConverterFillComplexBuffer(converter, input, this, &count, &list, &description);
            if (count) sink(packet.data(), list.mBuffers[0].mDataByteSize);
            if (status == kStarved || (status == noErr && !count)) break;
            if (status != noErr) return false;
        }
        pending.erase(pending.begin(), pending.begin() + long(head));
        head = 0;
        return true;
    }
};

AacEncoder::AacEncoder() : impl(new Impl) {}
AacEncoder::~AacEncoder() = default;

std::unique_ptr<AacEncoder> AacEncoder::open(int rate, int channels, int bitrate, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return nullptr;
    };
    if (channels < 1 || channels > 2 || std::find(std::begin(kRates), std::end(kRates), rate) == std::end(kRates)) return fail("audio layout not supported");
    std::unique_ptr<AacEncoder> encoder(new AacEncoder);
    Impl& a = *encoder->impl;
    a.rate = rate;
    a.channels = channels;
    a.bitrate = bitrate;
    AudioStreamBasicDescription from{};
    from.mSampleRate = rate;
    from.mFormatID = kAudioFormatLinearPCM;
    from.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    from.mBitsPerChannel = 16;
    from.mChannelsPerFrame = UInt32(channels);
    from.mBytesPerFrame = UInt32(channels) * 2;
    from.mBytesPerPacket = from.mBytesPerFrame;
    from.mFramesPerPacket = 1;
    AudioStreamBasicDescription to{};
    to.mSampleRate = rate;
    to.mFormatID = kAudioFormatMPEG4AAC;
    to.mChannelsPerFrame = UInt32(channels);
    to.mFramesPerPacket = 1024;
    if (AudioConverterNew(&from, &to, &a.converter) != noErr) return fail("the system AAC encoder is not available");
    UInt32 control = kAudioCodecBitRateControlMode_Constant;
    AudioConverterSetProperty(a.converter, kAudioCodecPropertyBitRateControlMode, sizeof(control), &control);
    UInt32 rateValue = UInt32(bitrate);
    UInt32 listSize = 0;
    if (AudioConverterGetPropertyInfo(a.converter, kAudioConverterApplicableEncodeBitRates, &listSize, nullptr) == noErr && listSize >= sizeof(AudioValueRange)) {
        std::vector<AudioValueRange> rates(listSize / sizeof(AudioValueRange));
        if (AudioConverterGetProperty(a.converter, kAudioConverterApplicableEncodeBitRates, &listSize, rates.data()) == noErr) {
            double best = 0, least = 0;
            for (const AudioValueRange& range : rates) {
                if (range.mMinimum <= bitrate) best = std::max(best, std::min<double>(range.mMaximum, bitrate));
                least = least ? std::min(least, range.mMinimum) : range.mMinimum;
            }
            if (best > 0 || least > 0) rateValue = UInt32(best > 0 ? best : least);
        }
    }
    AudioConverterSetProperty(a.converter, kAudioConverterEncodeBitRate, sizeof(rateValue), &rateValue);
    UInt32 largest = 0;
    UInt32 size = sizeof(largest);
    if (AudioConverterGetProperty(a.converter, kAudioConverterPropertyMaximumOutputPacketSize, &size, &largest) != noErr || !largest) largest = 8192;
    a.packet.resize(largest);
    AudioConverterPrimeInfo prime{};
    size = sizeof(prime);
    if (AudioConverterGetProperty(a.converter, kAudioConverterPrimeInfo, &size, &prime) == noErr) a.priming = int(prime.leadingFrames);
    return encoder;
}

bool AacEncoder::encode(const int16_t* pcm, size_t frames, const AudioSink& sink) {
    impl->pending.insert(impl->pending.end(), pcm, pcm + frames * size_t(impl->channels));
    return impl->drain(sink);
}

bool AacEncoder::finish(const AudioSink& sink) {
    impl->ending = true;
    return impl->drain(sink);
}

int AacEncoder::priming() const { return impl->priming; }

std::vector<uint8_t> AacEncoder::config() const {
    const int index = int(std::find(std::begin(kRates), std::end(kRates), impl->rate) - std::begin(kRates));
    Bytes b;
    b.u32(0);
    b.u8(0x03);
    b.u8(25);
    b.u16(0);
    b.u8(0);
    b.u8(0x04);
    b.u8(17);
    b.u8(0x40);
    b.u8(0x15);
    b.u24(0);
    b.u32(uint32_t(impl->bitrate));
    b.u32(uint32_t(impl->bitrate));
    b.u8(0x05);
    b.u8(2);
    b.u8((2 << 3) | (index >> 1));
    b.u8(((index & 1) << 7) | (impl->channels << 3));
    b.u8(0x06);
    b.u8(1);
    b.u8(0x02);
    return b.d;
}

}
