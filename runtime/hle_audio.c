// Sound: DirectSound buffers and DirectShow music mixed into one output
// stream, plus the WINMM mmio calls LF2 uses to read its .wav files.
#include <pthread.h>
#include <strings.h>
#include <unistd.h>
#include "hle.h"
#include "platform.h"
#include "music.h"

#define OUT_RATE 44100

static int g_dev; // audio output is running

static void lock_audio(void) { plat_audio_lock(); }
static void unlock_audio(void) { plat_audio_unlock(); }
static float db_to_gain(s32 hundredths) {
    if (hundredths <= -10000) return 0;
    if (hundredths > 0) hundredths = 0;
    return powf(10.0f, (float)hundredths / 2000.0f);
}

// ---- voices (IDirectSoundBuffer) ---------------------------------------------
typedef struct Voice {
    u32 obj, data, bytes;
    int channels, rate, bits, owner;
    int playing, loop;
    double pos; // in sample frames
    float gain;
    s32 volume, pan;
} Voice;
static Voice voices[1024];

// ---- music (DirectShow filter graph) -----------------------------------------
static struct Music {
    s16 *pcm;          // interleaved stereo at OUT_RATE, grows while decoding
    size_t frames, cap;
    size_t pos;
    int decoding, playing, ended, notify;
    float gain;
    pthread_t thread;
    int has_thread;
    volatile int cancel;
    char path[1024];
} music = {.gain = 1.0f};
static pthread_mutex_t music_lock = PTHREAD_MUTEX_INITIALIZER;

static void mix(s16 *out, int total_frames) {
    int frames = total_frames;
    static float acc[8192 * 2];
    if (frames > 8192) frames = 8192;
    memset(acc, 0, sizeof(float) * 2 * (size_t)frames);
    for (int v = 0; v < 1024; v++) {
        Voice *vo = &voices[v];
        if (!vo->obj || !vo->playing) continue;
        int bps = vo->bits / 8 * vo->channels;
        u32 total = vo->bytes / (u32)bps;
        double step = (double)vo->rate / OUT_RATE;
        const u8 *d = HOSTP(vo->data);
        float l = vo->gain, r = vo->gain;
        if (vo->pan < 0) r *= db_to_gain(vo->pan);
        if (vo->pan > 0) l *= db_to_gain(-vo->pan);
        for (int i = 0; i < frames; i++) {
            u32 p = (u32)vo->pos;
            if (p >= total) {
                if (!vo->loop || !total) {
                    vo->playing = 0;
                    vo->pos = 0;
                    break;
                }
                vo->pos -= total;
                p = (u32)vo->pos;
            }
            float a, b;
            if (vo->bits == 16) {
                const s16 *s = (const s16 *)(d + (size_t)p * (size_t)bps);
                a = s[0];
                b = vo->channels == 2 ? s[1] : s[0];
            } else {
                const u8 *s = d + (size_t)p * (size_t)bps;
                a = (float)((s[0] - 128) << 8);
                b = vo->channels == 2 ? (float)((s[1] - 128) << 8) : a;
            }
            acc[2 * i] += a * l;
            acc[2 * i + 1] += b * r;
            vo->pos += step;
        }
    }
    pthread_mutex_lock(&music_lock);
    if (music.playing && music.pcm) {
        for (int i = 0; i < frames; i++) {
            if (music.pos >= music.frames) {
                if (!music.decoding && !music.ended) {
                    music.ended = 1; // the game restarts the track when told
                    music.notify = 1;
                }
                break;
            }
            acc[2 * i] += music.pcm[2 * music.pos] * music.gain;
            acc[2 * i + 1] += music.pcm[2 * music.pos + 1] * music.gain;
            music.pos++;
        }
    }
    pthread_mutex_unlock(&music_lock);
    for (int i = 0; i < frames * 2; i++) {
        float s = acc[i];
        out[i] = s > 32767 ? 32767 : s < -32768 ? -32768 : (s16)s;
    }
    if (frames < total_frames)
        memset(out + frames * 2, 0, (size_t)(total_frames - frames) * 4);
}

static void open_device(void) {
    if (g_dev || win_headless()) return;
    g_dev = plat_audio_start(mix);
    if (!g_dev) logf_("audio: could not start the output device");
}

