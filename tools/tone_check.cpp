// tone_check -- sustain, body ring and attack after the 1.3 voicing changes
#include "../src/engine.h"
#include <cstdio>
#include <cmath>
#include <vector>

static double rms (const std::vector<float>& x, int from, int n)
{
	double a = 0;
	for (int i = 0; i < n; ++i) a += (double) x[from + i] * x[from + i];
	return std::sqrt (a / std::max (1, n));
}

static void run (const char* name, float body, float sustain, float bright)
{
	jm::Engine e;
	e.prepare (48000);
	e.setParam (jm::kDrive, 0.f);
	e.setParam (jm::kCab, 0.6f);
	e.setParam (jm::kChorus, 0.f);
	e.setParam (jm::kDelayMix, 0.f);
	e.setParam (jm::kRevMix, 0.f);
	e.setParam (jm::kBody, body);
	e.setParam (jm::kBehindBridge, 0.3f);
	e.setParam (jm::kAge, 0.15f);
	e.setParam (jm::kSustain, sustain);
	e.setParam (jm::kBrightness, bright);
	e.setParam (jm::kOutput, 0.f);

	e.noteOn (52, 0.9f);
	std::vector<float> l (48000 * 8), r (l.size ());
	for (int i = 0; i < 48000 * 4; i += 64) e.process (&l[i], &r[i], 64);
	e.noteOff (52);
	for (int i = 48000 * 4; i < 48000 * 8; i += 64) e.process (&l[i], &r[i], 64);

	std::printf ("%-22s attack %.4f  @1s %.5f  @3s %.5f  tail+0.5s %.5f  tail+1.5s %.6f\n",
	             name, rms (l, 0, 1200), rms (l, 48000, 4800), rms (l, 144000, 4800),
	             rms (l, 48000 * 4 + 24000, 4800), rms (l, 48000 * 4 + 72000, 4800));
}

int main ()
{
	run ("body off", 0.f, 0.65f, 0.66f);
	run ("body .68", 0.68f, 0.65f, 0.66f);
	run ("body .68 bright .8", 0.68f, 0.65f, 0.80f);
	run ("old sustain .55", 0.68f, 0.55f, 0.66f);
	return 0;
}
