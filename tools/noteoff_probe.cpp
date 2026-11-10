// noteoff_probe -- chord decay with and without note-off
#include "../src/engine.h"
#include "../src/presets.h"
#include <cstdio>
#include <cmath>
#include <vector>

static double rms (const std::vector<float>& x, int from, int n)
{
	double a = 0;
	for (int i = 0; i < n; ++i) a += (double) x[from + i] * x[from + i];
	return std::sqrt (a / std::max (1, n));
}

int main (int argc, char** argv)
{
	const bool sendOff = (argc > 1 && argv[1][0] == '1');
	const int sr = 48000;
	const int total = sr * 8;

	jm::Engine e;
	e.prepare (sr);
	jm::applyPresetTo (e, 0);          // shoegaze
	e.setParam (jm::kChorus, 0.f);
	e.setParam (jm::kDelayMix, 0.f);
	e.setParam (jm::kRevMix, 0.f);
	e.setParam (jm::kSympathy, (argc > 2) ? (float) atof (argv[2]) : 0.42f);

	std::vector<float> l (total), r (total);
	const int chord[6] = { 40, 47, 52, 55, 59, 64 };
	for (int k = 0; k < 6; ++k) e.noteOnDelayed (chord[k], 0.9f, k * 432);

	bool off = false;
	for (int i = 0; i < total; i += 64)
	{
		if (sendOff && !off && i >= sr * 6.4)
		{
			off = true;
			for (int k = 0; k < 6; ++k) e.noteOff (chord[k]);
			std::printf ("note-off at %.2f s, active %d\n", (double) i / sr, (int) e.activeStrings ());
		}
		e.process (&l[i], &r[i], std::min (64, total - i));
	}

	for (double t = 4.0; t < 8.0; t += 0.2)
	{
		const int i = (int) (t * sr);
		std::printf ("%5.2f s  rms %7.1f dB  active %d\n", t,
		             20 * std::log10 (rms (l, i, (int) (0.2 * sr)) + 1e-12),
		             (int) e.activeStrings ());
	}
	return 0;
}
