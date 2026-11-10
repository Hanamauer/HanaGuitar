// comb_probe -- per-harmonic bridge/neck ratio vs the aperture model
#include "../src/engine.h"
#include <cstdio>
#include <cmath>
#include <vector>

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
	const int note = 45;
	const double f0 = 440.0 * std::pow (2.0, (note - 69) / 12.0);
	const float xn = jm::Pickup::kNeckPos, xb = jm::Pickup::kBridgePos;
	const double ap = 0.5 * jm::Pickup::kAperture;

	auto spectrum = [&] (int sel) {
		jm::Engine e;
		e.prepare (48000);
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
		e.setParam (jm::kPickupSel, (float) sel);
		e.setParam (jm::kPickPos, 0.13f);
		e.setParam (jm::kPuHeight, 0.5f);
		e.noteOn (note, 0.9f);
		std::vector<float> l (512 + 4096), r (l.size ());
		for (size_t i = 0; i < l.size (); i += 64) e.process (&l[i], &r[i], 64);
		return std::vector<float> (l.begin () + 512, l.end ());
	};

	auto hn = spectrum (0), hb = spectrum (2);
	std::printf ("  k    neck       bridge     ratio    model    err\n");
	for (int k = 1; k <= 10; ++k)
	{
		const double a = goertzel (hn, f0 * k, 48000);
		const double b = goertzel (hb, f0 * k, 48000);
		const double ratio = std::sqrt (b / std::max (1e-30, a));
		const double w = 3.14159265358979 * k;
		const double sN = std::fabs (0.5 * (std::sin (w * (xn - ap)) + std::sin (w * (xn + ap))));
		const double sB = std::fabs (0.5 * (std::sin (w * (xb - ap)) + std::sin (w * (xb + ap))));
		const double model = sB / std::max (1e-9, sN);
		std::printf (" %2d  %9.2e  %9.2e  %7.2f  %7.2f  %6.2f\n", k, a, b, ratio, model,
		             std::fabs (std::log (ratio / model)));
	}
	return 0;
}
