#include "processor.h"

#include "cids.h"
#include "state.h"
#include "base/source/fstreamer.h"
#include "public.sdk/source/vst/utility/audiobuffers.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE2__)
#include <pmmintrin.h>
#include <xmmintrin.h>
#define DEREVERB_HAS_SSE_DENORMAL_CONTROL 1
#endif

namespace Steinberg::Dereverb {
namespace {

constexpr int32 kMaxProcessChannels = 8;
constexpr float kPi = 3.14159265358979323846f;

// Generous runaway ceiling (~+24 dBFS). Well above any musical level — including the
// +12 dB Output Gain maximum — so it only catches pathological values, never normal
// headroom. Paired with finite-guarding to keep non-finite values out of the output.
constexpr float kOutputSafetyCeiling = 16.0f;

// Forces flush-to-zero / denormals-are-zero on the audio thread for the duration
// of one process() call, then restores the host's mode. The DSP is full of
// recursive one-pole filters that decay toward zero during silence and tails;
// without this, those subnormal values can trigger large per-sample CPU spikes.
struct ScopedNoDenormals
{
#if defined(DEREVERB_HAS_SSE_DENORMAL_CONTROL)
	unsigned int savedCsr;
	ScopedNoDenormals () : savedCsr (_mm_getcsr ())
	{
		// Flush-to-zero (bit 15) | denormals-are-zero (bit 6).
		_mm_setcsr (savedCsr | 0x8040u);
	}
	~ScopedNoDenormals () { _mm_setcsr (savedCsr); }
#else
	ScopedNoDenormals () = default;
#endif
};

float normalizedToReleaseSeconds (Vst::ParamValue value)
{
	return 0.040f + static_cast<float> (clampNormalized (value)) * 0.560f;
}

float normalizedToOutputGain (Vst::ParamValue value)
{
	const auto decibels = -12.0f + static_cast<float> (clampNormalized (value)) * 24.0f;
	return std::pow (10.0f, decibels / 20.0f);
}

} // namespace

using namespace Steinberg::Vst;

VocalDeVerbProcessor::VocalDeVerbProcessor ()
{
	setControllerClass (ControllerUID);
	updateSpectralWindow ();
	initTwiddleTable ();
}

tresult PLUGIN_API VocalDeVerbProcessor::initialize (FUnknown* context)
{
	const auto result = AudioEffect::initialize (context);
	if (result == kResultTrue)
	{
		addAudioInput (STR ("Input"), SpeakerArr::kStereo);
		addAudioOutput (STR ("Output"), SpeakerArr::kStereo);
	}
	return result;
}

tresult PLUGIN_API VocalDeVerbProcessor::setActive (TBool state)
{
	if (state)
		resetProcessingState ();
	return AudioEffect::setActive (state);
}

tresult PLUGIN_API VocalDeVerbProcessor::setupProcessing (ProcessSetup& setup)
{
	const auto result = AudioEffect::setupProcessing (setup);
	if (result == kResultTrue && setup.sampleRate > 0.0)
	{
		sampleRate_ = setup.sampleRate;
		resetProcessingState ();
	}
	return result;
}

tresult PLUGIN_API VocalDeVerbProcessor::setState (IBStream* state)
{
	if (!state)
		return kInvalidArgument;

	IBStreamer streamer (state, kLittleEndian);
	auto values = makeDefaultParameterValues ();
	if (!readParameterState (streamer, values))
		return kResultFalse;

	parameters_ = values;
	return kResultTrue;
}

tresult PLUGIN_API VocalDeVerbProcessor::getState (IBStream* state)
{
	if (!state)
		return kInvalidArgument;

	IBStreamer streamer (state, kLittleEndian);
	return writeParameterState (streamer, parameters_) ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API VocalDeVerbProcessor::setBusArrangements (SpeakerArrangement* inputs,
                                                             int32 numIns,
                                                             SpeakerArrangement* outputs,
                                                             int32 numOuts)
{
	if (numIns != 1 || numOuts != 1)
		return kResultFalse;

	const auto inputChannels = SpeakerArr::getChannelCount (inputs[0]);
	const auto outputChannels = SpeakerArr::getChannelCount (outputs[0]);
	if (inputChannels <= 0 || inputChannels > kMaxProcessChannels ||
	    inputChannels != outputChannels)
	{
		return kResultFalse;
	}

	getAudioInput (0)->setArrangement (inputs[0]);
	getAudioOutput (0)->setArrangement (outputs[0]);
	return kResultTrue;
}

tresult PLUGIN_API VocalDeVerbProcessor::canProcessSampleSize (int32 symbolicSampleSize)
{
	return symbolicSampleSize == SymbolicSampleSizes::kSample32 ||
	               symbolicSampleSize == SymbolicSampleSizes::kSample64 ?
	           kResultTrue :
	           kResultFalse;
}

uint32 PLUGIN_API VocalDeVerbProcessor::getLatencySamples ()
{
	return static_cast<uint32> (kLatencySamples);
}

tresult PLUGIN_API VocalDeVerbProcessor::process (ProcessData& data)
{
	const ScopedNoDenormals noDenormals;

	if (data.numSamples <= 0 || data.numInputs == 0 || data.numOutputs == 0 ||
	    data.inputs == nullptr || data.outputs == nullptr)
	{
		// Parameter flush (or no audio): nothing to time-align against.
		applyAllParameterChanges (data.inputParameterChanges);
		return kResultTrue;
	}

	if (data.symbolicSampleSize == SymbolicSampleSizes::kSample32)
		processBlock<SymbolicSampleSizes::kSample32> (data);
	else
		processBlock<SymbolicSampleSizes::kSample64> (data);

	return kResultTrue;
}

void VocalDeVerbProcessor::applyAllParameterChanges (IParameterChanges* changes)
{
	if (!changes)
		return;

	const auto parameterCount = changes->getParameterCount ();
	for (int32 i = 0; i < parameterCount; ++i)
	{
		auto* queue = changes->getParameterData (i);
		const auto index = queue ? parameterIndex (queue->getParameterId ()) : -1;
		if (index < 0)
			continue;

		int32 sampleOffset = 0;
		ParamValue value = 0.0;
		const auto last = queue->getPointCount () - 1;
		if (last >= 0 && queue->getPoint (last, sampleOffset, value) == kResultTrue)
			parameters_[static_cast<std::size_t> (index)] = clampNormalized (value);
	}
}

template <SymbolicSampleSizes SampleSize>
void VocalDeVerbProcessor::processBlock (ProcessData& data)
{
	// Automation points are applied at their sample offsets by splitting the block into
	// segments between points. Gain/mix/bypass ramp per sample from there; spectral
	// controls take effect on the next STFT frame completed after the point.
	std::array<IParamValueQueue*, kParameterIDs.size ()> queues {};
	std::array<int32, kParameterIDs.size ()> nextPoint {};
	if (auto* changes = data.inputParameterChanges)
	{
		const auto parameterCount = changes->getParameterCount ();
		for (int32 i = 0; i < parameterCount; ++i)
		{
			auto* queue = changes->getParameterData (i);
			const auto index = queue ? parameterIndex (queue->getParameterId ()) : -1;
			if (index >= 0)
				queues[static_cast<std::size_t> (index)] = queue;
		}
	}

	int32 position = 0;
	while (position < data.numSamples)
	{
		auto segmentEnd = data.numSamples;
		for (std::size_t p = 0; p < queues.size (); ++p)
		{
			auto* queue = queues[p];
			if (!queue)
				continue;
			for (; nextPoint[p] < queue->getPointCount (); ++nextPoint[p])
			{
				int32 offset = 0;
				ParamValue value = 0.0;
				if (queue->getPoint (nextPoint[p], offset, value) != kResultTrue)
					continue;
				if (offset > position)
				{
					segmentEnd = std::min (segmentEnd, offset);
					break;
				}
				parameters_[p] = clampNormalized (value);
			}
		}

		processAudio<SampleSize> (data, position, segmentEnd);
		position = segmentEnd;
	}

	// Points at or past the block end are out of spec; apply them rather than drop them.
	applyAllParameterChanges (data.inputParameterChanges);

	// Every output sample was just written, so no stale silence bit may survive.
	// ponytail: never reports silence; track all-zero output per channel if a host needs it.
	data.outputs[0].silenceFlags = 0;

	const auto outputBuffers = getChannelBuffers<SampleSize> (data.outputs[0]);
	const auto processedChannels =
	    std::min<int32> (std::min (data.inputs[0].numChannels, data.outputs[0].numChannels),
	                     kMaxProcessChannels);
	for (int32 channelIndex = processedChannels; channelIndex < data.outputs[0].numChannels;
	     ++channelIndex)
	{
		std::fill (outputBuffers[channelIndex], outputBuffers[channelIndex] + data.numSamples, 0);
	}
}

void VocalDeVerbProcessor::updateSpectralWindow ()
{
	for (std::size_t index = 0; index < spectralWindow_.size (); ++index)
	{
		const auto phase =
		    kPi * (static_cast<float> (index) + 0.5f) / static_cast<float> (kSpectralFftSize);
		spectralWindow_[index] = std::sin (phase);
	}
}

void VocalDeVerbProcessor::initTwiddleTable ()
{
	// W_N^j = exp(-2*pi*i*j/N). Computed in double, stored as float — accurate and
	// drift-free, unlike the per-stage float recurrence it replaces.
	constexpr double pi = 3.14159265358979323846;
	for (std::size_t j = 0; j < twiddleReal_.size (); ++j)
	{
		const double angle = -2.0 * pi * static_cast<double> (j) /
		                     static_cast<double> (kSpectralFftSize);
		twiddleReal_[j] = static_cast<float> (std::cos (angle));
		twiddleImag_[j] = static_cast<float> (std::sin (angle));
	}
}

void VocalDeVerbProcessor::resetProcessingState ()
{
	for (auto& channel : channels_)
	{
		channel.analysis.fill (0.0f);
		channel.overlap.fill (0.0f);
		channel.normalization.fill (0.0f);
		channel.real.fill (0.0f);
		channel.imag.fill (0.0f);
		channel.readyWet.fill (0.0f);
		channel.readyDry.fill (0.0f);
		channel.analysisFill = 0;
		channel.readyRead = 0;
		channel.readyCount = 0;
		channel.wpeWriteIndex = 0;
		channel.wpeFramesSeen = 0;
		channel.bypassDelay.fill (0.0);
		channel.bypassWrite = 0;

		for (auto& bin : channel.spectralBins)
		{
			for (auto& coefficient : bin.wpeTaps)
				coefficient = {};
		}

		for (auto& historyFrame : channel.wpeHistory)
		{
			for (auto& bin : historyFrame)
				bin = {};
		}
	}

	for (auto& bin : linkedBins_)
	{
		bin.fastMag = 0.0f;
		bin.lateMag = 0.0f;
		bin.mask = 1.0f;
	}

	smoothedDryWet_ =
	    static_cast<float> (parameters_[static_cast<std::size_t> (parameterIndex (kParamDryWet))]);
	smoothedOutputGain_ = normalizedToOutputGain (
	    parameters_[static_cast<std::size_t> (parameterIndex (kParamOutputGain))]);
	bypassMix_ =
	    parameters_[static_cast<std::size_t> (parameterIndex (kParamBypass))] >= 0.5 ? 1.0f : 0.0f;
}

template <SymbolicSampleSizes SampleSize>
void VocalDeVerbProcessor::processAudio (ProcessData& data, int32 start, int32 end)
{
	using SampleType =
	    std::conditional_t<SampleSize == SymbolicSampleSizes::kSample32, Sample32, Sample64>;

	auto& inputBus = data.inputs[0];
	auto& outputBus = data.outputs[0];
	const auto inputBuffers = getChannelBuffers<SampleSize> (inputBus);
	const auto outputBuffers = getChannelBuffers<SampleSize> (outputBus);
	const auto channels = std::min<int32> (
	    std::min (inputBus.numChannels, outputBus.numChannels), kMaxProcessChannels);

	const auto amount =
	    static_cast<float> (parameters_[static_cast<std::size_t> (parameterIndex (kParamAmount))]);
	const auto sensitivity = static_cast<float> (
	    parameters_[static_cast<std::size_t> (parameterIndex (kParamSensitivity))]);
	const auto releaseSeconds = normalizedToReleaseSeconds (
	    parameters_[static_cast<std::size_t> (parameterIndex (kParamTailRelease))]);
	const auto vocalPreserve = static_cast<float> (
	    parameters_[static_cast<std::size_t> (parameterIndex (kParamVocalPreserve))]);
	const auto dryWet =
	    static_cast<float> (parameters_[static_cast<std::size_t> (parameterIndex (kParamDryWet))]);
	const auto outputGain = normalizedToOutputGain (
	    parameters_[static_cast<std::size_t> (parameterIndex (kParamOutputGain))]);

	const auto frameRate = sampleRate_ / static_cast<double> (kSpectralHopSize);
	const auto spectralThresholdDb = -74.0f + sensitivity * 30.0f;
	SpectralSettings spectralSettings {};
	spectralSettings.amount = std::pow (amount, 0.62f);
	spectralSettings.sensitivity = sensitivity;
	spectralSettings.vocalPreserve = vocalPreserve;
	spectralSettings.fastAttack = coefficientForTime (0.018f, frameRate);
	spectralSettings.fastRelease =
	    coefficientForTime (0.070f + vocalPreserve * 0.055f, frameRate);
	spectralSettings.lateAttack =
	    coefficientForTime (0.070f + vocalPreserve * 0.080f, frameRate);
	spectralSettings.lateRelease =
	    coefficientForTime (0.180f + releaseSeconds * 1.65f, frameRate);
	spectralSettings.maskAttack =
	    coefficientForTime (0.018f + vocalPreserve * 0.018f, frameRate);
	spectralSettings.maskRelease =
	    coefficientForTime (0.070f + releaseSeconds * 0.72f, frameRate);
	spectralSettings.noiseFloor =
	    std::pow (10.0f, spectralThresholdDb / 20.0f) *
	    static_cast<float> (kSpectralFftSize) * 0.25f;
	spectralSettings.wpeAmount = std::clamp (
	    std::pow (amount, 0.68f) * (0.70f + sensitivity * 0.55f), 0.0f, 1.25f);
	spectralSettings.wpeLearnRate =
	    0.0035f + 0.018f * sensitivity * (1.0f - vocalPreserve * 0.55f);
	spectralSettings.wpeLeak =
	    coefficientForTime (1.10f + releaseSeconds * 4.0f, frameRate);

	// The mix-stage multipliers are applied per sample, so step changes from block
	// to block would zipper. Ramp each one toward its block target with a ~10 ms
	// one-pole, advanced once per sample and shared by every channel.
	const auto paramSmoothCoeff = coefficientForTime (0.010f, sampleRate_);
	const auto dryWetTarget = dryWet;
	const auto outputGainTarget = outputGain;
	auto smoothedDryWet = smoothedDryWet_;
	auto smoothedOutputGain = smoothedOutputGain_;

	// Bypass crossfades linearly over ~10 ms between the wet path and the latency-matched
	// dry delay line. The spectral engine keeps running while bypassed, so leaving bypass
	// resumes on current audio instead of replaying stale frames. The ramp lands exactly
	// on 0 or 1, so fully bypassed output stays bit-exact.
	const auto bypassTarget =
	    parameters_[static_cast<std::size_t> (parameterIndex (kParamBypass))] >= 0.5 ? 1.0f : 0.0f;
	const auto bypassStep = static_cast<float> (1.0 / (0.010 * std::max (sampleRate_, 1.0)));
	auto bypassMix = bypassMix_;
	std::array<double, kMaxProcessChannels> bypassed {};

	// Channel-inner loop: all channels advance in lockstep so their spectral frames
	// complete together and can share one stereo-linked mask (processLinkedFrame).
	for (int32 sampleIndex = start; sampleIndex < end; ++sampleIndex)
	{
		bypassMix = bypassMix < bypassTarget ? std::min (bypassTarget, bypassMix + bypassStep)
		                                     : std::max (bypassTarget, bypassMix - bypassStep);
		smoothedDryWet = paramSmoothCoeff * smoothedDryWet + (1.0f - paramSmoothCoeff) * dryWetTarget;
		smoothedOutputGain =
		    paramSmoothCoeff * smoothedOutputGain + (1.0f - paramSmoothCoeff) * outputGainTarget;

		// Sanitize each input before it reaches the recursive FFT/WPE/envelope state:
		// a single NaN or Inf would otherwise poison that state permanently. Every input
		// for this sample is read before any output is written, so in-place is safe.
		for (int32 channelIndex = 0; channelIndex < channels; ++channelIndex)
		{
			auto& state = channels_[static_cast<std::size_t> (channelIndex)];
			const auto rawInput = inputBuffers[channelIndex][sampleIndex];
			bypassed[static_cast<std::size_t> (channelIndex)] = state.bypassDelay[state.bypassWrite];
			state.bypassDelay[state.bypassWrite] = static_cast<double> (rawInput);
			if (++state.bypassWrite >= kLatencySamples)
				state.bypassWrite = 0;

			const auto input = static_cast<float> (rawInput);
			pushAnalysis (state, isFiniteValue (input) ? input : 0.0f);
		}

		// Frames fill in lockstep, so channel 0 is representative.
		if (channels > 0 && channels_[0].analysisFill == kSpectralFftSize)
		{
			processLinkedFrame (channels, spectralSettings);
			for (int32 channelIndex = 0; channelIndex < channels; ++channelIndex)
				finalizeFrameOutput (channels_[static_cast<std::size_t> (channelIndex)]);
		}

		for (int32 channelIndex = 0; channelIndex < channels; ++channelIndex)
		{
			auto& state = channels_[static_cast<std::size_t> (channelIndex)];
			float delayedDry = 0.0f;
			const auto wet = popReady (state, delayedDry);

			const auto mixed = delayedDry + smoothedDryWet * (wet - delayedDry);
			const auto outSample = mixed * smoothedOutputGain;
			const auto processed =
			    isFiniteValue (outSample)
			        ? std::clamp (outSample, -kOutputSafetyCeiling, kOutputSafetyCeiling)
			        : 0.0f;
			const auto dry = bypassed[static_cast<std::size_t> (channelIndex)];

			auto& output = outputBuffers[channelIndex][sampleIndex];
			if (bypassMix >= 1.0f)
				output = static_cast<SampleType> (dry);
			else if (bypassMix <= 0.0f)
				output = static_cast<SampleType> (processed);
			else
				output = static_cast<SampleType> (
				    processed + bypassMix * ((isFiniteValue (dry) ? dry : 0.0) - processed));
		}
	}

	smoothedDryWet_ = smoothedDryWet;
	smoothedOutputGain_ = smoothedOutputGain;
	bypassMix_ = bypassMix;
}

void VocalDeVerbProcessor::pushAnalysis (ChannelState& state, float sample)
{
	state.analysis[state.analysisFill] = sample;
	++state.analysisFill;
}

void VocalDeVerbProcessor::finalizeFrameOutput (ChannelState& state)
{
	for (std::size_t index = 0; index < kSpectralHopSize; ++index)
	{
		const auto normalization = state.normalization[index];
		state.readyWet[index] =
		    normalization > 1.0e-7f ? state.overlap[index] / normalization : state.analysis[index];
		state.readyDry[index] = state.analysis[index];
	}
	state.readyRead = 0;
	state.readyCount = kSpectralHopSize;

	std::move (state.overlap.begin () + kSpectralHopSize, state.overlap.end (),
	           state.overlap.begin ());
	std::fill (state.overlap.end () - kSpectralHopSize, state.overlap.end (), 0.0f);
	std::move (state.normalization.begin () + kSpectralHopSize, state.normalization.end (),
	           state.normalization.begin ());
	std::fill (state.normalization.end () - kSpectralHopSize, state.normalization.end (), 0.0f);
	std::move (state.analysis.begin () + kSpectralHopSize, state.analysis.end (),
	           state.analysis.begin ());
	state.analysisFill -= kSpectralHopSize;
}

float VocalDeVerbProcessor::popReady (ChannelState& state, float& delayedDry)
{
	if (state.readyCount == 0)
	{
		delayedDry = 0.0f;
		return 0.0f;
	}

	const auto readIndex = state.readyRead;
	const auto wet = state.readyWet[readIndex];
	delayedDry = state.readyDry[readIndex];
	state.readyRead = (state.readyRead + 1) % kSpectralHopSize;
	--state.readyCount;
	return wet;
}

void VocalDeVerbProcessor::processLinkedFrame (int32 channels, const SpectralSettings& settings)
{
	const auto nyquistBin = kSpectralFftSize / 2;
	const auto binHz = static_cast<float> (sampleRate_) / static_cast<float> (kSpectralFftSize);

	// Forward transform every channel into its own real/imag scratch.
	for (int32 ch = 0; ch < channels; ++ch)
	{
		auto& state = channels_[static_cast<std::size_t> (ch)];
		for (std::size_t index = 0; index < kSpectralFftSize; ++index)
		{
			state.real[index] = state.analysis[index] * spectralWindow_[index];
			state.imag[index] = 0.0f;
		}
		fft (state.real, state.imag, false);
	}

	for (std::size_t bin = 0; bin <= nyquistBin; ++bin)
	{
		// The mask is driven by the linked magnitude (max across channels) and then
		// applied identically to every channel, so the stereo image does not move as
		// the mask opens and closes. The WPE prediction stays per-channel.
		float linkedMag = 0.0f;
		for (int32 ch = 0; ch < channels; ++ch)
		{
			auto& state = channels_[static_cast<std::size_t> (ch)];
			const auto re = state.real[bin];
			const auto im = state.imag[bin];
			linkedMag = std::max (linkedMag, std::sqrt (re * re + im * im));
		}

		auto& linked = linkedBins_[bin];
		const auto previousFast = linked.fastMag;
		const auto fastCoeff =
		    linkedMag > linked.fastMag ? settings.fastAttack : settings.fastRelease;
		linked.fastMag = fastCoeff * linked.fastMag + (1.0f - fastCoeff) * linkedMag;

		const auto attackPresence = std::clamp (
		    (linkedMag - previousFast * (1.12f + settings.vocalPreserve * 0.45f)) /
		        (linkedMag + settings.noiseFloor + 1.0e-8f),
		    0.0f, 1.0f);
		const auto learnGate =
		    1.0f - attackPresence * (0.78f + settings.vocalPreserve * 0.20f);
		const auto lateTarget = linkedMag * std::clamp (learnGate, 0.0f, 1.0f);
		const auto lateCoeff =
		    lateTarget > linked.lateMag ? settings.lateAttack : settings.lateRelease;
		linked.lateMag = lateCoeff * linked.lateMag + (1.0f - lateCoeff) * lateTarget;

		const auto frequency = static_cast<float> (bin) * binHz;
		auto frequencyWeight = 1.0f;
		if (frequency < 120.0f)
			frequencyWeight = 0.18f;
		else if (frequency < 320.0f)
			frequencyWeight = 0.18f + (frequency - 120.0f) / 200.0f * 0.62f;
		else if (frequency > 9800.0f)
			frequencyWeight = std::max (0.42f, 1.0f - (frequency - 9800.0f) / 7600.0f);

		const auto tailRatio =
		    linked.lateMag / (linkedMag + settings.noiseFloor + 1.0e-8f);
		const auto confidenceThreshold =
		    0.16f + settings.vocalPreserve * 0.28f - settings.sensitivity * 0.10f;
		const auto confidence = std::clamp (
		    (tailRatio - confidenceThreshold) * (1.55f + settings.sensitivity * 2.15f),
		    0.0f, 1.0f);
		const auto overSubtract =
		    settings.amount * (0.82f + settings.sensitivity * 1.45f) * confidence *
		    frequencyWeight;
		const auto predictorGate =
		    confidence * (1.0f - attackPresence * (0.70f + settings.vocalPreserve * 0.25f));
		const auto wpeSubtract =
		    std::clamp (settings.wpeAmount * predictorGate * frequencyWeight, 0.0f, 1.0f);

		const auto minimumGain = 0.035f + settings.vocalPreserve * 0.42f;
		auto targetGain =
		    (linkedMag - linked.lateMag * overSubtract) / (linkedMag + 1.0e-8f);
		targetGain = std::clamp (targetGain, minimumGain, 1.0f);
		targetGain += (1.0f - targetGain) * attackPresence *
		              (0.35f + settings.vocalPreserve * 0.50f);

		const auto maskCoeff =
		    targetGain < linked.mask ? settings.maskAttack : settings.maskRelease;
		linked.mask = maskCoeff * linked.mask + (1.0f - maskCoeff) * targetGain;
		const auto mask = std::clamp (linked.mask, minimumGain, 1.0f);

		for (int32 ch = 0; ch < channels; ++ch)
		{
			auto& state = channels_[static_cast<std::size_t> (ch)];
			auto& binState = state.spectralBins[bin];
			const auto wetReal = state.real[bin];
			const auto wetImag = state.imag[bin];
			const auto magnitude = std::sqrt (wetReal * wetReal + wetImag * wetImag);

			auto predictedReal = 0.0f;
			auto predictedImag = 0.0f;
			auto historyEnergy = settings.noiseFloor * settings.noiseFloor + 1.0e-8f;
			const auto activeTaps = state.wpeFramesSeen > kWpeDelayFrames ?
			                            std::min (kWpeTapCount,
			                                      state.wpeFramesSeen - kWpeDelayFrames) :
			                            std::size_t {0};
			for (std::size_t tap = 0; tap < activeTaps; ++tap)
			{
				const auto historyIndex =
				    (state.wpeWriteIndex + kWpeHistorySize - kWpeDelayFrames - tap) %
				    kWpeHistorySize;
				const auto& history = state.wpeHistory[historyIndex][bin];
				const auto& coefficient = binState.wpeTaps[tap];
				predictedReal +=
				    coefficient.real * history.real - coefficient.imag * history.imag;
				predictedImag +=
				    coefficient.real * history.imag + coefficient.imag * history.real;
				historyEnergy += history.real * history.real + history.imag * history.imag;
			}

			const auto predictionMagnitude =
			    std::sqrt (predictedReal * predictedReal + predictedImag * predictedImag);
			const auto predictionLimit =
			    magnitude * (0.28f + settings.wpeAmount * 0.58f) * frequencyWeight;
			if (predictionMagnitude > predictionLimit && predictionMagnitude > 1.0e-8f)
			{
				const auto predictionScale = predictionLimit / predictionMagnitude;
				predictedReal *= predictionScale;
				predictedImag *= predictionScale;
			}

			const auto wpeReal = wetReal - predictedReal * wpeSubtract;
			const auto wpeImag = wetImag - predictedImag * wpeSubtract;

			for (auto& coefficient : binState.wpeTaps)
			{
				coefficient.real *= settings.wpeLeak;
				coefficient.imag *= settings.wpeLeak;
			}

			if (activeTaps > 0)
			{
				const auto predictionErrorReal = wetReal - predictedReal;
				const auto predictionErrorImag = wetImag - predictedImag;
				const auto learning =
				    settings.wpeLearnRate * predictorGate / std::max (historyEnergy, 1.0e-8f);

				for (std::size_t tap = 0; tap < activeTaps; ++tap)
				{
					const auto historyIndex =
					    (state.wpeWriteIndex + kWpeHistorySize - kWpeDelayFrames - tap) %
					    kWpeHistorySize;
					const auto& history = state.wpeHistory[historyIndex][bin];
					auto& coefficient = binState.wpeTaps[tap];

					coefficient.real += learning *
					                       (predictionErrorReal * history.real +
					                        predictionErrorImag * history.imag);
					coefficient.imag += learning *
					                       (predictionErrorImag * history.real -
					                        predictionErrorReal * history.imag);
				}
			}

			state.real[bin] = wpeReal * mask;
			state.imag[bin] = wpeImag * mask;
			state.wpeHistory[state.wpeWriteIndex][bin] = {wetReal, wetImag};
			if (bin != 0 && bin != nyquistBin)
			{
				const auto mirror = kSpectralFftSize - bin;
				state.real[mirror] = state.real[bin];
				state.imag[mirror] = -state.imag[bin];
			}
			else
			{
				state.imag[bin] = 0.0f;
			}
		}
	}

	// Advance each channel's WPE ring, then inverse transform and overlap-add.
	for (int32 ch = 0; ch < channels; ++ch)
	{
		auto& state = channels_[static_cast<std::size_t> (ch)];
		state.wpeWriteIndex = (state.wpeWriteIndex + 1) % kWpeHistorySize;
		state.wpeFramesSeen = std::min (state.wpeFramesSeen + 1, kWpeHistorySize);

		fft (state.real, state.imag, true);

		for (std::size_t index = 0; index < kSpectralFftSize; ++index)
		{
			const auto window = spectralWindow_[index];
			state.overlap[index] += state.real[index] * window;
			state.normalization[index] += window * window;
		}
	}
}

void VocalDeVerbProcessor::fft (SpectralFrame& real, SpectralFrame& imag, bool inverse)
{
	for (std::size_t index = 1, reverse = 0; index < kSpectralFftSize; ++index)
	{
		std::size_t bit = kSpectralFftSize >> 1;
		for (; reverse & bit; bit >>= 1)
			reverse ^= bit;
		reverse ^= bit;

		if (index < reverse)
		{
			std::swap (real[index], real[reverse]);
			std::swap (imag[index], imag[reverse]);
		}
	}

	// Twiddles come from a table computed once in double (see initTwiddleTable),
	// instead of a per-stage float recurrence. That removes the rotation drift that
	// used to cap the noise floor, and keeps trig out of the hot loop.
	for (std::size_t length = 2; length <= kSpectralFftSize; length <<= 1)
	{
		const auto halfLength = length / 2;
		const auto stride = kSpectralFftSize / length;

		for (std::size_t offset = 0; offset < kSpectralFftSize; offset += length)
		{
			for (std::size_t butterfly = 0; butterfly < halfLength; ++butterfly)
			{
				const auto twiddle = butterfly * stride;
				const auto phaseReal = twiddleReal_[twiddle];
				const auto phaseImag = inverse ? -twiddleImag_[twiddle] : twiddleImag_[twiddle];

				const auto even = offset + butterfly;
				const auto odd = even + halfLength;
				const auto oddReal = real[odd] * phaseReal - imag[odd] * phaseImag;
				const auto oddImag = real[odd] * phaseImag + imag[odd] * phaseReal;

				real[odd] = real[even] - oddReal;
				imag[odd] = imag[even] - oddImag;
				real[even] += oddReal;
				imag[even] += oddImag;
			}
		}
	}

	if (inverse)
	{
		const auto scale = 1.0f / static_cast<float> (kSpectralFftSize);
		for (std::size_t index = 0; index < kSpectralFftSize; ++index)
		{
			real[index] *= scale;
			imag[index] *= scale;
		}
	}
}

float VocalDeVerbProcessor::coefficientForTime (float seconds, double sampleRate)
{
	const auto safeSeconds = std::max (seconds, 0.0001f);
	const auto safeSampleRate = std::max (sampleRate, 1.0);
	return std::exp (-1.0f / (safeSeconds * static_cast<float> (safeSampleRate)));
}

FUnknown* VocalDeVerbProcessor::createInstance (void*)
{
	return static_cast<IAudioProcessor*> (new VocalDeVerbProcessor);
}

} // namespace Steinberg::Dereverb
