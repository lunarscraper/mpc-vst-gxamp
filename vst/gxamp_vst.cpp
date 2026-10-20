/* =============================================================================
 * gxamp_vst.cpp - Gx Amp: Guitarix's guitar amplifier as a VST2 insert effect for the MPC OS
 * plugin host (Force, MPC Live/One/X/Key), armhf. For a real guitar on an audio input and for
 * RiffBox (or any synth) on a plugin track.
 *
 *   input (L+R / LEFT / RIGHT, -24..+24 dB, level readout) -> noise gate
 *   -> pedal: Guitarix's Tube Screamer (gxts9), Fuzz Face, Big Muff Pi, or the RAT of mpc-vst-rat
 *   -> Guitarix amp: 18 tube models (gx_amp.lv2) -> one of 27 tonestacks
 *   -> presence + one of 18 cabinets (Guitarix's impulse responses, gx_amp.lv2's presence blend)
 *   -> tempo-synced delay (mono or ping-pong) -> Guitarix stereo reverb (stereoverb) -> volume
 *   tuner (note + cents readouts, MUTE or LIVE), 32 preset slots with LOAD/SAVE.
 *
 * The amp, tonestack, cabinet, pedal and reverb DSP is Guitarix's own code, vendored unchanged
 * (gx/, see gx/VENDORED.md), reached through gx/gx_bridge.h. Written here: the plug-in, the
 * convolution (a zero-latency partitioned FFT convolver instead of Guitarix's zita-convolver,
 * which needs FFTW and a thread), the gate, the delay, the tuner, the presets.
 * GPL-3.0-or-later (see ../LICENSE): Guitarix is GPL-2.0-or-later, its zita-resampler GPL-3.0.
 * Guitarix: Hermann Meyer, Andreas Degert, Pete Shorthose, Steve Poskitt and contributors.
 * ========================================================================== */
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "params.h"
#include "popup.h"    /* mpc-vst-plugins wrapper/popup.h, copied into build/ by build.sh */
#include "rat_core.h" /* the RAT pedal of mpc-vst-rat (MIT) */
#include "gx/gx_bridge.h"
#include "gx/dsp/gx_pluginlv2.h"

/* ---- VST2 ABI (hand-written; no Steinberg SDK) ---------------------------- */
struct AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void (*process)(AEffect *, float **, float **, int32_t);
    void (*setParameter)(AEffect *, int32_t, float);
    float (*getParameter)(AEffect *, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect *, float **, float **, int32_t);
    void (*processDoubleReplacing)(AEffect *, double **, double **, int32_t);
    char future[56];
};
enum {
    effOpen = 0, effClose = 1, effSetProgram = 2, effGetProgram = 3, effSetProgramName = 4,
    effGetProgramName = 5, effGetProgramNameIndexed = 29, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effGetChunk = 23,
    effSetChunk = 24, effProcessEvents = 25, effCanBeAutomated = 26, effGetPlugCategory = 35,
    effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
    effGetVendorVersion = 49, effCanDo = 51, effGetVstVersion = 58,
};
enum { audioMasterAutomate = 0, audioMasterGetTime = 7, audioMasterUpdateDisplay = 42 };
enum { kVstTempoValid = 1 << 10 };
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;
enum { effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5 };

/* the parameters, by key (module.json); IDX[] is their position in the generated PARAMS[] */
enum {
    MODEL, PREGAIN, DRIVE, DIST, MASTER, BASS, MIDDLE, TREBLE, PRESENCE, STACK, CAB, CAB_LEVEL, DEEP,
    PEDAL, P_DRIVE, P_TONE, P_LEVEL,
    DLY_TIME, DLY_MS, DLY_FB, DLY_MIX, DLY_TONE, DLY_PP, REV_MIX, REV_ROOM, REV_DAMP, VOLUME,
    INPUT, IN_CH, GATE, TUNER, TUNE_REF,
    SLOT, LOAD, SAVE, IN_METER, TUNER_NOTE, TUNER_BAR, NKEYS
};
static const char *const KEYS[NKEYS] = {
    "model", "pregain", "drive", "dist", "master", "bass", "middle", "treble", "presence", "stack", "cab", "cab_level", "deep",
    "pedal", "p_drive", "p_tone", "p_level",
    "dly_time", "dly_ms", "dly_fb", "dly_mix", "dly_tone", "dly_pp", "rev_mix", "rev_room", "rev_damp", "volume",
    "input", "in_ch", "gate", "tuner", "tune_ref",
    "slot", "load", "save", "in_meter", "tuner_note", "tuner_bar",
};
static int IDX[NKEYS];
static int KEY_OF[NPARAMS];   /* PARAMS[] position -> key enum, -1 = not ours (popup flags) */
static bool is_trigger(int k) { return k == LOAD || k == SAVE; }
static bool is_readout(int k) { return k == IN_METER || k == TUNER_NOTE || k == TUNER_BAR; }
/* slot, the buttons and the readouts: never part of a chunk or a preset */
static bool is_bank_key(int i) { const int k = KEY_OF[i]; return k == SLOT || is_trigger(k) || is_readout(k); }
/* the input side belongs to the source (guitar, RiffBox, a synth), not to a sound: in the project
 * chunk, never in a preset slot (a preset made for the guitar also works on RiffBox's track) */
static bool is_ui_key(int i) {
    const int k = KEY_OF[i];
    return k == INPUT || k == IN_CH || k == GATE || k == TUNER || k == TUNE_REF;
}

enum { CHUNK = 256 };          /* audio is processed in pieces of at most this many frames */
enum { PEDAL_OFF, PEDAL_TS9, PEDAL_FF, PEDAL_BMP, PEDAL_RAT, NPEDALS };
enum { STACK_OFF = gxm::NSTACKS, CAB_OFF = gxm::NCABS };
enum { TUNER_OFF, TUNER_MUTE, TUNER_LIVE };

/* =============================================================================
 * Convolution: presence and cabinet in one zero-latency partitioned convolver.
 * gx_amp.lv2 runs a presence convolver (its contrast IR, blended in by the PRESENCE knob) and
 * then, after the tonestack, the cabinet convolver. Both are linear and so is the tonestack, so
 * the order doesn't matter and the two collapse into one filter:
 *     cab_gain * h_cab  (*)  (ca * delta + cb * ampf * h_presence)
 *   = ga * h_cab + gb * (h_cab (*) h_presence)
 * h_cab and h_cab (*) h_presence are prepared per cabinet when the sample rate is set (resampled
 * from Guitarix's 48 kHz with Guitarix's own BufferResampler); the knobs only move ga and gb.
 * The first CB taps run directly (no latency), the rest as uniform FFT partitions of CB taps.
 * ========================================================================== */
enum { CB = 64, CN = 2 * CB, CBINS = CB + 1 };

struct Fft {   /* in-place complex radix-2, size CN */
    float c[CN / 2], s[CN / 2];
    int rev[CN];
    Fft() {
        int bits = 0;
        while ((1 << bits) < CN) bits++;
        for (int i = 0; i < CN; i++) {
            int r = 0;
            for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev[i] = r;
        }
        for (int i = 0; i < CN / 2; i++) { c[i] = (float)std::cos(2 * M_PI * i / CN); s[i] = (float)-std::sin(2 * M_PI * i / CN); }
    }
    void run(float *re, float *im, bool inverse) const {
        for (int i = 0; i < CN; i++) if (i < rev[i]) { std::swap(re[i], re[rev[i]]); std::swap(im[i], im[rev[i]]); }
        for (int len = 2; len <= CN; len <<= 1) {
            const int half = len >> 1, step = CN / len;
            for (int i = 0; i < CN; i += len)
                for (int j = 0; j < half; j++) {
                    const float wr = c[j * step], wi = inverse ? -s[j * step] : s[j * step];
                    const int a = i + j, b = a + half;
                    const float tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr; im[b] = im[a] - ti;
                    re[a] += tr; im[a] += ti;
                }
        }
    }
};
static const Fft &fft() { static const Fft f; return f; }

