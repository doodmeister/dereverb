# DSP Review & Improvement Plan — Vocal DeVerb

_Review date: 2026-06-15. Scope: `source/processor.cpp` (the DSP), with notes on the
plugin plumbing around it._

> **Historical record.** Everything from "Verdict" through "Status" describes the code as
> of June 2026; line numbers and some structures (the 3-band stage, the Amount smoother)
> no longer exist. For current behavior see the README, and for what is still open see
> **Current open items** at the bottom.

## Verdict

The core spectral math is **sound**. The STFT analysis/synthesis is a correct
weighted-overlap-add (WOLA), the radix-2 FFT is correct, and the per-bin WPE predictor
is structurally legitimate. The real problems are in the **plugin plumbing** around the
DSP (latency/bypass handling, parameter handling, denormals) and in a few
**quality/robustness** choices — not in the spectral algebra.

## What is correct (verified by reading)

- **WOLA reconstruction** (`processor.cpp:587-592`, `:397-401`): normalizing by the
  accumulated `Σ window²` gives exact identity reconstruction for any window/hop, even
  during startup. Robust choice.
- **FFT** (`:595-653`): Gold-Rader bit-reversal and butterfly are correct;
  conjugate-symmetric mirroring (`:571-576`) and zeroing DC/Nyquist imaginary parts are
  correct, so the IFFT yields a real signal.
- **WPE history indexing** (`:494-506`, `:570-582`): the `delay >= 4` offset means taps
  never read the half-written current frame; leaky-NLMS update with energy
  normalization is a legitimate adaptive predictor.
- **Band crossover** (`:357-368`): `low + mid + high ≡ spectral` algebraically, so the
  residual stage is unity when bands aren't modified.
- The `std::move` buffer shifts (`:405-413`) are leftward (dest < source), so the
  overlapping-range moves are safe.

## Correctness issues (ranked by impact)

1. **Bypass breaks latency compensation** — `processor.cpp:166-170` vs `:150-153`.
   `getLatencySamples()` always reports 1024, but the bypass path copies input→output
   with **zero** latency. A host doing delay compensation will misalign the bypassed
   signal by ~1024 samples, plus a click on every toggle. Fix: run the bypass copy
   through a 1024-sample delay line (and/or crossfade) so reported latency is always
   honored.
2. **No parameter smoothing → zipper noise** — `:279-290`, `:196-204`. `Amount`,
   `Dry/Wet`, `Output Gain` are direct multipliers read once per block and applied as a
   step. `updateParameters` also keeps only the last queue point and discards
   `sampleOffset`, so sample-accurate automation is flattened to block rate.
3. **No denormal protection** — pervasive (`:663-665`, `:454`, `:465`, `:565`). Many
   recursive one-poles decay toward zero during silence/tails; without FTZ/DAZ this is
   the classic "CPU spikes when the track goes quiet" bug.
4. **Reported latency may be off by one** — `:150-153`. Hand-trace suggests the true
   delay is 1023 (N−1) while it reports 1024 (N). To be settled empirically by the
   null/latency test below.
5. **Independent per-channel processing → unstable stereo image** — `:342`. Each channel
   computes its own mask/gain; on stereo material the L/R masks diverge and smear the
   image. Want a stereo-linked mode (mask from `max(magL,magR)` or the mid signal).
6. **Output can exceed unity with no safety net** — `:368-372`. Residual overshoot plus
   up to +12 dB output gain, never clamped.

## Improvements beyond correctness

- **Real-input FFT**: currently a full *complex* 1024-pt FFT/IFFT on a real signal
  (`:436-442`, `:585`). An rFFT halves CPU and working memory.
- **Precompute twiddles in double**: the float trig *recurrence* (`:636-639`)
  accumulates rotation error over 512 steps and caps the noise floor — matters for a
  subtractive process.
- **Reconsider the 3-band time-domain residual stage** (`:655-725`): it re-attacks the
  same tail with a parallel set of hard-to-reason-about heuristics, roughly doubling the
  tuning surface for a second-order effect. Measure whether it earns its keep.
