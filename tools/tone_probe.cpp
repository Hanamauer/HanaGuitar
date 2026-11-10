// tone_probe -- what the two pickups and the two circuits actually do
#include "../src/engine.h"
#include "../src/presets.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

static void run (const char* name, int pickup, int circuit, float tone, float volume)
{
	jm::Engine e;
	e.prepare (48000);
	jm::applyPresetTo (e, 3);
	e.setParam (jm::kDrive, 0.f);
	e.setParam (jm::kDriveChar, 0.f);
	e.setParam (jm::kCab, 0.7f);
	e.setParam (jm::kChorus, 0.f);
	e.setParam (jm::kDelayMix, 0.f);
	e.setParam (jm::kRevMix, 0.f);
	e.setParam (jm::kBody, 0.f);
	e.setParam (jm::kBehindBridge, 0.f);
	e.setParam (jm::kAge, 0.f);
	e.setParam (jm::kPickupSel, (float) pickup);
	e.setParam (jm::kCircuit, (float) circuit);
	e.setParam (jm::kTone, tone);
	e.setParam (jm::kVolume, volume);
	e.setParam (jm::kOutput, 0.f);

	e.noteOn (52, 0.9f);   // E3
	std::vector<float> l (48000), r (48000);
	for (size_t i = 0; i < l.size (); i += 64) e.process (&l[i], &r[i], 64);

	// spectrum of the steady part
	const int n = 16384;
	std::vector<float> seg (l.begin () + 512, l.begin () + 512 + n);
	double peak = 0, rms = 0;
	for (float v : l) peak = std::max (peak, (double) std::fabs (v));
	for (float v : seg) rms += (double) v * v;
	rms = std::sqrt (rms / seg.size ());

	// spectral centroid via a simple DFT on 2048 samples (fast enough here)
	const int m = 2048;
	std::vector<float> s2 (seg.begin (), seg.begin () + m);
	for (int i = 0; i < m; ++i) s2[i] *= 0.5f - 0.5f * std::cos (6.2831853f * i / (m - 1));
	double num = 0, den = 0, hf = 0;
	for (int k = 1; k < m / 2; ++k)
	{
		double re = 0, im = 0;
		for (int i = 0; i < m; ++i)
		{
			const double w = 2.0 * 3.14159265358979 * k * i / m;
			re += s2[i] * std::cos (w);
			im -= s2[i] * std::sin (w);
		}
		const double p = re * re + im * im;
		num += p * k * 48000.0 / m;
		den += p;
		if (k * 48000.0 / m > 1000.0) hf += p;
	}
	std::printf ("%-22s peak %6.1f dB  rms %6.1f dB  centroid %6.0f Hz  HF %5.1f%%\n", name,
	             20 * std::log10 (peak + 1e-9), 20 * std::log10 (rms + 1e-9), num / den,
	             100.0 * hf / std::max (1e-12, den));
}

int main ()
{
	run ("P90  lead  tone .8", 0, 0, 0.8f, 1.f);
	run ("HB   lead  tone .8", 2, 0, 0.8f, 1.f);
	run ("both lead  tone .8", 1, 0, 0.8f, 1.f);
	run ("P90  lead  tone 0 ", 0, 0, 0.0f, 1.f);
	run ("P90  rhythm      ", 0, 1, 0.8f, 1.f);
	run ("P90  lead  vol .3 ", 0, 0, 0.8f, 0.3f);
	return 0;
}