/* one impulse response, split: CB direct taps (stored reversed) + nparts spectra of CB taps */
struct IrSet {
    int nparts = 0;
    float head[CB] = {0};
    std::vector<float> re, im;   /* nparts * CBINS */
    void build(const std::vector<float> &h) {
        for (int m = 0; m < CB; m++) head[CB - 1 - m] = m < (int)h.size() ? h[m] : 0.0f;
        nparts = (int)h.size() > CB ? ((int)h.size() - CB + CB - 1) / CB : 0;
        re.assign((size_t)nparts * CBINS, 0.0f);
        im.assign((size_t)nparts * CBINS, 0.0f);
        float r[CN], i_[CN];
        for (int k = 0; k < nparts; k++) {
            for (int m = 0; m < CN; m++) {
                const int t = (k + 1) * CB + m;
                r[m] = (m < CB && t < (int)h.size()) ? h[t] : 0.0f;
                i_[m] = 0;
            }
            fft().run(r, i_, false);
            for (int b = 0; b < CBINS; b++) { re[(size_t)k * CBINS + b] = r[b]; im[(size_t)k * CBINS + b] = i_[b]; }
        }
    }
};

struct Convolver {
    enum { MAXP = 48 };              /* up to CB * 49 taps (~70 ms at 44.1 kHz) */
    const IrSet *a = nullptr, *b = nullptr;
    float ga = 1, gb = 0, ga_t = 1, gb_t = 0;
    bool mix_dirty = true;
    int nparts = 0;
    float head[CB];
    float hre[MAXP][CBINS], him[MAXP][CBINS];
    float xre[MAXP][CBINS], xim[MAXP][CBINS];   /* frequency-domain delay line */
    int fpos = 0;
    float xb[CN];                    /* previous block, current block */
    float tail[CB];
    int pos = 0;

    void clear() {
        std::memset(xb, 0, sizeof xb); std::memset(tail, 0, sizeof tail);
        std::memset(xre, 0, sizeof xre); std::memset(xim, 0, sizeof xim);
        pos = 0; fpos = 0;
    }
    void set(const IrSet *ca, const IrSet *cb) {
        a = ca; b = cb;
        nparts = std::min<int>(MAXP, std::max(a ? a->nparts : 0, b ? b->nparts : 0));
        ga = ga_t; gb = gb_t;
        mix();
    }
    void gains(float ta, float tb) { ga_t = ta; gb_t = tb; }
    void mix() {
        for (int m = 0; m < CB; m++) head[m] = (a ? ga * a->head[m] : 0) + (b ? gb * b->head[m] : 0);
        for (int k = 0; k < nparts; k++)
            for (int i = 0; i < CBINS; i++) {
                float r = 0, im = 0;
                if (a && k < a->nparts) { r += ga * a->re[(size_t)k * CBINS + i]; im += ga * a->im[(size_t)k * CBINS + i]; }
                if (b && k < b->nparts) { r += gb * b->re[(size_t)k * CBINS + i]; im += gb * b->im[(size_t)k * CBINS + i]; }
                hre[k][i] = r; him[k][i] = im;
            }
        mix_dirty = false;
    }
    void block_done() {
        /* gains glide once per block (CB samples), so knob moves don't click */
        if (std::fabs(ga - ga_t) > 1e-5f || std::fabs(gb - gb_t) > 1e-5f) {
            ga += (ga_t - ga) * 0.25f; gb += (gb_t - gb) * 0.25f;
            if (std::fabs(ga - ga_t) < 1e-5f) ga = ga_t;
            if (std::fabs(gb - gb_t) < 1e-5f) gb = gb_t;
            mix_dirty = true;
        }
        if (mix_dirty) mix();
        if (nparts > 0) {
            float r[CN], i_[CN];
            for (int m = 0; m < CN; m++) { r[m] = xb[m]; i_[m] = 0; }
            fft().run(r, i_, false);
            for (int k = 0; k < CBINS; k++) { xre[fpos][k] = r[k]; xim[fpos][k] = i_[k]; }
            float yr[CN], yi[CN];
            for (int k = 0; k < CBINS; k++) { yr[k] = 0; yi[k] = 0; }
            for (int p = 0; p < nparts; p++) {
                const int s = (fpos - p + MAXP) % MAXP;
                const float *hr = hre[p], *hi = him[p], *sr = xre[s], *si = xim[s];
                for (int k = 0; k < CBINS; k++) {
                    yr[k] += hr[k] * sr[k] - hi[k] * si[k];
                    yi[k] += hr[k] * si[k] + hi[k] * sr[k];
                }
            }
            for (int k = CBINS; k < CN; k++) { yr[k] = yr[CN - k]; yi[k] = -yi[CN - k]; }
            fft().run(yr, yi, true);
            for (int m = 0; m < CB; m++) tail[m] = yr[CB + m] * (1.0f / CN);
            fpos = (fpos + 1) % MAXP;
        } else {
            for (int m = 0; m < CB; m++) tail[m] = 0;
        }
        std::memcpy(xb, xb + CB, sizeof(float) * CB);
        pos = 0;
    }
    void run(float *x, int n) {
        for (int i = 0; i < n; i++) {
            xb[CB + pos] = x[i];
            const float *w = xb + pos + 1;
            float y = 0;
            for (int m = 0; m < CB; m++) y += head[m] * w[m];
            x[i] = y + tail[pos];
            if (++pos == CB) block_done();
        }
    }
};

/* cabinet IR sets, shared by all instances at one sample rate (built once, read-only after) */
struct CabBank {
    uint32_t sr = 0;
    IrSet cab[gxm::NCABS + 1], cabpres[gxm::NCABS + 1];   /* [CAB_OFF]: a unit impulse */
};
static std::mutex g_cab_lock;
static std::vector<CabBank *> g_cab_banks;   /* never freed: one per sample rate seen */

static std::vector<float> conv_full(const std::vector<float> &x, const std::vector<float> &h) {
    std::vector<float> y(x.size() + h.size() - 1, 0.0f);
    for (size_t i = 0; i < x.size(); i++)
        for (size_t j = 0; j < h.size(); j++) y[i + j] += x[i] * h[j];
    return y;
}
static std::vector<float> resampled(const float *d, int n, uint32_t from, uint32_t to) {
    int on = 0;
    float *p = gxm::resample_ir(from, n, d, to, &on);
    std::vector<float> v(p, p + on);
    delete[] p;
    while (v.size() > 1 && std::fabs(v.back()) < 1e-7f) v.pop_back();   /* trailing silence costs partitions */
    return v;
}
static const CabBank *cab_bank(uint32_t sr) {
    std::lock_guard<std::mutex> lk(g_cab_lock);
    for (CabBank *b : g_cab_banks) if (b->sr == sr) return b;
    CabBank *b = new CabBank();
    b->sr = sr;
    const float *pd; uint32_t psr;
    const int pn = gxm::presence_ir(&pd, &psr);
    const std::vector<float> pres = resampled(pd, pn, psr, sr);
    for (int c = 0; c <= gxm::NCABS; c++) {
        std::vector<float> h;
        if (c == CAB_OFF) h.assign(1, 1.0f);
        else { const float *d; uint32_t isr; const int n = gxm::cab_ir(c, &d, &isr); h = resampled(d, n, isr, sr); }
        if ((int)h.size() > CB * (Convolver::MAXP + 1)) h.resize(CB * (Convolver::MAXP + 1));
        b->cab[c].build(h);
        std::vector<float> hp = conv_full(h, pres);
        if ((int)hp.size() > CB * (Convolver::MAXP + 1)) hp.resize(CB * (Convolver::MAXP + 1));
        b->cabpres[c].build(hp);
    }
    g_cab_banks.push_back(b);
    return b;
}

/* =============================================================================
 * Noise gate (in front of the pedal and the amp, where the hiss comes in): peak detector,
 * open above the threshold, closed 6 dB below it after a 60 ms hold; 1 ms attack, 80 ms release.
 * ========================================================================== */
struct Gate {
    float env = 0, g = 1, thr = 0, thr_lo = 0, rel_env = 0, att = 0, rel = 0;
    int hold = 0, hold_n = 0;
    bool open = true, on = false;
    void init(float sr) {
        rel_env = std::exp(-1.0f / (0.05f * sr));
        att = 1 - std::exp(-1.0f / (0.001f * sr));
        rel = 1 - std::exp(-1.0f / (0.08f * sr));
        hold_n = (int)(0.06f * sr);
    }
    void set(float knob) {   /* 0 = off, 1..100 = -89..-30 dBFS */
        on = knob > 0.5f;
        const float db = -90 + 0.6f * knob;
        thr = std::pow(10.0f, db / 20); thr_lo = thr * 0.5f;
    }
    void run(float *x, int n) {
        if (!on) { g = 1; open = true; return; }
        for (int i = 0; i < n; i++) {
            const float a = std::fabs(x[i]);
            env = a > env ? a : env * rel_env;
            if (env > thr) { open = true; hold = hold_n; }
            else if (env < thr_lo) { if (hold > 0) hold--; else open = false; }
            g += ((open ? 1.0f : 0.0f) - g) * (open ? att : rel);
            x[i] *= g;
        }
    }
};

