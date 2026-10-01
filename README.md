# Vocal DeVerb

Vocal DeVerb is a Steinberg VST3 SDK plug-in that starts as a real-time vocal dereverb effect.
The current DSP pass is a causal STFT dereverb stage with WPE-inspired delayed spectral prediction
and a stereo-linked spectral mask. It preserves attacks and reduces likely late-reflection energy
with Amount, Sensitivity, Tail Release, Vocal Preserve, Dry/Wet, Output Gain, and Bypass parameters.

This is intentionally a direct Steinberg SDK project, not a JUCE project.
It includes a bundled VSTGUI editor at `resource/vocaldeverb.uidesc`, so hosts can show a custom
graphic interface instead of only the generic parameter editor.

## Prerequisites

- CMake 3.24 or newer
- A C++17 compiler
- On Windows, Visual Studio 2022 or Build Tools with the C++ workload
- Git, if you want CMake to fetch the Steinberg SDK automatically

## Build

By default CMake fetches the Steinberg VST3 SDK tag pinned in `CMakeLists.txt`.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The verified local build used Visual Studio Build Tools' bundled CMake and Ninja:

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build-release
```

If you already have the SDK checked out:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DDEREVERB_VST3SDK_SOURCE_DIR=C:\path\to\vst3sdk
cmake --build build --config Release
```

The verified Release bundle is generated at:

```text
build-release\VST3\Release\VocalDeVerb.vst3
```

The resulting `VocalDeVerb.vst3` bundle can be copied to:

```text
C:\Program Files\Common Files\VST3
```

## Validate

The SDK validator is built alongside the plug-in and runs automatically after each plug-in build.
To run it by hand:

```powershell
build-release\bin\validator.exe build-release\VST3\Release\VocalDeVerb.vst3
```

Exact output paths can vary by generator and SDK CMake layout.

## Test

The offline DSP harness (`tests/dsp_tests.cpp`) is off by default. Enable it, build, and run it
through CTest:

```powershell
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DDEREVERB_BUILD_TESTS=ON
cmake --build build-release --target dsp_tests
ctest --test-dir build-release --output-on-failure
```

With a multi-config generator (Visual Studio), add `--config Release` to the build and `-C Release`
to `ctest`.

## DSP Notes

The current processor is an STFT-style spectral processor, not neural source separation or true
room impulse removal. Each channel is buffered into 1024-sample sine-windowed frames with a
256-sample hop. The processor stores complex STFT history per frequency bin and runs a small
adaptive delayed predictor, inspired by weighted prediction error dereverberation, to estimate
late reflections from older frames. It subtracts a bounded amount of that predicted tail, applies a
smoothed spectral mask, and reconstructs with overlap-add. The mask is **stereo-linked** — driven
by the maximum magnitude across channels and applied identically to each — so the stereo image
stays stable as it opens and closes.

Dry/Wet mixing is aligned to the processed frame output, and the processor reports 1023 samples of
latency to the host. Bypass crossfades over ~10 ms to the latency-matched dry signal while the spectral engine keeps
running, and automation points are applied at their sample offsets. An offline test harness
(`tests/dsp_tests.cpp`, target `dsp_tests`) exercises the real processor for null reconstruction,
latency, steady and toggled bypass, automation smoothing and timing, block-size independence,
non-finite containment, stereo stability, reverb reduction, output silence flags, and state round
trips, in 32/64-bit and in-place processing. The next quality step is a fuller WPE solver or
an optional diffusion/ML model.
