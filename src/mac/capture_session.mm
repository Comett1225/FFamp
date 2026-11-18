#include "../capture.h"
#import <AVFoundation/AVFoundation.h>
#include <dispatch/dispatch.h>

using absent::CaptureRecorder;

@interface AbsentCaptureTap : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate, AVCaptureAudioDataOutputSampleBufferDelegate>
@property(nonatomic, assign) CaptureRecorder* recorder;
@property(nonatomic, strong) AVCaptureOutput* pictures;
@end

@implementation AbsentCaptureTap

- (void)captureOutput:(AVCaptureOutput*)output didOutputSampleBuffer:(CMSampleBufferRef)sample fromConnection:(AVCaptureConnection*)connection {
    (void)connection;
    CaptureRecorder* recorder = self.recorder;
    if (!recorder || !CMSampleBufferDataIsReady(sample)) return;
    const double seconds = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample));
    if (output == self.pictures) {
        CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample);
        if (!image) return;
        const bool seen = absent::withPixelBuffer(image, [&](const absent::Picture& picture) {
            recorder->video(picture, image, seconds);
            return true;
        });
        if (!seen) recorder->fail("the camera gives pictures in a layout that is not supported");
        return;
    }
    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
    const AudioStreamBasicDescription* shape = format ? CMAudioFormatDescriptionGetStreamBasicDescription(format) : nullptr;
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    if (!shape || !block) return;
    const bool plain = shape->mFormatID == kAudioFormatLinearPCM && shape->mBitsPerChannel == 16 && (shape->mFormatFlags & kAudioFormatFlagIsSignedInteger) &&
        !(shape->mFormatFlags & kAudioFormatFlagIsBigEndian) && !(shape->mFormatFlags & kAudioFormatFlagIsNonInterleaved) && shape->mChannelsPerFrame >= 1 && shape->mChannelsPerFrame <= 2;
    if (!plain) {
        recorder->fail("the microphone gives sound in a layout that is not supported");
        return;
    }
    const size_t frames = size_t(CMSampleBufferGetNumSamples(sample));
    const size_t bytes = frames * shape->mChannelsPerFrame * 2;
    if (CMBlockBufferGetDataLength(block) < bytes) return;
    std::vector<int16_t> pcm(frames * shape->mChannelsPerFrame);
    if (CMBlockBufferCopyDataBytes(block, 0, bytes, pcm.data()) != kCMBlockBufferNoErr) return;
    recorder->audio(pcm.data(), frames, int(shape->mSampleRate + 0.5), int(shape->mChannelsPerFrame), seconds);
}

@end

namespace absent {

namespace {

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
NSArray<AVCaptureDevice*>* found(bool video) {
    NSMutableArray<AVCaptureDeviceType>* kinds = [NSMutableArray array];
    if (video) {
        [kinds addObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];
        if (@available(macOS 14.0, *)) {
            [kinds addObject:AVCaptureDeviceTypeExternal];
            [kinds addObject:AVCaptureDeviceTypeContinuityCamera];
        } else {
            [kinds addObject:AVCaptureDeviceTypeExternalUnknown];
        }
    } else if (@available(macOS 14.0, *)) {
        [kinds addObject:AVCaptureDeviceTypeMicrophone];
    } else {
        [kinds addObject:AVCaptureDeviceTypeBuiltInMicrophone];
    }
    AVCaptureDeviceDiscoverySession* search = [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:kinds mediaType:video ? AVMediaTypeVideo : AVMediaTypeAudio
        position:AVCaptureDevicePositionUnspecified];
    return search.devices ?: @[];
}
#pragma clang diagnostic pop

AVCaptureDevice* chosen(const std::string& id, bool video) {
    NSArray<AVCaptureDevice*>* all = found(video);
    if (id == "default") return all.count ? all[0] : nil;
    NSString* wanted = [NSString stringWithUTF8String:id.c_str()];
    for (AVCaptureDevice* device in all) if ([device.uniqueID isEqualToString:wanted]) return device;
    return nil;
}

bool allowed(bool video, bool ask) {
    AVMediaType kind = video ? AVMediaTypeVideo : AVMediaTypeAudio;
    AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:kind];
    if (status == AVAuthorizationStatusNotDetermined && ask) {
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [AVCaptureDevice requestAccessForMediaType:kind completionHandler:^(BOOL) { dispatch_semaphore_signal(done); }];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        status = [AVCaptureDevice authorizationStatusForMediaType:kind];
    }
    return status == AVAuthorizationStatusAuthorized;
}

}

std::vector<CaptureDevice> captureDevices() {
    std::vector<CaptureDevice> list;
    @autoreleasepool {
        for (int video = 1; video >= 0; --video) {
            for (AVCaptureDevice* device in found(video != 0)) {
                CaptureDevice item;
                item.id = device.uniqueID.UTF8String ?: "";
                item.name = device.localizedName.UTF8String ?: "";
                item.video = video != 0;
                list.push_back(item);
            }
        }
    }
    return list;
}