/* =============================================================================
 * Delay: tempo-synced (or free) echo, mono or ping-pong, the repeats darkened by DELAY TONE and
 * thinned below 90 Hz, feedback soft-limited. A send: the dry signal stays at full level.
 * ========================================================================== */
struct Delay {
    std::vector<float> l, r;
    int size = 0, w = 0;
    float t = -1, lp_l = 0, lp_r = 0, hp_l = 0, hp_r = 0, slew = 0, a_lp = 0.5f, a_hp = 0, sr = 44100;
    void init(float rate) {
        sr = rate;
        size = (int)(2.2f * rate) + 4;   /* 1/4 at 30 BPM, 1.5 s free */
        l.assign(size, 0.0f); r.assign(size, 0.0f);
        w = 0; t = -1; lp_l = lp_r = hp_l = hp_r = 0;
        slew = 1 - std::exp(-1.0f / (0.06f * rate));
        a_hp = 1 - std::exp(-2 * (float)M_PI * 90 / rate);
    }
    void clear() { std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f); lp_l = lp_r = hp_l = hp_r = 0; }
    void set_tone(float tone01) {
        const float hz = 1200 * std::pow(10.0f, tone01);   /* 1.2 .. 12 kHz */
        a_lp = 1 - std::exp(-2 * (float)M_PI * std::min(hz, 0.45f * sr) / sr);
    }
    static float soft(float x) { return x / (1 + std::fabs(x) * 0.25f); }
    inline float tap(const std::vector<float> &b, float d) const {
        float p = (float)w - d;
        while (p < 0) p += (float)size;
        const int i0 = (int)p;
        const float f = p - (float)i0;
        const int i1 = i0 + 1 >= size ? 0 : i0 + 1;
        return b[i0] + (b[i1] - b[i0]) * f;
    }
    /* mono x in, L/R out (dry + mix * wet) */
    void run(const float *x, float *L, float *R, int n, float target, float fb, float mix, bool pp) {
        if (t < 0) t = target;
        for (int i = 0; i < n; i++) {
            t += (target - t) * slew;
            const float d = std::max(1.0f, std::min(t, (float)size - 2));
            const float yl = tap(l, d), yr = tap(r, d);
            lp_l += (yl - lp_l) * a_lp; lp_r += (yr - lp_r) * a_lp;
            hp_l += (lp_l - hp_l) * a_hp; hp_r += (lp_r - hp_r) * a_hp;
            const float fl = lp_l - hp_l, fr = lp_r - hp_r;
            if (pp) {   /* input on the left line, the lines feed each other: L, R, L, ... */
                l[w] = soft(x[i] + fb * fr);
                r[w] = soft(fb * fl);
                L[i] = x[i] + mix * yl;
                R[i] = x[i] + mix * yr;
            } else {
                l[w] = soft(x[i] + fb * fl);
                r[w] = 0;
                L[i] = x[i] + mix * yl;
                R[i] = x[i] + mix * yl;
            }
            if (++w >= size) w = 0;
        }
    }
};

/* a Schroeder allpass of fixed length (the reverb's stereo decorrelator) */
struct Allpass {
    int len; float g;
    float buf[512] = {0};
    int p = 0;
    Allpass(int l, float gain) : len(l), g(gain) {}
    void clear() { std::memset(buf, 0, sizeof buf); p = 0; }
    inline float run(float x) {
        const float d = buf[p];
        const float v = x - g * d;
        buf[p] = v;
        if (++p >= len) p = 0;
        return d + g * v;
    }
};

/* =============================================================================
 * Tuner: YIN on the input decimated to ~11 kHz (window 1024, lags up to 256: 43 Hz .. 1.4 kHz),
 * a new estimate every 512 decimated samples (~46 ms). The lags are spread over the audio blocks,
 * so the work per block stays small and even. Results go to the two readouts.
 * ========================================================================== */
struct Tuner {
    enum { W = 1024, TMAX = 256, NB = W + TMAX, HOP = 512 };
    int D = 4, dcount = 0, wpos = 0, since = 0, tau = 0;
    float fsd = 11025;
    float ring[NB] = {0}, a[NB] = {0}, d[TMAX + 1] = {0};
    float lp1[2] = {0, 0}, lp2[2] = {0, 0}, b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    bool busy = false;
    float energy = 0;
    /* results (audio thread writes, display reads) */
    std::atomic<float> freq{0};
    std::atomic<int> age{1 << 30};   /* decimated samples since the last good estimate */
    void init(float sr) {
        D = std::max(1, (int)std::lround(sr / 11025.0f));
        fsd = sr / D;
        /* 2nd-order low-pass at fsd/4 before decimation (twice, cascaded) */
        const double w = 2 * M_PI * (fsd * 0.25) / sr, al = std::sin(w) / (2 * 0.7071), c = std::cos(w), a0 = 1 + al;
        b0 = (float)((1 - c) / 2 / a0); b1 = (float)((1 - c) / a0); b2 = b0; a1 = (float)(-2 * c / a0); a2 = (float)((1 - al) / a0);
        reset();
    }
    void reset() {
        std::memset(ring, 0, sizeof ring); lp1[0] = lp1[1] = lp2[0] = lp2[1] = 0;
        dcount = wpos = since = tau = 0; busy = false; age.store(1 << 30);
    }
    inline float bq(float x, float *z) {   /* transposed direct form II */
        const float y = b0 * x + z[0];
        z[0] = b1 * x - a1 * y + z[1];
        z[1] = b2 * x - a2 * y;
        return y;
    }
    void start() {
        for (int i = 0; i < NB; i++) a[i] = ring[(wpos + i) % NB];
        double e = 0;
        for (int i = 0; i < W; i++) e += (double)a[i] * a[i];
        energy = (float)(e / W);
        tau = 1; d[0] = 0; busy = true;
    }
    void finish() {
        busy = false;
        if (energy < 1e-7f) return;   /* below ~-70 dBFS: no note */
        const int tmin = std::max(2, (int)(fsd / 1400));
        float run = 0, dn[TMAX + 1];
        dn[0] = 1;
        for (int t = 1; t <= TMAX; t++) { run += d[t]; dn[t] = run > 0 ? d[t] * t / run : 1; }
        int best = -1;
        for (int t = tmin; t < TMAX; t++)
            if (dn[t] < 0.15f) { while (t + 1 < TMAX && dn[t + 1] < dn[t]) t++; best = t; break; }
        if (best < 0) {
            float m = 1e9f;
            for (int t = tmin; t < TMAX; t++) if (dn[t] < m) { m = dn[t]; best = t; }
            if (m > 0.35f) return;
        }
        float tf = (float)best;
        if (best > 1 && best < TMAX) {
            const float s0 = dn[best - 1], s1 = dn[best], s2 = dn[best + 1], den = s0 + s2 - 2 * s1;
            if (std::fabs(den) > 1e-9f) tf += 0.5f * (s0 - s2) / den;
        }
        if (tf <= 1) return;
        freq.store(fsd / tf);
        age.store(0);
    }
    void run(const float *x, int n) {
        int work = (int)((long long)n * TMAX / ((long long)HOP * D)) + 2;   /* lags this block */
        for (int i = 0; i < n; i++) {
            const float y = bq(bq(x[i], lp1), lp2);
            if (++dcount < D) continue;
            dcount = 0;
            ring[wpos] = y;
            if (++wpos >= NB) wpos = 0;
            const int ag = age.load();
            if (ag < (1 << 29)) age.store(ag + 1);
            if (++since >= HOP && !busy) { since = 0; start(); }
        }
        while (busy && work-- > 0) {
            float s = 0;
            const float *p = a, *q = a + tau;
            for (int j = 0; j < W; j++) { const float df = p[j] - q[j]; s += df * df; }
            d[tau] = s;
            if (++tau > TMAX) finish();
        }
    }
};

/* =============================================================================
 * The plug-in
 * ========================================================================== */
struct Plugin {
    AEffect fx;
    audioMasterCallback master = nullptr;
    std::atomic<float> cache[NPARAMS];
    std::atomic<int> notify[NPARAMS];
    float open[NPARAMS] = {0};
    volatile int release[NPARAMS] = {0};
    bool down[NPARAMS] = {false};
    std::atomic<bool> dirty{true};
    std::atomic<int> cur{0};
    float sr = 44100;
    std::vector<uint8_t> chunk;

