// Offline DSP test harness for Vocal DeVerb.
//
// Drives the real VocalDeVerbProcessor through the SDK audio-processing path
// (no plugin host required). Covers reconstruction, latency, bypass (steady and
// toggled), automation smoothing and timing, non-finite containment, stereo
// linking, dereverb strength, output silence flags, and state round trips, in
// 32/64-bit, separate and in-place buffers, several block sizes and sample rates.
//
// Build:  configure with -DDEREVERB_BUILD_TESTS=ON, build target `dsp_tests`,
//         then run it directly or via `ctest`.

#include "processor.h"
#include "parameters.h"

#include "pluginterfaces/base/smartpointer.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;
using namespace Steinberg::Dereverb;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int32 kBlock = 512;
constexpr int kWarmup = 4096; // skip the STFT fill-in / adaptive warm-up region
constexpr double kPi = 3.14159265358979323846;

using Signal = std::vector<float>;
using Params = std::vector<std::pair<ParamID, ParamValue>>;

struct Event
{
    int sample;
    ParamID id;
    ParamValue value;
};

struct Options
{
    int block = kBlock;
    double sampleRate = kSampleRate;
    bool f64 = false;
    bool inPlace = false;
    uint64 seedOutputSilence = 0;              // written to outputs[0].silenceFlags before each call
    std::vector<uint64>* silenceFlags = nullptr; // receives outputs[0].silenceFlags after each call
};

// Amount=0, Dry/Wet=100%, 0 dB: output should equal input delayed by the latency.
const Params kIdentity {{kParamAmount, 0.0},       {kParamSensitivity, 0.45}, {kParamTailRelease, 0.32},
                        {kParamVocalPreserve, 0.65}, {kParamDryWet, 1.0},       {kParamOutputGain, 0.5},
                        {kParamBypass, 0.0}};

// ~1.4 MB (8 channel states): too large for the stack.
std::unique_ptr<VocalDeVerbProcessor> makeProcessor ()
{
    return std::make_unique<VocalDeVerbProcessor> ();
}

// Deterministic broadband noise in [-amp, amp] (LCG; no host RNG dependence).
Signal makeNoise (int n, float amp, uint32_t seed = 12345u)
{
    Signal v (static_cast<std::size_t> (n));
    uint32_t s = seed;
    for (auto& sample : v)
    {
        s = s * 1664525u + 1013904223u;
        const float u = static_cast<float> (s >> 8) / static_cast<float> (1u << 24); // [0,1)
        sample = (2.0f * u - 1.0f) * amp;
    }
    return v;
}

double rms (const float* p, int n)
{
    double acc = 0.0;
    for (int i = 0; i < n; ++i)
        acc += static_cast<double> (p[i]) * p[i];
    return std::sqrt (acc / std::max (1, n));
}

Signal makeSine (int n, float amp, double freqHz)
{
    Signal v (static_cast<std::size_t> (n));
    for (int i = 0; i < n; ++i)
        v[static_cast<std::size_t> (i)] =
            amp * static_cast<float> (std::sin (2.0 * kPi * freqHz * i / kSampleRate));
    return v;
}

// A train of windowed-noise bursts: the "direct" (dry) events, with silent gaps.
Signal makeBurstTrain (int n, int period, int burstLen, float amp)
{
    const auto noise = makeNoise (n, amp, 9001u);
    Signal out (static_cast<std::size_t> (n), 0.0f);
    for (int i = 0; i < n; ++i)
    {
        const int phase = i % period;
        if (phase < burstLen)
        {
            const float w = 0.5f * (1.0f - std::cos (2.0 * kPi * phase / burstLen));
            out[static_cast<std::size_t> (i)] = noise[static_cast<std::size_t> (i)] * w;
        }
    }
    return out;
}

