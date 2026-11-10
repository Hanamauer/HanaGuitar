// Probe: replicate the host_test whammy sequence directly against the engine.
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

static double pitchAt (const std::vector<float>& x, int start, int n,
                       double lo, double hi, double sr)
{
	double best = lo, bp = -1.0;
	std::vector<float> win ((size_t) n);
	for (int i = 0; i < n; ++i) win[(size_t) i] = x[(size_t) (start + i)];
	for (double f = lo; f <= hi; f += 0.5)
	{
		const double p = goertzel (win, f, sr);
		if (p > bp) { bp = p; best = f; }
	}
	return best;
}

int main ()
{
	jm::Engine e;
	e.prepare (48000.0);

	// exactly what host_test does: these are NORMALISED values
	e.setParam (jm::kArmRange, jm::plainFromNorm (jm::kParamDefs[jm::kArmRange],
	                                              (float) jm::normFromPlain (jm::kParamDefs[jm::kArmRange], 3.f)));
	e.setParam (jm::kArmSpring, jm::plainFromNorm (jm::kParamDefs[jm::kArmSpring],
	                                               (float) jm::normFromPlain (jm::kParamDefs[jm::kArmSpring], 8.f)));
	e.setParam (jm::kArmFlutter, 0.f);
	e.setParam (jm::kGlideDepth, 0.f);
	e.setParam (jm::kArmPos, jm::plainFromNorm (jm::kParamDefs[jm::kArmPos], 0.f));

	std::printf ("arm plain at 'rest' = %.3f\n", e.getParam (jm::kArmPos));

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

	render (24000, nullptr);
	mono.clear ();
	render (8192, &mono);
	const double f0 = pitchAt (mono, 1000, 6000, 300.0, 360.0, 48000.0);
	std::printf ("f0 = %.1f Hz  (bar %.3f st, angle %.3f)\n", f0, e.barSemis (), e.barAngle ());

	e.setParam (jm::kArmPos, 1.0f);
	std::printf ("arm plain after dive = %.3f\n", e.getParam (jm::kArmPos));
	render (24000, nullptr);
	std::printf ("bar after dive = %.3f st (angle %.3f)\n", e.barSemis (), e.barAngle ());
	mono.clear ();
	render (8192, &mono);
	const double f1 = pitchAt (mono, 1000, 6000, 250.0, 360.0, 48000.0);
	std::printf ("f1 = %.1f Hz  (bar %.3f st)\n", f1, e.barSemis ());

	// and a scan over the whole band to see where the energy really is
	std::vector<float> win (mono.begin () + 1000, mono.begin () + 7000);
	double bestF = 0, bestP = -1;
	for (double f = 60; f < 1200; f += 1.0)
	{
		const double p = goertzel (win, f, 48000.0);
		if (p > bestP) { bestP = p; bestF = f; }
	}
	std::printf ("strongest component in f1 window: %.1f Hz\n", bestF);
	return 0;
}
