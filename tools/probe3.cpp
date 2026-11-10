#include "../src/dsp/string.h"
#include "../src/engine.h"
#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>

// autocorrelation period estimate over the plausible range
static double measurePeriod (const std::vector<float>& x, double sr, double fHint)
{
	const int n = (int) x.size ();
	const int lo = (int) (sr / (fHint * 1.5));
	const int hi = (int) (sr / (fHint * 0.7));
	double best = 0, bestD = -1e30;
	for (int d = lo; d <= hi && d < n / 2; ++d)
	{
		double s = 0, e0 = 0, e1 = 0;
		for (int i = 0; i < n - d; ++i) { s += x[i] * x[i + d]; e0 += x[i] * x[i]; e1 += x[i + d] * x[i + d]; }
		const double r = s / std::sqrt (std::max (1e-20, e0 * e1));
		if (r > bestD) { bestD = r; best = d; }
	}
	return best;
}

int main ()
{
	for (float f : { 82.41f, 110.f, 146.83f, 196.f, 246.94f, 329.63f })
	{
		jm::WaveString s;
		s.prepare (48000.f, 48000 / 20 + 128);
		s.setFrequency (f, 0.f, 1.f);
		s.setDecay (8.f, 1.f);
		s.setRatio (1.f);
		jm::Rng rng;
		s.pluck (0.9f, 0.13f, 0.5f, 0.7f, 0.f, rng);

		std::vector<float> out;
		for (int i = 0; i < 48000; ++i) out.push_back (s.process ());
		const double period = measurePeriod (out, 48000.0, f);
		const double got = 48000.0 / period;
		std::printf ("f=%8.2f  target=%8.3f  period=%8.2f  got=%8.2f  cents=%+7.2f\n",
		             f, s.delayTarget, period, got, 1200.0 * std::log2 (got / f));
	}
	return 0;
}
