// legato_probe -- hammer-on / pull-off / tapping must not re-pick the string
#include "../src/engine.h"
#include <cstdio>
#include <cmath>
#include <vector>

static double powerAt (const std::vector<float>& x, int from, int n, double f, double sr)
{
	double re = 0, im = 0;
	for (int i = 0; i < n; ++i)
	{
		const double w = 2.0 * 3.141592653589793 * f * i / sr;
		re += x[from + i] * std::cos (w);
		im -= x[from + i] * std::sin (w);
	}
	return (re * re + im * im) / ((double) n * n);
}

static double pitchAt (const std::vector<float>& x, int from, int n, double sr)
{
	// autocorrelation over one window
	double best = 0, bf = 0;
	for (double f = 120; f < 500; f += 0.5)
	{
		double re = 0, im = 0;
		for (int i = 0; i < n; ++i)
		{
			const double w = 2.0 * 3.141592653589793 * f * i / sr;
			re += x[from + i] * std::cos (w);
			im -= x[from + i] * std::sin (w);
		}
		const double p = re * re + im * im;
		if (p > best) { best = p; bf = f; }
	}
	return bf;
}

int main ()
{
	const int sr = 48000;
	jm::Engine e;
	e.prepare (sr);
	e.setParam (jm::kDrive, 0.f);
	e.setParam (jm::kCab, 0.f);
	e.setParam (jm::kChorus, 0.f);
	e.setParam (jm::kDelayMix, 0.f);
	e.setParam (jm::kRevMix, 0.f);
	e.setParam (jm::kBody, 0.f);
	e.setParam (jm::kBehindBridge, 0.f);
	e.setParam (jm::kAge, 0.f);
	e.setParam (jm::kPickupSel, 0.f);
	e.setParam (jm::kSustain, 0.95f);

	std::vector<float> l (sr * 2), r (l.size ());
	auto run = [&] (int n) {
		for (int i = 0; i < n; i += 64) e.process (&l[i], &r[i], std::min (64, n - i));
	};

	// E3 (52), then hammer A3 (57) without releasing, then pull off back to E3
	e.noteOn (52, 0.9f);
	run (12000);
	std::printf ("E3  P164 %.2e  P220 %.2e  P329 %.2e\n", powerAt (l, 8000, 2048, 164.81, sr), powerAt (l, 8000, 2048, 220.0, sr), powerAt (l, 8000, 2048, 329.63, sr));

	e.noteOn (57, 0.85f);              // hammer-on
	run (12000);
	std::printf ("A3  P164 %.2e  P220 %.2e  P329 %.2e\n", powerAt (l, 8000, 2048, 164.81, sr), powerAt (l, 8000, 2048, 220.0, sr), powerAt (l, 8000, 2048, 329.63, sr));

	e.noteOff (57);                    // pull-off
	run (12000);
	std::printf ("E3  P164 %.2e  P220 %.2e  P329 %.2e\n", powerAt (l, 8000, 2048, 164.81, sr), powerAt (l, 8000, 2048, 220.0, sr), powerAt (l, 8000, 2048, 329.63, sr));

	// energy right after the hammer must not drop to a fresh pluck level
	double before = 0, after = 0;
	for (int i = 0; i < 1024; ++i) before += (double) l[8000 + i] * l[8000 + i];
	std::printf ("energy after pull-off %.6f\n", std::sqrt (before / 1024));

	e.noteOff (52);
	run (sr);
	std::printf ("after release strings %.0f\n", e.activeStrings ());
	(void) after;
	return 0;
}
