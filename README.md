# mpc-vst-gxamp

Gx Amp: the [Guitarix](https://github.com/brummer10/guitarix) guitar amplifier as a VST2
**insert effect** for the Akai MPC OS plugin host (Force, MPC Live/One/X/Key). Made for a real
electric guitar on an audio input, and just as well behind RiffBox or any other plugin.

The amp, tonestacks, cabinets, pedals and reverb are Guitarix's own DSP code (the LV2 plugins'
Faust code, vendored unchanged in `vst/gx/`, see `vst/gx/VENDORED.md`). Written here: the plug-in
around it, the convolution, the noise gate, the delay, the tuner and the presets.

```
input (L+R / LEFT / RIGHT, -24..+24 dB, level readout)
 -> noise gate
 -> pedal: SCREAMER (GX Tube Screamer) | FUZZ FACE | BIG MUFF | RAT (mpc-vst-rat's model) | OFF
 -> amp: 18 Guitarix tube models (GX Amplifier X)
 -> tonestack: 27 Guitarix tonestacks | OFF
 -> presence + cabinet: Guitarix's 18 cabinet IRs | OFF
 -> delay (tempo-synced or free, mono or ping-pong)
 -> stereo reverb (GX Reverb)
 -> volume
```

## Pages

- **AMP** (home): AMP MODEL, PREGAIN, DRIVE, MASTER, BASS, MIDDLE, TREBLE, PRESENCE
- **CAB**: TONESTACK, CABINET, CAB LEVEL, DISTORTION (Guitarix's distortion mix in front of the
  tubes), DEEP (lets the amp through below 31 Hz, for drop tunings and bass), PRESENCE, MASTER, VOLUME
- **PEDAL**: PEDAL, DRIVE, TONE, LEVEL (each on the pedal's own controls: TS9 drive/tone/level,
  Fuzz Face fuzz/level, Big Muff sustain/tone/volume, RAT distortion/filter/volume), plus the
  amp's PREGAIN, DRIVE, MASTER and VOLUME
- **DELAY**: TIME (1/4, 1/4 T, 1/8 ., 1/8, 1/8 T, 1/16 ., 1/16 on the Force's tempo, or FREE with
  TIME (FREE) 20..1500 ms), FEEDBACK, MIX, TONE, PING-PONG, REVERB MIX, VOLUME
- **REVERB**: MIX, ROOM, DAMP, VOLUME, DELAY MIX, FEEDBACK
- **INPUT**: INPUT, CHANNEL (L+R, LEFT, RIGHT), NOISE GATE (OFF, then -89..-30 dBFS), VOLUME and
  the input level readout (peak, `CLIP` for 2 s after a clipped sample)
- **TUNER**: TUNER (OFF, MUTE: tune silently, LIVE: tune while playing), A4 (430..450 Hz), the
  note with its cents and a needle (`----#-----|----------`, `>#<` when in tune)
- **PRESET**: 32 slots, the knob browses, LOAD and SAVE act; the slots are also the plugin's
  program list. Shared by every instance and project in `gxamp_presets.txt` next to the plugin
  folder. INPUT, CHANNEL, NOISE GATE, TUNER and A4 belong to the source and the project, not to
  a preset: a sound made for the guitar works the same on RiffBox's track.

Factory sounds (slots 1-18, all at about -18 dBFS RMS for a guitar peaking around -12 dBFS):
CLEAN TWIN, GOA CLEAN DLY, CHIME AC, CLASSIC ROCK, PLEXI CRUNCH, BRIT GARAGE, 60s FUZZ,
STONER MUFF, DOOM, DESERT FUZZ, GOA LEAD, PSY CHUG, RAT GRIND, AMBIENT SWELL, SURF SPRING,
FUZZ LEAD DLY, RIFFBOX CRUNCH, BASS AMP.

## Using it on the Force

- **Guitar**: an audio track with the guitar's input (input 1 = CHANNEL LEFT, or a mono input
  with L+R), monitoring on, Gx Amp as an insert. Set the Force's input gain and INPUT so the
  readout peaks around -12 dB when you play hard. Noise: NOISE GATE up until the hiss is gone
  between notes.
- **RiffBox / synths**: Gx Amp as an insert behind them; synths are hotter than a guitar, so
  INPUT down (-6..-12 dB) until the readout peaks around -12 dB.
- Delay times follow the Force's tempo (1/8 . = the dotted eighth of Goa/psytrance leads).

## Notes on the port

- Guitarix's 18 models are up to 34 dB apart at the same knob settings; a fixed trim per model
  brings them to the same level at default settings, so switching models changes the tone,
  not the volume.
- Guitarix's convolver needs FFTW and a worker thread; here the same IRs (resampled from 48 kHz
  with Guitarix's own resampler) run through a zero-latency partitioned FFT convolver, and the
  presence and cabinet stages collapse into one filter (both linear, as is the tonestack).
  `vst/test_conv.cpp` checks it against a plain convolution.
- Guitarix's stereo reverb runs the same freeverb on both sides; the right side gets two
  allpasses in front so a mono guitar gets a wide tail, and the reverb runs wet only beside a
  clean dry signal.
- Switching model, tonestack, cabinet or pedal fades out for ~3 ms and back in (fresh state for
  what comes in), so there are no clicks.
- Not ported: Guitarix's neural amp modules (NAM, RTNeural: too heavy for the Force), its
  separate power-amp plugins, the UI/engine/JACK side.

`vst/test.sh` runs the offline test under ASan/UBSan (every model, tonestack, cabinet and pedal;
presence and cabinet laws; delay on tempo and ping-pong; stereo reverb; gate; tuner; input
readout; switching without clicks; the 32 slots and factory levels; chunk restore; 48/96 kHz),
the convolver test and a benchmark: the full chain is ~1.5-2.5 % of an x86 core (mpc-vst-rat: 1.6 %).

Build/deploy workflow: see `sd88me/mpc-vst-plugins`' `docs/PORTING.md`. License: GPL-3.0-or-later
(Guitarix is GPL-2.0-or-later, its zita-resampler GPL-3.0-or-later). `vst/rat_core.h` is the RAT
model of mpc-vst-rat (MIT). Guitarix is by Hermann Meyer, Andreas Degert, Pete Shorthose, Steve
Poskitt and contributors; no affiliation. Amp, pedal and cabinet names describe what the
Guitarix models are styled after; no affiliation with their makers.

## Hinweis

Entwickelt mit Unterstützung von Claude (Anthropic)
