#pragma once

#include "parameters.h"
#include "base/source/fstreamer.h"

namespace Steinberg::Dereverb {

bool writeParameterState (IBStreamer& streamer, const ParameterValues& values);
bool readParameterState (IBStreamer& streamer, ParameterValues& values);

} // namespace Steinberg::Dereverb

