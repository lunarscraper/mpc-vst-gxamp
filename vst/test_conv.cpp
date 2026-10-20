/* test_conv.cpp - the partitioned convolver of gxamp_vst.cpp against a plain direct convolution:
 * random IRs of several lengths (shorter than one partition, exact multiples, odd lengths, the
 * real Guitarix cabinets), random input in odd-sized pieces, and the gain glide settling on the
 * exact mix. Prints PASSED/FAILED. */
#include "gxamp_vst.cpp"

static int fails = 0;
static unsigned rnd = 12345;
static float frand() { rnd = rnd * 1664525u + 1013904223u; return ((rnd >> 9) * (1.0f / 4194304.0f)) - 1.0f; }

static double check(const std::vector<float> &ha, const std::vector<float> &hb, float ga, float gb, const char *what) {
    IrSet A, B;
    A.build(ha); B.build(hb);
    Convolver *c = new Convolver();
    c->gains(ga, gb);
    c->set(&A, &B);
    c->clear();
    const int N = 9000;
    std::vector<float> x(N), y(N);
    for (auto &v : x) v = frand();
    for (int i = 0, step = 1; i < N; step = step % 300 + 37) {   /* odd piece sizes */
        const int n = std::min(step, N - i);
        std::memcpy(&y[i], &x[i], sizeof(float) * n);
        c->run(&y[i], n);
        i += n;
    }
    double err = 0, ref = 0;
    for (int n = 0; n < N; n++) {
        double s = 0;
        for (size_t m = 0; m < std::max(ha.size(), hb.size()) && (int)m <= n; m++) {
            const double h = (m < ha.size() ? ga * ha[m] : 0.0) + (m < hb.size() ? gb * hb[m] : 0.0);
            s += h * x[n - m];
        }
        err = std::max(err, std::fabs(s - y[n]));
        ref = std::max(ref, std::fabs(s));
    }
    delete c;
    const double rel = err / (ref + 1e-20);
    std::printf("  %-34s max error %.2e (relative %.2e)\n", what, err, rel);
    if (!(rel < 1e-5)) { fails++; std::printf("FAIL: %s\n", what); }
    return rel;
}

int main() {
    for (int len : {1, 17, 64, 65, 128, 129, 500, 1000, 1500}) {
        std::vector<float> a(len), b(len / 2 + 3);
        for (auto &v : a) v = frand() * std::exp(-3.0f * (&v - &a[0]) / len);
        for (auto &v : b) v = frand() * 0.5f;
        char w[64];
        std::snprintf(w, sizeof w, "IR %d taps + %d taps", len, (int)b.size());
        check(a, b, 0.7f, 0.4f, w);
    }
    /* the real cabinets, resampled to 44.1 kHz, with the presence path */
    const CabBank *bank = cab_bank(44100);
    for (int cab : {0, 2, 13, 18}) {
        std::vector<float> ha, hb;
        {   /* rebuild time-domain IRs the way cab_bank does */
            const float *pd; uint32_t psr;
            const int pn = gxm::presence_ir(&pd, &psr);
            std::vector<float> pres = resampled(pd, pn, psr, 44100);
            if (cab == CAB_OFF) ha.assign(1, 1.0f);
            else { const float *d; uint32_t isr; const int n = gxm::cab_ir(cab, &d, &isr); ha = resampled(d, n, isr, 44100); }
            hb = conv_full(ha, pres);
        }
        char w[64];
        std::snprintf(w, sizeof w, "cabinet %d (%d / %d taps)", cab, (int)ha.size(), (int)hb.size());
        check(ha, hb, 0.6f, 0.8f, w);
        (void)bank;
    }
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
