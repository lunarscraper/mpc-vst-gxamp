/* gxm_ts9.cpp - gxts9.lv2's Tube Screamer simulation (ts9sim.cc + its ts9nonlin table) behind
 * gx_bridge.h. A file of its own because ts9sim.cc #defines min/max. Vendored files unchanged.
 * GPL-2.0-or-later (Guitarix). */
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "gx_bridge.h"

namespace gxm_ts9 {
using std::fabs; using std::pow; using std::tan;
typedef enum { TS9_LEVEL, TS9_TONE, TS9_DRIVE, EFFECTS_OUTPUT, EFFECTS_INPUT, BYPASS } PortIndex;   /* gxts9.lv2/gxts9.h */
#include "ts9sim.cc"
#undef max
#undef min
static_assert((int)TS9_LEVEL == gxm::TS9_LEVEL && (int)TS9_TONE == gxm::TS9_TONE && (int)TS9_DRIVE == gxm::TS9_DRIVE, "ts9 ports");
}

namespace gxm {

struct Ts9 { gxm_ts9::ts9sim sim; };
Ts9 *ts9_new() { return new Ts9(); }
void ts9_free(Ts9 *t) { delete t; }
void ts9_init(Ts9 *t, uint32_t sr) { gxm_ts9::ts9sim::init_static(sr, &t->sim); }
void ts9_connect(Ts9 *t, int port, float *v) { gxm_ts9::ts9sim::connect_static((uint32_t)port, v, &t->sim); }
void ts9_clear(Ts9 *t) { gxm_ts9::ts9sim::clear_state_static(&t->sim); }
void ts9_run(Ts9 *t, int n, float *in, float *out) { gxm_ts9::ts9sim::run_static((uint32_t)n, in, out, &t->sim); }

}  // namespace gxm
