#include "../audio_codec.h"
#include "../soft_video.h"

namespace absent {

#ifdef ABSENT_VAAPI
std::unique_ptr<SoftDecoder> openVaapi(const Track& track, const std::vector<uint8_t>& config, const StreamShape& shape, std::string* error);
std::unique_ptr<SoftDecoder> openPlatformVideo(const Track& track, const std::vector<uint8_t>& config, const StreamShape& shape, std::string* error) { return openVaapi(track, config, shape, error); }
#else
std::unique_ptr<SoftDecoder> openPlatformVideo(const Track&, const std::vector<uint8_t>&, const StreamShape&, std::string*) { return nullptr; }
#endif
std::unique_ptr<SoftEncoder> openPlatformEncoder(const EncodeSpec&, std::string*) { return nullptr; }
std::unique_ptr<PacketDecoder> openPlatformAudio(const Track&, const std::vector<uint8_t>&, std::string*) { return nullptr; }
std::unique_ptr<AacCore> openPlatformAac(int, int, int, std::string*) { return nullptr; }

}
