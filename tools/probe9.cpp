// Probe: what is actually in the spectrum after a dive, with effects on?
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

static void topPeaks (const std::vector<float>& x, double lo, double hi, double sr,
                      const char* tag)
{
	struct Pk { double f, p; };
	std::vector<Pk> all;
	for (double f = lo; f <= hi; f += 0.5) all.push_back ({ f, goertzel (x, f, sr) });
	std::sort (all.begin (), all.end (), [] (const Pk& a, const Pk& b) { return a.p > b.p; });
	std::printf ("%s:", tag);
	int shown = 0;
	for (size_t i = 0; i < all.size () && shown < 4; ++i)
	{
		// only report local maxima so harmonics don't repeat the same peak
		bool isMax = true;
		if (i > 0 && all[i - 1].p > all[i].p) isMax = false;
		if (isMax) { std::printf ("  %.1f Hz (%.4f)", all[i].f, all[i].p); ++shown; }
	}
	std::printf ("\n");
}

static void run (const char* tag, bool effects)
{
	jm::Engine e;
	e.prepare (48000.0);
	e.setParam (jm::kArmPos, jm::plainFromNorm (jm::kParamDefs[jm::kArmPos], 0.f));
	e.setParam (jm::kArmRange, 3.f);
	e.setParam (jm::kArmSpring, 8.f);
	e.setParam (jm::kArmFlutter, 0.f);
	e.setParam (jm::kGlideDepth, 0.f);
	if (!effects)
	{
		e.setParam (jm::kChorus, 0.f);
		e.setParam (jm::kDelayMix, 0.f);
		e.setParam (jm::kRevMix, 0.f);
		e.setParam (jm::kDrive, 0.f);
	}

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
	mono.clear (); render (8192, &mono);
	char t[64]; std::snprintf (t, sizeof (t), "%s rest  bar %+.2f", tag, e.barSemis ());
	topPeaks (mono, 180, 500, 48000.0, t);

	e.setParam (jm::kArmPos, 1.0f);
	render (24000, nullptr);
	mono.clear (); render (8192, &mono);
	std::snprintf (t, sizeof (t), "%s dive  bar %+.2f", tag, e.barSemis ());
	topPeaks (mono, 180, 500, 48000.0, t);
}

int main ()
{
	run ("[fx off]", false);
	run ("[fx on ]", true);
	return 0;
}
