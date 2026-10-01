#pragma once

#include "pluginterfaces/vst/vsttypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Steinberg::Dereverb {

using ParameterValues = std::array<Vst::ParamValue, 7>;

enum ParameterID : Vst::ParamID
{
	kParamAmount = 100,
	kParamSensitivity,
	kParamTailRelease,
	kParamVocalPreserve,
	kParamDryWet,
	kParamOutputGain,
	kParamBypass,
};

constexpr std::array<Vst::ParamID, 7> kParameterIDs {
	kParamAmount,
	kParamSensitivity,
	kParamTailRelease,
	kParamVocalPreserve,
	kParamDryWet,
	kParamOutputGain,
	kParamBypass,
};

constexpr Vst::ParamValue kDefaultAmount = 0.55;
constexpr Vst::ParamValue kDefaultSensitivity = 0.45;
constexpr Vst::ParamValue kDefaultTailRelease = 0.3214285714285714; // 220 ms in the 40-600 ms range.
constexpr Vst::ParamValue kDefaultVocalPreserve = 0.65;
constexpr Vst::ParamValue kDefaultDryWet = 0.80;
constexpr Vst::ParamValue kDefaultOutputGain = 0.50; // 0 dB in the -12 to +12 dB range.
constexpr Vst::ParamValue kDefaultBypass = 0.0;

inline ParameterValues makeDefaultParameterValues ()
{
	return {
		kDefaultAmount,
		kDefaultSensitivity,
		kDefaultTailRelease,
		kDefaultVocalPreserve,
		kDefaultDryWet,
		kDefaultOutputGain,
		kDefaultBypass,
	};
}

inline int parameterIndex (Vst::ParamID id)
{
	for (std::size_t i = 0; i < kParameterIDs.size (); ++i)
	{
		if (kParameterIDs[i] == id)
			return static_cast<int> (i);
	}
	return -1;
}

// Finite test on the bit pattern: the SDK builds with /fp:fast / -ffast-math, under
// which std::isfinite and NaN comparisons may be optimized away.
inline bool isFiniteValue (double value)
{
	std::uint64_t bits = 0;
	std::memcpy (&bits, &value, sizeof bits);
	return (bits & 0x7FF0000000000000ull) != 0x7FF0000000000000ull;
}

inline bool isFiniteValue (float value)
{
	std::uint32_t bits = 0;
	std::memcpy (&bits, &value, sizeof bits);
	return (bits & 0x7F800000u) != 0x7F800000u;
}

// Non-finite values (NaN, +/-Inf) are garbage, not intent: they map to 0.
inline Vst::ParamValue clampNormalized (Vst::ParamValue value)
{
	if (!isFiniteValue (value) || value < 0.0)
		return 0.0;
	if (value > 1.0)
		return 1.0;
	return value;
}

} // namespace Steinberg::Dereverb

