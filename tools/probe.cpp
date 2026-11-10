// quick compile probe for the DSP core
#include "../src/engine.h"
#include <cstdio>

int main ()
{
	jm::Engine e;
	e.prepare (48000.0);
	float l[64], r[64];
	e.noteOn (52, 0.8f);
	for (int b = 0; b < 40; ++b) e.process (l, r, 64);
	std::printf ("ok peak=%f strings=%.0f\n", e.takePeak (), e.activeStrings ());
	return 0;
}
