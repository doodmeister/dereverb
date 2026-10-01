#pragma once

#include "parameters.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

namespace Steinberg::Dereverb {

class VocalDeVerbController : public Vst::EditController
{
public:
	tresult PLUGIN_API initialize (FUnknown* context) SMTG_OVERRIDE;
	tresult PLUGIN_API setComponentState (IBStream* state) SMTG_OVERRIDE;
	tresult PLUGIN_API setState (IBStream* state) SMTG_OVERRIDE;
	tresult PLUGIN_API getState (IBStream* state) SMTG_OVERRIDE;
	IPlugView* PLUGIN_API createView (FIDString name) SMTG_OVERRIDE;

	static FUnknown* createInstance (void*);

private:
	void applyState (const ParameterValues& values);
	ParameterValues collectState () const;
};

} // namespace Steinberg::Dereverb
