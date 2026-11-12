#import <AVFoundation/AVFoundation.h>

static long count(AVAsset* asset, AVMediaType type, NSDictionary* settings) {
    NSArray<AVAssetTrack*>* tracks = [asset tracksWithMediaType:type];
    if (tracks.count == 0) return -1;
    NSError* error = nil;
    AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:asset error:&error];
    if (!reader) return -2;
    AVAssetReaderTrackOutput* output = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:tracks[0] outputSettings:settings];
    [reader addOutput:output];
    if (![reader startReading]) return -3;
    long total = 0;
    for (;;) {
        CMSampleBufferRef sample = [output copyNextSampleBuffer];
        if (!sample) break;
        total += [type isEqualToString:AVMediaTypeAudio] ? CMSampleBufferGetNumSamples(sample) : 1;
        CFRelease(sample);
    }
    return reader.status == AVAssetReaderStatusCompleted ? total : -4;
}

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 2) return 2;
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]];
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        BOOL playable = asset.playable;
        double seconds = CMTimeGetSeconds(asset.duration);
        long pictures = count(asset, AVMediaTypeVideo, @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)});
        long sound = count(asset, AVMediaTypeAudio, @{AVFormatIDKey: @(kAudioFormatLinearPCM)});
        AVAssetImageGenerator* generator = [AVAssetImageGenerator assetImageGeneratorWithAsset:asset];
        generator.requestedTimeToleranceBefore = kCMTimeZero;
        generator.requestedTimeToleranceAfter = kCMTimeZero;
        NSError* error = nil;
        CGImageRef image = [generator copyCGImageAtTime:CMTimeMakeWithSeconds(seconds / 2, 600) actualTime:NULL error:&error];
#pragma clang diagnostic pop
        printf("playable %d duration %.3f video_frames %ld audio_frames %ld still %s\n", playable, seconds, pictures, sound, image ? "ok" : "failed");
        BOOL ok = playable && pictures > 0 && sound != -2 && sound != -3 && sound != -4 && image;
        if (image) CGImageRelease(image);
        return ok ? 0 : 1;
    }
}
