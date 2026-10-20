/* Offline x86 test of gxamp.so (test.sh builds it with ASan/UBSan): every amp model, tonestack,
 * cabinet and pedal sounds and stays finite, presence and the cabinet do what Guitarix's do,
 * the delay sits on the host tempo, ping-pong alternates, the reverb is stereo, the gate closes
 * on hiss and opens on playing, the tuner names notes and cents, the input readout, the 32
 * preset slots (program list, factory sounds at sane levels, SAVE/LOAD, bank file), chunk
 * restore, switching without clicks, 48 and 96 kHz.
 * With "bench" as the second argument it only measures CPU load; with "levels" it prints the
 * level of every factory sound (for tuning factory.h). Prints PASSED/FAILED. */
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dlfcn.h>

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
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;
static VstTimeInfo g_time;
static int automated = 0;
static intptr_t master(AEffect *, int32_t op, int32_t, intptr_t, void *, float) {
    if (op == 0) automated++;
    if (op == 7) return (intptr_t)&g_time;
    return 0;
}
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)
static float SR = 44100;
static const int BS = 128;
static bool bad = false;

static int param(AEffect *e, const char *name) {
    char buf[64];
    for (int i = 0; i < e->numParams; i++) { buf[0] = 0; e->dispatcher(e, 8, i, 0, buf, 0); if (!std::strcmp(buf, name)) return i; }
    std::printf("FAIL: no parameter %s\n", name); fails++; return 0;
}
static std::string display(AEffect *e, const char *name) { char buf[64] = {0}; e->dispatcher(e, 7, param(e, name), 0, buf, 0); return buf; }
struct Range { const char *n; float lo, hi; };
static const Range RANGES[] = {
    {"Amp Model", 0, 17}, {"Pregain", -20, 20}, {"Drive", 0, 100}, {"Distortion", 0, 100}, {"Master", -20, 20},
    {"Bass", 0, 100}, {"Middle", 0, 100}, {"Treble", 0, 100}, {"Presence", 0, 100}, {"Tonestack", 0, 27},
    {"Cabinet", 0, 18}, {"Cab Level", 1, 20}, {"Deep", 0, 1}, {"Pedal", 0, 4}, {"Pedal Drive", 0, 100},
    {"Pedal Tone", 0, 100}, {"Pedal Level", 0, 100}, {"Delay Time", 0, 7}, {"Delay ms", 20, 1500},
    {"Feedback", 0, 95}, {"Delay Mix", 0, 100}, {"Delay Tone", 0, 100}, {"Ping-Pong", 0, 1},
    {"Reverb Mix", 0, 100}, {"Room", 0, 100}, {"Damp", 0, 100}, {"Volume", -24, 12}, {"Input", -24, 24},
    {"Input Ch", 0, 2}, {"Noise Gate", 0, 100}, {"Tuner", 0, 2}, {"Tuner A4", 430, 450}, {"Preset", 1, 32},
    {"Load", 0, 1}, {"Save", 0, 1},
};
static void set(AEffect *e, const char *name, float v) {
    for (auto &r : RANGES) if (!std::strcmp(r.n, name)) { e->setParameter(e, param(e, name), (v - r.lo) / (r.hi - r.lo)); return; }
    std::printf("FAIL: no range for %s\n", name); fails++;
}
static void press(AEffect *e, const char *name) { e->setParameter(e, param(e, name), 1.0f); }
static std::string progname(AEffect *e, int i) { char b[64] = {0}; e->dispatcher(e, 29, i, 0, b, 0); return b; }

/* a guitar-ish source: Karplus-Strong plucks (a power chord every half second), peak ~amp */
struct Guitar {
    std::vector<float> s[3];
    int p[3] = {0, 0, 0};
    unsigned rnd = 1;
    long long t = 0;
    float amp = 0.25f;
    double f0 = 82.41;
    float noise() { rnd = rnd * 1664525u + 1013904223u; return ((rnd >> 9) * (1.0f / 4194304.0f)) - 1.0f; }
    void pluck() {
        const double fs[3] = {f0, f0 * 1.4983, f0 * 2};
        for (int k = 0; k < 3; k++) {
            s[k].assign((size_t)std::lround(SR / fs[k]), 0.0f);
            for (auto &x : s[k]) x = noise();
            p[k] = 0;
        }
    }
    float next() {
        if (t % (long long)(0.5 * SR) == 0) pluck();
        t++;
        float y = 0;
        for (int k = 0; k < 3; k++) {
            auto &b = s[k];
            const size_t n = b.size(), i = p[k], j = (i + 1) % n;
            const float v = b[i];
            b[i] = 0.4985f * (b[i] + b[j]);
            p[k] = (int)j;
            y += v;
        }
        return amp * y / 3;
    }
};