// Adds a Schroeder feedback-comb reverb tail to a dry signal (the thing the plug-in
// is supposed to reduce). Returns direct + reverberant.
Signal addReverb (const Signal& dry)
{
    const int delays[4] = {1557, 1617, 1491, 1422};
    const float fb[4] = {0.84f, 0.82f, 0.85f, 0.83f};
    std::vector<Signal> buf (4);
    int idx[4] = {0, 0, 0, 0};
    for (int c = 0; c < 4; ++c)
        buf[static_cast<std::size_t> (c)].assign (static_cast<std::size_t> (delays[c]), 0.0f);

    Signal out (dry.size ());
    for (std::size_t i = 0; i < dry.size (); ++i)
    {
        float wet = 0.0f;
        for (int c = 0; c < 4; ++c)
        {
            float& slot = buf[static_cast<std::size_t> (c)][static_cast<std::size_t> (idx[c])];
            wet += slot;
            slot = dry[i] + fb[c] * slot;
            if (++idx[c] >= delays[c])
                idx[c] = 0;
        }
        out[i] = dry[i] + 0.35f * wet;
    }
    return out;
}

// Direct-to-reverberant ratio: energy during the dry bursts vs energy in the silent
// gap (which only the reverb tail fills). `offset` aligns a processed output to the dry.
double measureDRR (const Signal& sig, int period, int burstLen, int gapStart, int gapEnd, int offset)
{
    const int n = static_cast<int> (sig.size ());
    double directE = 0.0, tailE = 0.0;
    for (int i = 2 * period; i < n; ++i) // skip warm-up
    {
        const int src = i - offset;
        if (src < 0)
            continue;
        const int phase = src % period;
        const double v = static_cast<double> (sig[static_cast<std::size_t> (i)]) *
                         sig[static_cast<std::size_t> (i)];
        if (phase < burstLen)
            directE += v;
        else if (phase >= gapStart && phase < gapEnd)
            tailE += v;
    }
    return directE / (tailE + 1e-12);
}

template <typename T>
std::vector<Signal> renderAs (VocalDeVerbProcessor& proc, const std::vector<Signal>& in,
                              const Params& initial, const std::vector<Event>& events,
                              const Options& o)
{
    const auto channels = static_cast<int32> (in.size ());
    const int n = static_cast<int> (in[0].size ());

    std::vector<std::vector<T>> src (in.size ()), dst (in.size ());
    for (std::size_t c = 0; c < in.size (); ++c)
    {
        src[c].assign (in[c].begin (), in[c].end ());
        dst[c].assign (o.inPlace ? 0 : in[c].size (), T {});
    }

    ProcessSetup setup {};
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = o.f64 ? kSample64 : kSample32;
    setup.maxSamplesPerBlock = o.block;
    setup.sampleRate = o.sampleRate;
    proc.setupProcessing (setup);

    std::vector<T*> inPtr (in.size ()), outPtr (in.size ());
    for (int offset = 0; offset < n; offset += o.block)
    {
        const int blk = std::min (o.block, n - offset);
        ParameterChanges changes;
        auto addPoint = [&changes] (ParamID id, int32 sampleOffset, ParamValue value) {
            int32 queueIndex = 0, pointIndex = 0;
            changes.addParameterData (id, queueIndex)->addPoint (sampleOffset, value, pointIndex);
        };
        if (offset == 0)
            for (const auto& pr : initial)
                addPoint (pr.first, 0, pr.second);
        for (const auto& e : events)
            if (e.sample >= offset && e.sample < offset + blk)
                addPoint (e.id, e.sample - offset, e.value);

        for (std::size_t c = 0; c < in.size (); ++c)
        {
            inPtr[c] = src[c].data () + offset;
            outPtr[c] = o.inPlace ? inPtr[c] : dst[c].data () + offset;
        }

        AudioBusBuffers inBus {};
        inBus.numChannels = channels;
        AudioBusBuffers outBus {};
        outBus.numChannels = channels;
        outBus.silenceFlags = o.seedOutputSilence;
        if constexpr (std::is_same_v<T, float>)
        {
            inBus.channelBuffers32 = inPtr.data ();
            outBus.channelBuffers32 = outPtr.data ();
        }
        else
        {
            inBus.channelBuffers64 = inPtr.data ();
            outBus.channelBuffers64 = outPtr.data ();
        }

        ProcessData data {};
        data.processMode = kRealtime;
        data.symbolicSampleSize = setup.symbolicSampleSize;
        data.numSamples = blk;
        data.numInputs = 1;
        data.numOutputs = 1;
        data.inputs = &inBus;
        data.outputs = &outBus;
        data.inputParameterChanges = changes.getParameterCount () > 0 ? &changes : nullptr;

        proc.process (data);
        if (o.silenceFlags)
            o.silenceFlags->push_back (outBus.silenceFlags);
    }

    std::vector<Signal> out (in.size ());
    for (std::size_t c = 0; c < in.size (); ++c)
    {
        const auto& result = o.inPlace ? src[c] : dst[c];
        out[c].assign (result.begin (), result.end ());
    }
    return out;
}