    /* Guitarix modules (all models exist; the selected one runs) */
    PluginLV2 *amp[gxm::NAMPS] = {nullptr};
    PluginLV2 *stack[gxm::NSTACKS] = {nullptr};
    PluginLV2 *ff = nullptr, *bmp = nullptr, *rev = nullptr;
    gxm::Ts9 *ts9 = nullptr;
    rat::Core rat;
    /* their control values (Guitarix reads them through these pointers every block) */
    float g_gain1 = 0, g_pregain = 0, g_wet_dry = 100, g_drive = 0.25f, g_high = 0;
    float g_bass = 0.5f, g_middle = 0.5f, g_treble = 0.5f;
    float g_ts_level = -10, g_ts_tone = 400, g_ts_drive = 0.5f;
    float g_ff_fuzz = 0.5f, g_ff_level = 0.5f, g_bmp_sus = 0.5f, g_bmp_tone = 0.5f, g_bmp_vol = 0.5f;
    float g_rev_mix = 0, g_rev_lfo = 0.2f, g_rev_room = 0.5f, g_rev_damp = 0.25f, g_rev_inv = 0;
    float g_dummy = 0;

    /* the structure in use, and the one asked for (switched while the output is faded out) */
    int a_model = 0, a_stack = 0, a_cab = 0, a_pedal = 0;
    int w_model = 0, w_stack = 0, w_cab = 0, w_pedal = 0;
    float sw_gain = 1;               /* fade around a switch */
    float model_trim = 1;
    bool first = true;

    const CabBank *cabs = nullptr;
    Convolver conv;
    Gate gate;
    Delay dly;
    Tuner tuner;
    bool rev_on = false;
    float rev_mix = 0, rev_mix_s = 0;
    Allpass ap1{331, 0.6f}, ap2{127, -0.55f};

    /* smoothed levels */
    float in_g = 1, in_g_t = 1, out_g = 1, out_g_t = 1, mute_g = 1, pedal_mk = 1;
    int in_ch = 0, tuner_mode = TUNER_OFF;
    float ref = 440;
    float dly_target_s = 0.375f, dly_fb = 0.35f, dly_mix = 0, tempo = 120;
    bool dly_pp = false, dly_free = false;
    float dly_beats = 0.75f, dly_ms = 375;
    /* input meter */
    std::atomic<float> peak_db{-120};
    float pk = 0, pk_hold = 0;
    int pk_hold_n = 0, clip_n = 0;
    /* readouts: pushed to the host now and then */
    int ro_count = 0;
    float ro_tick = 0;
    std::string ro_last[3];

    float mono[CHUNK], bufL[CHUNK], bufR[CHUNK], revL[CHUNK], revR[CHUNK], revIn[CHUNK];
};

static int param_index(const char *key) {
    for (int i = 0; i < NPARAMS; i++) if (!std::strcmp(PARAMS[i].key, key)) return i;
    return -1;
}
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void copy_str(void *dst, const char *s, size_t max) {
    std::strncpy((char *)dst, s, max - 1);
    ((char *)dst)[max - 1] = 0;
}
static int norm_to_ui(const param_t *p, float n) {
    if (p->nopts) return (int)std::lround(clamp01(n) * (p->nopts - 1));
    return (int)std::lround(p->min + (p->max - p->min) * clamp01(n));
}
static float ui_to_norm(const param_t *p, double v) {
    if (p->nopts) return p->nopts > 1 ? clamp01((float)(v / (p->nopts - 1))) : 0.0f;
    return p->max > p->min ? clamp01((float)((v - p->min) / (p->max - p->min))) : 0.0f;
}
/* a knob at full resolution (the integer range only names its ends), a switch as its index */
static float val_at(Plugin *w, int i) {
    const param_t *p = &PARAMS[i];
    const float n = w->cache[i].load();
    if (p->nopts) return (float)norm_to_ui(p, n);
    return p->min + (p->max - p->min) * clamp01(n);
}
static float val(Plugin *w, int k) { return IDX[k] < 0 ? 0.0f : val_at(w, IDX[k]); }
static int opt(Plugin *w, int k) { return (int)std::lround(val(w, k)); }
static float pct(Plugin *w, int k) { return val(w, k) * 0.01f; }
static void set_val(Plugin *w, int i, double v) {
    if (i < 0) return;
    w->cache[i].store(ui_to_norm(&PARAMS[i], v));
    w->notify[i].store(1);
}
static void start(Plugin *w, int k, double v) { set_val(w, IDX[k], v); }
static float db2g(float db) { return std::pow(10.0f, db / 20); }

struct NoDenormals {
#if defined(__arm__) && defined(__ARM_FP)
    uint32_t old = 0;
    NoDenormals() { asm volatile("vmrs %0, fpscr" : "=r"(old)); asm volatile("vmsr fpscr, %0" : : "r"(old | (1u << 24))); }
    ~NoDenormals() { asm volatile("vmsr fpscr, %0" : : "r"(old)); }
#elif defined(__x86_64__) || defined(__i386__)
    unsigned old = __builtin_ia32_stmxcsr();
    NoDenormals() { __builtin_ia32_ldmxcsr(old | 0x8040); }
    ~NoDenormals() { __builtin_ia32_ldmxcsr(old); }
#endif
};

/* gx_amp.lv2's presence blend (DELT_PRESENCE_HACK) and its Ampf/Impf level laws */
static void conv_gains(Plugin *w, float &ga, float &gb) {
    const float alevel = val(w, PRESENCE) * 0.1f;               /* 0..10 */
    const float p = alevel * 0.1f, t = std::min(p * 2.0f, 1.0f);
    const float mix = t * t * (3.0f - 2.0f * t);
    float ca = 1, cb = 0;
    if (mix >= 0.9999f) { ca = 0; cb = 1; }
    else if (mix > 1e-4f) { ca = std::cos(1.57079632679f * mix); cb = std::sin(1.57079632679f * mix); }
    const float half = alevel * 0.5f;
    const float ampf = half * std::pow(10.0f, -0.1f * half);
    float cabg = 1;
    if (w->a_cab != CAB_OFF) {
        const float cl = val(w, CAB_LEVEL) * (w->a_cab == 17 ? 0.5f : 1.0f);   /* 1x8: Guitarix halves it */
        cabg = cl * cl * 0.01f;
    }
    ga = ca * cabg;
    gb = cb * ampf * cabg;
}

static void configure(Plugin *w) {
    w->g_pregain = val(w, PREGAIN);
    w->g_drive = 0.01f + 0.99f * pct(w, DRIVE);
    w->g_wet_dry = std::max(1.0f, val(w, DIST));
    w->g_gain1 = val(w, MASTER);
    w->g_bass = pct(w, BASS); w->g_middle = pct(w, MIDDLE); w->g_treble = pct(w, TREBLE);
    w->g_high = opt(w, DEEP) ? 1.0f : 0.0f;
    /* pedals: DRIVE / TONE / LEVEL on each pedal's own controls */
    const float pd = pct(w, P_DRIVE), pt = pct(w, P_TONE), pl = pct(w, P_LEVEL);
    w->g_ts_drive = pd; w->g_ts_tone = 100 + 900 * pt; w->g_ts_level = -20 + 24 * pl;
    w->g_ff_fuzz = pd; w->g_ff_level = pl;
    w->g_bmp_sus = pd; w->g_bmp_tone = pt; w->g_bmp_vol = pl;
    w->rat.dist = pd; w->rat.filter = 1 - pt; w->rat.volume = pl; w->rat.diodes = rat::SILICON; w->rat.ruetz = false;
    w->rat.configure();
    /* the structure */
    w->w_model = clampi(opt(w, MODEL), 0, gxm::NAMPS - 1);
    w->w_stack = clampi(opt(w, STACK), 0, STACK_OFF);
    w->w_cab = clampi(opt(w, CAB), 0, CAB_OFF);
    w->w_pedal = clampi(opt(w, PEDAL), 0, NPEDALS - 1);
    /* convolver gains (for the cabinet in use; a new one gets its own when it is switched in) */
    float ga, gb;
    conv_gains(w, ga, gb);
    w->conv.gains(ga, gb);
    /* delay */
    static const float BEATS[7] = {1.0f, 2.0f / 3, 0.75f, 0.5f, 1.0f / 3, 0.375f, 0.25f};
    const int dt = clampi(opt(w, DLY_TIME), 0, 7);
    w->dly_free = dt == 7;
    if (!w->dly_free) w->dly_beats = BEATS[dt];
    w->dly_ms = val(w, DLY_MS);
    w->dly_fb = pct(w, DLY_FB);
    w->dly_mix = pct(w, DLY_MIX);
    w->dly.set_tone(pct(w, DLY_TONE));
    w->dly_pp = opt(w, DLY_PP) != 0;
    /* reverb */
    w->rev_mix = 1.4f * pct(w, REV_MIX);   /* stereoverb wet only: 100 = a bit above the dry level */
    w->g_rev_mix = 100;
    w->g_rev_room = pct(w, REV_ROOM);
    w->g_rev_damp = pct(w, REV_DAMP);
    /* levels, input, tuner */
    w->out_g_t = db2g(val(w, VOLUME));
    w->in_g_t = db2g(val(w, INPUT));
    w->in_ch = clampi(opt(w, IN_CH), 0, 2);
    w->gate.set(val(w, GATE));
    const int tm = clampi(opt(w, TUNER), 0, 2);
    if (tm != TUNER_OFF && w->tuner_mode == TUNER_OFF) w->tuner.reset();
    w->tuner_mode = tm;
    w->ref = val(w, TUNE_REF);
}

