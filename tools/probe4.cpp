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
	s.setFrequency (f, 0.f, 1.f);
	s.setDecay (8.f, 1.f);
	jm::Rng rng;
	s.pluck (0.9f, 0.13f, 0.5f, 0.7f, 0.f, rng);

	// raw loop output (what the string produces before any pickup)
	std::vector<float> raw;
	for (int i = 0; i < 48000; ++i) raw.push_back (s.process ());
	std::printf ("raw loop harmonics (f0=%.1f):\n", f);
	for (int h = 1; h <= 8; ++h)
		std::printf ("  h%d %8.2f  %9.5f\n", h, f * h, gz (raw, f * h, 48000.0));

	// now the pickup reads
	jm::WaveString s2;
	s2.prepare (48000.f, 2500);
	s2.setFrequency (f, 0.f, 1.f);
	s2.setDecay (8.f, 1.f);
	s2.pluck (0.9f, 0.13f, 0.5f, 0.7f, 0.f, rng);
	std::vector<float> neck, bridge, atPoint;
	for (int i = 0; i < 48000; ++i)
	{
		s2.process ();
		neck.push_back (s2.readPos (0.245f));
		bridge.push_back (s2.readPos (0.105f));
		atPoint.push_back (readHermite (s2.delay, 0.245f * s2.smoothedDelay ()));
	}
	std::printf ("\nneck readPos(0.245):\n");
	for (int h = 1; h <= 8; ++h)
		std::printf ("  h%d %9.5f\n", h, gz (neck, f * h, 48000.0));
	std::printf ("\nbridge readPos(0.105):\n");
	for (int h = 1; h <= 8; ++h)
		std::printf ("  h%d %9.5f\n", h, gz (bridge, f * h, 48000.0));
	std::printf ("\nreadAt(delay*0.245) -- single tap:\n");
	for (int h = 1; h <= 8; ++h)
		std::printf ("  h%d %9.5f\n", h, gz (atPoint, f * h, 48000.0));
	return 0;
}
