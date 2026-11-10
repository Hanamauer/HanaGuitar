// perf_probe -- why tremolo/feedback tests disagree
#include "../src/engine.h"
#include <cstdio>
#include <cmath>
#include <vector>

int main ()
{
	auto run = [] (const char* name, bool trem, bool fb)
	{
		jm::Engine e;
		e.prepare (48000);
		e.setParam (jm::kDrive, 0.f);
		e.setParam (jm::kCab, 0.f);
		e.setParam (jm::kChorus, 0.f);
		e.setParam (jm::kDelayMix, 0.f);
		e.setParam (jm::kRevMix, 0.f);
		e.setParam (jm::kBody, 0.f);
		e.setParam (jm::kBehindBridge, 0.f);
		e.setParam (jm::kAge, 0.f);
		e.setParam (jm::kSustain, 0.9f);
		if (trem) e.keyswitch (jm::kKsTremPick, true);
		if (fb) e.keyswitch (jm::kKsFeedback, true);
		e.noteOn (52, 0.9f);

		std::vector<float> l (48000 * 2), r (l.size ());
		for (size_t i = 0; i < l.size (); i += 64) e.process (&l[i], &r[i], 64);

		std::printf ("%-16s mask=%d\n", name, e.techniqueMask ());
		for (int w = 0; w < 10; ++w)
		{
			double a = 0;
			const int from = w * 9600;
			for (int i = 0; i < 9600; ++i) a += (double) l[from + i] * l[from + i];
			std::printf ("  %.2fs rms %.5f\n", w * 0.2, std::sqrt (a / 9600));
		}
	};

	run ("plain", false, false);
	run ("trem", true, false);
	run ("feedback", false, true);
	return 0;
}