CaptureAccess captureAccess(bool video) {
    switch ([AVCaptureDevice authorizationStatusForMediaType:video ? AVMediaTypeVideo : AVMediaTypeAudio]) {
        case AVAuthorizationStatusAuthorized: return CaptureAccess::Authorized;
        case AVAuthorizationStatusNotDetermined: return CaptureAccess::Undetermined;
        case AVAuthorizationStatusDenied: return CaptureAccess::Denied;
        default: return CaptureAccess::Restricted;
    }
}

struct CaptureSession::Impl {
    std::unique_ptr<CaptureRecorder> recorder;
    std::unique_ptr<TestSource> test;
    AVCaptureSession* session = nil;
    AbsentCaptureTap* tap = nil;
    dispatch_queue_t queue = nullptr;
    id watcher = nil;

    void halt() {
        test.reset();
        if (session) {
            [session stopRunning];
            if (watcher) [[NSNotificationCenter defaultCenter] removeObserver:watcher];
            watcher = nil;
            AbsentCaptureTap* held = tap;
            dispatch_sync(queue, ^{ held.recorder = nullptr; });
            session = nil;
        }
    }

    ~Impl() { halt(); }
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
    std::unique_ptr<CaptureSession> out(new CaptureSession);
    Impl& s = *out->impl;
    s.recorder = std::make_unique<CaptureRecorder>(spec, sound);
    if (test) {
        s.test = std::make_unique<TestSource>(*s.recorder, sound);
        return out;
    }
    @autoreleasepool {
        if (pictures && !allowed(true, spec.ask)) return fail("camera access has not been granted");
        if (sound && !spec.record.empty() && !allowed(false, spec.ask)) return fail("microphone access has not been granted");
        AVCaptureDevice* camera = pictures ? chosen(spec.video, true) : nil;
        if (pictures && !camera) return fail("no camera with the id " + spec.video);
        AVCaptureDevice* microphone = sound && !spec.record.empty() ? chosen(spec.audio, false) : nil;
        if (sound && !spec.record.empty() && !microphone) return fail("no microphone with the id " + spec.audio);
        NSError* trouble = nil;
        AVCaptureSession* session = [[AVCaptureSession alloc] init];
        [session beginConfiguration];
        if ([session canSetSessionPreset:AVCaptureSessionPresetHigh]) session.sessionPreset = AVCaptureSessionPresetHigh;
        s.tap = [[AbsentCaptureTap alloc] init];
        s.tap.recorder = s.recorder.get();
        s.queue = dispatch_queue_create("absent.capture", DISPATCH_QUEUE_SERIAL);
        if (camera) {
            AVCaptureDeviceInput* lens = [AVCaptureDeviceInput deviceInputWithDevice:camera error:&trouble];
            if (!lens || ![session canAddInput:lens]) return fail("the camera could not be opened");
            [session addInput:lens];
            AVCaptureVideoDataOutput* frames = [[AVCaptureVideoDataOutput alloc] init];
            frames.videoSettings = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)};
            frames.alwaysDiscardsLateVideoFrames = YES;
            [frames setSampleBufferDelegate:s.tap queue:s.queue];
            if (![session canAddOutput:frames]) return fail("the camera pictures could not be read");
            [session addOutput:frames];
            s.tap.pictures = frames;
        }
        if (microphone) {
            AVCaptureDeviceInput* voice = [AVCaptureDeviceInput deviceInputWithDevice:microphone error:&trouble];
            if (!voice || ![session canAddInput:voice]) return fail("the microphone could not be opened");
            [session addInput:voice];
            AVCaptureAudioDataOutput* sounds = [[AVCaptureAudioDataOutput alloc] init];
            sounds.audioSettings = @{AVFormatIDKey: @(kAudioFormatLinearPCM), AVSampleRateKey: @48000, AVNumberOfChannelsKey: @2, AVLinearPCMBitDepthKey: @16,
                AVLinearPCMIsFloatKey: @NO, AVLinearPCMIsBigEndianKey: @NO, AVLinearPCMIsNonInterleaved: @NO};
            [sounds setSampleBufferDelegate:s.tap queue:s.queue];
            if (![session canAddOutput:sounds]) return fail("the microphone sound could not be read");
            [session addOutput:sounds];
        }
        [session commitConfiguration];
        AbsentCaptureTap* tap = s.tap;
        dispatch_queue_t queue = s.queue;
        s.watcher = [[NSNotificationCenter defaultCenter] addObserverForName:AVCaptureSessionRuntimeErrorNotification object:session queue:nil usingBlock:^(NSNotification* note) {
            NSError* reason = note.userInfo[AVCaptureSessionErrorKey];
            const std::string text = std::string("the capture stopped: ") + (reason.localizedDescription.UTF8String ?: "unknown reason");
            dispatch_async(queue, ^{
                if (CaptureRecorder* recorder = tap.recorder) recorder->fail(text);
            });
        }];
        s.session = session;
        [session startRunning];
        if (!session.isRunning) return fail("the capture did not start");
    }
    return out;
}

bool CaptureSession::failed(std::string* error) const { return impl->recorder->failed(error); }
CaptureTotals CaptureSession::totals() const { return impl->recorder->totals(); }

bool CaptureSession::finish(std::string* error) {
    impl->halt();
    return impl->recorder->finish(error);
}

}
