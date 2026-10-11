// Decodes music with AudioToolbox, which handles every format macOS knows
// (AAC, MP3, WAV, ...) and converts it to the mixer's sample format.
#include <AudioToolbox/AudioToolbox.h>
#include <string.h>
#include "music.h"

int music_decode_file(const char *path, MusicSink sink, void *user, volatile int *cancel) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(0, (const UInt8 *)path,
                                                           (CFIndex)strlen(path), false);
    if (!url) return -1;
    ExtAudioFileRef file = 0;
    OSStatus err = ExtAudioFileOpenURL(url, &file);
    CFRelease(url);
    if (err) return -1;
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = 44100;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 16;
    f.mBytesPerFrame = 4;
    f.mFramesPerPacket = 1;
    f.mBytesPerPacket = 4;
    int rc = -1;
    if (!ExtAudioFileSetProperty(file, kExtAudioFileProperty_ClientDataFormat, sizeof f, &f)) {
        static _Thread_local s16 buf[8192 * 2];
        rc = 0;
        while (!*cancel) {
            AudioBufferList list = {1, {{2, sizeof buf, buf}}};
            UInt32 frames = 8192;
            if (ExtAudioFileRead(file, &frames, &list)) {
                rc = -1;
                break;
            }
            if (!frames) break;
            sink(user, buf, frames);
        }
    }
    ExtAudioFileDispose(file);
    return rc;
}