// ---- IDirectSoundBuffer ------------------------------------------------------
static u32 buffer_vtbl(void);
static Voice *voice_of(u32 obj) {
    u32 i = obj ? M32(obj + 4) : 0;
    if (!i || i >= 1024 || voices[i].obj != obj) fatal("bad sound buffer %08x", obj);
    return &voices[i];
}
static Voice *new_voice(void) {
    for (int i = 1; i < 1024; i++)
        if (!voices[i].obj) {
            Voice *v = &voices[i];
            u32 obj = gcalloc(8);
            M32(obj) = buffer_vtbl();
            M32(obj + 4) = (u32)i;
            lock_audio();
            memset(v, 0, sizeof *v);
            v->gain = 1.0f;
            v->obj = obj;
            unlock_audio();
            return v;
        }
    return 0;
}
static void dsb_Release(CPU *c) {
    Voice *v = voice_of(ARG(0));
    lock_audio();
    u32 obj = v->obj, data = v->owner ? v->data : 0;
    v->obj = 0;
    v->playing = 0;
    unlock_audio();
    // Duplicates share the sample data; only release it with its last user.
    for (int i = 1; i < 1024 && data; i++)
        if (voices[i].obj && voices[i].data == data) {
            voices[i].owner = 1;
            data = 0;
        }
    gfree(data);
    gfree(obj);
    RET_STD(1, 0);
}
static void dsb_GetCurrentPosition(CPU *c) {
    Voice *v = voice_of(ARG(0));
    u32 p = (u32)v->pos * (u32)(v->bits / 8 * v->channels);
    if (ARG(1)) M32(ARG(1)) = p;
    if (ARG(2)) M32(ARG(2)) = p;
    RET_STD(3, 0);
}
static void dsb_GetVolume(CPU *c) {
    M32(ARG(1)) = (u32)voice_of(ARG(0))->volume;
    RET_STD(2, 0);
}
static void dsb_GetStatus(CPU *c) {
    Voice *v = voice_of(ARG(0));
    M32(ARG(1)) = v->playing ? (v->loop ? 5 : 1) : 0;
    RET_STD(2, 0);
}
static void dsb_Lock(CPU *c) {
    Voice *v = voice_of(ARG(0));
    u32 off = ARG(1), n = ARG(2);
    if (ARG(7) & 2) off = 0, n = v->bytes; // DSBLOCK_ENTIREBUFFER
    if (off > v->bytes) off = v->bytes;
    if (n > v->bytes - off) n = v->bytes - off;
    M32(ARG(3)) = v->data + off;
    M32(ARG(4)) = n;
    if (ARG(5)) M32(ARG(5)) = 0;
    if (ARG(6)) M32(ARG(6)) = 0;
    RET_STD(8, 0);
}
static void dsb_Unlock(CPU *c) { RET_STD(5, 0); }
static void dsb_Play(CPU *c) {
    Voice *v = voice_of(ARG(0));
    lock_audio();
    v->loop = ARG(3) & 1;
    v->playing = 1;
    unlock_audio();
    RET_STD(4, 0);
}
static void dsb_Stop(CPU *c) {
    Voice *v = voice_of(ARG(0));
    lock_audio();
    v->playing = 0;
    unlock_audio();
    RET_STD(1, 0);
}
static void dsb_SetCurrentPosition(CPU *c) {
    Voice *v = voice_of(ARG(0));
    lock_audio();
    v->pos = (double)(ARG(1) / (u32)(v->bits / 8 * v->channels));
    unlock_audio();
    RET_STD(2, 0);
}
static void dsb_SetVolume(CPU *c) {
    Voice *v = voice_of(ARG(0));
    v->volume = (s32)ARG(1);
    v->gain = db_to_gain(v->volume);
    RET_STD(2, 0);
}
static void dsb_SetPan(CPU *c) {
    voice_of(ARG(0))->pan = (s32)ARG(1);
    RET_STD(2, 0);
}
static void dsb_SetFrequency(CPU *c) {
    Voice *v = voice_of(ARG(0));
    if (ARG(1) >= 100 && ARG(1) <= 200000) v->rate = (int)ARG(1);
    RET_STD(2, 0);
}
static u32 buffer_vtbl(void) {
    static u32 vt;
    static const struct Method m[] = {
        {0x00, com_noiface}, {0x04, com_addref}, {0x08, dsb_Release},
        {0x10, dsb_GetCurrentPosition}, {0x18, dsb_GetVolume}, {0x24, dsb_GetStatus},
        {0x2c, dsb_Lock}, {0x30, dsb_Play}, {0x34, dsb_SetCurrentPosition},
        {0x3c, dsb_SetVolume}, {0x40, dsb_SetPan}, {0x44, dsb_SetFrequency},
        {0x48, dsb_Stop}, {0x4c, dsb_Unlock}, {0x50, com_ok0},
    };
    if (!vt) vt = VTBL("IDirectSoundBuffer", 21, m);
    return vt;
}