/* Guitarix's 18 models leave the amp up to 34 dB apart at the same knob settings (12AU7 is a
 * quiet tube, the "pre X > 6V6" models are loud). A fixed trim per model, measured with the
 * test guitar (-12 dBFS peaks, DRIVE 25, PREGAIN/MASTER 0, see host_test.cpp), brings them to
 * about -20 dBFS, so switching models changes the tone and not the volume. */
static const float MODEL_TRIM_DB[gxm::NAMPS] = {
    3.4f, 26.3f, 11.7f, -1.6f, 13.2f, -0.4f,      /* 12AX7 12AU7 12AT7 6DJ8 6C16 6V6 */
    11.5f, 23.8f, 17.7f, 14.4f,                   /* the feedback models */
    -7.6f, 5.5f, 1.2f, -5.6f,                     /* pre X > master 6V6 */
    4.6f, 6.9f, 10.4f, 6.4f,                      /* pre X > push-pull 6V6 */
};

/* a pedal at default settings should change the sound, not the volume: rough makeup per pedal
 * at PEDAL LEVEL 50 (measured with a -20 dBFS guitar-like signal, see host_test.cpp) */
static const float PEDAL_MAKEUP[NPEDALS] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

/* switch to w_* (the output is faded out); fresh state for whatever comes in */
static void apply_structure(Plugin *w) {
    if (w->w_model != w->a_model || w->first) {
        w->a_model = w->w_model;
        w->amp[w->a_model]->clear_state(w->amp[w->a_model]);
        w->model_trim = db2g(MODEL_TRIM_DB[w->a_model]);
    }
    if (w->w_stack != w->a_stack || w->first) {
        w->a_stack = w->w_stack;
        if (w->a_stack < gxm::NSTACKS) w->stack[w->a_stack]->clear_state(w->stack[w->a_stack]);
    }
    if (w->w_pedal != w->a_pedal || w->first) {
        w->a_pedal = w->w_pedal;
        switch (w->a_pedal) {
        case PEDAL_TS9: gxm::ts9_clear(w->ts9); break;
        case PEDAL_FF: w->ff->clear_state(w->ff); break;
        case PEDAL_BMP: w->bmp->clear_state(w->bmp); break;
        case PEDAL_RAT: w->rat.init(w->sr); w->rat.configure(); break;
        default: break;
        }
    }
    if (w->w_cab != w->a_cab || w->first) {
        w->a_cab = w->w_cab;
        float ga, gb;
        conv_gains(w, ga, gb);
        w->conv.gains(ga, gb);
        w->conv.set(&w->cabs->cab[w->a_cab], &w->cabs->cabpres[w->a_cab]);
        w->conv.clear();
    }
    w->first = false;
}
static bool structure_pending(Plugin *w) {
    return w->w_model != w->a_model || w->w_stack != w->a_stack || w->w_cab != w->a_cab || w->w_pedal != w->a_pedal;
}

/* every module at the sample rate (not real-time: effSetSampleRate / creation) */
static void init_rate(Plugin *w, float sr) {
    w->sr = sr;
    const uint32_t r = (uint32_t)std::lround(sr);
    for (auto *p : w->amp) p->set_samplerate(r, p);
    for (auto *p : w->stack) p->set_samplerate(r, p);
    gxm::ts9_init(w->ts9, r);
    w->ff->set_samplerate(r, w->ff);
    w->bmp->set_samplerate(r, w->bmp);
    w->rev->set_samplerate(r, w->rev);
    w->rat.init(sr);
    w->cabs = cab_bank(r);
    w->gate.init(sr);
    w->dly.init(sr);
    w->tuner.init(sr);
    w->first = true;
    w->dirty.store(true);
}

/* ---- readouts ------------------------------------------------------------------ */
static const char *const NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static bool tuner_read(Plugin *w, int &note, int &oct, float &cents) {
    if (w->tuner_mode == TUNER_OFF || opt(w, TUNER) == TUNER_OFF) return false;
    if (w->tuner.age.load() > (int)(1.0f * w->tuner.fsd)) return false;   /* hold a reading 1 s */
    const float f = w->tuner.freq.load();
    if (f <= 0) return false;
    const float midi = 69 + 12 * std::log2(f / w->ref);
    const int n = (int)std::lround(midi);
    cents = (midi - n) * 100;
    note = ((n % 12) + 12) % 12;
    oct = n / 12 - 1;
    return true;
}
static std::string readout_text(Plugin *w, int key) {
    char b[48];
    if (key == IN_METER) {
        const float db = w->peak_db.load();
        if (db < -90) return "IN  --";
        const int bars = clampi((int)std::lround((db + 60) / 5), 0, 12);   /* -60 .. 0 dBFS */
        std::string bar(bars, '#');
        bar += std::string(12 - bars, '.');
        std::snprintf(b, sizeof b, "%s%.0f dB %s", w->clip_n > 0 ? "CLIP " : "", db, bar.c_str());
        return b;
    }
    int note, oct; float c;
    const bool ok = tuner_read(w, note, oct, c);
    if (key == TUNER_NOTE) {
        if (opt(w, TUNER) == TUNER_OFF) return "TUNER OFF";
        if (!ok) return "--";
        if (std::fabs(c) < 3) std::snprintf(b, sizeof b, "%s%d  OK", NOTE_NAMES[note], oct);
        else std::snprintf(b, sizeof b, "%s%d  %+d ct", NOTE_NAMES[note], oct, (int)std::lround(c));
        return b;
    }
    /* TUNER_BAR: 21 marks for -50 .. +50 cents, the centre marked */
    if (!ok) return "----------|----------";
    std::string bar(21, '-');
    bar[10] = '|';
    const int p = clampi(10 + (int)std::lround(c / 5), 0, 20);
    if (std::fabs(c) < 3) { bar[9] = '>'; bar[10] = '#'; bar[11] = '<'; }
    else bar[p] = '#';
    return bar;
}

/* ---- state as text: "key=value;" for every sound parameter. Used for the project chunk
 * and for the preset slots alike, restored by key (later additions keep old ones loading).
 * The chunk also carries the input side (with_ui), a preset slot doesn't. --------------- */
static std::string build_state(Plugin *w, bool with_ui) {
    std::string t;
    char buf[96];
    for (int i = 0; i < NPARAMS; i++) {
        if (KEY_OF[i] < 0 || is_bank_key(i) || (!with_ui && is_ui_key(i))) continue;
        std::snprintf(buf, sizeof buf, "%s=%.3f;", PARAMS[i].key, (double)val_at(w, i));
        t += buf;
    }
    return t;
}
static void apply_state(Plugin *w, const std::string &t, bool with_slot) {
    for (size_t pos = 0; pos < t.size();) {
        size_t semi = t.find(';', pos);
        if (semi == std::string::npos) break;
        std::string kv = t.substr(pos, semi - pos);
        pos = semi + 1;
        size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = kv.substr(0, eq);
        if (key == "prog") {
            const int v = std::atoi(kv.c_str() + eq + 1);
            if (with_slot && v >= 0 && v < 32) w->cur.store(v);
            continue;
        }
        const int i = param_index(key.c_str());
        if (i >= 0 && KEY_OF[i] >= 0 && !is_bank_key(i) && (with_slot || !is_ui_key(i))) set_val(w, i, std::atof(kv.c_str() + eq + 1));
    }
    w->dirty.store(true);
}