enum Src { SILENCE, SINE, GUITAR, NOISE, IMPULSE };
struct Run { std::vector<float> L, R; };
static Run run(AEffect *e, Src src, double sec, double f = 220, double amp = 0.1, int ch = 2) {
    Run o;
    std::vector<float> il(BS), ir(BS), ol(BS), orr(BS);
    float *in[2] = {il.data(), ir.data()}, *out[2] = {ol.data(), orr.data()};
    static Guitar g;
    g.amp = (float)amp;
    double ph = 0;
    unsigned rnd = 7;
    long long n0 = 0;
    for (int b = 0; b < (int)(sec * SR / BS); b++) {
        for (int i = 0; i < BS; i++) {
            float s = 0;
            if (src == SINE) { s = (float)(amp * std::sin(ph)); ph += 2 * M_PI * f / SR; }
            else if (src == GUITAR) s = g.next();
            else if (src == NOISE) { rnd = rnd * 1664525u + 1013904223u; s = (float)amp * (((rnd >> 9) * (1.0f / 4194304.0f)) - 1.0f); }
            else if (src == IMPULSE) s = (n0 == 0) ? (float)amp : 0.0f;
            n0++;
            il[i] = (ch == 0 || ch == 2) ? s : 0; ir[i] = (ch == 1 || ch == 2) ? s : 0;
        }
        e->processReplacing(e, in, out, BS);
        g_time.samplePos += BS;
        for (int i = 0; i < BS; i++) {
            for (float s : {ol[i], orr[i]}) if (!std::isfinite(s) || std::fpclassify(s) == FP_SUBNORMAL) bad = true;
            o.L.push_back(ol[i]); o.R.push_back(orr[i]);
        }
    }
    return o;
}
static double rms(const std::vector<float> &a, double t0 = 0.2, double t1 = 1e9) {
    double s = 0; size_t i0 = (size_t)(t0 * SR), i1 = std::min(a.size(), (size_t)(t1 * SR)), n = 0;
    for (size_t i = i0; i < i1; i++) { s += (double)a[i] * a[i]; n++; }
    return n ? std::sqrt(s / n) : 0;
}
static double peak(const std::vector<float> &a) { double p = 0; for (float s : a) p = std::max(p, (double)std::fabs(s)); return p; }
static double goertzel(const std::vector<float> &a, double f, double t0) {
    size_t i0 = (size_t)(t0 * SR), i1 = a.size();
    double w = 2 * M_PI * f / SR, c = std::cos(w), s1 = 0, s2 = 0;
    for (size_t i = i0; i < i1; i++) { double s0 = a[i] + 2 * c * s1 - s2; s2 = s1; s1 = s0; }
    return 2 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - 2 * c * s1 * s2)) / (i1 - i0);
}
static double db(double x) { return 20 * std::log10(x + 1e-12); }
static double db2lin(double d) { return std::pow(10.0, d / 20); }
/* energy above f Hz relative to all (two one-pole high-passes) */
static double above(const std::vector<float> &a, double f) {
    double l1 = 0, l2 = 0, k = 1 - std::exp(-2 * M_PI * f / SR), hi = 0, all = 0;
    for (size_t i = (size_t)(0.2 * SR); i < a.size(); i++) {
        l1 += (a[i] - l1) * k; const double h1 = a[i] - l1;
        l2 += (h1 - l2) * k; const double h = h1 - l2;
        hi += h * h; all += (double)a[i] * a[i];
    }
    return hi / (all + 1e-20);
}
static double brightness(const std::vector<float> &a) { return above(a, 3000); }
/* a plain chain: no pedal, no delay, no reverb, cab on */
static void plain(AEffect *e) {
    set(e, "Pedal", 0); set(e, "Delay Mix", 0); set(e, "Reverb Mix", 0); set(e, "Volume", 0); set(e, "Noise Gate", 0);
    set(e, "Tuner", 0); set(e, "Input", 0); set(e, "Input Ch", 0);
}

