/* gxm_pedals.cpp - Guitarix's pedal DSP behind gx_bridge.h, each in its own namespace (they all
 * define their own table types and port enums): gx_fuzzface.lv2 (Fuzz Face), gx_bmp.lv2 (Big
 * Muff Pi, 96 kHz inside via gx_resampler) and gx_reverb.lv2 (stereoverb). The Tube Screamer
 * (gxts9.lv2's ts9sim, which #defines min/max) lives in gxm_ts9.cpp. Vendored files unchanged.
 * GPL-2.0-or-later (Guitarix). */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unistd.h>

#include "gx_common.h"
#include "gx_pluginlv2.h"
#include "gx_resampler.h"
#include "gx_bridge.h"

namespace gxm_fuzzface {
typedef enum { EFFECTS_OUTPUT, EFFECTS_INPUT, FUZZ, LEVEL } PortIndex;   /* gx_fuzzface.lv2/gx_fuzzface.h */
#include "fuzzface.cc"
static_assert((int)FUZZ == gxm::FF_FUZZ && (int)LEVEL == gxm::FF_LEVEL, "fuzzface ports");
}

namespace gxm_bmp {
typedef enum { EFFECTS_OUTPUT, EFFECTS_INPUT, SUSTAIN, TONE, VOLUME } PortIndex;   /* gx_bmp.lv2/gx_bmp.h */
#include "bmp.cc"
static_assert((int)SUSTAIN == gxm::BMP_SUSTAIN && (int)TONE == gxm::BMP_TONE && (int)VOLUME == gxm::BMP_VOLUME, "bmp ports");
}

namespace gxm_reverb {
typedef enum { WET_DRY, LFOFREQ, ROOMSIZE, DAMP, INVERT,                       /* gx_reverb.lv2/gx_reverb.h */
               EFFECTS_OUTPUT, EFFECTS_OUTPUT1, EFFECTS_INPUT, EFFECTS_INPUT1 } PortIndex;
#include "stereoverb.cc"
static_assert((int)WET_DRY == gxm::REV_WET_DRY && (int)LFOFREQ == gxm::REV_LFOFREQ && (int)ROOMSIZE == gxm::REV_ROOMSIZE &&
              (int)DAMP == gxm::REV_DAMP && (int)INVERT == gxm::REV_INVERT, "reverb ports");
}

namespace gxm {

PluginLV2 *make_fuzzface() { return gxm_fuzzface::fuzzface::plugin(); }
PluginLV2 *make_bmp() { return gxm_bmp::bmp::plugin(); }
PluginLV2 *make_reverb() { return gxm_reverb::stereoverb::plugin(); }

float *resample_ir(uint32_t from_sr, int n, const float *in, uint32_t to_sr, int *out_n) {
    if (from_sr == to_sr || n <= 0) {
        float *o = new float[n > 0 ? n : 1];
        if (n > 0) std::memcpy(o, in, sizeof(float) * n);
        *out_n = n;
        return o;
    }
    gx_resample::BufferResampler r;
    int32_t olen = 0;
    float *p = r.process((int32_t)from_sr, n, const_cast<float *>(in), (int32_t)to_sr, &olen);
    if (!p || olen <= 0) {   /* never expected; the IR then just plays at the wrong rate */
        float *o = new float[n];
        std::memcpy(o, in, sizeof(float) * n);
        *out_n = n;
        return o;
    }
    *out_n = olen;
    return p;   /* BufferResampler::process returns new float[] */
}

}  // namespace gxm