// Runs the processor over `in` (one Signal per channel). `initial` is applied at
// sample 0; each event is applied at its exact sample offset within its block.
std::vector<Signal> render (VocalDeVerbProcessor& proc, const std::vector<Signal>& in,
                            const Params& initial, const std::vector<Event>& events = {},
                            const Options& o = {})
{
    return o.f64 ? renderAs<double> (proc, in, initial, events, o)
                 : renderAs<float> (proc, in, initial, events, o);
}

Signal renderMono (const Signal& x, const Params& initial, const std::vector<Event>& events = {},
                   const Options& o = {})
{
    auto proc = makeProcessor ();
    return render (*proc, {x}, initial, events, o)[0];
}

// Integer delay d in [0, maxLag] minimizing sum (y[n] - x[n-d])^2 over [start, N).
int measureDelay (const Signal& x, const Signal& y, int maxLag, int start)
{
    const int n = static_cast<int> (x.size ());
    double best = 1e300;
    int bestD = 0;
    for (int d = 0; d <= maxLag; ++d)
    {
        double acc = 0.0;
        for (int i = std::max (start, d); i < n; ++i)
        {
            const double e = static_cast<double> (y[static_cast<std::size_t> (i)]) -
                             static_cast<double> (x[static_cast<std::size_t> (i - d)]);
            acc += e * e;
        }
        if (acc < best)
        {
            best = acc;
            bestD = d;
        }
    }
    return bestD;
}

// Max |y[i] - x[i - delay]| over [from, to). NaN in y propagates as +inf.
double maxDelayedError (const Signal& x, const Signal& y, int delay, int from, int to)
{
    double worst = 0.0;
    for (int i = std::max (from, delay); i < to; ++i)
    {
        const double e = std::fabs (static_cast<double> (y[static_cast<std::size_t> (i)]) -
                                    static_cast<double> (x[static_cast<std::size_t> (i - delay)]));
        worst = !isFiniteValue (e) ? std::numeric_limits<double>::infinity () : std::max (worst, e);
    }
    return worst;
}

double maxAbsDiff (const Signal& a, const Signal& b, int from, int to)
{
    return maxDelayedError (a, b, 0, from, to);
}

int failures = 0;

// `ok` must be written so NaN metrics evaluate false (e.g. `db <= limit`, not `!(db > limit)`).
void check (bool ok, const char* passText, const char* failText)
{
    if (ok)
        std::printf ("  PASS: %s\n", passText);
    else
    {
        std::printf ("  FAIL: %s\n", failText);
        ++failures;
    }
}

} // namespace

