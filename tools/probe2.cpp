#include "../src/engine.h"
#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>

static double goertzel (const std::vector<float>& x, double freq, double sr)
{
	const int n = (int) x.size ();
	const double w = 2.0 * 3.141592653589793 * freq / sr;
	const double cw = std::cos (w), sw = std::sin (w);
	const double coeff = 2.0 * cw;
	double s0 = 0, s1 = 0, s2 = 0;
	for (int i = 0; i < n; ++i) { s0 = x[i] + coeff * s1 - s2; s2 = s1; s1 = s0; }
	const double re = s1 - s2 * cw, im = s2 * sw;
	return (re * re + im * im) / ((double) n * n);
}

int main ()
{
	// --- idle noise hunt ---
	{
		jm::Engine e;
		e.prepare (48000.0);
		std::vector<float> l (64), r (64);
		float peak = 0; int firstIdx = -1;
		for (int b = 0; b < 400; ++b)
		{
			e.process (l.data (), r.data (), 64);
			for (int i = 0; i < 64; ++i)
				if (std::fabs (l[i]) > peak) { peak = std::fabs (l[i]); if (firstIdx < 0) firstIdx = b * 64 + i; }
		}
		std::printf ("idle peak %.9f first at sample %d\n", peak, firstIdx);

		// which param is responsible?
		for (int id = 0; id < (int) jm::kNumParams; ++id)
		{
			jm::Engine e2;
			e2.prepare (48000.0);
			e2.setParam (id, 0.f);
			std::vector<float> a (64), b2 (64);
			float p = 0;
			for (int k = 0; k < 200; ++k)
			{
				e2.process (a.data (), b2.data (), 64);
				for (int i = 0; i < 64; ++i) p = std::max (p, std::fabs (a[i]));
			}
			if (p > 1e-6f) std::printf ("  zeroing %-14s -> peak %.6f\n", jm::kParamDefs[id].name, p);
		}
	}

	// --- harmonic profile ---
	{
		jm::Engine e;
		e.prepare (48000.0);
		e.setParam (jm::kDrive, 0.f);
		e.setParam (jm::kCab, 0.f);
		e.setParam (jm::kChorus, 0.f);
		e.setParam (jm::kDelayMix, 0.f);
		e.setParam (jm::kRevMix, 0.f);
		e.setParam (jm::kBody, 0.f);
		e.setParam (jm::kBehindBridge, 0.f);
		e.setParam (jm::kAge, 0.f);
		e.setParam (jm::kBrightness, 0.9f);
		e.setParam (jm::kSustain, 0.95f);
		e.setParam (jm::kArmFlutter, 0.f);
		e.setParam (jm::kPickupSel, 0.f);
		e.noteOn (40, 0.9f);

		std::vector<float> l (64), r (64);
		std::vector<float> out;
		for (int b = 0; b < 100; ++b) e.process (l.data (), r.data (), 64);
		for (int b = 0; b < 256; ++b) { e.process (l.data (), r.data (), 64); out.insert (out.end (), l.begin (), l.end ()); }

		const double f0 = 440.0 * std::pow (2.0, (40 - 69) / 12.0);
		std::printf ("\nneck pickup, note 40, f0=%.2f\n", f0);
		double mx = 0;
		for (int h = 1; h <= 14; ++h)
		{
			const double p = goertzel (out, f0 * h, 48000.0) * h * h;
			mx = std::max (mx, p);
			std::printf ("  h%2d %8.2f Hz  %10.3e  %6.1f dB\n", h, f0 * h, p,
			             10 * std::log10 (p / 1e-20));
		}

		// exact fundamental by scanning
		double best = 0, bp = -1;
		for (double f = f0 * 0.9; f < f0 * 1.1; f += 0.02)
		{
			const double p = goertzel (out, f, 48000.0);
			if (p > bp) { bp = p; best = f; }
		}
		std::printf ("  peak scan: %.3f Hz  (%.1f cents off)\n", best, 1200 * std::log2 (best / f0));
	}
	return 0;
}
