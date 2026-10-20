/* gxm_amps.cpp - Guitarix's gx_amp.lv2 DSP: the 18 tube models (gx_amp.lv2/gx_amp.cc), the 27
 * tonestacks (gx_tonestack.cc) and the cabinet / presence impulse responses (cab_data.cc), as
 * Guitarix builds them, behind gx_bridge.h. The vendored files are included unchanged.
 * GPL-2.0-or-later (Guitarix: Hermann Meyer, Andreas Degert, Pete Shorthose, Steve Poskitt). */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unistd.h>

#include "gx_common.h"
#include "gx_pluginlv2.h"
#include "gx_bridge.h"

namespace gxm_amps {

/* gx_amp.lv2/gxamp.h's PortIndex (that header pulls in the LV2 SDK, which this port doesn't need) */
typedef enum {
    GAIN1 = 0, PREGAIN, WET_DRY, DRIVE, MIDDLE, BASS, TREBLE, CLevel, ALevel, MODEL, T_MODEL, C_MODEL,
    AMP_CONTROL, AMP_NOTIFY, SCHEDULE, AMP_OUTPUT, AMP_INPUT, BYPASS, HIGHGAIN, TRIM
} PortIndex;

#include "gxamp.cc"
#include "gxamp2.cc"
#include "gxamp3.cc"
#include "gxamp4.cc"
#include "gxamp5.cc"
#include "gxamp6.cc"
#include "gxamp7.cc"
#include "gxamp8.cc"
#include "gxamp9.cc"
#include "gxamp10.cc"
#include "gxamp11.cc"
#include "gxamp12.cc"
#include "gxamp13.cc"
#include "gxamp14.cc"
#include "gxamp15.cc"
#include "gxamp16.cc"
#include "gxamp17.cc"
#include "gxamp18.cc"

#include "tonestack_default.cc"
#include "tonestack_bassman.cc"
#include "tonestack_twin.cc"
#include "tonestack_princeton.cc"
#include "tonestack_jcm800.cc"
#include "tonestack_jcm2000.cc"
#include "tonestack_mlead.cc"
#include "tonestack_m2199.cc"
#include "tonestack_ac30.cc"
#include "tonestack_soldano.cc"
#include "tonestack_mesa.cc"
#include "tonestack_jtm45.cc"
#include "tonestack_ac15.cc"
#include "tonestack_peavey.cc"
#include "tonestack_ibanez.cc"
#include "tonestack_roland.cc"
#include "tonestack_ampeg.cc"
#include "tonestack_ampeg_rev.cc"
#include "tonestack_sovtek.cc"
#include "tonestack_bogner.cc"
#include "tonestack_groove.cc"
#include "tonestack_crunch.cc"
#include "tonestack_fender_blues.cc"
#include "tonestack_fender_default.cc"
#include "tonestack_fender_deville.cc"
#include "tonestack_gibsen.cc"
#include "tonestack_engl.cc"

#include "cab_data_table.cc"

typedef PluginLV2 *(*maker)();
/* gx_amp.lv2/gxamp.cpp's amp_model[] order, without gxnoamp */
static const maker AMPS[] = {
    gxamp::plugin, gxamp3::plugin, gxamp14::plugin, gxamp10::plugin, gxamp18::plugin,
    gxamp2::plugin,
    gxamp9::plugin, gxamp11::plugin, gxamp17::plugin, gxamp13::plugin,
    gxamp5::plugin, gxamp4::plugin, gxamp15::plugin, gxamp12::plugin,
    gxamp7::plugin, gxamp8::plugin, gxamp16::plugin, gxamp6::plugin,
};
/* gxamp.cpp's tonestack_model[] order (its "Off" lives outside the list) */
static const maker STACKS[] = {
    tonestack_default::plugin, tonestack_bassman::plugin, tonestack_twin::plugin,
    tonestack_princeton::plugin, tonestack_jcm800::plugin, tonestack_jcm2000::plugin,
    tonestack_mlead::plugin, tonestack_m2199::plugin, tonestack_ac30::plugin,
    tonestack_soldano::plugin, tonestack_mesa::plugin, tonestack_jtm45::plugin,
    tonestack_ac15::plugin, tonestack_peavey::plugin, tonestack_ibanez::plugin,
    tonestack_roland::plugin, tonestack_ampeg::plugin, tonestack_ampeg_rev::plugin,
    tonestack_sovtek::plugin, tonestack_bogner::plugin, tonestack_groove::plugin,
    tonestack_crunch::plugin, tonestack_fender_blues::plugin, tonestack_fender_default::plugin,
    tonestack_fender_deville::plugin, tonestack_gibsen::plugin, tonestack_engl::plugin,
};
static_assert(sizeof AMPS / sizeof AMPS[0] == gxm::NAMPS, "amp count");
static_assert(sizeof STACKS / sizeof STACKS[0] == gxm::NSTACKS, "tonestack count");
static_assert(sizeof cab_table / sizeof cab_table[0] == gxm::NCABS, "cabinet count");
static_assert((int)GAIN1 == gxm::AMP_GAIN1 && (int)PREGAIN == gxm::AMP_PREGAIN && (int)WET_DRY == gxm::AMP_WET_DRY &&
              (int)DRIVE == gxm::AMP_DRIVE && (int)MIDDLE == gxm::AMP_MIDDLE && (int)BASS == gxm::AMP_BASS &&
              (int)TREBLE == gxm::AMP_TREBLE && (int)HIGHGAIN == gxm::AMP_HIGHGAIN, "gx_amp port numbers");

}  // namespace gxm_amps

namespace gxm {

PluginLV2 *make_amp(int i) { return (i >= 0 && i < NAMPS) ? gxm_amps::AMPS[i]() : nullptr; }
PluginLV2 *make_stack(int i) { return (i >= 0 && i < NSTACKS) ? gxm_amps::STACKS[i]() : nullptr; }

int cab_ir(int i, const float **data, uint32_t *sr) {
    if (i < 0 || i >= (int)gxm_amps::cab_table_size) return 0;
    gxm_amps::CabDesc &c = *gxm_amps::cab_table[i].data;
    *data = c.ir_data;
    *sr = c.ir_sr;
    return c.ir_count;
}
int presence_ir(const float **data, uint32_t *sr) {
    gxm_amps::CabDesc &c = gxm_amps::contrast_ir_desc;
    *data = c.ir_data;
    *sr = c.ir_sr;
    return c.ir_count;
}

}  // namespace gxm
