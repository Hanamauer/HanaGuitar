#include "../src/dsp/string.h"
#include <cstdio>
#include <vector>
#include <cmath>

static double gz (const std::vector<float>& x, double f, double sr)
{
	const int n = (int) x.size ();
	const double w = 2.0 * 3.141592653589793 * f / sr;
	const double cw = std::cos (w), sw = std::sin (w), k = 2 * cw;
	double s0 = 0, s1 = 0, s2 = 0;
	for (int i = 0; i < n; ++i) { s0 = x[i] + k * s1 - s2; s2 = s1; s1 = s0; }
	const double re = s1 - s2 * cw, im = s2 * sw;
	return std::sqrt (re * re + im * im) / n;
}

int main ()
{
	const float f = 110.f;
	jm::WaveString s;
	s.prepare (48000.f, 2500);
	s.setFrequency (f, 0.06f, 0.35f);
	s.setDecay (8.f, 0.35f);
	jm::Rng rng;
	s.pluck (0.9f, 0.13f, 0.5f, 0.7f, 0.f, rng);

	std::vector<float> raw;
	for (int i = 0; i < 48000; ++i) raw.push_back (s.process ());

	std::printf ("delayTarget=%.3f  fs/delay=%.3f  fs/(2*delay)=%.3f\n",
	             s.delayTarget, 48000.0 / s.delayTarget, 48000.0 / (2 * s.delayTarget));

	double bp = -1, bf = 0;
	for (double t = 20; t < 1200; t += 0.25)
	{
		const double p = gz (raw, t, 48000.0);
		if (p > bp) { bp = p; bf = t; }
	}
	std::printf ("strongest partial: %.2f Hz  (target %.2f, ratio %.4f)\n", bf, f, bf / f);

	// print the full low spectrum
	for (double t = 25; t < 500; t += 27.5)
		std::printf ("  %7.1f Hz  %9.5f\n", t, gz (raw, t, 48000.0));
	return 0;
}
