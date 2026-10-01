#include "controller.h"

#include "state.h"
#include "base/source/fstreamer.h"
#include "public.sdk/source/vst/vstparameters.h"
#include "vstgui/plugin-bindings/vst3editor.h"

namespace Steinberg::Dereverb {

using namespace Steinberg::Vst;

tresult PLUGIN_API VocalDeVerbController::initialize (FUnknown* context)
{
	const auto result = EditController::initialize (context);
	if (result != kResultOk)
		return result;

	parameters.addParameter (new RangeParameter (STR ("Amount"), kParamAmount, STR ("%"), 0.0,
	                                            100.0, kDefaultAmount * 100.0, 0,
	                                            ParameterInfo::kCanAutomate));
	parameters.addParameter (new RangeParameter (STR ("Sensitivity"), kParamSensitivity, STR ("%"),
	                                            0.0, 100.0, kDefaultSensitivity * 100.0, 0,
	                                            ParameterInfo::kCanAutomate));
	parameters.addParameter (new RangeParameter (STR ("Tail Release"), kParamTailRelease, STR ("ms"),
	                                            40.0, 600.0, 220.0, 0,
	                                            ParameterInfo::kCanAutomate));
	parameters.addParameter (new RangeParameter (STR ("Vocal Preserve"), kParamVocalPreserve,
	                                            STR ("%"), 0.0, 100.0,
	                                            kDefaultVocalPreserve * 100.0, 0,
	                                            ParameterInfo::kCanAutomate));
	parameters.addParameter (new RangeParameter (STR ("Dry/Wet"), kParamDryWet, STR ("%"), 0.0,
	                                            100.0, kDefaultDryWet * 100.0, 0,
	                                            ParameterInfo::kCanAutomate));
	parameters.addParameter (new RangeParameter (STR ("Output Gain"), kParamOutputGain, STR ("dB"),
	                                            -12.0, 12.0, 0.0, 0,
	                                            ParameterInfo::kCanAutomate));
	parameters.addParameter (STR ("Bypass"), nullptr, 1, kDefaultBypass,
	                         ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass,
	                         kParamBypass);

	return kResultOk;
}

tresult PLUGIN_API VocalDeVerbController::setComponentState (IBStream* state)
{
	return setState (state);
}

tresult PLUGIN_API VocalDeVerbController::setState (IBStream* state)
{
	if (!state)
		return kInvalidArgument;

	IBStreamer streamer (state, kLittleEndian);
	auto values = makeDefaultParameterValues ();
	if (!readParameterState (streamer, values))
		return kResultFalse;

	applyState (values);
	return kResultTrue;
}

tresult PLUGIN_API VocalDeVerbController::getState (IBStream* state)
{
	if (!state)
		return kInvalidArgument;

	IBStreamer streamer (state, kLittleEndian);
	return writeParameterState (streamer, collectState ()) ? kResultTrue : kResultFalse;
}

IPlugView* PLUGIN_API VocalDeVerbController::createView (FIDString name)
{
	if (FIDStringsEqual (name, ViewType::kEditor))
		return new VSTGUI::VST3Editor (this, "view", "vocaldeverb.uidesc");
	return nullptr;
}

void VocalDeVerbController::applyState (const ParameterValues& values)
{
	for (std::size_t i = 0; i < kParameterIDs.size (); ++i)
	{
		if (auto* parameter = parameters.getParameter (kParameterIDs[i]))
			parameter->setNormalized (values[i]);
	}
}

ParameterValues VocalDeVerbController::collectState () const
{
	auto values = makeDefaultParameterValues ();
	for (std::size_t i = 0; i < kParameterIDs.size (); ++i)
	{
		if (auto* parameter = parameters.getParameter (kParameterIDs[i]))
			values[i] = parameter->getNormalized ();
	}
	return values;
}

FUnknown* VocalDeVerbController::createInstance (void*)
{
	return static_cast<IEditController*> (new VocalDeVerbController);
}

} // namespace Steinberg::Dereverb
