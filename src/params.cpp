// HanaGuitar for indie -- parameter construction.
//
// JmParam itself lives in params.h (the wrapper needs its type), so this file
// is only the container fill.
#include "params.h"

#include <cstdio>

namespace jm {

void initParameters (Steinberg::Vst::ParameterContainer& params)
{
	using namespace Steinberg::Vst;
	for (int i = 0; i < kNumDefs; ++i)
	{
		JmParam* p = new JmParam (kParamDefs[i]);
		params.addParameter (p);
	}
}

} // namespace jm
