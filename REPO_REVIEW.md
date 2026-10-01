# Repository review

Date: 2026-10-01. Scope: first-party C++, VST3 integration, CMake, UI description,
documentation, and offline DSP tests. This is a correctness and maintainability
review, not an exhaustive security audit or listening evaluation.

## Baseline

- Release build targets `dsp_tests` and `VocalDeVerb` are up to date.
- All seven offline DSP checks pass: identity residual -139.0 dB, measured and
  reported latency 1023 samples, exact delayed bypass, gain smoothing, non-finite
  audio containment, stereo drift about 0.1%, and synthetic DRR improvement 1.03 dB.
- The existing Release bundle passes all 47 Steinberg validator tests.
- This directory has no Git metadata, so history and tracked-file status could
  not be inspected.

## Priority findings

### 1. Bypass freezes the spectral engine and replays stale audio

Location: `source/processor.cpp:170-185`.

`feedBypassDelay` runs continuously, but `processAudio` runs only when bypass is
off. Analysis frames, overlap-add output, predictor history, and mix smoothers
therefore stop advancing while bypass is on. Disabling bypass resumes the earlier
timeline. This is more than the already documented missing bypass crossfade.

Reproduction: identity settings, deterministic noise with amplitude 0.25, bypass
on at sample 8192 and off at 16384. In the first 1023 samples after disabling
bypass, the maximum difference from the correctly delayed input is 0.488891.
Even with Amount=0, the transition fails identity reconstruction.

Recommendation: keep the wet engine advancing during bypass and crossfade between
aligned wet and dry output. If saving bypass CPU is required, define and test a
reset/warm-up policy before selecting wet output again. Cover startup in bypass,
long bypass intervals, repeated toggles, and in-place buffers in both sample formats.

### 2. Automation applies future values before their offsets

Location: `source/processor.cpp:190-215`.

`updateParameters` reads the last point in each queue and applies it before the
block begins; it ignores every point's sample offset. Output smoothing reduces
steps but cannot correct the timing or preserve intermediate automation points.

Reproduction: in a 512-sample block, schedule Output Gain at offset 511
(absolute sample 24063). Compare against an otherwise identical run with no gain
change. Output already differs before the event, with maximum error 0.485987
over samples 24000 through 24062.

Recommendation: consume automation at its offsets, retaining the existing gain
and mix smoothing. Define how spectral controls update at frame boundaries.
Test multiple points in one block and equivalent automation across different
block sizes. Treat this as correctness work despite the validator passing.

### 3. Output silence flags can contradict generated audio

Location: `source/processor.cpp:279-439`.

Both audio paths write output samples without setting `outputBus.silenceFlags`.
A stale silence bit can survive when delayed or reconstructed audio is nonzero.
A host using that bit to skip downstream processing can discard valid audio.

Reproduction: process six 512-sample blocks of constant 0.2 input while seeding
the output silence flag to 1 before each call. The final output RMS is 0.200000,
but the output silence flag remains 1.

Recommendation: at minimum clear output silence flags when producing audio.
Accurate silence detection must consider buffered tails and delay lines, rather
than copying the current input silence flags. Add a regression test for nonzero
delayed output during silent input and for preexisting output flags.

## Test and maintenance improvements

- Register the harness with CTest (`enable_testing` / `add_test`) and document the
  `DEREVERB_BUILD_TESTS=ON` build and run commands. The executable currently exists
  only as an optional target, with no project CTest registration or CI workflow.
- Expand DSP assertions to 64-bit processing, variable block sizes, multiple
  sample rates, in-place processing, state round trips, and the three cases above.
  The SDK validator exercises some of these formats but does not verify waveform
  correctness for these transitions.
- Make metric assertions fail on non-finite values. Comparisons such as
  `if (improvementDb < 0.5)` and `if (db > -60.0)` do not fail when the metric is NaN.
- Tighten the identity threshold: the current -60 dB threshold permits almost
  80 dB of regression from the measured -139 dB baseline. Choose a tolerance
  supported by measurements across the intended compilers and platforms.
- Explicitly validate non-finite parameter/state values. `clampNormalized` only
  checks less-than and greater-than, so its source-level semantics preserve NaN.
  The Release NaN-gain probe recovered; persistent silence was not reproduced.
  Avoid relying on compiler-specific floating-point optimization for validation.
- Replace intentionally leaked test processors with suitable scoped ownership.
- Extend `.gitignore` to cover `build-*` directories. The local workspace has
  several of these, each containing generated output and SDK downloads.
- Update `DSP_REVIEW.md` to separate historical findings from current behavior;
  remove duplicate open items and stale references to removed smoothing fields.
  Correct the README validator example: this local build uses
  `build-release/bin/validator.exe`.

## DSP quality and performance

The synthetic test shows modest reverb reduction (1.03 dB). Build a small evaluation
set of real vocals and varied room responses, checking direct-signal loss and
attack preservation alongside tail reduction. Use listening comparisons before
changing suppression strength or implementing a larger WPE solver.

Benchmark worst-case processing time per block before replacing the FFT. FFT/WPE
work occurs in bursts at frame boundaries, and the predictor also does substantial
work. A real-input FFT may help, but its benefit should be measured against the
whole callback cost at realistic sample rates and channel counts.

## Evidence and proposed order

Diagnostic source and build script are in `build-release/review_probe.cpp` and
`build-release/review_probe.cmd`. They use the existing test helpers and link the
actual Release processor object. No production source was changed for this review.

Recommended order: bypass continuity, output silence flags, automation timing,
then broader automated checks and real-vocal evaluation. Optimize CPU after
measuring where the callback spends its time.

## Status (2026-10-01, same day)

All three priority findings and the test/maintenance items are addressed; details in
`DSP_REVIEW.md` under "Follow-up from REPO_REVIEW.md". `ctest` passes (`dsp_tests`, now
with toggle, timing, block-size, silence-flag, NaN-parameter and state tests), and the
Release bundle still passes 47/47 validator tests. Built against the original processor,
the new harness fails 7 checks (toggle x4, timing, block size, silence flags), which
reproduces findings 1-3. Real-vocal evaluation and CPU benchmarking remain open.
