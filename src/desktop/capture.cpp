#include "../capture.h"

namespace absent {

std::vector<CaptureDevice> captureDevices() { return {}; }

CaptureAccess captureAccess(bool) { return CaptureAccess::Authorized; }

struct CaptureSession::Impl {
    std::unique_ptr<CaptureRecorder> recorder;
    std::unique_ptr<TestSource> test;
};

CaptureSession::CaptureSession() : impl(new Impl) {}
CaptureSession::~CaptureSession() = default;

std::unique_ptr<CaptureSession> CaptureSession::open(const CaptureSpec& spec, std::string* error) {
    auto fail = [&](const std::string& text) {
        if (error) *error = text;
        return nullptr;
    };
    if (spec.video.empty() && (spec.audio.empty() || spec.record.empty())) return fail("capture needs a camera, or a microphone and a file to record");
    if (spec.record.empty() && !spec.preview) return fail("capture needs a file to record or a preview");
    if ((spec.previewWidth > 0) != (spec.previewHeight > 0) || spec.previewWidth > 16384 || spec.previewHeight > 16384) return fail("bad preview size");
    const bool sound = !spec.audio.empty();
    const bool pictures = !spec.video.empty();
    const bool test = pictures ? spec.video == kTestDevice : spec.audio == kTestDevice;
    if (pictures && sound && test != (spec.audio == kTestDevice)) return fail("the test devices cannot be mixed with real ones");
    if (test && !pictures) return fail("the test microphone needs the test camera");
    if (!test) return fail("cameras and microphones are not captured on this system yet");
    std::unique_ptr<CaptureSession> out(new CaptureSession);
    out->impl->recorder = std::make_unique<CaptureRecorder>(spec, sound);
    out->impl->test = std::make_unique<TestSource>(*out->impl->recorder, sound);
    return out;
}

bool CaptureSession::failed(std::string* error) const { return impl->recorder->failed(error); }
CaptureTotals CaptureSession::totals() const { return impl->recorder->totals(); }

bool CaptureSession::finish(std::string* error) {
    impl->test.reset();
    return impl->recorder->finish(error);
}

}