// ---- IDirectSound ------------------------------------------------------------
static void ds_CreateSoundBuffer(CPU *c) {
    u32 d = ARG(1), bytes = M32(d + 8), fmt = M32(d + 16);
    if (!fmt || !bytes) RET_STD(4, 0x80070057u);
    Voice *v = new_voice();
    if (!v) RET_STD(4, 0x8007000Eu);
    v->channels = M16(fmt + 2);
    v->rate = (int)M32(fmt + 4);
    v->bits = M16(fmt + 14);
    if ((v->channels != 1 && v->channels != 2) || (v->bits != 8 && v->bits != 16))
        fatal("sound format unsupported: %d channels, %d bits", v->channels, v->bits);
    v->bytes = bytes;
    v->data = gcalloc(bytes + 8);
    v->owner = 1;
    M32(ARG(2)) = v->obj;
    RET_STD(4, 0);
}
static void ds_DuplicateSoundBuffer(CPU *c) {
    Voice *src = voice_of(ARG(1)), *v = new_voice();
    if (!v) RET_STD(3, 0x8007000Eu);
    u32 obj = v->obj;
    lock_audio();
    *v = *src;
    v->obj = obj;
    v->owner = 0;
    v->playing = 0;
    v->pos = 0;
    unlock_audio();
    M32(ARG(2)) = obj;
    RET_STD(3, 0);
}
HLE_NAMED(DSOUND, "ord1", DirectSoundCreate) {
    static u32 vt;
    static const struct Method m[] = {
        {0x00, com_noiface}, {0x04, com_addref}, {0x08, com_release},
        {0x0c, ds_CreateSoundBuffer}, {0x14, ds_DuplicateSoundBuffer}, {0x18, com_ok2},
    };
    if (!vt) vt = VTBL("IDirectSound", 11, m);
    open_device();
    u32 obj = gcalloc(8);
    M32(obj) = vt;
    M32(ARG(1)) = obj;
    RET_STD(3, 0);
}

// ---- music decoding ----------------------------------------------------------
// Tracks are decoded on a worker thread while playback starts from whatever
// has been decoded so far.
static void music_sink(void *user, const s16 *stereo, size_t frames) {
    pthread_mutex_lock(&music_lock);
    if (music.frames + frames > music.cap) {
        music.cap = (music.frames + frames) * 2 + 65536;
        music.pcm = realloc(music.pcm, music.cap * 4);
    }
    memcpy(music.pcm + music.frames * 2, stereo, frames * 4);
    music.frames += frames;
    pthread_mutex_unlock(&music_lock);
}
static void *decode_thread(void *arg) {
    if (music_decode_file(music.path, music_sink, 0, &music.cancel))
        logf_("music: could not decode %s", music.path);
    pthread_mutex_lock(&music_lock);
    music.decoding = 0;
    pthread_mutex_unlock(&music_lock);
    return 0;
}
static void music_stop(void) {
    if (music.has_thread) {
        music.cancel = 1;
        pthread_join(music.thread, 0);
        music.has_thread = 0;
    }
    pthread_mutex_lock(&music_lock);
    free(music.pcm);
    music.pcm = 0;
    music.frames = music.cap = music.pos = 0;
    music.playing = music.ended = music.notify = music.decoding = 0;
    music.cancel = 0;
    pthread_mutex_unlock(&music_lock);
}

void audio_pump(void) {
    if (music.notify) {
        music.notify = 0;
        win_post(0x400, 0, 0); // the notification message LF2 registered
    }
}

