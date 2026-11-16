#include "../audio_codec.h"
#include "../soft_video.h"

namespace absent {

std::unique_ptr<SoftDecoder> openPlatformVideo(const Track&, const std::vector<uint8_t>&, const StreamShape&, std::string*) { return nullptr; }
std::unique_ptr<SoftEncoder> openPlatformEncoder(const EncodeSpec&, std::string*) { return nullptr; }
std::unique_ptr<PacketDecoder> openPlatformAudio(const Track&, const std::vector<uint8_t>&, std::string*) { return nullptr; }
std::unique_ptr<AacCore> openPlatformAac(int, int, int, std::string*) { return nullptr; }

}