/* the start sound: a clean-ish 12AU7 into a 2x12 with a little room */
static void start_values(Plugin *w) {
    start(w, MODEL, 1); start(w, PREGAIN, 0); start(w, DRIVE, 25); start(w, DIST, 20); start(w, MASTER, 0);
    start(w, BASS, 50); start(w, MIDDLE, 50); start(w, TREBLE, 50); start(w, PRESENCE, 50);
    start(w, STACK, 0); start(w, CAB, 1); start(w, CAB_LEVEL, 10); start(w, DEEP, 0);
    start(w, PEDAL, PEDAL_OFF); start(w, P_DRIVE, 50); start(w, P_TONE, 50); start(w, P_LEVEL, 50);
    start(w, DLY_TIME, 2); start(w, DLY_MS, 375); start(w, DLY_FB, 35); start(w, DLY_MIX, 0); start(w, DLY_TONE, 50); start(w, DLY_PP, 0);
    start(w, REV_MIX, 15); start(w, REV_ROOM, 50); start(w, REV_DAMP, 25);
    start(w, VOLUME, 0);
}
static void ui_defaults(Plugin *w) {
    start(w, INPUT, 0); start(w, IN_CH, 0); start(w, GATE, 0); start(w, TUNER, TUNER_OFF); start(w, TUNE_REF, 440);
}

/* ---------------------------------------------------------------------------
 * Preset bank (pattern of mpc-vst-acid / mpc-vst-rattler): 32 slots shared by every Gx Amp
 * instance and project, one text file on the SD card ("index<TAB>name<TAB>state" per saved
 * slot). The slots are the plugin's VST programs (the host's PRESET list); the PRESET knob +
 * LOAD/SAVE on the PRESET tab do the same from the skin. The first slots carry factory sounds
 * until SAVE overwrites them (a factory sound is never written to the file).
 * ------------------------------------------------------------------------- */
#define NSLOTS 32
static std::mutex g_bank_lock;
static bool g_bank_loaded;
static std::string g_slot_chunk[NSLOTS], g_slot_name[NSLOTS], g_bank_path;

#include "factory.h"
enum { NFACTORY = (int)(sizeof FACTORY / sizeof FACTORY[0]) };
static_assert(NFACTORY <= NSLOTS, "more factory sounds than slots");

/* next to the plugin's folder (a reinstall keeps the presets), inside it if the parent can't be
 * written; /proc/self/maps has the .so's path. GXAMP_PRESETS overrides it (offline test). */
static std::string so_dir() {
    std::string dir;
    if (FILE *f = std::fopen("/proc/self/maps", "r")) {
        char line[1024];
        while (std::fgets(line, sizeof line, f)) {
            char *p = std::strstr(line, "/gxamp.so");
            char *first = std::strchr(line, '/');
            if (!p || !first || first > p) continue;
            dir.assign(first, (size_t)(p - first));
            break;
        }
        std::fclose(f);
    }
    return dir;
}
static void bank_load_locked() {
    if (g_bank_loaded) return;
    g_bank_loaded = true;
    g_bank_path.clear();
    if (const char *env = std::getenv("GXAMP_PRESETS")) g_bank_path = env;
    else {
        std::string dir = so_dir(), cand[2];
        if (dir.empty()) dir = "/tmp";
        size_t cut = dir.rfind('/');
        cand[0] = (cut != std::string::npos && cut > 0 ? dir.substr(0, cut) : dir) + "/gxamp_presets.txt";
        cand[1] = dir + "/gxamp_presets.txt";
        for (const std::string &c : cand)
            if (FILE *f = std::fopen(c.c_str(), "r")) { std::fclose(f); g_bank_path = c; break; }
        for (int i = 0; i < 2 && g_bank_path.empty(); i++)
            if (FILE *f = std::fopen(cand[i].c_str(), "a")) { std::fclose(f); g_bank_path = cand[i]; }
    }
    if (g_bank_path.empty()) return;
    if (FILE *f = std::fopen(g_bank_path.c_str(), "r")) {
        static char line[4096];
        while (std::fgets(line, sizeof line, f)) {
            line[std::strcspn(line, "\r\n")] = 0;
            char *t1 = std::strchr(line, '\t');
            char *t2 = t1 ? std::strchr(t1 + 1, '\t') : nullptr;
            int idx = std::atoi(line);
            if (!t2 || idx < 1 || idx > NSLOTS) continue;
            *t1 = *t2 = 0;
            g_slot_name[idx - 1] = t1 + 1;
            g_slot_chunk[idx - 1] = t2 + 1;
        }
        std::fclose(f);
    }
}
static bool bank_write_locked() {
    if (g_bank_path.empty()) return false;
    std::string tmp = g_bank_path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "w");
    if (!f) return false;
    for (int i = 0; i < NSLOTS; i++)
        if (!g_slot_chunk[i].empty())
            std::fprintf(f, "%d\t%s\t%s\n", i + 1, g_slot_name[i].c_str(), g_slot_chunk[i].c_str());
    return std::fclose(f) == 0 && std::rename(tmp.c_str(), g_bank_path.c_str()) == 0;
}
static std::string slot_title(int i) {
    std::lock_guard<std::mutex> lk(g_bank_lock);
    char buf[32];
    if (g_slot_chunk[i].empty()) {
        if (i < NFACTORY) return FACTORY[i].name;
        std::snprintf(buf, sizeof buf, "%02d (empty)", i + 1);
        return buf;
    }
    if (!g_slot_name[i].empty()) return g_slot_name[i];
    std::snprintf(buf, sizeof buf, "Gx Amp %02d", i + 1);
    return buf;
}
static void slot_load(Plugin *w) {
    const int n = clampi(w->cur.load(), 0, NSLOTS - 1);
    std::string c;
    { std::lock_guard<std::mutex> lk(g_bank_lock); c = g_slot_chunk[n]; }
    if (c.empty() && n < NFACTORY) c = FACTORY[n].state;
    if (c.empty()) return;
    start_values(w);
    apply_state(w, c, false);
}
static bool slot_save(Plugin *w) {
    const int n = clampi(w->cur.load(), 0, NSLOTS - 1);
    const std::string c = build_state(w, false);
    std::lock_guard<std::mutex> lk(g_bank_lock);
    g_slot_chunk[n] = c;
    return bank_write_locked();
}

