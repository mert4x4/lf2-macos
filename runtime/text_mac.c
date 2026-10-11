// GDI TextOutA replacement: renders with CoreText into a 32-bit surface.
// GDI's default background mode is OPAQUE, so the text box is filled first.
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include "cpu.h"

#define LINE_H 16

void text_draw(u32 *px, int w, int h, int x, int y, const char *s, int n, u32 color,
               u32 bk) {
    static CTFontRef font;
    static CGColorSpaceRef gray;
    if (!font) {
        font = CTFontCreateWithName(CFSTR("Arial-BoldMT"), 13.0, 0);
        gray = CGColorSpaceCreateDeviceGray();
    }
    if (n <= 0) return;
    CFStringRef str = CFStringCreateWithBytes(0, (const UInt8 *)s, n,
                                              kCFStringEncodingWindowsLatin1, false);
    if (!str) return;
    CGFloat white[] = {1, 1};
    CGColorRef fg = CGColorCreate(gray, white);
    const void *keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
    const void *vals[] = {font, fg};
    CFDictionaryRef attr = CFDictionaryCreate(0, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = CFAttributedStringCreate(0, str, attr);
    CTLineRef line = CTLineCreateWithAttributedString(as);
    int tw = (int)ceil(CTLineGetTypographicBounds(line, 0, 0, 0)) + 1;
    if (tw > 0 && tw < 4096) {
        u8 *cov = calloc((size_t)tw, LINE_H);
        CGContextRef cg = CGBitmapContextCreate(cov, (size_t)tw, LINE_H, 8, (size_t)tw, gray,
                                                kCGImageAlphaNone);
        CGContextSetTextPosition(cg, 0, 4);
        CTLineDraw(line, cg);
        CGContextRelease(cg);
        u32 cr = (color >> 16) & 255, cg_ = (color >> 8) & 255, cb = color & 255;
        u32 br = (bk >> 16) & 255, bg = (bk >> 8) & 255, bb = bk & 255;
        for (int j = 0; j < LINE_H; j++) {
            int ty = y + j;
            if (ty < 0 || ty >= h) continue;
            for (int i = 0; i < tw; i++) {
                int tx = x + i;
                if (tx < 0 || tx >= w) continue;
                u32 a = cov[j * tw + i];
                u32 r = (cr * a + br * (255 - a)) / 255;
                u32 g = (cg_ * a + bg * (255 - a)) / 255;
                u32 b = (cb * a + bb * (255 - a)) / 255;
                px[(size_t)ty * (size_t)w + (size_t)tx] = (r << 16) | (g << 8) | b;
            }
        }
        free(cov);
    }
    CFRelease(line);
    CFRelease(as);
    CFRelease(attr);
    CGColorRelease(fg);
    CFRelease(str);
}
