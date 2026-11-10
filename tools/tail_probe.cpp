// tail_probe -- what is still sounding 4 seconds after everything was released?
#include "../src/engine.h"
#include <cstdio>
#include <cmath>
#include <vector>

int main ()
{
	jm::Engine e;
	e.prepare (48000);
	e.setParam (jm::kDrive, 0.22f);
	e.setParam (jm::kDriveChar, 1.f);
	e.setParam (jm::kRevMix, 0.05f);
	e.setParam (jm::kSustain, 0.92f);
	e.setParam (jm::kChorus, 0.f);
	e.setParam (jm::kDelayMix, 0.f);

	e.keyswitch (jm::kKsFeedback, true);
	e.noteOn (52, 0.9f);
	std::vector<float> l (48000 * 6), r (l.size ());
	for (size_t i = 0; i < l.size (); i += 64) e.process (&l[i], &r[i], 64);

	e.keyswitch (jm::kKsFeedback, false);
	e.noteOff (52);
	for (size_t i = 0; i < l.size (); i += 64) e.process (&l[i], &r[i], 64);

	for (int w = 0; w < 12; ++w)
	{
		double a = 0;
		for (int i = 0; i < 24000; ++i) a += (double) l[w * 24000 + i] * l[w * 24000 + i];
		std::printf ("%.1fs rms %.6f  strings %.0f\n", w * 0.5, std::sqrt (a / 24000), e.activeStrings ());
	}
	return 0;
}
