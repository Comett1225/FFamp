#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc < 3) return 2;
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]];
        ExtAudioFileRef file = NULL;
        if (ExtAudioFileOpenURL((__bridge CFURLRef)url, &file) != noErr) return 1;
        AudioStreamBasicDescription source;
        UInt32 size = sizeof(source);
        if (ExtAudioFileGetProperty(file, kExtAudioFileProperty_FileDataFormat, &size, &source) != noErr) return 1;
        BOOL floating = strcmp(argv[2], "f32") == 0;
        int bits = floating ? 32 : atoi(argv[2] + 1);
        AudioStreamBasicDescription wanted = {0};
        wanted.mSampleRate = source.mSampleRate;
        wanted.mFormatID = kAudioFormatLinearPCM;
        wanted.mFormatFlags = (floating ? kAudioFormatFlagIsFloat : kAudioFormatFlagIsSignedInteger) | kAudioFormatFlagIsPacked;
        wanted.mBitsPerChannel = bits;
        wanted.mChannelsPerFrame = source.mChannelsPerFrame;
        wanted.mBytesPerFrame = source.mChannelsPerFrame * bits / 8;
        wanted.mBytesPerPacket = wanted.mBytesPerFrame;
        wanted.mFramesPerPacket = 1;
        if (ExtAudioFileSetProperty(file, kExtAudioFileProperty_ClientDataFormat, sizeof(wanted), &wanted) != noErr) return 1;
        NSMutableData* block = [NSMutableData dataWithLength:wanted.mBytesPerFrame * 4096];
        long total = 0;
        for (;;) {
            AudioBufferList list;
            list.mNumberBuffers = 1;
            list.mBuffers[0].mNumberChannels = wanted.mChannelsPerFrame;
            list.mBuffers[0].mDataByteSize = (UInt32)block.length;
            list.mBuffers[0].mData = block.mutableBytes;
            UInt32 frames = 4096;
            if (ExtAudioFileRead(file, &frames, &list) != noErr) return 1;
            if (!frames) break;
            total += frames;
            if (argc > 3) fwrite(block.bytes, wanted.mBytesPerFrame, frames, stdout);
        }
        ExtAudioFileDispose(file);
        if (argc <= 3) printf("rate %.0f channels %u frames %ld\n", source.mSampleRate, (unsigned)source.mChannelsPerFrame, total);
        return 0;
    }
}
