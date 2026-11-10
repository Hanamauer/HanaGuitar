// Probe: is setRatio actually moving the loop period?
#include "../src/engine.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

int main ()
{
	jm::WaveString s;
	s.prepare (48000.f, 48000 / 20 + 128);
	s.setFrequency (329.63f, 0.1f, 0.4f);
	s.setDecay (6.8f, 0.4f);
	s.setRatio (1.f);
	s.pluck (0.8f, 0.13f, 0.6f, 0.55f, 0.f, *(new jm::Rng (1)));
	std::printf ("ratio 1.0 : baseFreq %.2f target %.3f smooth %.3f\n",
	             s.fundamental (), s.targetDelay (), s.smoothedDelay ());
	for (int i = 0; i < 4800; ++i) s.process ();
	std::printf ("  after 4800: target %.3f smooth %.3f\n", s.targetDelay (), s.smoothedDelay ());

	s.setRatio (0.8409f);   // -3 semitones
	std::printf ("ratio 0.84: target %.3f smooth %.3f\n", s.targetDelay (), s.smoothedDelay ());
	for (int i = 0; i < 4800; ++i) s.process ();
	std::printf ("  after 4800: target %.3f smooth %.3f  -> freq %.2f Hz\n",
	             s.targetDelay (), s.smoothedDelay (), 48000.f / (s.smoothedDelay () + 1.f));

	// Now the engine path, printing the string's period while the arm dives.
	jm::Engine e;
	e.prepare (48000.0);
	e.setParam (jm::kArmPos, jm::plainFromNorm (jm::kParamDefs[jm::kArmPos], 0.f));
	e.noteOn (64, 0.9f);
	std::vector<float> bl (512), br (512);
	auto run = [&] (int n) {
		int done = 0;
		while (done < n) { const int m = std::min (512, n - done); e.process (bl.data (), br.data (), m); done += m; }
	};
	run (2400);
	std::printf ("engine rest : bar %.3f st\n", e.barSemis ());
	e.setParam (jm::kArmPos, 1.0f);
	for (int k = 0; k < 8; ++k)
	{
		run (2400);
		std::printf ("  t=%4dms bar %.3f st\n", (k + 1) * 50, e.barSemis ());
	}
	return 0;
}