int main(int argc, char **argv) {
    const char *bank = "/tmp/gxamp_test_presets.txt";
    std::remove(bank);
    setenv("GXAMP_PRESETS", bank, 1);
    void *h = dlopen(argc > 1 ? argv[1] : "./gxamp.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) { std::printf("FAILED: dlopen %s\n", dlerror()); return 1; }
    auto mainf = (AEffect * (*)(audioMasterCallback)) dlsym(h, "VSTPluginMain");
    g_time.tempo = 120; g_time.sampleRate = SR; g_time.flags = 1 << 10;
    AEffect *e = mainf(master);
    CHECK(e->numInputs == 2 && e->numOutputs == 2, "not a stereo effect");
    CHECK(e->dispatcher(e, 35, 0, 0, nullptr, 0) == 1, "not an effect category");
    e->dispatcher(e, 0, 0, 0, nullptr, 0);
    e->dispatcher(e, 10, 0, 0, nullptr, SR);
    const std::string mode = argc > 2 ? argv[2] : "";

    if (mode == "bench") {
        auto bench = [&](const char *what) {
            run(e, GUITAR, 0.5, 0, 0.25);
            auto t0 = std::chrono::steady_clock::now();
            run(e, GUITAR, 10.0, 0, 0.25);
            double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("bench (this CPU, one core): %-52s %.1f %%\n", what, 10.0 * s);
        };
        plain(e); set(e, "Amp Model", 0); set(e, "Cabinet", 0);
        bench("amp 12AX7 + tonestack + 4x12 cab");
        set(e, "Delay Mix", 30); set(e, "Reverb Mix", 25);
        bench("+ delay + reverb");
        set(e, "Pedal", 3);
        bench("+ Big Muff (96 kHz inside)");
        set(e, "Pedal", 1); set(e, "Amp Model", 17); set(e, "Cabinet", 13); set(e, "Presence", 80); set(e, "Tuner", 2);
        bench("TS9 + 6DJ8>PP + Mesa cab + presence + tuner");
        return 0;
    }
    if (mode == "levels") {
        for (int i = 0; i < 32; i++) {
            const std::string nm = progname(e, i);
            if (nm.find("(empty)") != std::string::npos) continue;
            e->dispatcher(e, 2, 0, i, nullptr, 0);
            set(e, "Input", 0);
            auto a = run(e, GUITAR, 3.0, 0, 0.25);
            std::printf("  %2d %-16s rms %6.1f dBFS  peak %.3f\n", i + 1, nm.c_str(), db(rms(a.L, 0.5)), peak(a.L));
        }
        return 0;
    }

    std::printf("defaults: %s / %s / %s / %s / cab %s\n", display(e, "Amp Model").c_str(), display(e, "Pregain").c_str(),
                display(e, "Drive").c_str(), display(e, "Tonestack").c_str(), display(e, "Cabinet").c_str());
    plain(e);

    /* 1. every amp model: sounds, finite, below the ceiling; gain rises with DRIVE */
    {
        double lo = 1e9, hi = 0;
        for (int m = 0; m < 18; m++) {
            set(e, "Amp Model", m);
            auto a = run(e, GUITAR, 1.2, 0, 0.25);
            const double r = rms(a.L, 0.4);
            lo = std::min(lo, r); hi = std::max(hi, r);
            CHECK(r > 1e-3 && peak(a.L) <= 0.981, "model %s: rms %.1f dB, peak %.3f", display(e, "Amp Model").c_str(), db(r), peak(a.L));
            std::printf("  model %-12s %6.1f dBFS\n", display(e, "Amp Model").c_str(), db(r));
        }
        std::printf("  models span %.1f .. %.1f dBFS\n", db(lo), db(hi));
        set(e, "Amp Model", 0);
        double t[2];
        for (int k = 0; k < 2; k++) {
            set(e, "Drive", k ? 100 : 0);
            auto a = run(e, SINE, 1.0, 110, 0.1);
            double h1 = goertzel(a.L, 110, 0.4), hs = 0;
            for (int j = 2; j <= 8; j++) { double x = goertzel(a.L, 110 * j, 0.4); hs += x * x; }
            t[k] = std::sqrt(hs) / std::max(h1, 1e-12);
        }
        std::printf("  drive: THD %.1f %% at 0, %.1f %% at 100\n", 100 * t[0], 100 * t[1]);
        CHECK(t[1] > t[0] * 1.5, "DRIVE does not add distortion");
        set(e, "Drive", 25);
    }

    /* 2. every tonestack and cabinet runs; TREBLE brightens; the cab takes the fizz off */
    {
        for (int s = 0; s < 28; s++) {
            set(e, "Tonestack", s);
            auto a = run(e, GUITAR, 0.6, 0, 0.25);
            CHECK(rms(a.L, 0.3) > 1e-3, "tonestack %s silent", display(e, "Tonestack").c_str());
        }
        set(e, "Tonestack", 4);   /* JCM-800 */
        double br[2];
        for (int k = 0; k < 2; k++) { set(e, "Treble", k ? 100 : 0); br[k] = brightness(run(e, GUITAR, 1.0, 0, 0.25).L); }
        std::printf("  treble: >3 kHz share %.4f at 0, %.4f at 100\n", br[0], br[1]);
        CHECK(br[1] > br[0] * 1.3, "TREBLE does not brighten");
        set(e, "Treble", 50);
        for (int c = 0; c < 19; c++) {
            set(e, "Cabinet", c);
            auto a = run(e, GUITAR, 0.6, 0, 0.25);
            CHECK(rms(a.L, 0.3) > 1e-3 && peak(a.L) <= 0.981, "cabinet %s: rms %.1f dB", display(e, "Cabinet").c_str(), db(rms(a.L, 0.3)));
        }
        /* the cabinet rolls the top off (Guitarix's 4x12: about -24 dB at 8 kHz): hiss through a
         * clean amp, tonestack off, presence 0 */
        set(e, "Amp Model", 1); set(e, "Drive", 0); set(e, "Presence", 0); set(e, "Tonestack", 27);
        set(e, "Cabinet", 18); const double off = above(run(e, NOISE, 1.0, 0, 0.02).L, 7000);
        set(e, "Cabinet", 0); const double on = above(run(e, NOISE, 1.0, 0, 0.02).L, 7000);
        std::printf("  cabinet: >7 kHz share %.4f off, %.4f with 4x12\n", off, on);
        CHECK(on < off * 0.6, "the cabinet does not roll the top off");
        set(e, "Tonestack", 4); set(e, "Amp Model", 0);
        /* CAB LEVEL: +6 dB at 20 vs 10 (Guitarix's level squared) */
        set(e, "Drive", 25); set(e, "Amp Model", 1);
        set(e, "Cab Level", 10); const double l10 = rms(run(e, SINE, 1.0, 220, 0.02).L, 0.4);
        set(e, "Cab Level", 5); const double l5 = rms(run(e, SINE, 1.0, 220, 0.02).L, 0.4);
        std::printf("  cab level 5 vs 10: %.1f dB (Guitarix: -12 dB)\n", db(l5 / l10));
        CHECK(std::fabs(db(l5 / l10) + 12) < 1.5, "CAB LEVEL law");
        set(e, "Cab Level", 10);
        /* PRESENCE: Guitarix's contrast IR (a scoop around 1 kHz, the 2 kHz bite kept) shifts the
         * balance from the low mids to the upper mids: 2 kHz against 1 kHz rises */
        auto bite = [&]() {   /* 2 kHz against 1 kHz through a clean amp */
            const double g1 = goertzel(run(e, SINE, 0.8, 1000, 0.01).L, 1000, 0.4);
            const double g2 = goertzel(run(e, SINE, 0.8, 2000, 0.01).L, 2000, 0.4);
            return g2 / std::max(1e-12, g1); };
        set(e, "Amp Model", 1); set(e, "Drive", 0); set(e, "Tonestack", 27); set(e, "Cabinet", 0);
        set(e, "Presence", 0); const double p0 = bite();
        set(e, "Presence", 100); const double p1 = bite();
        std::printf("  presence: 2 kHz vs 1 kHz %+.1f dB at 0, %+.1f dB at 100\n", db(p0), db(p1));
        CHECK(db(p1 / p0) > 3, "PRESENCE does not lift the upper mids");
        set(e, "Amp Model", 0); set(e, "Drive", 25); set(e, "Tonestack", 4);
        set(e, "Presence", 50);
    }

    /* 3. pedals: each one changes the sound, none blows the level up */
    {
        set(e, "Amp Model", 1); set(e, "Drive", 20); set(e, "Cabinet", 1);
        auto a0 = run(e, GUITAR, 1.0, 0, 0.25);
        const double r0 = rms(a0.L, 0.3);
        const char *pn[5] = {"OFF", "SCREAMER", "FUZZ FACE", "BIG MUFF", "RAT"};
        for (int p = 1; p < 5; p++) {
            set(e, "Pedal", p); set(e, "Pedal Drive", 60); set(e, "Pedal Tone", 50); set(e, "Pedal Level", 50);
            auto a = run(e, GUITAR, 1.2, 0, 0.25);
            const double r = rms(a.L, 0.4);
            std::printf("  pedal %-9s %+5.1f dB against no pedal\n", pn[p], db(r / r0));
            CHECK(r > 1e-3 && peak(a.L) <= 0.981, "pedal %s silent or too hot", pn[p]);
            CHECK(std::fabs(db(r / r0)) < 12, "pedal %s level %.1f dB off", pn[p], db(r / r0));
        }
        set(e, "Pedal", 0);
    }

    /* 4. delay: on the host tempo (1/8 dotted at 120 BPM = 0.375 s), ping-pong alternates */
    {
        set(e, "Reverb Mix", 0); set(e, "Delay Mix", 100); set(e, "Delay Time", 2); set(e, "Feedback", 50); set(e, "Ping-Pong", 0);
        set(e, "Cabinet", 18); set(e, "Tonestack", 27); set(e, "Amp Model", 1); set(e, "Drive", 0);
        run(e, SILENCE, 0.5);
        auto a = run(e, IMPULSE, 1.5, 0, 0.3);
        auto first_peak = [&](const std::vector<float> &v, double from, double to) {
            size_t best = 0; double m = 0;
            for (size_t i = (size_t)(from * SR); i < std::min(v.size(), (size_t)(to * SR)); i++) if (std::fabs(v[i]) > m) { m = std::fabs(v[i]); best = i; }
            return std::make_pair(best / (double)SR, m);
        };
        auto p0 = first_peak(a.L, 0, 0.1), p1 = first_peak(a.L, 0.2, 0.6);
        std::printf("  delay: dry at %.4f s, echo at %.4f s (expect +0.375)\n", p0.first, p1.first);
        CHECK(std::fabs((p1.first - p0.first) - 0.375) < 0.004, "delay not on tempo");
        g_time.tempo = 140;
        run(e, SILENCE, 2.0);
        a = run(e, IMPULSE, 1.0, 0, 0.3);
        p0 = first_peak(a.L, 0, 0.1); p1 = first_peak(a.L, 0.2, 0.6);
        std::printf("  delay at 140 BPM: echo after %.4f s (expect %.4f)\n", p1.first - p0.first, 0.75 * 60 / 140);
        CHECK(std::fabs((p1.first - p0.first) - 0.75 * 60 / 140) < 0.004, "delay does not follow the tempo");
        g_time.tempo = 120;
        set(e, "Ping-Pong", 1);
        run(e, SILENCE, 2.0);
        a = run(e, IMPULSE, 1.3, 0, 0.3);
        /* echo 1 on the left only, echo 2 on the right only (the dry signal is on both) */
        const double l1 = first_peak(a.L, 0.3, 0.45).second, r1 = first_peak(a.R, 0.3, 0.45).second;
        const double l2 = first_peak(a.L, 0.68, 0.82).second, r2 = first_peak(a.R, 0.68, 0.82).second;
        std::printf("  ping-pong: echo 1 L %.3f R %.3f, echo 2 L %.3f R %.3f\n", l1, r1, l2, r2);
        CHECK(l1 > 5 * r1 && r2 > 5 * l2, "ping-pong does not alternate");
        set(e, "Delay Mix", 0); set(e, "Ping-Pong", 0); set(e, "Feedback", 35);
        set(e, "Cabinet", 1); set(e, "Tonestack", 0); set(e, "Drive", 25);
        run(e, SILENCE, 3.0);
    }

    /* 5. reverb: a stereo tail */
    {
        set(e, "Reverb Mix", 60); set(e, "Room", 80);
        run(e, SILENCE, 0.5);
        auto a = run(e, GUITAR, 0.5, 0, 0.25);
        auto t = run(e, SILENCE, 1.0);
        double corr = 0, el = 0, er = 0;
        for (size_t i = 0; i < t.L.size() / 2; i++) { corr += (double)t.L[i] * t.R[i]; el += (double)t.L[i] * t.L[i]; er += (double)t.R[i] * t.R[i]; }
        corr /= std::sqrt(el * er + 1e-30);
        std::printf("  reverb tail %.1f dBFS, L/R correlation %.2f\n", db(rms(t.L, 0, 0.5)), corr);
        CHECK(rms(t.L, 0, 0.5) > 1e-4 && corr < 0.9, "reverb: no stereo tail");
        set(e, "Reverb Mix", 0); set(e, "Room", 50);
        run(e, SILENCE, 2.0);
    }

    /* 6. gate: hiss alone is muted, playing passes */
    {
        set(e, "Noise Gate", 50);   /* -60 dBFS */
        run(e, SILENCE, 0.3);
        auto n = run(e, NOISE, 1.0, 0, 0.0003);   /* ~ -70 dBFS hiss */
        set(e, "Noise Gate", 0);
        auto n0 = run(e, NOISE, 1.0, 0, 0.0003);
        set(e, "Noise Gate", 50);
        auto g = run(e, GUITAR, 1.0, 0, 0.25);
        std::printf("  gate: hiss %.1f dBFS gated, %.1f dBFS open; guitar %.1f dBFS\n", db(rms(n.L, 0.5)), db(rms(n0.L, 0.5)), db(rms(g.L, 0.3)));
        CHECK(rms(n.L, 0.5) < rms(n0.L, 0.5) * 0.1, "the gate doesn't close on hiss");
        CHECK(rms(g.L, 0.3) > 1e-3, "the gate blocks playing");
        set(e, "Noise Gate", 0);
    }

    /* 7. tuner: E2, A2 (+20 cents), A4 = 432; MUTE silences, LIVE passes */
    {
        set(e, "Tuner", 1);
        auto m = run(e, SINE, 1.0, 82.41, 0.1);
        std::string note = display(e, "Note"), bar = display(e, "Tuning");
        std::printf("  tuner E2: \"%s\" \"%s\", output %.1f dBFS (MUTE)\n", note.c_str(), bar.c_str(), db(rms(m.L, 0.5)));
        CHECK(note.compare(0, 2, "E2") == 0 && note.find("OK") != std::string::npos, "tuner E2: %s", note.c_str());
        CHECK(rms(m.L, 0.5) < 1e-4, "MUTE does not mute");
        set(e, "Tuner", 2);
        auto l = run(e, SINE, 1.0, 110 * std::pow(2.0, 20 / 1200.0), 0.1);
        note = display(e, "Note"); bar = display(e, "Tuning");
        std::printf("  tuner A2+20: \"%s\" \"%s\", output %.1f dBFS (LIVE)\n", note.c_str(), bar.c_str(), db(rms(l.L, 0.5)));
        CHECK(note.compare(0, 2, "A2") == 0 && note.find("+2") != std::string::npos, "tuner A2 +20 ct: %s", note.c_str());
        CHECK(rms(l.L, 0.5) > 1e-3, "LIVE mutes");
        set(e, "Tuner A4", 432);
        run(e, SINE, 1.0, 432, 0.1);
        note = display(e, "Note");
        std::printf("  tuner 432 Hz at A4 = 432: \"%s\"\n", note.c_str());
        CHECK(note.compare(0, 2, "A4") == 0 && note.find("OK") != std::string::npos, "tuner reference: %s", note.c_str());
        set(e, "Tuner A4", 440);
        run(e, SINE, 1.0, 329.63, 0.1);
        note = display(e, "Note");
        CHECK(note.compare(0, 2, "E4") == 0, "tuner E4: %s", note.c_str());
        run(e, SILENCE, 1.5);
        note = display(e, "Note");
        CHECK(note == "--", "tuner holds a note in silence: %s", note.c_str());
        set(e, "Tuner", 0);
        CHECK(display(e, "Note") == "TUNER OFF", "tuner off text");
    }

    /* 8. input: the readout, the channel choice, the level */
    {
        run(e, SINE, 0.5, 220, 0.25);
        const std::string m = display(e, "Input Level");
        std::printf("  input readout at -12 dBFS: \"%s\"\n", m.c_str());
        CHECK(m.compare(0, 3, "-12") == 0, "input readout: %s", m.c_str());
        run(e, SINE, 0.3, 220, 1.0);
        CHECK(display(e, "Input Level").compare(0, 4, "CLIP") == 0, "no CLIP: %s", display(e, "Input Level").c_str());
        set(e, "Input Ch", 1);
        const double left = rms(run(e, SINE, 0.8, 220, 0.05, 0).L, 0.4);
        const double right = rms(run(e, SINE, 0.8, 220, 0.05, 1).L, 0.4);
        std::printf("  input LEFT: left-only %.1f dBFS, right-only %.1f dBFS\n", db(left), db(right));
        CHECK(left > 30 * right, "LEFT picks the right channel too");
        set(e, "Input Ch", 0);
        int before = automated;
        run(e, SINE, 0.5, 220, 0.01);
        CHECK(automated > before, "readouts are never pushed to the host");
    }

    /* 9. switching models/cabs/pedals while playing: no click above the music */
    {
        plain(e); set(e, "Amp Model", 0); set(e, "Cabinet", 0);
        auto ref = run(e, SINE, 0.5, 110, 0.1);
        double md = 0, mref = 0;
        for (size_t i = 1; i < ref.L.size(); i++) mref = std::max(mref, (double)std::fabs(ref.L[i] - ref.L[i - 1]));
        for (int k = 0; k < 12; k++) {
            set(e, "Amp Model", (k * 7) % 18); set(e, "Cabinet", (k * 5) % 19); set(e, "Pedal", k % 5); set(e, "Tonestack", (k * 3) % 28);
            auto a = run(e, SINE, 0.1, 110, 0.1);
            for (size_t i = 1; i < a.L.size(); i++) md = std::max(md, (double)std::fabs(a.L[i] - a.L[i - 1]));
        }
        std::printf("  switching: largest step %.3f (steady largest step %.3f)\n", md, mref);
        CHECK(md < std::max(0.5, 4 * mref), "switching clicks");
        plain(e); set(e, "Amp Model", 1); set(e, "Cabinet", 1); set(e, "Tonestack", 0);
    }

    /* 10. the 32 slots: program list, factory sounds at sane levels, SAVE/LOAD, shared bank */
    {
        CHECK(e->numPrograms == 32, "numPrograms %d", e->numPrograms);
        int nf = 0;
        for (int i = 0; i < 32; i++) {
            const std::string nm = progname(e, i);
            if (nm.find("(empty)") != std::string::npos) continue;
            nf++;
            e->dispatcher(e, 2, 0, i, nullptr, 0);
            CHECK(e->dispatcher(e, 3, 0, 0, nullptr, 0) == i, "effGetProgram");
            auto a = run(e, GUITAR, 2.0, 0, 0.25);
            const double r = rms(a.L, 0.5);
            std::printf("  preset %2d %-16s %6.1f dBFS, peak %.3f\n", i + 1, nm.c_str(), db(r), peak(a.L));
            CHECK(r > db2lin(-30) && r < db2lin(-6) && peak(a.L) <= 0.981, "preset %s level %.1f dB", nm.c_str(), db(r));
            run(e, SILENCE, 1.5);
        }
        CHECK(nf >= 12, "only %d factory sounds", nf);
        /* browsing does not load */
        e->dispatcher(e, 2, 0, 0, nullptr, 0);
        const std::string model = display(e, "Amp Model");
        set(e, "Preset", 30);
        CHECK(display(e, "Amp Model") == model, "browsing changed the sound");
        CHECK(display(e, "Preset") == "30 (empty)", "slot 30 not empty: %s", display(e, "Preset").c_str());
        set(e, "Amp Model", 4); set(e, "Cabinet", 12); set(e, "Pedal", 3); set(e, "Input", 6);
        press(e, "Save");
        run(e, SILENCE, 0.1);
        CHECK(display(e, "Preset") == "Gx Amp 30" && progname(e, 29) == "Gx Amp 30", "saved slot name: %s", display(e, "Preset").c_str());
        set(e, "Amp Model", 0); set(e, "Cabinet", 0); set(e, "Pedal", 0); set(e, "Input", -6);
        press(e, "Load");
        run(e, SILENCE, 0.1);
        CHECK(display(e, "Amp Model") == "6C16" && display(e, "Cabinet") == "1x15" && display(e, "Pedal") == "BIG MUFF", "LOAD does not restore: %s %s %s",
              display(e, "Amp Model").c_str(), display(e, "Cabinet").c_str(), display(e, "Pedal").c_str());
        CHECK(display(e, "Input") == "-6.0 dB", "a preset changed the input: %s", display(e, "Input").c_str());
        CHECK(e->getParameter(e, param(e, "Load")) == 0 && e->getParameter(e, param(e, "Save")) == 0, "buttons do not read released");
        press(e, "Load");   /* a second tap works (the Force sends no release) */
        run(e, SILENCE, 0.1);
        AEffect *e2 = mainf(master);
        e2->dispatcher(e2, 10, 0, 0, nullptr, SR);
        CHECK(progname(e2, 29) == "Gx Amp 30", "second instance does not see the bank");
        e2->dispatcher(e2, 1, 0, 0, nullptr, 0);
        FILE *f = std::fopen(bank, "r");
        CHECK(f != nullptr, "no bank file");
        if (f) std::fclose(f);
        set(e, "Input", 0);
    }

    /* 11. chunk: sound and input side restored, the slot too */
    {
        set(e, "Amp Model", 9); set(e, "Tonestack", 10); set(e, "Cabinet", 13); set(e, "Pedal", 1); set(e, "Pedal Drive", 37);
        set(e, "Delay Time", 4); set(e, "Input", -9); set(e, "Input Ch", 2); set(e, "Noise Gate", 40); set(e, "Preset", 7);
        void *chunk = nullptr;
        intptr_t len = e->dispatcher(e, 23, 0, 0, &chunk, 0);
        std::vector<uint8_t> copy((uint8_t *)chunk, (uint8_t *)chunk + len);
        AEffect *e2 = mainf(master);
        e2->dispatcher(e2, 10, 0, 0, nullptr, SR);
        e2->dispatcher(e2, 24, 0, (intptr_t)copy.size(), copy.data(), 0);
        for (const char *k : {"Amp Model", "Tonestack", "Cabinet", "Pedal", "Pedal Drive", "Delay Time", "Input", "Input Ch", "Noise Gate", "Preset"})
            CHECK(display(e2, k) == display(e, k), "chunk: %s %s vs %s", k, display(e2, k).c_str(), display(e, k).c_str());
        std::printf("  chunk %ld bytes: %s / %s / %s / %s\n", (long)len, display(e2, "Amp Model").c_str(), display(e2, "Tonestack").c_str(),
                    display(e2, "Cabinet").c_str(), display(e2, "Input").c_str());
        auto a = run(e2, GUITAR, 0.5, 0, 0.25);
        CHECK(rms(a.L, 0.2) > 1e-3, "restored instance silent");
        e2->dispatcher(e2, 1, 0, 0, nullptr, 0);
    }

    /* 12. other sample rates: every model and the cabinet still work */
    for (float rate : {48000.0f, 96000.0f}) {
        SR = rate;
        AEffect *e3 = mainf(master);
        e3->dispatcher(e3, 10, 0, 0, nullptr, rate);
        set(e3, "Cabinet", 0); set(e3, "Pedal", 3); set(e3, "Reverb Mix", 30); set(e3, "Delay Mix", 30);
        auto a = run(e3, GUITAR, 1.0, 0, 0.25);
        std::printf("  %.0f kHz: %.1f dBFS\n", rate / 1000, db(rms(a.L, 0.3)));
        CHECK(rms(a.L, 0.3) > 1e-3 && peak(a.L) <= 0.981, "%.0f Hz broken", rate);
        e3->dispatcher(e3, 1, 0, 0, nullptr, 0);
    }
    SR = 44100;

    CHECK(!bad, "NaN, Inf or denormals in the output");
    e->dispatcher(e, 1, 0, 0, nullptr, 0);
    std::remove(bank);
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
