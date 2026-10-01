#pragma once

#include "parameters.h"
#include "public.sdk/source/vst/vstaudioeffect.h"

#include <array>
#include <cstddef>

namespace Steinberg::Dereverb {

class VocalDeVerbProcessor : public Vst::AudioEffect
{
public:
	VocalDeVerbProcessor ();

	tresult PLUGIN_API initialize (FUnknown* context) SMTG_OVERRIDE;
	tresult PLUGIN_API setActive (TBool state) SMTG_OVERRIDE;
	tresult PLUGIN_API setupProcessing (Vst::ProcessSetup& setup) SMTG_OVERRIDE;
	tresult PLUGIN_API setState (IBStream* state) SMTG_OVERRIDE;
	tresult PLUGIN_API getState (IBStream* state) SMTG_OVERRIDE;
	tresult PLUGIN_API setBusArrangements (Vst::SpeakerArrangement* inputs, int32 numIns,
	                                       Vst::SpeakerArrangement* outputs,
	                                       int32 numOuts) SMTG_OVERRIDE;
	tresult PLUGIN_API canProcessSampleSize (int32 symbolicSampleSize) SMTG_OVERRIDE;
	uint32 PLUGIN_API getLatencySamples () SMTG_OVERRIDE;
	tresult PLUGIN_API process (Vst::ProcessData& data) SMTG_OVERRIDE;

	static FUnknown* createInstance (void*);

private:
	static constexpr std::size_t kSpectralFftSize = 1024;
	static constexpr std::size_t kSpectralHopSize = 256;
	static constexpr std::size_t kSpectralBinCount = kSpectralFftSize / 2 + 1;
	static constexpr std::size_t kWpeTapCount = 16;
	static constexpr std::size_t kWpeDelayFrames = 4;
	static constexpr std::size_t kWpeHistorySize = kWpeTapCount + kWpeDelayFrames;

	// Reported processing latency. The overlap-add queue emits a frame's first hop
	// on the same sample that fills the analysis buffer, so the true algorithmic
	// delay is one sample short of the FFT size (verified by the null test in
	// tests/dsp_tests.cpp). The bypass delay line uses the same value so soft bypass
	// stays latency-compensated.
	static constexpr std::size_t kLatencySamples = kSpectralFftSize - 1;

	using SpectralFrame = std::array<float, kSpectralFftSize>;
	using SpectralHop = std::array<float, kSpectralHopSize>;

	struct ComplexValue
	{
		float real {0.0f};
		float imag {0.0f};
	};

	// Per-channel WPE predictor coefficients. The mask/magnitude tracking that used
	// to live here is now shared across channels in LinkedBinState.
	struct SpectralBinState
	{
		std::array<ComplexValue, kWpeTapCount> wpeTaps {};
	};

	// Spectral mask state shared across channels, so one gain is applied to all of
	// them (stereo-linked). Driven by the linked magnitude (max across channels),
	// which keeps the stereo image from wandering as the mask opens and closes.
	struct LinkedBinState
	{
		float fastMag {0.0f};
		float lateMag {0.0f};
		float mask {1.0f};
	};

	struct ChannelState
	{
		SpectralFrame analysis {};
		SpectralFrame overlap {};
		SpectralFrame normalization {};
		SpectralFrame real {};
		SpectralFrame imag {};
		SpectralHop readyWet {};
		SpectralHop readyDry {};
		std::array<SpectralBinState, kSpectralBinCount> spectralBins {};
		std::array<std::array<ComplexValue, kSpectralBinCount>, kWpeHistorySize> wpeHistory {};
		std::size_t analysisFill {0};
		std::size_t readyRead {0};
		std::size_t readyCount {0};
		std::size_t wpeWriteIndex {0};
		std::size_t wpeFramesSeen {0};
		// Dry delay line for soft bypass, matched to kLatencySamples. Stored as
		// double so 64-bit bypass stays bit-transparent.
		std::array<double, kLatencySamples> bypassDelay {};
		std::size_t bypassWrite {0};
	};

	struct SpectralSettings
	{
		float amount {0.0f};
		float sensitivity {0.0f};
		float vocalPreserve {0.0f};
		float fastAttack {0.0f};
		float fastRelease {0.0f};
		float lateAttack {0.0f};
		float lateRelease {0.0f};
		float maskAttack {0.0f};
		float maskRelease {0.0f};
		float noiseFloor {0.0f};
		float wpeAmount {0.0f};
		float wpeLearnRate {0.0f};
		float wpeLeak {1.0f};
	};

	// Applies the last point of every queue (used when there is no audio to align to).
	void applyAllParameterChanges (Vst::IParameterChanges* changes);
	void updateSpectralWindow ();
	void initTwiddleTable ();
	void resetProcessingState ();

	// Splits the block at automation points and renders each segment.
	template <Vst::SymbolicSampleSizes SampleSize>
	void processBlock (Vst::ProcessData& data);

	// Renders samples [start, end) of the block with the current parameters.
	template <Vst::SymbolicSampleSizes SampleSize>
	void processAudio (Vst::ProcessData& data, int32 start, int32 end);

	void pushAnalysis (ChannelState& state, float sample);
	void finalizeFrameOutput (ChannelState& state);
	float popReady (ChannelState& state, float& delayedDry);
	void processLinkedFrame (int32 channels, const SpectralSettings& settings);

	void fft (SpectralFrame& real, SpectralFrame& imag, bool inverse);

	static float coefficientForTime (float seconds, double sampleRate);

	ParameterValues parameters_ {makeDefaultParameterValues ()};
	std::array<ChannelState, 8> channels_ {};
	std::array<LinkedBinState, kSpectralBinCount> linkedBins_ {};
	SpectralFrame spectralWindow_ {};
	std::array<float, kSpectralFftSize / 2> twiddleReal_ {};
	std::array<float, kSpectralFftSize / 2> twiddleImag_ {};
	double sampleRate_ {44100.0};

	// Per-sample smoothed values for the direct output-stage multipliers, to remove
	// zipper noise when these parameters are automated. Re-seeded from the current
	// parameter values on reset so there is no start-up ramp.
	float smoothedDryWet_ {static_cast<float> (kDefaultDryWet)};
	float smoothedOutputGain_ {1.0f};
	// Bypass crossfade position: 0 = processed, 1 = latency-matched dry.
	float bypassMix_ {0.0f};
};

} // namespace Steinberg::Dereverb
