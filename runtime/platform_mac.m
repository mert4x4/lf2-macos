// macOS implementation of platform.h using AppKit, Core Animation and
// AudioToolbox. No third-party libraries.
#import <AudioToolbox/AudioToolbox.h>
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <pthread.h>
#include <time.h>
#include "platform.h"

// ---- time -------------------------------------------------------------------
u64 plat_micros(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000; }
void plat_delay(u32 ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, 0);
}

// ---- events -----------------------------------------------------------------
static PlatEvent ring[256];
static int r_head, r_tail;
static void push(PlatEvent e) {
    int n = (r_tail + 1) % 256;
    if (n == r_head) return;
    ring[r_tail] = e;
    r_tail = n;
}

@interface GameView : NSView
@end
@implementation GameView
- (BOOL)isFlipped {
    return YES;
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (BOOL)acceptsFirstMouse:(NSEvent *)event {
    return YES;
}
@end

@interface GameDelegate : NSObject <NSWindowDelegate>
@end
@implementation GameDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender {
    push((PlatEvent){.type = PLAT_CLOSE});
    return NO;
}
- (void)quit:(id)sender {
    push((PlatEvent){.type = PLAT_CLOSE});
}
- (void)active:(NSNotification *)n {
    push((PlatEvent){.type = PLAT_FOCUS, .down = 1});
}
- (void)inactive:(NSNotification *)n {
    push((PlatEvent){.type = PLAT_FOCUS, .down = 0});
}
@end

static NSWindow *g_window;
static GameView *g_view;
static GameDelegate *g_delegate;
static int g_w, g_h;
static NSEventModifierFlags g_mods;

// macOS virtual key codes (physical key positions) -> Windows VK codes.
static int mac_to_vk(unsigned short k) {
    static const unsigned char map[128] = {
        [0] = 'A', [1] = 'S', [2] = 'D', [3] = 'F', [4] = 'H', [5] = 'G', [6] = 'Z',
        [7] = 'X', [8] = 'C', [9] = 'V', [11] = 'B', [12] = 'Q', [13] = 'W', [14] = 'E',
        [15] = 'R', [16] = 'Y', [17] = 'T', [18] = '1', [19] = '2', [20] = '3',
        [21] = '4', [22] = '6', [23] = '5', [24] = 0xBB, [25] = '9', [26] = '7',
        [27] = 0xBD, [28] = '8', [29] = '0', [30] = 0xDD, [31] = 'O', [32] = 'U',
        [33] = 0xDB, [34] = 'I', [35] = 'P', [36] = 0x0D, [37] = 'L', [38] = 'J',
        [39] = 0xDE, [40] = 'K', [41] = 0xBA, [42] = 0xDC, [43] = 0xBC, [44] = 0xBF,
        [45] = 'N', [46] = 'M', [47] = 0xBE, [48] = 0x09, [49] = 0x20, [50] = 0xC0,
        [51] = 0x08, [53] = 0x1B, [65] = 0x6E, [67] = 0x6A, [69] = 0x6B, [75] = 0x6F,
        [76] = 0x0D, [78] = 0x6D, [82] = 0x60, [83] = 0x61, [84] = 0x62, [85] = 0x63,
        [86] = 0x64, [87] = 0x65, [88] = 0x66, [89] = 0x67, [91] = 0x68, [92] = 0x69,
        [96] = 0x74, [97] = 0x75, [98] = 0x76, [99] = 0x72, [100] = 0x77, [101] = 0x78,
        [103] = 0x7A, [109] = 0x79, [111] = 0x7B, [115] = 0x24, [116] = 0x21,
        [117] = 0x2E, [118] = 0x73, [119] = 0x23, [120] = 0x71, [121] = 0x22,
        [122] = 0x70, [123] = 0x25, [124] = 0x27, [125] = 0x28, [126] = 0x26,
    };
    return k < 128 ? map[k] : 0;
}

// Maps a window location to game pixels; returns 0 when outside the picture.
static int game_point(NSEvent *ev, int *x, int *y) {
    if (ev.window != g_window) return 0;
    NSPoint p = [g_view convertPoint:ev.locationInWindow fromView:nil];
    NSSize b = g_view.bounds.size;
    double s = fmin(b.width / g_w, b.height / g_h);
    if (s <= 0) return 0;
    double gx = (p.x - (b.width - g_w * s) / 2) / s, gy = (p.y - (b.height - g_h * s) / 2) / s;
    *x = (int)gx;
    *y = (int)gy;
    return gx >= 0 && gy >= 0 && gx < g_w && gy < g_h;
}

static void modifier(NSEventModifierFlags now, NSEventModifierFlags bit, int vk) {
    if ((now & bit) != (g_mods & bit))
        push((PlatEvent){.type = PLAT_KEY, .vk = vk, .down = (now & bit) != 0});
}

static void pump(void) {
    @autoreleasepool {
        for (;;) {
            NSEvent *ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                             untilDate:nil
                                                inMode:NSDefaultRunLoopMode
                                               dequeue:YES];
            if (!ev) break;
            int x, y;
            switch (ev.type) {
            case NSEventTypeKeyDown:
                if (ev.modifierFlags & NSEventModifierFlagCommand) break; // menu shortcuts
                if (!ev.isARepeat)
                    push((PlatEvent){.type = PLAT_KEY, .vk = mac_to_vk(ev.keyCode), .down = 1});
                continue; // not forwarded: AppKit would beep at unhandled keys
            case NSEventTypeKeyUp:
                push((PlatEvent){.type = PLAT_KEY, .vk = mac_to_vk(ev.keyCode), .down = 0});
                continue;
            case NSEventTypeFlagsChanged: {
                NSEventModifierFlags now = ev.modifierFlags;
                modifier(now, NSEventModifierFlagShift, 0x10);
                modifier(now, NSEventModifierFlagControl, 0x11);
                modifier(now, NSEventModifierFlagOption, 0x12);
                g_mods = now;
                break;
            }
            case NSEventTypeMouseMoved:
            case NSEventTypeLeftMouseDragged:
            case NSEventTypeRightMouseDragged:
                if (game_point(ev, &x, &y))
                    push((PlatEvent){.type = PLAT_MOUSE_MOVE, .x = x, .y = y});
                break;
            case NSEventTypeLeftMouseDown:
            case NSEventTypeLeftMouseUp:
            case NSEventTypeRightMouseDown:
            case NSEventTypeRightMouseUp:
                if (game_point(ev, &x, &y))
                    push((PlatEvent){.type = PLAT_MOUSE_BUTTON,
                                     .x = x,
                                     .y = y,
                                     .left = ev.type == NSEventTypeLeftMouseDown ||
                                             ev.type == NSEventTypeLeftMouseUp,
                                     .down = ev.type == NSEventTypeLeftMouseDown ||
                                             ev.type == NSEventTypeRightMouseDown});
                break;
            default:
                break;
            }
            [NSApp sendEvent:ev];
        }
    }
}