/* ---- audio ---------------------------------------------------------------------- */
static void run_chunk(Plugin *w, float **in, float **out, int off, int n) {
    float *x = w->mono;
    /* input: channel choice and level */
    const float *iL = in[0] + off, *iR = in[1] + off;
    const float gstep = (w->in_g_t - w->in_g) / n;
    for (int i = 0; i < n; i++) {
        const float s = w->in_ch == 1 ? iL[i] : w->in_ch == 2 ? iR[i] : 0.5f * (iL[i] + iR[i]);
        w->in_g += gstep;
        x[i] = s * w->in_g;
    }
    w->in_g = w->in_g_t;
    /* level readout: peak, held 1 s, then falling; CLIP stays 2 s */
    float m = 0;
    for (int i = 0; i < n; i++) m = std::max(m, std::fabs(x[i]));
    if (m >= 0.98f) w->clip_n = (int)(2 * w->sr);
    else if (w->clip_n > 0) w->clip_n -= n;
    if (m >= w->pk_hold) { w->pk_hold = m; w->pk_hold_n = (int)w->sr; }
    else if ((w->pk_hold_n -= n) <= 0) w->pk_hold *= std::pow(0.5f, n / (0.25f * w->sr));
    w->peak_db.store(w->pk_hold > 1e-6f ? 20 * std::log10(w->pk_hold) : -120.0f);
    if (w->tuner_mode != TUNER_OFF) w->tuner.run(x, n);

    w->gate.run(x, n);
    switch (w->a_pedal) {
    case PEDAL_TS9: gxm::ts9_run(w->ts9, n, x, x); break;
    case PEDAL_FF: w->ff->mono_audio(n, x, x, w->ff); break;
    case PEDAL_BMP: w->bmp->mono_audio(n, x, x, w->bmp); break;
    case PEDAL_RAT: for (int i = 0; i < n; i++) x[i] = w->rat.process(x[i]); break;
    default: break;
    }
    if (w->a_pedal != PEDAL_OFF && PEDAL_MAKEUP[w->a_pedal] != 1.0f)
        for (int i = 0; i < n; i++) x[i] *= PEDAL_MAKEUP[w->a_pedal];
    w->amp[w->a_model]->mono_audio(n, x, x, w->amp[w->a_model]);
    {
        const float tg = w->model_trim;
        for (int i = 0; i < n; i++) x[i] *= tg;
    }
    if (w->a_stack < gxm::NSTACKS) w->stack[w->a_stack]->mono_audio(n, x, x, w->stack[w->a_stack]);
    w->conv.run(x, n);
    /* fade around a structure switch: ~3 ms out, ~6 ms in */
    const bool pend = structure_pending(w);
    const float target = pend ? 0.0f : 1.0f, step = pend ? 1.0f / 128 : 1.0f / 256;
    for (int i = 0; i < n; i++) {
        if (w->sw_gain < target) w->sw_gain = std::min(target, w->sw_gain + step);
        else if (w->sw_gain > target) w->sw_gain = std::max(target, w->sw_gain - step);
        x[i] *= w->sw_gain;
    }
    /* delay, reverb */
    const float beat_s = 60.0f / std::max(20.0f, w->tempo);
    const float dsec = w->dly_free ? w->dly_ms * 0.001f : w->dly_beats * beat_s;
    w->dly.run(x, w->bufL, w->bufR, n, dsec * w->sr, w->dly_fb, w->dly_mix, w->dly_pp);
    float *L = w->bufL, *R = w->bufR;
    if (w->rev_mix > 0.0005f || w->rev_mix_s > 0.0005f) {
        /* Guitarix's stereoverb runs the same freeverb on both sides, so a mono guitar would come
         * out mono: the right side's input goes through two allpasses first (a decorrelated,
         * wider tail), and the reverb runs wet only (WET_DRY 100) with the dry kept here, clean */
        if (!w->rev_on) { w->rev->clear_state(w->rev); w->ap1.clear(); w->ap2.clear(); w->rev_on = true; }
        for (int i = 0; i < n; i++) w->revIn[i] = w->ap2.run(w->ap1.run(R[i]));
        w->rev->stereo_audio(n, L, w->revIn, w->revL, w->revR, w->rev);
        const float step = (w->rev_mix - w->rev_mix_s) / n;
        for (int i = 0; i < n; i++) {
            w->rev_mix_s += step;
            w->revL[i] = L[i] + w->rev_mix_s * w->revL[i];
            w->revR[i] = R[i] + w->rev_mix_s * w->revR[i];
        }
        w->rev_mix_s = w->rev_mix;
        L = w->revL; R = w->revR;
    } else w->rev_on = false;
    /* volume, tuner mute, output safety above -3 dBFS (ceiling 0.98) */
    const float mute_t = w->tuner_mode == TUNER_MUTE ? 0.0f : 1.0f;
    const float ostep = (w->out_g_t - w->out_g) / n;
    float *oL = out[0] + off, *oR = out[1] + off;
    for (int i = 0; i < n; i++) {
        w->out_g += ostep;
        w->mute_g += (mute_t - w->mute_g) * 0.002f;
        const float g = w->out_g * w->mute_g;
        float v[2] = {L[i] * g, R[i] * g};
        for (float &s : v) {
            if (!(s == s)) s = 0;   /* never pass a NaN on */
            const float a = std::fabs(s);
            if (a > 0.7f) {
                float t = std::min((a - 0.7f) / 0.3f, 3.0f), t2 = t * t;
                s = std::copysign(0.7f + 0.28f * t * (27 + t2) / (27 + 9 * t2), s);
            }
        }
        oL[i] = v[0]; oR[i] = v[1];
    }
    w->out_g = w->out_g_t;
}

static void processReplacing(AEffect *e, float **in, float **out, int32_t n) {
    Plugin *w = (Plugin *)e->object;
    NoDenormals nd;
    if (w->master) {
        const VstTimeInfo *ti = (const VstTimeInfo *)w->master(&w->fx, audioMasterGetTime, 0, kVstTempoValid, nullptr, 0.0f);
        if (ti && (ti->flags & kVstTempoValid) && ti->tempo > 10) w->tempo = (float)ti->tempo;
    }
    if (w->dirty.exchange(false)) configure(w);
    if (w->first) apply_structure(w);
    for (int off = 0; off < n;) {
        if (structure_pending(w) && w->sw_gain <= 0.0f) apply_structure(w);
        const int m = std::min((int)CHUNK, (int)n - off);
        run_chunk(w, in, out, off, m);
        off += m;
    }
    /* readouts: tell the host every ~100 ms that their text changed */
    bool any = false;
    w->ro_tick += n;
    if (w->ro_tick >= 0.1f * w->sr) {
        w->ro_tick = 0;
        static const int RO[3] = {IN_METER, TUNER_NOTE, TUNER_BAR};
        for (int r = 0; r < 3; r++) {
            const int i = IDX[RO[r]];
            if (i < 0) continue;
            std::string t = readout_text(w, RO[r]);
            if (t == w->ro_last[r]) continue;
            w->ro_last[r] = t;
            w->ro_count = (w->ro_count + 1) % 1000;
            w->cache[i].store(w->ro_count / 1000.0f);
            w->master(&w->fx, audioMasterAutomate, i, 0, 0, w->cache[i].load());
            any = true;
        }
    }
    for (int i = 0; i < NPARAMS; i++) {
        /* a button reports itself released - and is released here too: the Force doesn't send
         * setParameter(0) after a tap */
        if (w->release[i]) { w->release[i] = 0; w->down[i] = false; any = true; w->master(&w->fx, audioMasterAutomate, i, 0, 0, 0.0f); }
        if (!w->notify[i].exchange(0)) continue;
        any = true;
        w->master(&w->fx, audioMasterAutomate, i, 0, 0, w->cache[i].load());
    }
    if (any) w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
}

static void setParameter(AEffect *e, int32_t i, float n) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return;
    const param_t *p = &PARAMS[i];
    if (popup_set(w->open, i, n)) return;
    const int key = KEY_OF[i];
    if (key < 0 || is_readout(key)) return;
    if (key == SLOT) {   /* browsing only: LOAD loads, so turning the knob can't change the sound */
        const int v = (int)std::lround(clamp01(n) * (NSLOTS - 1));
        if (v != w->cur.load()) { w->cur.store(v); w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f); }
        return;
    }
    if (is_trigger(key)) {
        const bool dn = n > 0.5f;
        const bool rising = dn && !w->down[i];
        w->down[i] = dn;
        if (rising) {
            if (key == LOAD) slot_load(w);
            else slot_save(w);
            w->release[i] = 1;
            w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
        }
        return;
    }
    bool nudge = false;
    if (p->nopts > 1) {
        float pos = clamp01(n) * (p->nopts - 1);
        if (std::fabs(pos - std::round(pos)) > 0.001f) {
            float cur = w->cache[i].load() * (p->nopts - 1);
            n = (float)clampi((int)std::lround(cur) + (pos > cur ? 1 : -1), 0, p->nopts - 1) / (p->nopts - 1);
            nudge = true;
        }
    }
    w->cache[i].store(clamp01(n));
    w->dirty.store(true);
    if (!nudge) popup_picked(w->open, w->release, i);
}
static float getParameter(AEffect *e, int32_t i) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return 0.0f;
    const int key = KEY_OF[i];
    if (key == SLOT) return (float)w->cur.load() / (NSLOTS - 1);
    if (key >= 0 && is_trigger(key)) return 0.0f;
    if (popup_is(i)) return w->open[i];
    return w->cache[i].load();
}

/* project chunk: "GXA1;" + the state + "prog=<slot>;" */
static intptr_t get_chunk(Plugin *w, void **ptr) {
    std::string t = "GXA1;" + build_state(w, true);
    char buf[24];
    std::snprintf(buf, sizeof buf, "prog=%d;", w->cur.load());
    t += buf;
    w->chunk.assign(t.begin(), t.end());
    *ptr = w->chunk.data();
    return (intptr_t)w->chunk.size();
}
static intptr_t set_chunk(Plugin *w, const void *data, intptr_t len) {
    if (!data || len < 5) return 0;
    std::string t((const char *)data, (size_t)len);
    t.resize(std::strlen(t.c_str()));
    if (t.compare(0, 5, "GXA1;")) return 0;
    apply_state(w, t.substr(5), true);
    return 1;
}

