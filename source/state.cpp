#include "state.h"

namespace Steinberg::Dereverb {
namespace {

constexpr uint32 kStateMagic = 0x44525642; // DRVB
constexpr uint32 kStateVersion = 1;

} // namespace

bool writeParameterState (IBStreamer& streamer, const ParameterValues& values)
{
	if (!streamer.writeInt32u (kStateMagic))
		return false;
	if (!streamer.writeInt32u (kStateVersion))
		return false;
	if (!streamer.writeInt32u (static_cast<uint32> (values.size ())))
		return false;

	for (auto value : values)
	{
		if (!streamer.writeDouble (clampNormalized (value)))
			return false;
	}
	return true;
}

bool readParameterState (IBStreamer& streamer, ParameterValues& values)
{
	uint32 magic = 0;
	uint32 version = 0;
	uint32 count = 0;

	if (!streamer.readInt32u (magic) || magic != kStateMagic)
		return false;
	if (!streamer.readInt32u (version) || version != kStateVersion)
		return false;
	if (!streamer.readInt32u (count) || count != values.size ())
		return false;

	ParameterValues incoming {};
	for (auto& value : incoming)
	{
		if (!streamer.readDouble (value))
			return false;
		value = clampNormalized (value);
	}

	values = incoming;
	return true;
}

} // namespace Steinberg::Dereverb