int plat_poll(PlatEvent *e) {
    if (r_head == r_tail && NSApp) pump();
    if (r_head == r_tail) return 0;
    *e = ring[r_head];
    r_head = (r_head + 1) % 256;
    return 1;
}

// ---- window -----------------------------------------------------------------
void plat_window_open(const char *title, int w, int h) {
    @autoreleasepool {
        g_w = w;
        g_h = h;
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        g_delegate = [GameDelegate new];
        NSString *name = [NSString stringWithUTF8String:title];

        NSMenu *bar = [NSMenu new], *app = [NSMenu new], *view = [NSMenu new];
        NSMenuItem *appItem = [bar addItemWithTitle:@"" action:nil keyEquivalent:@""];
        NSMenuItem *viewItem = [bar addItemWithTitle:@"View" action:nil keyEquivalent:@""];
        [[app addItemWithTitle:[@"Hide " stringByAppendingString:name]
                        action:@selector(hide:)
                 keyEquivalent:@"h"] setTarget:NSApp];
        [app addItem:[NSMenuItem separatorItem]];
        [[app addItemWithTitle:[@"Quit " stringByAppendingString:name]
                        action:@selector(quit:)
                 keyEquivalent:@"q"] setTarget:g_delegate];
        NSMenuItem *fs = [view addItemWithTitle:@"Toggle Full Screen"
                                         action:@selector(toggleFullScreen:)
                                  keyEquivalent:@"f"];
        fs.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
        view.title = @"View";
        appItem.submenu = app;
        viewItem.submenu = view;
        NSApp.mainMenu = bar;

        g_window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, w, h)
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        g_window.title = name;
        g_window.delegate = g_delegate;
        g_window.releasedWhenClosed = NO;
        g_window.acceptsMouseMovedEvents = YES;
        g_window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        g_window.contentAspectRatio = NSMakeSize(w, h);
        g_window.backgroundColor = NSColor.blackColor;

        g_view = [[GameView alloc] initWithFrame:NSMakeRect(0, 0, w, h)];
        g_view.wantsLayer = YES;
        g_view.layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
        g_view.layer.contentsGravity = kCAGravityResizeAspect;
        g_view.layer.magnificationFilter = kCAFilterNearest;
        g_window.contentView = g_view;
        [g_window makeFirstResponder:g_view];
        [g_window center];

        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
        [nc addObserver:g_delegate
               selector:@selector(active:)
                   name:NSApplicationDidBecomeActiveNotification
                 object:nil];
        [nc addObserver:g_delegate
               selector:@selector(inactive:)
                   name:NSApplicationDidResignActiveNotification
                 object:nil];

        [NSApp finishLaunching];
        [g_window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        g_mods = 0;
    }
}

