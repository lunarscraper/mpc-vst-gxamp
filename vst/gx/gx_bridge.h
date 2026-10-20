/* =============================================================================
 * gx_bridge.h - the Guitarix DSP modules Gx Amp uses, behind one small interface.
 *
 * Every module is Guitarix's own LV2 DSP code (vendored unchanged under gx/faust and gx/dsp,
 * see gx/VENDORED.md). The gxm_*.cpp files each compile a group of them in its own namespace,
 * because the Guitarix headers define the same table types and port enums for every plugin.
 * The port numbers below are those of the Guitarix LV2 plugins (gx_amp.lv2/gxamp.h,
 * gx_fuzzface.h, gx_bmp.h, gxts9.h, gx_reverb.h); the gxm_*.cpp files check them.
 * GPL-2.0-or-later (Guitarix); this file GPL-3.0-or-later like the whole plugin.
 * ========================================================================== */
#pragma once
#include <cstdint>

struct PluginLV2;   /* gx/dsp/gx_pluginlv2.h */

namespace gxm {

/* gx_amp.lv2: the 18 tube models and the 27 tonestacks ------------------------- */
enum { AMP_GAIN1 = 0, AMP_PREGAIN = 1, AMP_WET_DRY = 2, AMP_DRIVE = 3,
       AMP_MIDDLE = 4, AMP_BASS = 5, AMP_TREBLE = 6, AMP_HIGHGAIN = 18 };
enum { NAMPS = 18, NSTACKS = 27, NCABS = 18 };
/* i in Guitarix's model list order: 12ax7, 12AU7, 12AT7, 6DJ8, 6C16, 6V6, the four feedback
 * models, the four "pre X / master 6V6" and the four "pre X / push-pull 6V6" */
PluginLV2 *make_amp(int i);
/* i in Guitarix's tonestack order without its "Off" entry: default, Bassman, Twin, Princeton,
 * JCM-800, JCM-2000, M-Lead, M2199, AC-30, SOL 100, Mesa, JTM-45, AC-15, Peavey, Ibanez,
 * Roland, Ampeg, Rev.Rocket, MIG 100 H, Triple Giant, Trio Preamp, Hughes&Kettner,
 * Fender Junior, Fender, Fender Deville, Gibsen, Engl */
PluginLV2 *make_stack(int i);

/* gx_amp.lv2's cabinet impulse responses (cab_data.cc, 48 kHz) and the presence IR */
int cab_ir(int i, const float **data, uint32_t *sr);
int presence_ir(const float **data, uint32_t *sr);

/* pedals --------------------------------------------------------------------------- */
enum { TS9_LEVEL = 0, TS9_TONE = 1, TS9_DRIVE = 2 };            /* dB -20..4, Hz 100..1000, 0..1 */
struct Ts9;                                                      /* gxts9.lv2's ts9sim */
Ts9 *ts9_new();
void ts9_free(Ts9 *);
void ts9_init(Ts9 *, uint32_t sr);
void ts9_connect(Ts9 *, int port, float *v);
void ts9_clear(Ts9 *);
void ts9_run(Ts9 *, int n, float *in, float *out);

enum { FF_FUZZ = 2, FF_LEVEL = 3 };                              /* gx_fuzzface.lv2, 0..1 */
PluginLV2 *make_fuzzface();
enum { BMP_SUSTAIN = 2, BMP_TONE = 3, BMP_VOLUME = 4 };          /* gx_bmp.lv2 (Big Muff Pi), 0..1 */
PluginLV2 *make_bmp();

/* gx_reverb.lv2 (stereoverb) -------------------------------------------------------- */
enum { REV_WET_DRY = 0, REV_LFOFREQ = 1, REV_ROOMSIZE = 2, REV_DAMP = 3, REV_INVERT = 4 };
PluginLV2 *make_reverb();

/* IR resampling with Guitarix's own BufferResampler (zita-resampler). Not real-time:
 * returns new[] memory, the caller delete[]s it. */
float *resample_ir(uint32_t from_sr, int n, const float *in, uint32_t to_sr, int *out_n);

}  // namespace gxm
