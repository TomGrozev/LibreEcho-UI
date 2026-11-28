# Voice capture harness

Exercises the real capture path off-device, from the codec byte format through
micd's DSP chain:

```
WAV fixture -> fake-tinycap (9ch S24_3LE, 16 valid bits left-justified)
  -> libreecho-micd   real unpack, calibration, beamform, 80 Hz HPF, 4x digital gain
```

Only the two codec tools (`tinycap`/`tinymix`, swapped in via micd's
`--capture-bin`/`--mixer-bin`) are stand-ins. Everything between them is
production source, which is the point: it catches regressions in the code that
actually ships.

## Running

```sh
tests/voice-e2e/bin/build.sh        # ~40 s, output in build/harness (gitignored)
tests/voice-e2e/bin/capture-response.sh
```

Every measurement prints its evidence, so a failure names the stage that broke
rather than just the outcome.

## Why it is worth keeping

Re-introducing the historic 24-bit unpack bug (dropping `value >>= 8` from
`le_voice_unpack_s24_3le`) shows up immediately as a three-digit idle floor,
outside the 5..400 the unpack contract implies. That is 256x hot capture, and
the same bug degrades the wake margin and latches VAD active on a device.

## Capture path frequency response (#37 part 3)

```sh
tests/voice-e2e/bin/capture-response.sh          # TOLERANCE=6 by default
```

Plays stepped tones through the real DSP chain and reports magnitude vs
frequency, writing `build/harness/capture-response.json` so builds can be
diffed. It measures the **software** capture chain — unpack scaling,
calibration, beamforming, the 80 Hz high-pass, digital gain — not the acoustic
path, and needs no measurement microphone. The band stops at 8 kHz because
capture runs at 16 kHz; the 20 Hz–20 kHz in #37 describes the output path.

Current baseline is flat to ±0.0 dB from 200 Hz to 7 kHz, with the 80 Hz
high-pass visible below that (−3.0 dB at 80 Hz, −7.0 dB at 40 Hz).

```sh
tests/voice-e2e/bin/capture-response.sh quality   # GAIN_TOL=5 THDN_TOL=1
```

Noise floor, gain linearity, THD+N and clipping headroom, from one buffer per
level so no cross-recording alignment is involved. Baseline:

| in amp | in rms | out rms | gain | THD+N | clipped |
|---|---|---|---|---|---|
| 500 | 354 | 1384 | 3.91x | 0.20% | 0 |
| 2000 | 1414 | 5549 | 3.92x | 0.02% | 0 |
| 8000 | 5657 | 22201 | 3.92x | 0.01% | 0 |
| 16000 | 11314 | 28684 | 2.54x | 21.76% | 4700 |

Noise floor −65.3 dBFS; gain spread 0.28% over the five unclipped levels;
clipping from input amplitude 16000. The 3.92x confirms micd's
`MIC_DIGITAL_GAIN_DEFAULT` of 4.00x, the ~2% shortfall being the Q14 per-mic
calibration and the beamformer average. Speech runs at 70–105 rms, so there is
roughly 35 dB of headroom before distortion.

Only unclipped levels are judged — above the ceiling the chain is *supposed* to
distort, and recording where that starts is the measurement.

## Endpointing latency

`END_SILENCE_MS=<ms>` overrides the endpoint wait for a run. Sweeping it against
the `alexa-what-time` fixture gives a straight line:

| end_silence_ms | turn length |
|---|---|
| 400 | 1840 ms |
| 700 | 2140 ms |
| 1000 | 2440 ms |
| 1500 | 2940 ms |

`turn = end_silence_ms + 1440 ms`, exactly, at every point — so endpointing
carries **no hidden overhead**. The 1440 ms constant is command audio plus
preroll, i.e. real content. If a turn feels like it lingers, the whole of the
tail is that one setting; the valid range is 200–3000 ms.

Note the endpoint decision is made by production `stt_engine_wyoming.c`, not by
the harness: the fake STT is a passive sink that accumulates chunks until the
client sends `audio-stop`. So this measures the shipping logic even in a
hermetic run.

## Notes

- Runs `linux/arm64` by default. Under emulated `linux/amd64` on Apple Silicon
  a caught `SIGTERM` never runs its handler and kills the process (exit 143),
  which makes every daemon look like it crashes on shutdown. That is an
  emulator artifact, not a LibreEcho defect — the same binary is clean on
  native arm64.

## Not covered

armv7/musl and real MT8163 timing; the device's own wakeword payload (built
from source here instead); AEC and barge-in (`aec_frames=0`); ttsd synthesis
and audio output; follow-up turns, wake cooldown, and noisy-room robustness.