static void display(Plugin *w, int idx, char *buf, size_t n) {
    const param_t *pp = &PARAMS[idx];
    const int key = KEY_OF[idx];
    if (key == SLOT) { std::snprintf(buf, n, "%s", slot_title(clampi(w->cur.load(), 0, NSLOTS - 1)).c_str()); return; }
    if (key >= 0 && is_trigger(key)) { buf[0] = 0; return; }
    if (key >= 0 && is_readout(key)) { std::snprintf(buf, n, "%s", readout_text(w, key).c_str()); return; }
    if (popup_is(idx)) {
        const int u = norm_to_ui(pp, w->open[idx]);
        if (pp->nopts) std::snprintf(buf, n, "%s", pp->opts[u]); else std::snprintf(buf, n, "%d", u);
        return;
    }
    const int u = norm_to_ui(pp, w->cache[idx].load());
    if (pp->nopts) { std::snprintf(buf, n, "%s", pp->opts[u]); return; }
    const float v = val_at(w, idx);
    switch (key) {
    case PREGAIN: case MASTER: case VOLUME: case INPUT: std::snprintf(buf, n, "%+.1f dB", v); break;
    case CAB_LEVEL: std::snprintf(buf, n, "%+.1f dB", 20 * std::log10(std::max(1e-6f, v * v * 0.01f))); break;
    case P_LEVEL:
        if (opt(w, PEDAL) == PEDAL_TS9) std::snprintf(buf, n, "%+.1f dB", -20 + 0.24f * v);
        else std::snprintf(buf, n, "%d %%", u);
        break;
    case P_TONE:
        if (opt(w, PEDAL) == PEDAL_TS9) std::snprintf(buf, n, "%.0f Hz", 100 + 9 * v);
        else if (opt(w, PEDAL) == PEDAL_FF) std::snprintf(buf, n, "--");
        else std::snprintf(buf, n, "%d %%", u);
        break;
    case DLY_MS: std::snprintf(buf, n, "%d ms", u); break;
    case DLY_TONE: std::snprintf(buf, n, "%.1f kHz", 1.2f * std::pow(10.0f, v * 0.01f)); break;
    case GATE: if (u == 0) std::snprintf(buf, n, "OFF"); else std::snprintf(buf, n, "%.0f dB", -90 + 0.6f * v); break;
    case TUNE_REF: std::snprintf(buf, n, "%d Hz", u); break;
    default: std::snprintf(buf, n, "%d %%", u); break;
    }
}

static void destroy(Plugin *w) {
    for (auto *p : w->amp) if (p) p->delete_instance(p);
    for (auto *p : w->stack) if (p) p->delete_instance(p);
    if (w->ff) w->ff->delete_instance(w->ff);
    if (w->bmp) w->bmp->delete_instance(w->bmp);
    if (w->rev) w->rev->delete_instance(w->rev);
    if (w->ts9) gxm::ts9_free(w->ts9);
    delete w;
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    Plugin *w = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose: destroy(w); return 1;
    case effSetProgram:
        if (v >= 0 && v < NSLOTS) {
            w->cur.store((int)v);
            slot_load(w);
            w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
        }
        return 0;
    case effGetProgram: return w->cur.load();
    case effGetProgramName: if (p) copy_str(p, slot_title(clampi(w->cur.load(), 0, NSLOTS - 1)).c_str(), 24); return 0;
    case effGetProgramNameIndexed:
        if (idx < 0 || idx >= NSLOTS || !p) return 0;
        copy_str(p, slot_title(idx).c_str(), 24);
        return 1;
    case effSetProgramName: {
        const int n = clampi(w->cur.load(), 0, NSLOTS - 1);
        std::lock_guard<std::mutex> lk(g_bank_lock);
        if (!p || g_slot_chunk[n].empty()) return 0;
        std::string nm((const char *)p);
        for (char &c : nm) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        g_slot_name[n] = nm.substr(0, 23);
        bank_write_locked();
        return 0;
    }
    case effGetPlugCategory: return 1;   /* kPlugCategEffect */
    case effGetEffectName:
    case effGetProductString: copy_str(p, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(p, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    case effCanBeAutomated: return idx >= 0 && idx < NPARAMS && KEY_OF[idx] >= 0 && !is_bank_key(idx);
    case effGetParamName: if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].name, 32); return 1;
    case effGetParamLabel: if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].unit, 8); return 1;
    case effGetParamDisplay: {
        if (idx < 0 || idx >= NPARAMS) return 0;
        char buf[48];
        display(w, idx, buf, sizeof buf);
        copy_str(p, buf, 24);
        return 1;
    }
    case effSetSampleRate:
        if (o > 0 && std::fabs(o - w->sr) > 0.5f) init_rate(w, o);
        return 1;
    case effSetBlockSize: return 1;
    case effMainsChanged: return 1;
    case effCanDo:
        if (p && !std::strcmp((const char *)p, "receiveVstTimeInfo")) return 1;
        return -1;
    case effGetChunk: return get_chunk(w, (void **)p);
    case effSetChunk: return set_chunk(w, p, v);
    default: return 0;
    }
}

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    static std::once_flag once;
    std::call_once(once, [] {
        for (int i = 0; i < NPARAMS; i++) KEY_OF[i] = -1;
        for (int k = 0; k < NKEYS; k++) { IDX[k] = param_index(KEYS[k]); if (IDX[k] >= 0) KEY_OF[IDX[k]] = k; }
    });
    { std::lock_guard<std::mutex> lk(g_bank_lock); bank_load_locked(); }
    Plugin *w = new Plugin();
    w->master = master;
    for (int i = 0; i < NPARAMS; i++) { w->cache[i].store(PARAMS[i].def); w->notify[i].store(0); }
    start_values(w);
    ui_defaults(w);
    for (int i = 0; i < NPARAMS; i++) w->notify[i].store(0);
    /* the Guitarix modules and their control values */
    for (int i = 0; i < gxm::NAMPS; i++) {
        PluginLV2 *a = w->amp[i] = gxm::make_amp(i);
        a->connect_ports(gxm::AMP_GAIN1, &w->g_gain1, a);
        a->connect_ports(gxm::AMP_PREGAIN, &w->g_pregain, a);
        a->connect_ports(gxm::AMP_WET_DRY, &w->g_wet_dry, a);
        a->connect_ports(gxm::AMP_DRIVE, &w->g_drive, a);
        a->connect_ports(gxm::AMP_HIGHGAIN, &w->g_high, a);
    }
    for (int i = 0; i < gxm::NSTACKS; i++) {
        PluginLV2 *s = w->stack[i] = gxm::make_stack(i);
        s->connect_ports(gxm::AMP_BASS, &w->g_bass, s);
        s->connect_ports(gxm::AMP_MIDDLE, &w->g_middle, s);
        s->connect_ports(gxm::AMP_TREBLE, &w->g_treble, s);
    }
    w->ts9 = gxm::ts9_new();
    gxm::ts9_connect(w->ts9, gxm::TS9_LEVEL, &w->g_ts_level);
    gxm::ts9_connect(w->ts9, gxm::TS9_TONE, &w->g_ts_tone);
    gxm::ts9_connect(w->ts9, gxm::TS9_DRIVE, &w->g_ts_drive);
    w->ff = gxm::make_fuzzface();
    w->ff->connect_ports(gxm::FF_FUZZ, &w->g_ff_fuzz, w->ff);
    w->ff->connect_ports(gxm::FF_LEVEL, &w->g_ff_level, w->ff);
    w->bmp = gxm::make_bmp();
    w->bmp->connect_ports(gxm::BMP_SUSTAIN, &w->g_bmp_sus, w->bmp);
    w->bmp->connect_ports(gxm::BMP_TONE, &w->g_bmp_tone, w->bmp);
    w->bmp->connect_ports(gxm::BMP_VOLUME, &w->g_bmp_vol, w->bmp);
    w->rev = gxm::make_reverb();
    w->rev->connect_ports(gxm::REV_WET_DRY, &w->g_rev_mix, w->rev);
    w->rev->connect_ports(gxm::REV_LFOFREQ, &w->g_rev_lfo, w->rev);
    w->rev->connect_ports(gxm::REV_ROOMSIZE, &w->g_rev_room, w->rev);
    w->rev->connect_ports(gxm::REV_DAMP, &w->g_rev_damp, w->rev);
    w->rev->connect_ports(gxm::REV_INVERT, &w->g_rev_inv, w->rev);
    w->sr = 0;
    init_rate(w, 44100);
    configure(w);
    AEffect *e = &w->fx;
    std::memset(e, 0, sizeof *e);
    e->magic = 0x56737450;
    e->dispatcher = dispatcher;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing = processReplacing;
    e->numParams = NPARAMS;
    e->numPrograms = NSLOTS;
    e->numInputs = 2;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsProgramChunks;
    e->uniqueID = PLUG_UID;
    e->version = PLUG_VERSION;
    e->object = w;
    return e;
}
