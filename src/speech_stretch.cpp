#include "absent/capture.h"
#include <algorithm>
#include <cmath>

namespace absent {

namespace {

const double kPi = 3.14159265358979323846;
const double kSlowest = 0.25;
const double kFastest = 4.0;
const int64_t kLongest = int64_t(1) << 28;

std::vector<float> fitted(std::vector<float> sound, size_t length) {
    sound.resize(length, 0.0f);
    return sound;
}

}

std::vector<float> stretchSound(const std::vector<float>& in, int rate, double speed) {
    if (rate < 1000 || !(speed >= kSlowest) || speed > kFastest) return {};
    const size_t total = in.size();
    const size_t wanted = size_t(std::llround(double(total) / speed));
    const size_t hop = size_t(std::max(8, int(std::lround(rate * 0.020))));
    const size_t window = hop * 2;
    const long reach = std::max(4L, std::lround(rate * 0.010));
    if (std::fabs(speed - 1.0) < 1e-9 || total < window * 2) return fitted(in, wanted);
    std::vector<float> source(total + window * 2 + size_t(reach) * 2, 0.0f);
    std::copy(in.begin(), in.end(), source.begin());
    const long last = long(total);
    std::vector<float> rise(hop);
    for (size_t i = 0; i < hop; ++i) rise[i] = float(0.5 - 0.5 * std::cos(kPi * (double(i) + 0.5) / double(hop)));
    std::vector<float> out(wanted + window * 2, 0.0f);
    long position = 0;
    for (size_t i = 0; i < window; ++i) out[i] = source[i] * (i < hop ? 1.0f : rise[window - 1 - i]);
    for (size_t k = 1; k * hop < wanted + hop; ++k) {
        const long natural = position + long(hop);
        const long ideal = std::lround(double(k * hop) * speed);
        long best = std::clamp(ideal, 0L, last);
        if (natural >= ideal - reach && natural <= ideal + reach && natural <= last) {
            best = natural;
        } else if (natural <= last) {
            const float* expected = source.data() + natural;
            double bestScore = -1e300;
            const long low = std::clamp(ideal - reach, 0L, last), high = std::clamp(ideal + reach, 0L, last);
            double energy = 0;
            for (size_t i = 0; i < hop; ++i) energy += double(source[size_t(low) + i]) * source[size_t(low) + i];
            for (long candidate = low; candidate <= high; ++candidate) {
                const float* tried = source.data() + candidate;
                float dot = 0;
                for (size_t i = 0; i < hop; ++i) dot += tried[i] * expected[i];
                const double score = double(dot) / std::sqrt(energy + 1e-9);
                if (score > bestScore || (score == bestScore && std::labs(candidate - ideal) < std::labs(best - ideal))) {
                    bestScore = score;
                    best = candidate;
                }
                energy += double(tried[hop]) * tried[hop] - double(tried[0]) * tried[0];
                if (energy < 0) energy = 0;
            }
        }
        position = best;
        const float* frame = source.data() + position;
        float* target = out.data() + k * hop;
        for (size_t i = 0; i < hop; ++i) target[i] += frame[i] * rise[i];
        for (size_t i = 0; i < hop; ++i) target[hop + i] += frame[hop + i] * rise[hop - 1 - i];
    }
    return fitted(std::move(out), wanted);
}

std::vector<float> shapeSpeech(const std::vector<float>& mono, int rate, double speed, double pitch) {
    if (rate < 1000 || rate > 384000 || !(speed >= kSlowest) || speed > kFastest || !(pitch >= kSlowest) || pitch > kFastest) return {};
    const size_t wanted = size_t(std::llround(double(mono.size()) / speed));
    const int played = int(std::lround(rate * pitch));
    if (played == rate) return stretchSound(mono, rate, speed);
    const double first = std::clamp(speed / pitch, kSlowest, kFastest);
    std::vector<float> stretched = stretchSound(mono, rate, first);
    Resampler resampler(played, 1, rate, 1);
    resampler.push(stretched.data(), stretched.size());
    resampler.finish();
    std::vector<float> out(wanted + 4096);
    size_t made = 0;
    for (;;) {
        if (made == out.size()) out.resize(out.size() * 2);
        const size_t got = resampler.pull(out.data() + made, out.size() - made);
        if (!got) break;
        made += got;
    }
    out.resize(made);
    return fitted(std::move(out), wanted);
}

bool convertSpeech(const std::string& in, const std::string& out, int rate, double speed, double pitch, std::string* error) {
    auto fail = [&](const char* text) {
        if (error) *error = text;
        return false;
    };
    if (rate < 8000 || rate > 192000) return fail("the rate must be 8000 to 192000");
    if (!(speed >= kSlowest) || speed > kFastest || !(pitch >= kSlowest) || pitch > kFastest) return fail("speed and pitch must be 0.25 to 4");
    auto reader = PcmReader::open(in, rate, 1, error);
    if (!reader) return false;
    if (reader->length() <= 0) return fail("the voice file has no sound");
    if (reader->length() > kLongest) return fail("the voice file is too long");
    std::vector<float> voice(static_cast<size_t>(reader->length()));
    if (!reader->read(0, voice.size(), voice.data())) return fail("the voice file did not decode");
    const std::vector<float> shaped = shapeSpeech(voice, rate, speed, pitch);
    if (shaped.empty()) return fail("the voice came out empty");
    auto writer = AudioFileWriter::open(out, AudioFormat::Wav16, rate, 1, 0, error);
    if (!writer) return false;
    if (!writer->write(shaped.data(), shaped.size())) return fail("writing the voice failed");
    return writer->finish(error);
}

}
