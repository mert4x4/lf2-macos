// Music file decoding through macOS's own audio codecs (see music_mac.c).
#pragma once
#include <stddef.h>
#include "cpu.h"

// Receives decoded audio as interleaved stereo 16-bit samples at 44100 Hz.
typedef void (*MusicSink)(void *user, const s16 *stereo, size_t frames);

// Decodes a whole AAC/MP3/WAV/... file, calling `sink` as audio becomes
// available. Stops early when *cancel becomes non-zero. Returns 0 on success.
int music_decode_file(const char *path, MusicSink sink, void *user, volatile int *cancel);