// ---- DirectShow interfaces ---------------------------------------------------
static u32 g_graph_refs;
static u32 simple_object(u32 vt) {
    u32 obj = gcalloc(8);
    M32(obj) = vt;
    return obj;
}
static void sub_Release(CPU *c) { RET_STD(1, 0); }
static void mc_Run(CPU *c) {
    pthread_mutex_lock(&music_lock);
    music.playing = 1;
    pthread_mutex_unlock(&music_lock);
    RET_STD(1, 0);
}
static void mc_Stop(CPU *c) {
    pthread_mutex_lock(&music_lock);
    music.playing = 0;
    pthread_mutex_unlock(&music_lock);
    RET_STD(1, 0);
}
static void me_GetEvent(CPU *c) {
    if (!music.ended) RET_STD(5, 0x80004004u); // E_ABORT: no event waiting
    music.ended = 0;
    M32(ARG(1)) = 1; // EC_COMPLETE
    M32(ARG(2)) = 0;
    M32(ARG(3)) = 0;
    RET_STD(5, 0);
}
static void mp_put_CurrentPosition(CPU *c) {
    double t = MF64(ESP + 8);
    pthread_mutex_lock(&music_lock);
    music.pos = t <= 0 ? 0 : (size_t)(t * OUT_RATE);
    music.ended = 0;
    pthread_mutex_unlock(&music_lock);
    RET_STD(3, 0);
}
static s32 g_music_volume;
static void ba_put_Volume(CPU *c) {
    g_music_volume = (s32)ARG(1);
    music.gain = db_to_gain(g_music_volume);
    RET_STD(2, 0);
}
static void ba_get_Volume(CPU *c) {
    M32(ARG(1)) = (u32)g_music_volume;
    RET_STD(2, 0);
}
static void graph_QueryInterface(CPU *c) {
    static u32 mc, me, mp, ba;
    static const struct Method m_mc[] = {
        {0x04, com_addref}, {0x08, sub_Release}, {0x1c, mc_Run}, {0x20, mc_Stop},
        {0x24, mc_Stop},
    };
    static const struct Method m_me[] = {
        {0x04, com_addref}, {0x08, sub_Release}, {0x20, me_GetEvent}, {0x30, com_ok3},
        {0x34, com_ok3}, {0x38, com_ok1},
    };
    static const struct Method m_mp[] = {
        {0x04, com_addref}, {0x08, sub_Release}, {0x20, mp_put_CurrentPosition},
    };
    static const struct Method m_ba[] = {
        {0x04, com_addref}, {0x08, sub_Release}, {0x1c, ba_put_Volume},
        {0x20, ba_get_Volume},
    };
    if (!mc) {
        mc = simple_object(VTBL("IMediaControl", 18, m_mc));
        me = simple_object(VTBL("IMediaEventEx", 16, m_me));
        mp = simple_object(VTBL("IMediaPosition", 18, m_mp));
        ba = simple_object(VTBL("IBasicAudio", 11, m_ba));
    }
    u32 iid = M32(ARG(1)), out = 0;
    switch (iid) { // first dword of the IID
    case 0x56a868b1: out = mc; break;
    case 0x56a868b6: out = me; break;
    case 0x56a868b2: out = mp; break;
    case 0x56a868b3: out = ba; break;
    }
    M32(ARG(2)) = out;
    RET_STD(3, out ? 0 : 0x80004002u);
}
static void graph_Release(CPU *c) {
    if (g_graph_refs && --g_graph_refs == 0) music_stop();
    RET_STD(1, 0);
}
// The game asks for its original .wma tracks, which macOS cannot decode. The
// port tool converts them to .m4a beforehand, so that file is preferred; any
// other format macOS can read is played as it is.
static int find_music(const char *guest, char *host, size_t n) {
    char alt[520];
    snprintf(alt, sizeof alt - 5, "%s", guest);
    char *dot = strrchr(alt, '.');
    if (dot && !strchr(dot, '\\') && !strchr(dot, '/')) {
        strcpy(dot, ".m4a");
        host_path(alt, host, n);
        if (!access(host, R_OK)) return 1;
    }
    host_path(guest, host, n);
    dot = strrchr(host, '.');
    return !access(host, R_OK) && !(dot && !strcasecmp(dot, ".wma"));
}
static void graph_RenderFile(CPU *c) {
    char guest[512], host[1024];
    u32 w = ARG(1);
    int n = 0;
    for (; n < 511 && M16(w + 2 * (u32)n); n++) guest[n] = (char)M16(w + 2 * (u32)n);
    guest[n] = 0;
    music_stop();
    if (!find_music(guest, host, sizeof host)) {
        LOG("music: no playable file for %s", guest);
        RET_STD(3, 0x80070002u);
    }
    LOG("music: %s", host);
    if (!win_headless()) {
        open_device();
        snprintf(music.path, sizeof music.path, "%s", host);
        music.decoding = 1;
        music.has_thread = !pthread_create(&music.thread, 0, decode_thread, 0);
        if (!music.has_thread) music.decoding = 0;
    }
    RET_STD(3, 0);
}
HLE(ole32, CoCreateInstance) {
    static u32 vt;
    static const struct Method m[] = {
        {0x00, graph_QueryInterface}, {0x04, com_addref}, {0x08, graph_Release},
        {0x34, graph_RenderFile}, {0x3c, com_ok1},
    };
    if (M32(ARG(0)) != 0xe436ebb3u) { // only CLSID_FilterGraph
        M32(ARG(4)) = 0;
        RET_STD(5, 0x80040154u);
    }
    if (!vt) vt = VTBL("IGraphBuilder", 18, m);
    g_graph_refs = 1;
    M32(ARG(4)) = simple_object(vt);
    RET_STD(5, 0);
}

