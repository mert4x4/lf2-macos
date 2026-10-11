// Tables emitted by recomp.py describing the guest image.
#pragma once
#include "cpu.h"

struct GuestSection {
    const char *name;
    u32 va, vsize, raw, rawsize;
};
struct GuestImport {
    u32 slot;
    const char *dll, *name;
};
extern const u32 guest_entry, guest_image_base;
extern const struct GuestSection guest_sections[];
extern const struct GuestImport guest_imports[];
int guest_dispatch(CPU *c, u32 target);