int main ()
{
    const int n = 48000;
    const auto x = makeNoise (n, 0.25f);
    const auto latency = static_cast<int> (makeProcessor ()->getLatencySamples ());

    // ---- null/identity + measured latency, across sample rates ----
    for (const double rate : {44100.0, 48000.0, 96000.0})
    {
        Options o;
        o.sampleRate = rate;
        const auto y = renderMono (x, kIdentity, {}, o);
        const int measured = measureDelay (x, y, 1200, kWarmup);

        double resid = 0.0;
        int count = 0;
        for (int i = measured + kWarmup; i < n; ++i)
        {
            const double e = static_cast<double> (y[static_cast<std::size_t> (i)]) -
                             static_cast<double> (x[static_cast<std::size_t> (i - measured)]);
            resid += e * e;
            ++count;
        }
        const double residRms = std::sqrt (resid / std::max (1, count));
        const double db = 20.0 * std::log10 (residRms / rms (x.data () + kWarmup, n - kWarmup) + 1e-300);

        // Baseline is about -139 dB (MSVC Release); -120 dB leaves headroom for other
        // compilers while still catching any real reconstruction regression.
        std::printf ("[null %.0f Hz] reported=%d measured=%d residual=%.1f dB\n", rate, latency,
                     measured, db);
        check (db <= -120.0, "reconstruction is clean at Amount=0", "reconstruction residual too high");
        check (measured == latency, "reported latency matches measured delay",
               "reported latency != measured delay");
    }

    // ---- steady bypass: output == input delayed by the reported latency ----
    {
        const auto y = renderMono (x, {{kParamBypass, 1.0}});
        const double worst = maxDelayedError (x, y, latency, 0, n);
        std::printf ("[bypass]  maxAbsErr(delayed)=%.3e\n", worst);
        check (worst <= 1e-6, "bypass output is the latency-delayed input",
               "bypass output not delayed by reported latency");
    }

    // ---- bypass toggles: engine keeps running, so identity holds across transitions ----
    // Starts bypassed, then a long bypass interval and rapid repeated toggles.
    {
        const std::vector<Event> toggles {{4096, kParamBypass, 0.0},  {8192, kParamBypass, 1.0},
                                          {16384, kParamBypass, 0.0}, {30000, kParamBypass, 1.0},
                                          {30500, kParamBypass, 0.0}, {30700, kParamBypass, 1.0},
                                          {31000, kParamBypass, 0.0}};
        auto initial = kIdentity;
        initial.back () = {kParamBypass, 1.0};

        for (const bool f64 : {false, true})
        {
            for (const bool inPlace : {false, true})
            {
                Options o;
                o.f64 = f64;
                o.inPlace = inPlace;
                const auto y = renderMono (x, initial, toggles, o);
                const double worst = maxDelayedError (x, y, latency, 0, n);
                std::printf ("[toggle %s %s] maxAbsErr(delayed)=%.3e\n", f64 ? "f64" : "f32",
                             inPlace ? "in-place" : "separate", worst);
                check (worst <= 1e-5, "bypass transitions stay aligned with the delayed input",
                       "bypass transition replays stale or misaligned audio");
            }
        }
    }

    // ---- gain automation is smoothed (no zipper) ----
    {
        const auto sine = makeSine (n, 0.25f, 200.0);
        // A hard 0 dB -> +12 dB step would jump the output by ~4x.
        const auto y = renderMono (sine, kIdentity, {{24000, kParamOutputGain, 1.0}});

        double maxStep = 0.0, steadySlew = 0.0;
        for (int i = 3000; i < n; ++i)
            maxStep = std::max (maxStep, static_cast<double> (std::fabs (
                                             y[static_cast<std::size_t> (i)] - y[static_cast<std::size_t> (i - 1)])));
        for (int i = 33000; i < 43000; ++i) // natural slew in the loud, settled region
            steadySlew = std::max (steadySlew, static_cast<double> (std::fabs (
                                                   y[static_cast<std::size_t> (i)] - y[static_cast<std::size_t> (i - 1)])));
        const double ratio = maxStep / (steadySlew + 1e-12);
        std::printf ("[zipper]  maxStep=%.4f steadySlew=%.4f ratio=%.1f\n", maxStep, steadySlew, ratio);
        check (ratio <= 4.0, "gain automation is smoothed", "output steps at the gain change");
    }

    // ---- automation is applied at its sample offset, not at block start ----
    {
        // 24063 is offset 511 of a 512-sample block.
        const auto reference = renderMono (x, kIdentity);
        const auto y = renderMono (x, kIdentity, {{24063, kParamOutputGain, 1.0}});
        const double early = maxAbsDiff (reference, y, 0, 24063);
        const double after = maxAbsDiff (reference, y, 24063, n);
        std::printf ("[timing]  diffBeforeEvent=%.3e diffAfter=%.3f\n", early, after);
        check (early == 0.0 && after > 0.1, "change takes effect exactly at its offset",
               "automation applied before its sample offset (or not at all)");
    }

    // ---- several points in one block render identically at any block size ----
    {
        const Params initial {{kParamAmount, 0.6},        {kParamSensitivity, 0.5}, {kParamTailRelease, 0.3},
                              {kParamVocalPreserve, 0.5}, {kParamDryWet, 0.9},      {kParamOutputGain, 0.5},
                              {kParamBypass, 0.0}};
        const std::vector<Event> events {
            {20000, kParamOutputGain, 0.8}, {20100, kParamOutputGain, 0.3}, {20200, kParamDryWet, 0.4},
            {20300, kParamAmount, 0.95},    {20400, kParamBypass, 1.0},     {21000, kParamBypass, 0.0},
            {21003, kParamSensitivity, 0.9}};

        const auto reference = renderMono (x, initial, events);
        double worst = 0.0;
        for (const int block : {1, 37, 100, 1024})
        {
            Options o;
            o.block = block;
            worst = std::max (worst, maxAbsDiff (reference, renderMono (x, initial, events, o), 0, n));
        }
        std::printf ("[blocks]  maxDiff across block sizes 1/37/100/1024 vs 512 = %.3e\n", worst);
        check (worst == 0.0, "output is independent of block size", "output depends on block size");
    }

    // ---- non-finite input is contained (no poisoned state) ----
    {
        auto noisy = makeNoise (n, 0.25f, 777u);
        for (int i = 10000; i < 10006; ++i)
            noisy[static_cast<std::size_t> (i)] = std::numeric_limits<float>::quiet_NaN ();
        for (int i = 10006; i < 10011; ++i)
            noisy[static_cast<std::size_t> (i)] = std::numeric_limits<float>::infinity ();

        auto params = kIdentity;
        params[0] = {kParamAmount, 0.55};
        params[4] = {kParamDryWet, 0.8};
        const auto y = renderMono (noisy, params);

        const bool allFinite =
            std::all_of (y.begin (), y.end (), [] (float v) { return isFiniteValue (v); });
        const double lateRms = rms (y.data () + 20000, 10000); // proves state recovered
        std::printf ("[robust]  allFinite=%s lateRMS=%.4f\n", allFinite ? "yes" : "no", lateRms);
        check (allFinite && lateRms >= 0.01, "non-finite input contained, output recovers",
               "non-finite output or state did not recover");
    }

    // ---- non-finite parameter values are sanitized ----
    {
        const auto nan = std::numeric_limits<double>::quiet_NaN ();
        const auto y = renderMono (x, kIdentity, {{10000, kParamOutputGain, nan}, {10000, kParamDryWet, nan}});
        const bool allFinite =
            std::all_of (y.begin (), y.end (), [] (float v) { return isFiniteValue (v); });
        std::printf ("[nanparam] allFinite=%s lateRMS=%.4f\n", allFinite ? "yes" : "no",
                     rms (y.data () + 20000, 10000));
        check (allFinite && rms (y.data () + 20000, 10000) > 0.01 && clampNormalized (nan) == 0.0,
               "NaN parameter values are clamped", "NaN parameter value leaks into processing");
    }

    // ---- stereo image stability (linked gain across channels) ----
    {
        const auto xL = makeNoise (n, 0.12f, 4242u);
        Signal xR (xL.size ());
        std::transform (xL.begin (), xL.end (), xR.begin (), [] (float v) { return 0.5f * v; });

        auto proc = makeProcessor ();
        const auto y = render (*proc, {xL, xR},
                               {{kParamAmount, 0.85},
                                {kParamSensitivity, 0.7},
                                {kParamTailRelease, 0.3},
                                {kParamVocalPreserve, 0.3},
                                {kParamDryWet, 1.0},
                                {kParamOutputGain, 0.5},
                                {kParamBypass, 0.0}});

        // Input keeps R = 0.5*L. Linked spectral gain keeps that ratio on output.
        double resid = 0.0, ref = 0.0;
        for (std::size_t i = 5000; i < y[0].size (); ++i)
        {
            const double e = static_cast<double> (y[1][i]) - 0.5 * static_cast<double> (y[0][i]);
            resid += e * e;
            ref += 0.25 * static_cast<double> (y[0][i]) * static_cast<double> (y[0][i]);
        }
        const double ratio = std::sqrt (resid) / (std::sqrt (ref) + 1e-12);
        std::printf ("[stereo]  imageDrift=%.1f%%\n", 100.0 * ratio);
        check (ratio <= 0.05, "stereo image preserved (channels share the gain)",
               "per-channel gains drift the stereo image");
    }

    // ---- the processor actually reduces reverb (DRR improves) ----
    {
        const int period = 8000, burstLen = 2000, gapStart = 3000, gapEnd = 7800;
        const auto reverberant = addReverb (makeBurstTrain (n, period, burstLen, 0.5f));
        const auto y = renderMono (reverberant, {{kParamAmount, 0.9},
                                                 {kParamSensitivity, 0.6},
                                                 {kParamTailRelease, 0.4},
                                                 {kParamVocalPreserve, 0.4},
                                                 {kParamDryWet, 1.0},
                                                 {kParamOutputGain, 0.5},
                                                 {kParamBypass, 0.0}});

        const double drrIn = measureDRR (reverberant, period, burstLen, gapStart, gapEnd, 0);
        const double drrOut = measureDRR (y, period, burstLen, gapStart, gapEnd, latency);
        const double improvementDb = 10.0 * std::log10 (drrOut / drrIn);
        std::printf ("[dereverb] DRR in=%.1f dB out=%.1f dB improvement=%.2f dB\n",
                     10.0 * std::log10 (drrIn), 10.0 * std::log10 (drrOut), improvementDb);
        check (improvementDb >= 0.5, "reverb tail reduced relative to direct",
               "processor does not measurably reduce reverb");
    }

    // ---- output silence flags never claim silence over generated audio ----
    {
        // 1024 samples of 0.2, then digital silence: the delayed tail is nonzero output
        // during silent input. The host pre-sets every output silence bit.
        Signal burst (6 * kBlock, 0.0f);
        std::fill (burst.begin (), burst.begin () + 1024, 0.2f);
        std::vector<uint64> flags;
        Options o;
        o.seedOutputSilence = ~uint64 {0};
        o.silenceFlags = &flags;
        const auto y = renderMono (burst, kIdentity, {}, o);

        bool stale = false;
        for (std::size_t b = 0; b < flags.size (); ++b)
            if (flags[b] != 0 && rms (y.data () + b * kBlock, kBlock) > 0.0)
                stale = true;
        std::printf ("[silence] tailBlockRMS=%.3f flags=%llx\n", rms (y.data () + 3 * kBlock, kBlock),
                     static_cast<unsigned long long> (flags[3]));
        check (!stale, "no silence bit survives on audible output",
               "stale output silence flag over nonzero audio");
    }

    // ---- state round trip (and NaN in a stored state is sanitized) ----
    {
        auto proc = makeProcessor ();
        render (*proc, {Signal (kBlock, 0.0f)},
                {{kParamAmount, 0.11}, {kParamDryWet, 0.37}, {kParamOutputGain, 0.73}, {kParamBypass, 1.0}});

        auto saved = owned (new MemoryStream ());
        const bool wrote = proc->getState (saved) == kResultTrue;

        auto restored = makeProcessor ();
        saved->seek (0, IBStream::kIBSeekSet, nullptr);
        const bool read = restored->setState (saved) == kResultTrue;
        auto again = owned (new MemoryStream ());
        restored->getState (again);

        const bool same = saved->getSize () == again->getSize () &&
                          std::memcmp (saved->getData (), again->getData (),
                                       static_cast<std::size_t> (saved->getSize ())) == 0;

        // Corrupt one stored value to NaN: header is 3 x uint32, then doubles.
        const auto nan = std::numeric_limits<double>::quiet_NaN ();
        std::memcpy (saved->getData () + 12, &nan, sizeof nan);
        saved->seek (0, IBStream::kIBSeekSet, nullptr);
        restored->setState (saved);
        auto sanitized = owned (new MemoryStream ());
        restored->getState (sanitized);
        double firstValue = 1.0;
        std::memcpy (&firstValue, sanitized->getData () + 12, sizeof firstValue);

        std::printf ("[state]   roundTrip=%s nanStored->%g\n", same ? "identical" : "differs", firstValue);
        check (wrote && read && same && firstValue == 0.0, "state round-trips and NaN is sanitized",
               "state round trip lost or corrupted values");
    }

    std::printf ("\n%s (%d failure(s))\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
