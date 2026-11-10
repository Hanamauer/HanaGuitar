// Probe: measure engine pitch over time during a dive.
#include "../src/engine.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

static double goertzel (const std::vector<float>& x, double freq, double sr)
{
	const int n = (int) x.size ();
	if (n < 8) return 0.0;
	const double w = 2.0 * 3.141592653589793 * freq / sr;
	const double cw = std::cos (w), sw = std::sin (w), k = 2.0 * cw;
	double s0 = 0.0, s1 = 0.0, s2 = 0.0;
	for (int i = 0; i < n; ++i) { s0 = x[i] + k * s1 - s2; s2 = s1; s1 = s0; }
	const double re = s1 - s2 * cw, im = s2 * sw;
	return std::sqrt (re * re + im * im) / n;
}

static double bestPitch (const std::vector<float>& x, double lo, double hi, double sr)
{
	double best = lo, bp = -1.0;
	for (double f = lo; f <= hi; f += 0.25)
	{
		const double p = goertzel (x, f, sr);
		if (p > bp) { bp = p; best = f; }
	}
	return best;
}

int main ()
{
	jm::Engine e;
	e.prepare (48000.0);
	// rest = plain 0 for the arm
	e.setParam (jm::kArmPos, 0.f);
	e.setParam (jm::kArmRange, 3.f);
	e.setParam (jm::kArmSpring, 8.f);
	e.setParam (jm::kArmFlutter, 0.f);
	e.setParam (jm::kGlideDepth, 0.f);
	e.setParam (jm::kChorus, 0.f);
	e.setParam (jm::kDelayMix, 0.f);
	e.setParam (jm::kRevMix, 0.f);
	e.setParam (jm::kDrive, 0.f);

	e.noteOn (64, 0.9f);

	std::vector<float> bl (512), br (512), mono;
	auto render = [&] (int n, std::vector<float>* out) {
		int done = 0;
		while (done < n)
		{
			const int m = std::min (512, n - done);
			e.process (bl.data (), br.data (), m);
			if (out) for (int i = 0; i < m; ++i) out->push_back (bl[i]);
			done += m;
		}
	};

	auto report = [&] (const char* tag) {
		mono.clear ();
		render (8192, &mono);
		std::printf ("%-12s bar %+6.2f st  pitch %7.2f Hz\n", tag, e.barSemis (),
		             bestPitch (mono, 180.0, 500.0, 48000.0));
	};

	render (12000, nullptr);
	report ("rest");

	e.setParam (jm::kArmPos, 1.0f);
	for (int k = 0; k < 5; ++k)
	{
		render (4800, nullptr);
		char t[32];
		std::snprintf (t, sizeof (t), "dive %dms", (k + 1) * 100);
		report (t);
	}
	return 0;
}
