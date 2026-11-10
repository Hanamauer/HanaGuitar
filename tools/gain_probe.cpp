// gain_probe -- measures the level the amp stage actually produces, per
// character, so the output trim is set from numbers instead of taste.
#include "../src/engine.h"
#include "../src/presets.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

static void stats (const std::vector<float>& l, const std::vector<float>& r,
                   double& peak, double& rms, double& limited)
{
	peak = rms = 0; limited = 0;
	double acc = 0;
	for (size_t i = 0; i < l.size (); ++i)
	{
		const double a = std::max (std::fabs (l[i]), std::fabs (r[i]));
		if (a > peak) peak = a;
		if (a >= 0.94) ++limited;
		acc += 0.5 * ((double) l[i] * l[i] + (double) r[i] * r[i]);
	}
	rms = std::sqrt (acc / std::max<size_t> (1, l.size ()));
	limited = 100.0 * limited / std::max<size_t> (1, l.size ());
}

int main ()
{
	const int sr = 48000;
	const char* names[4] = { "clean", "overdrive", "fuzz", "muff" };

	for (int ch = 0; ch < 4; ++ch)
	{
		for (float drive : { 0.0f, 0.3f, 0.7f, 1.0f })
		{
			jm::Engine e;
			e.prepare (sr);
			jm::applyPresetTo (e, 3);         // clean reference, then override
			e.setParam (jm::kDrive, drive);
			e.setParam (jm::kDriveChar, (float) ch);
			e.setParam (jm::kChorus, 0.f);
			e.setParam (jm::kDelayMix, 0.f);
			e.setParam (jm::kRevMix, 0.f);
			e.setParam (jm::kOutput, -30.f);

			std::vector<float> l (sr * 2), r (sr * 2);
			e.noteOn (52, 0.9f);             // E3
			for (int i = 0; i < sr * 2; i += 64) e.process (&l[i], &r[i], std::min (64, sr * 2 - i));
			double p, rm, lim;
			stats (l, r, p, rm, lim);
			std::printf ("%-9s drive %.1f  single  peak %6.2f dB  rms %6.2f dB  lim %5.2f%%\n",
			             names[ch], drive, 20 * std::log10 (p + 1e-9) + 30, 20 * std::log10 (rm + 1e-9) + 30, lim);

			jm::Engine e2;
			e2.prepare (sr);
			jm::applyPresetTo (e2, 3);
			e2.setParam (jm::kDrive, drive);
			e2.setParam (jm::kDriveChar, (float) ch);
			e2.setParam (jm::kChorus, 0.f);
			e2.setParam (jm::kDelayMix, 0.f);
			e2.setParam (jm::kRevMix, 0.f);
			e2.setParam (jm::kOutput, -30.f);
			const int chord[6] = { 40, 47, 52, 55, 59, 64 };
			for (int k = 0; k < 6; ++k) e2.noteOnDelayed (chord[k], 0.9f, k * 432);
			std::vector<float> l2 (sr * 2), r2 (sr * 2);
			for (int i = 0; i < sr * 2; i += 64) e2.process (&l2[i], &r2[i], std::min (64, sr * 2 - i));
			stats (l2, r2, p, rm, lim);
			std::printf ("%-9s drive %.1f  chord   peak %6.2f dB  rms %6.2f dB  lim %5.2f%%\n",
			             names[ch], drive, 20 * std::log10 (p + 1e-9) + 30, 20 * std::log10 (rm + 1e-9) + 30, lim);
		}
	}
	return 0;
}
