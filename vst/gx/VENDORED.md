# Vendored Guitarix DSP

Upstream: [brummer10/guitarix](https://github.com/brummer10/guitarix), `master` as downloaded
(`guitarix-master.zip`, file dates 2026-10-05; a GitHub branch zip carries no commit id). License:
GPL-2.0-or-later for Guitarix's own files, GPL-3.0-or-later for zita-resampler, so the whole
plugin is GPL-3.0-or-later (`COPYING.guitarix` is Guitarix's own license file).

Everything here is copied **unchanged**. The files this port adds are `gx_bridge.h` and the
three `gxm_*.cpp` units, which include the vendored files inside their own namespaces (the
Guitarix plugins each define the same table types and port enums) and declare the LV2 port
enums the vendored `.cc` files expect (the original headers pull in the LV2 SDK).

| Here | From `trunk/src/` | What |
|---|---|---|
| `faust/gxamp*.cc` (18) | `LV2/faust-generated/` | the tube models of gx_amp.lv2 ("GX Amplifier X") |
| `faust/tonestack_*.cc` (27) | `LV2/faust-generated/` | the tonestacks of gx_amp.lv2 |
| `faust/fuzzface.cc`, `faust/bmp.cc` | `LV2/faust-generated/` | GX Fuzz Face, GX Big Muff Pi |
| `faust/stereoverb.cc` | `LV2/faust-generated/` | GX Reverb (stereo freeverb) |
| `faust/ts9sim.cc`, `faust/ts9nonlin.*` | `LV2/gxts9.lv2/` | GX Tube Screamer |
| `dsp/cab_data.cc`, `dsp/cab_data_table.cc` | `LV2/DSP/` | the 18 cabinet IRs and the presence ("contrast") IR, 48 kHz |
| `dsp/valve.h`, `dsp/trany.h`, `tube_tables/*` | `LV2/DSP/` | tube and transistor tables |
| `dsp/clipping.h`, `dsp/clipt*.cc` | `LV2/faust/` | the Big Muff's clipping tables |
| `dsp/gx_common.h`, `dsp/gx_compiler.h`, `dsp/gx_pluginlv2.h` | `LV2/DSP/` | Faust support, the PluginLV2 struct |
| `dsp/gx_resampler.*` | `LV2/DSP/` | Guitarix's resamplers (Big Muff at 96 kHz inside, cabinet IRs to the host rate) |
| `zita-resampler/` | `zita-resampler-1.1.0/` | Fons Adriaensen's resampler |

Not taken over: Guitarix's convolver (`gx_convolver.*`, zita-convolver with FFTW and a worker
thread). `gxamp_vst.cpp` runs the same impulse responses through its own zero-latency
partitioned convolver, checked against a plain convolution by `test_conv.cpp`; the presence
blend and the cabinet level follow gx_amp.lv2's `gxamp.cpp` (`DELT_PRESENCE_HACK`, `Impf`,
`Ampf`, the halved 1x8). Not ported: the neural modules (NAM, RTNeural), the power-amp models
and Guitarix's UI, engine and JACK side.

To re-vendor: copy the same files from a newer Guitarix and rebuild; `gxm_*.cpp` check the port
numbers and model counts at compile time.
