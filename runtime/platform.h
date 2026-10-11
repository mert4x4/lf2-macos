// Host platform services: window, input, audio output, timing, dialogs.
// platform_mac.m implements them with macOS system frameworks only.
#pragma once
#include "cpu.h"

enum { PLAT_KEY, PLAT_MOUSE_MOVE, PLAT_MOUSE_BUTTON, PLAT_FOCUS, PLAT_CLOSE };
typedef struct PlatEvent {
    int type;
    int vk;    // PLAT_KEY: Windows virtual-key code
    int down;  // PLAT_KEY / PLAT_MOUSE_BUTTON: pressed; PLAT_FOCUS: gained
    int left;  // PLAT_MOUSE_BUTTON: left button (else right)
    int x, y;  // mouse position in game pixels
} PlatEvent;

void plat_window_open(const char *title, int w, int h);
int plat_poll(PlatEvent *e); // 1 when an event was returned
void plat_present(const u32 *xrgb, int w, int h);

enum { PLAT_BOX_OK, PLAT_BOX_OKCANCEL, PLAT_BOX_YESNO };
int plat_message_box(const char *title, const char *text, int kind); // Win32 IDOK/IDCANCEL/IDYES/IDNO

u64 plat_micros(void); // monotonic microseconds
static inline u32 plat_ticks(void) { return (u32)(plat_micros() / 1000); }
void plat_delay(u32 ms);

// Starts audio output; `mix` fills interleaved stereo 16-bit frames at 44100 Hz
// on the audio thread while the audio lock is held.
int plat_audio_start(void (*mix)(s16 *out, int frames));
void plat_audio_lock(void);
void plat_audio_unlock(void);