- **No test/measurement harness** exists. For DSP this is the highest-leverage gap.

## Plan (this branch of work)

Ordered by leverage. The test harness comes first so the subsequent fixes are
verifiable (and so issue #4 is settled by measurement, not by hand-trace).

1. **Offline test harness** — add a `tests/` executable that links the already-built
   SDK static libs and drives the real `VocalDeVerbProcessor::process()`. Tests:
   - **Null / identity**: `Amount=0`, `Dry/Wet=100%`, `Output Gain=0 dB` ⇒ output equals
     input delayed by the reported latency (validates WOLA + reconstruction).
   - **Measured latency**: cross-correlate input vs output to find the true integer
     delay; compare to `getLatencySamples()` (settles issue #4).
   - **Bypass**: `Bypass=1` ⇒ output equals input delayed by the reported latency
     (fails before fix #1, passes after).
   Build via `build-release` reconfigured with `-DDEREVERB_BUILD_TESTS=ON`
   (reuses prebuilt SDK libs; only compiles `processor.cpp`, `state.cpp`, test main).
2. **Denormal flushing** (issue #3) — set FTZ/DAZ at the top of `process()`.
3. **Bypass delay-line + correct latency** (issues #1 and #4) — delay the bypass copy by
   the reported latency; fix the reported value if the test shows an off-by-one.

Deferred (follow-up, not this turn): per-sample parameter smoothing (#2), stereo linking
(#5), output safety clamp (#6), real-input FFT, twiddle tables.

## Status

- [x] Review complete; this document written.
- [x] **Test harness added and building** — `tests/dsp_tests.cpp`, target `dsp_tests`
      (enable with `-DDEREVERB_BUILD_TESTS=ON`). Null residual measures **−99 dB**, so
      WOLA reconstruction is confirmed clean.
- [x] **Issue #4 (latency) — fixed.** The harness measured the true delay as **1023**
      (N−1), confirming the hand-trace. `getLatencySamples()` now reports
      `kLatencySamples = kSpectralFftSize - 1`; the null/latency test passes
      (reported == measured == 1023).
- [x] **Issue #3 (denormals) — fixed.** `ScopedNoDenormals` sets FTZ/DAZ for the
      duration of each `process()` call and restores the host's mode on return
      (`processor.cpp`).
- [x] **Issue #1 (bypass latency) — fixed.** A per-channel dry delay line
      (`ChannelState::bypassDelay`, length `kLatencySamples`, stored as `double` for
      64-bit transparency) is advanced every block — even while processing — so soft
      bypass stays latency-compensated. Bypass null test is **bit-exact**
      (`maxAbsErr = 0`).

- [x] **Issue #2 (parameter smoothing) — fixed.** The mix-stage multipliers (Amount
      blend, Dry/Wet, Output Gain) are now ramped per sample toward their block target
      with a ~10 ms one-pole (`smoothedDryWet_` / `smoothedOutputGain_` today; the Amount
      smoother was later removed with the band stage), re-seeded on reset so there is no
      start-up ramp. A new zipper test steps Output
      Gain 0 dB → +12 dB mid-stream: the worst one-sample output step fell from **0.536
      (20.6x the natural slew)** to **0.026 (1.0x)** — i.e. the step is gone.
- [x] **Issue #6 (output safety) — fixed as non-finite containment.** Input is
      sanitized (`NaN`/`Inf` → 0) before it can poison the recursive FFT/WPE/envelope
      state, and the output is finite-guarded plus clamped to a generous ±16 (~+24 dBFS)
      runaway ceiling. Chosen over a hard unity clamp so legitimate headroom (incl. the
      +12 dB Output Gain) is preserved. A new robustness test injects NaN/Inf samples and
      asserts the output stays finite **and recovers** (`lateRMS=0.069`); before the fix
      the state was poisoned permanently (`lateRMS=nan`).

- [x] **Issue #5 (stereo-linked mask) — fixed.** The per-sample loop was restructured
      to channel-inner / frame-synchronized: all channels fill their analysis buffers in
      lockstep, so each spectral frame is now processed jointly (`processLinkedFrame`).
      The mask is driven by the **linked magnitude** (`max` across channels) held in a
      shared `linkedBins_` table and applied identically to every channel, so the stereo
      image no longer wanders as the mask opens and closes. The WPE predictor stays
      per-channel. A new stereo test feeds `R = 0.5*L` and checks the ratio survives:
      image drift fell from **12.6% → 1.8%** (the small remainder is the still
      per-channel band stage). The five mono tests are **byte-for-byte unchanged**, so
      the refactor introduced no single-channel regression.

- [x] **3-band residual stage — retired (measured, then removed).** Added a DRR
      (direct-to-reverberant ratio) test that runs a synthetic Schroeder-reverb signal
      through the processor and measures tail reduction. Comparison decided it:

      | | DRR improvement | stereo drift |
      |---|---|---|
      | band stage ON  | 0.93 dB | 1.8% |
      | band stage OFF | **1.03 dB** | **0.1%** |

      The stage *slightly worsened* the dereverb and was the sole cause of the residual
      stereo drift, while being the most complex code in the DSP. Removed it entirely
      (the `BandState`/`BandSettings` structs, `processBand`, the band crossover filters
      `updateFilterCoefficients`/`onePoleLowpassCoefficient`, and the now-unused Amount
      smoother) — ~140 lines of the hardest-to-reason-about code. Stereo drift is now
      0.1% and the DRR test (`improvement >= 0.5 dB`) guards against regressing the
      remaining dereverb.

Verification: `dsp_tests` → **ALL TESTS PASSED** (null, latency, bypass, zipper, robust,
stereo, dereverb). Full plugin rebuilt and the Steinberg `validator` → **47/47 tests
passed** (32- and 64-bit). README DSP notes updated to match.

- [x] **Precomputed twiddle tables — done.** The per-stage float trig *recurrence* in
      the FFT butterfly was replaced with a table (`twiddleReal_`/`twiddleImag_`)
      computed once in double (`initTwiddleTable`). This removes the rotation drift that
      capped the noise floor and keeps `cos`/`sin` out of the hot loop. The null
      reconstruction floor dropped from **−99 dB to −139 dB** — empirical confirmation of
      the review's precision claim — with every other test unchanged.

Verification: `dsp_tests` → **ALL TESTS PASSED** (null −139 dB, latency, bypass, zipper,
robust, stereo, dereverb). Full plugin rebuilt and the Steinberg `validator` → **47/47**.

### Follow-up from REPO_REVIEW.md (2026-10-01)

- [x] **Bypass continuity + crossfade.** The spectral engine now runs while bypassed, and
      bypass crossfades linearly over ~10 ms to the latency-matched dry line (fully
      bypassed output stays bit-exact). Toggle error vs delayed input: 0.49 → 7e-8.
- [x] **Sample-accurate automation.** Blocks are split at queue-point offsets; output is
      now bit-identical across block sizes 1/37/100/512/1024.
- [x] **Output silence flags** are cleared whenever audio is written.
- [x] **Non-finite parameters/state** map to 0 via a bit-pattern finite test. The SDK
      builds with `/fp:fast` / `-ffast-math`, so `std::isfinite` and NaN comparisons are
      not reliable; the processor's input/output guards use the same helper.
- [x] Harness registered with CTest; 32/64-bit, in-place, sample-rate, silence-flag and
      state round-trip coverage added; identity threshold tightened to −120 dB.

## Current open items

- **Dereverb strength is mild** (~1 dB DRR on aggressive synthetic reverb). Build a small
  set of real reverberant vocals and listen before retuning the Amount→suppression
  mapping or moving to a fuller WPE solver; the DRR test keeps it measurable.
- **Measure CPU before optimizing.** Benchmark worst-case per-block time (FFT/WPE work
  bursts at frame boundaries) before replacing the FFT with a real-input transform.
- **Bypass CPU.** The engine keeps running while bypassed, by design, for continuity.
- **Silence flags are never set**, only cleared; report true silence only if a host
  benefits, and account for the delay line and overlap-add tail when doing so.