// ---- WINMM mmio: just enough RIFF parsing for wave files -----------------------
typedef struct Mmio {
    u8 *buf;
    u32 len, pos;
} Mmio;
static Mmio mm[8];

HLE(WINMM, mmioOpenA) {
    char path[1024];
    host_path(ARGS(0), path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG("mmioOpenA: missing %s", path);
        RET_STD(3, 0);
    }
    for (int i = 0; i < 8; i++)
        if (!mm[i].buf) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            mm[i].buf = malloc((size_t)sz + 1);
            mm[i].len = (u32)fread(mm[i].buf, 1, (size_t)sz, f);
            mm[i].pos = 0;
            fclose(f);
            RET_STD(3, 0x700 + i);
        }
    fclose(f);
    RET_STD(3, 0);
}
static Mmio *mm_of(u32 h) {
    u32 i = h - 0x700;
    if (i >= 8 || !mm[i].buf) fatal("bad HMMIO %08x", h);
    return &mm[i];
}
HLE(WINMM, mmioClose) {
    Mmio *m = mm_of(ARG(0));
    free(m->buf);
    m->buf = 0;
    RET_STD(2, 0);
}
HLE(WINMM, mmioRead) {
    Mmio *m = mm_of(ARG(0));
    u32 n = ARG(2);
    if (n > m->len - m->pos) n = m->len - m->pos;
    memcpy(ARGP(1), m->buf + m->pos, n);
    m->pos += n;
    RET_STD(3, n);
}
HLE(WINMM, mmioDescend) {
    Mmio *m = mm_of(ARG(0));
    u32 ck = ARG(1), parent = ARG(2), flags = ARG(3);
    u32 end = parent ? M32(parent + 12) + M32(parent + 4) : m->len;
    if (end > m->len) end = m->len;
    u32 want_id = M32(ck), want_type = M32(ck + 8);
    while (m->pos + 8 <= end) {
        u32 id, size, type = 0;
        memcpy(&id, m->buf + m->pos, 4);
        memcpy(&size, m->buf + m->pos + 4, 4);
        int list = id == 0x46464952 || id == 0x5453494c; // RIFF / LIST
        if (list && m->pos + 12 <= end) memcpy(&type, m->buf + m->pos + 8, 4);
        int ok = !(flags & 0x70) || ((flags & 0x10) && id == want_id) ||
                 ((flags & 0x20) && id == 0x46464952 && type == want_type) ||
                 ((flags & 0x40) && id == 0x5453494c && type == want_type);
        if (ok) {
            M32(ck) = id;
            M32(ck + 4) = size;
            M32(ck + 8) = type;
            M32(ck + 12) = m->pos + 8;
            M32(ck + 16) = 0;
            m->pos += list ? 12 : 8;
            RET_STD(4, 0);
        }
        m->pos += 8 + size + (size & 1);
    }
    RET_STD(4, 265); // MMIOERR_CHUNKNOTFOUND
}
HLE(WINMM, mmioAscend) {
    Mmio *m = mm_of(ARG(0));
    u32 ck = ARG(1);
    u32 p = M32(ck + 12) + M32(ck + 4);
    p += p & 1;
    m->pos = p > m->len ? m->len : p;
    RET_STD(3, 0);
}
