#include "cids.h"
#include "controller.h"
#include "processor.h"
#include "version.h"

#include "public.sdk/source/main/pluginfactory.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

#define stringPluginName "Vocal DeVerb"

using namespace Steinberg;
using namespace Steinberg::Vst;
using namespace Steinberg::Dereverb;

BEGIN_FACTORY_DEF ("Local Audio",
                   "https://example.invalid",
                   "mailto:dev@example.invalid")

	DEF_CLASS2 (INLINE_UID_FROM_FUID (ProcessorUID),
	            PClassInfo::kManyInstances,
	            kVstAudioEffectClass,
	            stringPluginName,
	            Vst::kDistributable,
	            VocalDeVerbVST3Category,
	            FULL_VERSION_STR,
	            kVstVersionString,
	            VocalDeVerbProcessor::createInstance)

	DEF_CLASS2 (INLINE_UID_FROM_FUID (ControllerUID),
	            PClassInfo::kManyInstances,
	            kVstComponentControllerClass,
	            stringPluginName " Controller",
	            0,
	            "",
	            FULL_VERSION_STR,
	            kVstVersionString,
	            VocalDeVerbController::createInstance)

END_FACTORY