void plat_present(const u32 *xrgb, int w, int h) {
    if (!g_view) return;
    @autoreleasepool {
        static CGColorSpaceRef space;
        if (!space) space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CFDataRef data = CFDataCreate(0, (const UInt8 *)xrgb, (CFIndex)w * h * 4);
        CGDataProviderRef prov = CGDataProviderCreateWithCFData(data);
        CGImageRef img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, space,
                                       kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little,
                                       prov, 0, false, kCGRenderingIntentDefault);
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        g_view.layer.contents = (__bridge id)img;
        [CATransaction commit];
        [CATransaction flush];
        CGImageRelease(img);
        CGDataProviderRelease(prov);
        CFRelease(data);
    }
}

int plat_message_box(const char *title, const char *text, int kind) {
    @autoreleasepool {
        if (!NSApp) return kind == PLAT_BOX_YESNO ? 6 : 1;
        NSAlert *a = [NSAlert new];
        a.messageText = title && *title ? [NSString stringWithUTF8String:title]
                                        : @"Little Fighter 2";
        a.informativeText = [NSString stringWithCString:text
                                               encoding:NSWindowsCP1252StringEncoding]
                                ?: @"";
        if (kind == PLAT_BOX_YESNO) {
            [a addButtonWithTitle:@"Yes"];
            [a addButtonWithTitle:@"No"];
        } else {
            [a addButtonWithTitle:@"OK"];
            if (kind == PLAT_BOX_OKCANCEL) [a addButtonWithTitle:@"Cancel"];
        }
        int first = [a runModal] == NSAlertFirstButtonReturn;
        g_mods = NSEvent.modifierFlags;
        if (kind == PLAT_BOX_YESNO) return first ? 6 : 7;
        return first ? 1 : 2;
    }
}

// ---- audio ------------------------------------------------------------------
static pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static void (*g_mix)(s16 *out, int frames);
static AudioUnit g_unit;

void plat_audio_lock(void) { pthread_mutex_lock(&audio_mutex); }
void plat_audio_unlock(void) { pthread_mutex_unlock(&audio_mutex); }

static OSStatus render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts,
                       UInt32 bus, UInt32 frames, AudioBufferList *io) {
    pthread_mutex_lock(&audio_mutex);
    g_mix((s16 *)io->mBuffers[0].mData, (int)frames);
    pthread_mutex_unlock(&audio_mutex);
    return noErr;
}

int plat_audio_start(void (*mix)(s16 *out, int frames)) {
    if (g_unit) return 1;
    g_mix = mix;
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
                                   kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(0, &d);
    if (!comp || AudioComponentInstanceNew(comp, &g_unit)) return 0;
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = 44100;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 16;
    f.mBytesPerFrame = 4;
    f.mFramesPerPacket = 1;
    f.mBytesPerPacket = 4;
    AURenderCallbackStruct cb = {render, 0};
    if (AudioUnitSetProperty(g_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                             &f, sizeof f) ||
        AudioUnitSetProperty(g_unit, kAudioUnitProperty_SetRenderCallback,
                             kAudioUnitScope_Input, 0, &cb, sizeof cb) ||
        AudioUnitInitialize(g_unit) || AudioOutputUnitStart(g_unit)) {
        AudioComponentInstanceDispose(g_unit);
        g_unit = 0;
        return 0;
    }
    return 1;
}
