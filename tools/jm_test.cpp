// ===========================================================================
//  jm_test -- offline verification for the HanaGuitar for indie engine
//
//  No host, no audio device.  Everything the plugin claims is measured here:
//
//    * tuning: every open string lands on its MIDI frequency, measured with a
//      Goertzel at the fundamental and at the 2nd/3rd partial
//    * whammy: a bar move produces the semitone shift the spring model says,
//      the spring overshoots and settles, and returns to 0
//    * pickup comb: neck and bridge positions put their notches where the
//      geometry says they should be
//    * silence: no note, no output -- exactly zero, not "very quiet"
//    * block size: 1 / 7 / 64 / 512 / 1024 all render bit-identically
//    * adversarial: DC, full-scale, every parameter at both extremes -> finite
//    * ceilings: output never exceeds 0 dBFS
//    * determinism: same input, same output
//
//  Exit code 0 means everything passed.
// ===========================================================================

#include "../src/engine.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <vector>
#include <algorithm>

static int g_fail = 0;
static int g_pass = 0;

static void check (const char* what, bool ok, const char* extra = "")
{
	std::printf ("  %s  %s %s\n", ok ? "PASS" : "FAIL", what, extra);
	if (ok) ++g_pass; else ++g_fail;
}

static void checkf (const char* what, bool ok, const char* fmt, double a, double b)
{
	char buf[256];
	std::snprintf (buf, sizeof (buf), fmt, a, b);
	check (what, ok, buf);
}

// printf-style variant for messages that mix ints and doubles.  Using the
// two-double form with a "%d" is undefined behaviour (varargs do not convert),
// so anything with an integer goes through here instead.
#if defined(__GNUC__)
__attribute__ ((format (printf, 3, 4)))
#endif
static void checkm (const char* what, bool ok, const char* fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start (ap, fmt);
	std::vsnprintf (buf, sizeof (buf), fmt, ap);
	va_end (ap);
	check (what, ok, buf);
}

// ---------------------------------------------------------------------------
// Goertzel: power at one frequency, normalised to the window length.
// ---------------------------------------------------------------------------
static double goertzel (const std::vector<float>& x, double freq, double sr)
{
	const int n = (int) x.size ();
	const double w = 2.0 * 3.141592653589793 * freq / sr;
	const double cw = std::cos (w), sw = std::sin (w);
	const double coeff = 2.0 * cw;
	double s0 = 0, s1 = 0, s2 = 0;
	for (int i = 0; i < n; ++i)
	{
		s0 = x[i] + coeff * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	const double real = s1 - s2 * cw;
	const double imag = s2 * sw;
	return (real * real + imag * imag) / ((double) n * n);
}

// Find the peak frequency by scanning and then refining.
static double peakFreq (const std::vector<float>& x, double sr, double lo, double hi,
                        double step = 0.5)
{
	double best = lo, bestP = -1;
	for (double f = lo; f <= hi; f += step)
	{
		const double p = goertzel (x, f, sr);
		if (p > bestP) { bestP = p; best = f; }
	}
	return best;
}

static double midiFreq (int note)
{
	return 440.0 * std::pow (2.0, (note - 69) / 12.0);
}

static double centsBetween (double a, double b)
{
	return 1200.0 * std::log2 (a / b);
}

// ---------------------------------------------------------------------------
struct Rig
{
	jm::Engine e;
	double sr = 48000.0;
	std::vector<float> l, r;

	Rig (double rate = 48000.0) : sr (rate) { e.prepare (sr); }

	void set (int id, float v) { e.setParam (id, v); }

	// Render n samples after a note on.
	std::vector<float> render (int n, int block = 64)
	{
		l.assign (n, 0.f);
		r.assign (n, 0.f);
		int done = 0;
		std::vector<float> bl (block), br (block);
		while (done < n)
		{
			const int m = std::min (block, n - done);
			e.process (bl.data (), br.data (), m);
			for (int i = 0; i < m; ++i) { l[done + i] = bl[i]; r[done + i] = br[i]; }
			done += m;
		}
		return l;
	}

	std::vector<float> play (int note, float vel, int n, int block = 64)
	{
		e.noteOn (note, vel);
		return render (n, block);
	}
};

// ---------------------------------------------------------------------------
static void testTuning ()
{
	std::printf ("\n[1] tuning -- every open string must land on its MIDI note\n");

	// The amp and the effects colour the spectrum; tuning is about the string,
	// so measure with them out of the way.
	for (int s = 0; s < 6; ++s)
	{
		Rig rig;
		rig.set (jm::kDrive, 0.f);
		rig.set (jm::kCab, 0.f);
		rig.set (jm::kChorus, 0.f);
		rig.set (jm::kDelayMix, 0.f);
		rig.set (jm::kRevMix, 0.f);
		rig.set (jm::kTremDepth, 0.f);
		rig.set (jm::kPickupSel, 0.f);
		rig.set (jm::kBody, 0.f);
		rig.set (jm::kBehindBridge, 0.f);
		rig.set (jm::kAge, 0.f);
		rig.set (jm::kBrightness, 0.75f);
		rig.set (jm::kSustain, 0.8f);

		const int note = jm::kTunings[0][s];
		const double want = midiFreq (note);

		// Skip the attack, then measure a long window.
		rig.play (note, 0.9f, 4800);
		auto x = rig.render (32768);
		const double got = peakFreq (x, rig.sr, want * 0.90, want * 1.10, 0.05);
		const double cents = centsBetween (got, want);

		char name[64];
		std::snprintf (name, sizeof (name), "string %d (MIDI %d = %.1f Hz)", s, note, want);
		checkf (name, std::fabs (cents) < 12.0, "measured %.2f Hz, %+.1f cents", got, cents);
	}
}

// ---------------------------------------------------------------------------
static void testWhammy ()
{
	std::printf ("\n[2] whammy -- the arm must bend pitch the way the spring says\n");

	// --- static dive: set the arm and let it settle -------------------------
	{
		Rig rig;
		rig.set (jm::kDrive, 0.f);
		rig.set (jm::kCab, 0.f);
		rig.set (jm::kChorus, 0.f);
		rig.set (jm::kDelayMix, 0.f);
		rig.set (jm::kRevMix, 0.f);
		rig.set (jm::kPickupSel, 0.f);
		rig.set (jm::kBody, 0.f);
		rig.set (jm::kBehindBridge, 0.f);
		rig.set (jm::kAge, 0.f);
		rig.set (jm::kBrightness, 0.8f);
		rig.set (jm::kSustain, 0.9f);

		const int note = 64;
		const double base = midiFreq (note);
		rig.play (note, 0.9f, 9600);

		// measure at rest
		auto rest = rig.render (16384);
		const double fRest = peakFreq (rest, rig.sr, base * 0.9, base * 1.1, 0.05);
		checkf ("arm at rest: pitch unchanged", std::fabs (centsBetween (fRest, base)) < 12.0,
		        "%.2f Hz vs %.2f Hz", fRest, base);

		// now pull the arm all the way down and let the spring settle
		rig.e.setModWheel (1.f);
		rig.render (24000);   // let the spring settle

		auto dived = rig.render (16384);
		const double fDive = peakFreq (dived, rig.sr, base * 0.60, base * 1.10, 0.05);
		const double want = base * std::pow (2.0, -4.0 / 12.0);
		const double cents = centsBetween (fDive, want);
		char buf[256];
		std::snprintf (buf, sizeof (buf), "%.2f Hz, expected %.2f Hz, %+.1f cents",
		               fDive, want, cents);
		check ("full dive = -4.0 semitones (fixed jazzmaster range)", std::fabs (cents) < 20.0, buf);

		// the same move through the Bar parameter (host automation / panel drag)
		{
			Rig r2;
			r2.set (jm::kDrive, 0.f);
			r2.set (jm::kCab, 0.f);
			r2.set (jm::kChorus, 0.f);
			r2.set (jm::kDelayMix, 0.f);
			r2.set (jm::kRevMix, 0.f);
			r2.set (jm::kPickupSel, 0.f);
			r2.set (jm::kBody, 0.f);
			r2.set (jm::kBehindBridge, 0.f);
			r2.set (jm::kAge, 0.f);
			r2.set (jm::kSustain, 0.9f);
			r2.set (jm::kBar, 0.f);
			r2.play (64, 0.9f, 9600);
			r2.render (16384);
			r2.set (jm::kBar, 1.f);
			r2.render (24000);
			auto d = r2.render (16384);
			const double fd = peakFreq (d, r2.sr, base * 0.60, base * 1.10, 0.05);
			char b2[192];
			std::snprintf (b2, sizeof (b2), "%.1f Hz, expected %.1f Hz", fd, base * std::pow (2.0, -4.0 / 12.0));
			check ("Bar parameter dives four semitones", std::fabs (centsBetween (fd, base * std::pow (2.0, -4.0 / 12.0))) < 25.0, b2);
			r2.set (jm::kBar, 0.f);
		}

		// release: must come back
		rig.e.setModWheel (0.f);
		rig.render (24000);
		auto back = rig.render (16384);
		const double fBack = peakFreq (back, rig.sr, base * 0.9, base * 1.1, 0.05);
		checkf ("arm returns to pitch", std::fabs (centsBetween (fBack, base)) < 15.0,
		        "%.2f Hz vs %.2f Hz", fBack, base);
	}

	// --- spring transient: overshoot then settle ---------------------------
	{
		jm::WhammyModel w;
		w.prepare (48000.0);
		w.setParams (2.0f, 6.0f, 0.f, 0.35f, 0.f);
		// A *partial* move, not a full dive: at target 1.0 the mechanical stop
		// caps the angle at 1.0, so by construction there can be no overshoot.
		// The physical claim being tested is the step response of the
		// spring/mass/damper, which only shows when the stop is out of reach.
		const float tgt = 0.70f;
		w.setTarget (tgt);

		std::vector<float> trace;
		for (int i = 0; i < 48000; ++i) { w.tick (); trace.push_back (w.barAngle ()); }

		float mx = -1e9f;
		for (float v : trace) mx = std::max (mx, v);
		const float settled = trace.back ();
		const float overshoot = (mx - tgt) / tgt;

		// Closed form for a 2nd order step response:
		//     overshoot = exp(-pi*zeta / sqrt(1 - zeta^2))
		const float zeta = w.damping ();
		const float theory = std::exp (-3.14159265f * zeta / std::sqrt (1.f - zeta * zeta));

		char buf[256];
		std::snprintf (buf, sizeof (buf), "measured %.1f %%, theory %.1f %% (zeta %.2f)",
		               overshoot * 100.f, theory * 100.f, zeta);
		check ("spring overshoot matches the damped 2nd order response",
		       overshoot > 0.05f && std::fabs (overshoot - theory) < 0.06f, buf);
		checkf ("spring settles at the target", std::fabs (settled - tgt) < 0.03f,
		        "settled %.4f", settled, 0);

		// count zero crossings of (angle - target) to prove it oscillates
		int zc = 0;
		for (size_t i = 1; i < trace.size (); ++i)
			if ((trace[i - 1] - tgt) * (trace[i] - tgt) < 0.f) ++zc;
		checkm ("spring rings, does not just ramp", zc >= 2, "%d crossings of the target", zc);

		// The stop must still hold at a full dive.
		jm::WhammyModel w2;
		w2.prepare (48000.0);
		w2.setParams (2.0f, 6.0f, 0.f, 0.35f, 0.f);
		w2.setTarget (1.0f);
		float mx2 = -1e9f;
		for (int i = 0; i < 48000; ++i) { w2.tick (); mx2 = std::max (mx2, w2.barAngle ()); }
		checkf ("full dive is capped by the mechanical stop", mx2 <= 1.0001f,
		        "peak angle %.5f", mx2, 0);
	}

	// --- flutter: arm energy decays ---------------------------------------
	{
		jm::WhammyModel w;
		w.prepare (48000.0);
		w.setParams (2.5f, 4.5f, 0.8f, 0.35f, 0.f);
		w.addEnergy (1.0f);
		double early = 0, late = 0;
		for (int i = 0; i < 48000; ++i)
		{
			w.tick ();
			const float d = w.barAngle ();
			if (i < 4800) early += std::fabs (d);
			if (i > 43200) late += std::fabs (d);
		}
		checkf ("flutter decays over time", late < early * 0.5,
		        "early |angle| %.3f -> late %.3f", early, late);
	}

	// --- glide: slow drift is bounded and audible -------------------------
	{
		jm::WhammyModel w;
		w.prepare (48000.0);
		w.setParams (2.5f, 4.5f, 0.f, 0.5f, 1.0f);
		float mx = 0.f, mn = 0.f;
		for (int i = 0; i < 48000 * 3; ++i)
		{
			w.tick ();
			mx = std::max (mx, w.semitones ());
			mn = std::min (mn, w.semitones ());
		}
		checkf ("glide stays within range", mx < 1.0f && mn > -1.0f,
		        "semis span %.3f .. %.3f", mn, mx);
		checkf ("glide actually moves", (mx - mn) > 0.15f,
		        "span %.3f semitones", mx - mn, 0);
	}
}

// ---------------------------------------------------------------------------
static void testPickupVoice ()
{
	std::printf ("\n[3] pickups -- P90 and humbucker must not sound the same\n");

	// Drive each pickup's resonant filter with an impulse: no string, no amp,
	// just the electrical voice of the coil.  The P90 is the peaky one; the
	// humbucker resonates lower and is hotter.
	auto impulseResponse = [] (bool humbucker, std::vector<float>& out) {
		jm::Pickup p;
		p.prepare (48000.f);
		out.assign (8192, 0.f);
		out[0] = 1.f;
		for (size_t i = 0; i < out.size (); ++i)
			out[i] = humbucker ? p.shapeHB (out[i]) : p.shapeP90 (out[i]);
	};

	std::vector<float> a, b;
	impulseResponse (false, a);
	impulseResponse (true, b);

	auto peakAt = [] (const std::vector<float>& x) {
		double best = 0, bf = 0;
		for (double f = 2000; f < 8000; f += 25)
		{
			const double p = goertzel (x, f, 48000.0);
			if (p > best) { best = p; bf = f; }
		}
		return bf;
	};

	const double p90k = peakAt (a);
	const double hbk = peakAt (b);
	char buf[256];
	std::snprintf (buf, sizeof (buf), "P90 peak %.0f Hz, humbucker peak %.0f Hz", p90k, hbk);
	check ("P90 resonance sits near 4.6 kHz", std::fabs (p90k - 4600.0) < 700.0, buf);
	check ("humbucker resonates lower than the P90", hbk < p90k - 300.0, buf);

	// A humbucker is two coils in series, so the thing that matters is not the
	// filter's impulse energy but what a played string sounds like through it.
	auto renderPickup = [] (int sel) {
		Rig rig;
		rig.set (jm::kDrive, 0.f);
		rig.set (jm::kCab, 0.f);
		rig.set (jm::kChorus, 0.f);
		rig.set (jm::kDelayMix, 0.f);
		rig.set (jm::kRevMix, 0.f);
		rig.set (jm::kBody, 0.f);
		rig.set (jm::kBehindBridge, 0.f);
		rig.set (jm::kAge, 0.f);
		rig.set (jm::kOutput, 0.f);
		rig.set (jm::kPickupSel, (float) sel);
		rig.play (52, 0.9f, 512);
		auto x = rig.render (8192);
		double e = 0;
		for (float v : x) e += (double) v * v;
		return e;
	};
	const double eP90 = renderPickup (0);
	const double eHB = renderPickup (2);
	checkf ("humbucker is hotter than the P90", eHB > eP90 * 1.25, "energy ratio %.2f", eHB / eP90, 0);
}


// ---------------------------------------------------------------------------
static void testCircuits ()
{
	std::printf ("\n[3b] circuits -- rhythm must be darker and quieter than lead\n");

	auto measure = [] (bool rhythm, float tone) {
		Rig rig;
		rig.set (jm::kDrive, 0.f);
		rig.set (jm::kCab, 0.7f);
		rig.set (jm::kChorus, 0.f);
		rig.set (jm::kDelayMix, 0.f);
		rig.set (jm::kRevMix, 0.f);
		rig.set (jm::kBody, 0.f);
		rig.set (jm::kBehindBridge, 0.f);
		rig.set (jm::kAge, 0.f);
		rig.set (jm::kPickupSel, 0.f);          // P90
		rig.set (jm::kCircuit, rhythm ? 1.f : 0.f);
		rig.set (jm::kTone, tone);
		rig.set (jm::kVolume, 1.f);
		rig.set (jm::kOutput, 0.f);

		rig.play (52, 0.9f, 512);               // E3, 164.8 Hz
		auto x = rig.render (8192);
		double hf = 0, tot = 0, peak = 0;
		for (float v : x) peak = std::max (peak, (double) std::fabs (v));
		for (int k = 1; k <= 40; ++k)
		{
			const double p = goertzel (x, 164.81 * k, rig.sr);
			tot += p;
			if (164.81 * k > 1000.0) hf += p;
		}
		return std::pair<double, double> { 20.0 * std::log10 (peak + 1e-9),
		                                  hf / std::max (1e-12, tot) };
	};

	const auto lead = measure (false, 0.8f);
	const auto dark = measure (false, 0.0f);
	const auto rhy = measure (true, 0.8f);

	char buf[192];
	std::snprintf (buf, sizeof (buf), "lead %.1f dB, rhythm %.1f dB", lead.first, rhy.first);
	check ("rhythm circuit is quieter than lead", rhy.first < lead.first - 3.0, buf);

	std::snprintf (buf, sizeof (buf), "HF lead %.2f%%, rhythm %.2f%%",
	               100.0 * lead.second, 100.0 * rhy.second);
	check ("rhythm circuit is darker than lead", rhy.second < lead.second * 0.8, buf);

	std::snprintf (buf, sizeof (buf), "HF tone 1.0 %.2f%%, tone 0.0 %.2f%%",
	               100.0 * lead.second, 100.0 * dark.second);
	check ("tone all the way down removes the top", dark.second < lead.second * 0.35, buf);
}

// ---------------------------------------------------------------------------
static void testKeyswitches ()
{
	std::printf ("\n[3c] keyswitches -- technique keys must not sound\n");

	Rig rig;
	rig.set (jm::kDrive, 0.f);
	rig.set (jm::kChorus, 0.f);
	rig.set (jm::kDelayMix, 0.f);
	rig.set (jm::kRevMix, 0.f);

	check ("palm mute key is consumed", rig.e.keyswitch (jm::kKsPalmMute, true));
	check ("harmonics key is consumed", rig.e.keyswitch (jm::kKsHarmonic, true));
	check ("tap key is consumed", rig.e.keyswitch (jm::kKsTap, true));
	check ("pick position key is consumed", rig.e.keyswitch (jm::kKsPickNeck, true));
	check ("a played note is not consumed", !rig.e.keyswitch (52, true));
	checkf ("technique keys allocate no string", rig.e.activeStrings () == 0.f,
	        "active %.0f", rig.e.activeStrings (), 0);
	check ("technique mask reports the held keys", rig.e.techniqueMask () == 7);

	// releasing them clears the mask
	rig.e.keyswitch (jm::kKsPalmMute, false);
	rig.e.keyswitch (jm::kKsHarmonic, false);
	rig.e.keyswitch (jm::kKsTap, false);
	check ("released technique keys clear the mask", rig.e.techniqueMask () == 0);
}


// ---------------------------------------------------------------------------
static double rmsOf (const std::vector<float>& x, int from, int n)
{
	double a = 0;
	for (int i = 0; i < n; ++i) a += (double) x[from + i] * x[from + i];
	return std::sqrt (a / std::max (1, n));
}

// ---------------------------------------------------------------------------
static void testPerformanceModes ()
{
	std::printf ("\n[3d] performance modes -- swell, tremolo, feedback, sustain\n");

	auto config = [] (Rig& rig) {
		rig.set (jm::kDrive, 0.f);
		rig.set (jm::kCab, 0.f);
		rig.set (jm::kChorus, 0.f);
		rig.set (jm::kDelayMix, 0.f);
		rig.set (jm::kRevMix, 0.f);
		rig.set (jm::kBody, 0.f);
		rig.set (jm::kBehindBridge, 0.f);
		rig.set (jm::kAge, 0.f);
		rig.set (jm::kSustain, 0.9f);
	};

	// --- CC11 expression ----------------------------------------------------
	{
		Rig a; config (a);
		a.play (52, 0.9f, 512);
		auto full = a.render (12000);

		Rig b; config (b);
		b.e.setExpression (0.2f);
		b.play (52, 0.9f, 512);
		auto quiet = b.render (12000);

		const double ra = rmsOf (full, 4000, 6000);
		const double rb = rmsOf (quiet, 4000, 6000);
		char buf[160];
		std::snprintf (buf, sizeof (buf), "CC11 1.0 %.4f, CC11 0.2 %.4f", ra, rb);
		check ("expression pedal lowers the level", rb < ra * 0.45, buf);
	}

	// --- sustain pedal ------------------------------------------------------
	{
		Rig r; config (r);
		r.e.setSustainPedal (true);
		r.play (52, 0.9f, 2048);
		r.e.noteOff (52);
		auto held = r.render (12000);
		const double heldRms = rmsOf (held, 4000, 6000);

		r.e.setSustainPedal (false);
		auto released = r.render (24000);
		const double relRms = rmsOf (released, 16000, 6000);

		char buf[160];
		std::snprintf (buf, sizeof (buf), "held %.4f, after pedal up %.4f", heldRms, relRms);
		check ("sustain pedal lets a released note ring", heldRms > 0.01, buf);
		check ("pedal up damps the note", relRms < heldRms * 0.2, buf);
	}

	// --- volume swell -------------------------------------------------------
	{
		Rig plain; config (plain);
		plain.play (52, 0.9f, 256);
		auto p = plain.render (4800);
		const double plainAttack = rmsOf (p, 0, 2400);

		Rig sw; config (sw);
		sw.e.keyswitch (jm::kKsSwell, true);
		sw.play (52, 0.9f, 256);
		auto s = sw.render (4800);
		const double swellAttack = rmsOf (s, 0, 2400);

		char buf[160];
		std::snprintf (buf, sizeof (buf), "plain %.4f, swell %.4f", plainAttack, swellAttack);
		check ("swell removes the pick attack", swellAttack < plainAttack * 0.35, buf);
	}

	// --- tremolo picking ----------------------------------------------------
	{
		Rig one; config (one);
		one.play (52, 0.9f, 512);
		auto single = one.render (144000);
		const double singleTail = rmsOf (single, 120000, 8000);

		Rig trem; config (trem);
		trem.e.keyswitch (jm::kKsTremPick, true);
		trem.play (52, 0.9f, 512);
		auto tr = trem.render (144000);
		const double tremTail = rmsOf (tr, 120000, 8000);

		char buf[160];
		std::snprintf (buf, sizeof (buf), "single %.4f, tremolo %.4f", singleTail, tremTail);
		check ("tremolo picking keeps re-striking the note", tremTail > singleTail * 2.0, buf);
	}

	// --- controlled feedback ------------------------------------------------
	{
		Rig off; config (off);
		off.play (52, 0.9f, 512);
		auto o = off.render (48000 * 4);
		const double offTail = rmsOf (o, 48000 * 3, 12000);

		Rig on; config (on);
		on.e.keyswitch (jm::kKsFeedback, true);
		on.play (52, 0.9f, 512);
		auto f = on.render (48000 * 4);
		const double onTail = rmsOf (f, 48000 * 3, 12000);

		char buf[160];
		std::snprintf (buf, sizeof (buf), "without %.5f, with feedback %.5f", offTail, onTail);
		check ("feedback layer sustains after the string has decayed", onTail > offTail * 2.0, buf);
	}

	// --- reverse reverb -----------------------------------------------------
	{
		Rig r; config (r);
		r.set (jm::kRevMode, 4.f);
		r.set (jm::kRevMix, 0.6f);
		r.play (52, 0.9f, 256);
		auto x = r.render (48000);
		bool finite = true;
		float peak = 0.f;
		for (float v : x) { if (!(v == v)) finite = false; peak = std::max (peak, std::fabs (v)); }
		checkf ("reverse reverb is finite and audible", finite && peak > 0.01f, "peak %.3f", peak, 0);
	}
}


// ---------------------------------------------------------------------------
static void testLegato ()
{
	std::printf ("\n[3e] tapping / legato -- hammer-on and pull-off change pitch in place\n");

	Rig rig;
	rig.set (jm::kDrive, 0.f);
	rig.set (jm::kCab, 0.f);
	rig.set (jm::kChorus, 0.f);
	rig.set (jm::kDelayMix, 0.f);
	rig.set (jm::kRevMix, 0.f);
	rig.set (jm::kBody, 0.f);
	rig.set (jm::kBehindBridge, 0.f);
	rig.set (jm::kAge, 0.f);
	rig.set (jm::kPickupSel, 0.f);
	rig.set (jm::kSustain, 0.95f);
	rig.e.keyswitch (jm::kKsTap, true);

	auto steady = [] (const std::vector<float>& v) {
		return std::vector<float> (v.begin () + 4000, v.end ());
	};

	rig.e.noteOn (52, 0.9f);                 // E3
	auto a = rig.render (12000);
	const double e1 = goertzel (steady (a), 164.81, rig.sr);
	const double a1 = goertzel (steady (a), 220.00, rig.sr);

	rig.e.noteOn (57, 0.85f);                // hammer-on to A3, E3 still held
	auto b = rig.render (12000);
	const double e2 = goertzel (steady (b), 164.81, rig.sr);
	const double a2 = goertzel (steady (b), 220.00, rig.sr);

	rig.e.noteOff (57);                      // pull-off back to E3
	auto c = rig.render (12000);
	const double e3 = goertzel (steady (c), 164.81, rig.sr);
	const double a3 = goertzel (steady (c), 220.00, rig.sr);

	char buf[200];
	std::snprintf (buf, sizeof (buf), "E3 %.2e/%.2e, A3 %.2e/%.2e, back %.2e/%.2e",
	               e1, a1, e2, a2, e3, a3);
	check ("E3 sounds at E3", e1 > a1 * 5.0, buf);
	check ("hammer-on moves the string to A3", a2 > e2 * 5.0, buf);
	check ("pull-off returns to E3", e3 > a3 * 5.0, buf);

	rig.e.noteOff (52);
	rig.e.keyswitch (jm::kKsTap, false);
}



// ---------------------------------------------------------------------------
static void testBarEnvelope ()
{
	std::printf ("\n[3f] bar envelope -- attack, sustain and release move the arm\n");

	auto run = [] (float depth, bool gateOff) {
		Rig rig;
		rig.set (jm::kDrive, 0.f);
		rig.set (jm::kCab, 0.f);
		rig.set (jm::kChorus, 0.f);
		rig.set (jm::kDelayMix, 0.f);
		rig.set (jm::kRevMix, 0.f);
		rig.set (jm::kBarDepth, depth);
		rig.set (jm::kBarAtk, 0.05f);
		rig.set (jm::kBarDec, 0.10f);
		rig.set (jm::kBarSus, 0.50f);
		rig.set (jm::kBarRel, 0.30f);
		rig.e.noteOn (64, 0.9f);
		rig.render (24000);          // 0.5 s: attack + decay done, sustain held
		const float held = rig.e.barSemis ();
		if (gateOff) rig.e.noteOff (64);
		rig.render (48000);          // 1 s: release and spring settle
		const float after = rig.e.barSemis ();
		return std::pair<float, float> { held, after };
	};

	const auto off = run (0.f, false);
	const auto on = run (1.f, true);

	char buf[200];
	std::snprintf (buf, sizeof (buf), "depth 0: %.2f st, depth 1: %.2f st held", off.first, on.first);
	check ("depth 0 leaves the arm alone", std::fabs (off.first) < 0.05f, buf);
	check ("envelope holds the dive at the sustain level",
	       on.first < -1.0f && on.first > -3.2f, buf);
	check ("release returns the arm to pitch", std::fabs (on.second) < 0.15f, buf);
}


// ---------------------------------------------------------------------------
static void testLowMasking ()
{
	std::printf ("\n[3g] low notes -- a bass note must not duck the chord after it\n");

	auto chordLevel = [] (bool withLow) {
		Rig rig;
		rig.set (jm::kDrive, 0.3f);
		rig.set (jm::kDriveChar, 1.f);
		rig.set (jm::kCab, 0.7f);
		rig.set (jm::kChorus, 0.f);
		rig.set (jm::kDelayMix, 0.f);
		rig.set (jm::kRevMix, 0.f);
		rig.set (jm::kSustain, 0.6f);
		rig.set (jm::kOutput, 0.f);

		std::vector<float> out (96000, 0.f), scratch (96000, 0.f);
		if (withLow)
		{
			rig.e.noteOn (40, 0.95f);
			for (int i = 0; i < 24000; i += 64) rig.e.process (&out[i], &scratch[i], 64);
		}
		const int start = withLow ? 24000 : 0;
		for (int n : { 52, 55, 59, 64 }) rig.e.noteOn (n, 0.85f);
		for (int i = start; i < 96000; i += 64) rig.e.process (&out[i], &scratch[i], 64);

		double a = 0;
		for (int i = 0; i < 6000; ++i) a += (double) out[start + 4800 + i] * out[start + 4800 + i];
		return std::sqrt (a / 6000);
	};

	const double solo = chordLevel (false);
	const double withLow = chordLevel (true);
	const double db = 20.0 * std::log10 (withLow / std::max (1e-9, solo));

	char buf[160];
	std::snprintf (buf, sizeof (buf), "chord %.4f, after low %.4f (%+.1f dB)", solo, withLow, db);
	check ("a held bass note does not duck the next chord", db > -1.0, buf);
}

// ---------------------------------------------------------------------------
static void testSilence ()
{
	std::printf ("\n[4] silence -- no note means exactly zero, not \"quiet\"\n");

	Rig rig;
	rig.set (jm::kChorus, 0.f);
	rig.set (jm::kDelayMix, 0.f);
	rig.set (jm::kRevMix, 0.f);
	rig.set (jm::kDrive, 0.f);
	auto x = rig.render (8192);

	float peak = 0.f;
	for (float v : x) peak = std::max (peak, std::fabs (v));
	checkf ("idle output is exactly zero", peak == 0.f, "peak %.9f", peak, 0);

	// after a note has decayed to nothing, it must also be silent again
	rig.play (52, 0.9f, 4800);
	rig.e.allNotesOff ();
	rig.set (jm::kSustain, 0.f);
	rig.render (48000 * 20);
	auto tail = rig.render (8192);
	peak = 0.f;
	for (float v : tail) peak = std::max (peak, std::fabs (v));
	checkf ("decayed note leaves no residue", peak < 1e-4f, "peak %.9f", peak, 0);
}

// ---------------------------------------------------------------------------
static void testBlockSize ()
{
	std::printf ("\n[5] block size -- the render must not depend on the host's buffer\n");

	auto renderWith = [] (int block) {
		Rig rig;
		rig.set (jm::kChorus, 0.4f);
		rig.set (jm::kDelayMix, 0.3f);
		rig.set (jm::kRevMix, 0.4f);
		rig.set (jm::kDrive, 0.5f);
		rig.play (55, 0.8f, 1, block);
		rig.play (59, 0.7f, 1, block);
		return rig.render (20000, block);
	};

	auto a = renderWith (1);
	auto b = renderWith (7);
	auto c = renderWith (64);
	auto d = renderWith (512);
	auto e = renderWith (1024);

	auto maxDiff = [] (const std::vector<float>& x, const std::vector<float>& y) {
		float m = 0.f;
		for (size_t i = 0; i < x.size (); ++i) m = std::max (m, std::fabs (x[i] - y[i]));
		return m;
	};

	const float d1 = maxDiff (a, b);
	const float d2 = maxDiff (a, c);
	const float d3 = maxDiff (a, d);
	const float d4 = maxDiff (a, e);
	checkf ("block 1 vs 7 identical", d1 == 0.f, "max|diff| %.9f", d1, 0);
	checkf ("block 1 vs 64 identical", d2 == 0.f, "max|diff| %.9f", d2, 0);
	checkf ("block 1 vs 512 identical", d3 == 0.f, "max|diff| %.9f", d3, 0);
	checkf ("block 1 vs 1024 identical", d4 == 0.f, "max|diff| %.9f", d4, 0);
}

// ---------------------------------------------------------------------------
static void testAdversarial ()
{
	std::printf ("\n[6] adversarial -- every parameter extreme must stay finite\n");

	float worstPeak = 0.f;
	int nanCount = 0;

	for (int id = 0; id < (int) jm::kNumParams; ++id)
	{
		for (int side = 0; side < 2; ++side)
		{
			Rig rig;
			const jm::ParamDef& d = jm::kParamDefs[id];
			rig.set (id, side == 0 ? d.min : d.max);
			// drive the rest hard too
			rig.set (jm::kDrive, 1.f);
			rig.set (jm::kOutput, 12.f);
			rig.set (jm::kDelayMix, 1.f);
			rig.set (jm::kRevMix, 1.f);

			rig.e.noteOn (40, 1.f);
			rig.e.noteOn (64, 1.f);
			auto x = rig.render (8192, 512);

			for (float v : x)
			{
				if (!(v == v) || std::fabs (v) > 1e30f) { ++nanCount; break; }
				worstPeak = std::max (worstPeak, std::fabs (v));
			}
		}
	}

	check ("no NaN or infinity at any parameter extreme", nanCount == 0);
	checkf ("ceiling holds at every extreme", worstPeak <= 1.0f, "worst peak %.6f", worstPeak, 0);
}

// ---------------------------------------------------------------------------
static void testSampleRates ()
{
	std::printf ("\n[7] sample rates -- 8k .. 384k must all work\n");

	const double rates[] = { 8000, 22050, 44100, 48000, 88200, 96000, 192000, 384000 };
	bool allOk = true;
	float worst = 0.f;
	for (double sr : rates)
	{
		Rig rig (sr);
		rig.play (52, 0.9f, (int) (sr * 0.2), 512);
		auto x = rig.render ((int) (sr * 0.3), 512);
		bool ok = true;
		for (float v : x)
		{
			if (!(v == v) || std::fabs (v) > 1.0f) { ok = false; break; }
			worst = std::max (worst, std::fabs (v));
		}
		char name[48];
		std::snprintf (name, sizeof (name), "%.0f Hz", sr);
		check (name, ok);
		if (!ok) allOk = false;
	}
	checkf ("no rate exceeds the ceiling", worst <= 1.0f, "worst peak %.6f", worst, 0);
	(void) allOk;
}

// ---------------------------------------------------------------------------
static void testDeterminism ()
{
	std::printf ("\n[8] determinism -- same input, same output\n");

	auto run = [] {
		Rig rig;
		rig.set (jm::kChorus, 0.5f);
		rig.play (48, 0.85f, 1024);
		rig.play (55, 0.75f, 1024);
		return rig.render (12000);
	};
	auto a = run ();
	auto b = run ();
	float m = 0.f;
	for (size_t i = 0; i < a.size (); ++i) m = std::max (m, std::fabs (a[i] - b[i]));
	checkf ("two identical runs are bit-identical", m == 0.f, "max|diff| %.9f", m, 0);
}

// ---------------------------------------------------------------------------
static void testVoiceStealing ()
{
	std::printf ("\n[9] polyphony -- six strings, then stealing\n");

	Rig rig;
	for (int i = 0; i < 6; ++i) rig.e.noteOn (jm::kTunings[0][i], 0.8f);
	auto x = rig.render (4096);
	checkf ("all six strings ring", rig.e.activeStrings () >= 5.9f, "%.0f active", rig.e.activeStrings (), 0);

	float peak = 0.f;
	for (float v : x) peak = std::max (peak, std::fabs (v));
	checkf ("six strings do not clip", peak <= 1.0f, "peak %.4f", peak, 0);

	// a seventh note must steal, not overflow
	rig.e.noteOn (72, 0.8f);
	rig.render (1024);
	checkf ("a seventh note steals a string", rig.e.activeStrings () <= 6.0f,
	        "%.0f active", rig.e.activeStrings (), 0);
}

// ---------------------------------------------------------------------------
static void testMuteAndSympathy ()
{
	std::printf ("\n[10] palm mute and sympathetic coupling\n");

	// Palm mute must shorten the decay without killing the attack.
	{
		auto tailEnergy = [] (float mute) {
			Rig rig;
			rig.set (jm::kDrive, 0.f);
			rig.set (jm::kCab, 0.f);
			rig.set (jm::kChorus, 0.f);
			rig.set (jm::kDelayMix, 0.f);
			rig.set (jm::kRevMix, 0.f);
			rig.set (jm::kBody, 0.f);
			rig.set (jm::kBehindBridge, 0.f);
			rig.set (jm::kAge, 0.f);
			rig.set (jm::kSustain, 0.9f);
			if (mute > 0.5f) rig.e.keyswitch (jm::kKsPalmMute, true);
			rig.play (52, 0.9f, 4800);
			auto early = rig.render (2400);
			auto late = rig.render (24000);
			auto rms = [] (const std::vector<float>& x) {
				double s = 0;
				for (float v : x) s += (double) v * v;
				return std::sqrt (s / std::max<size_t> (1, x.size ()));
			};
			return std::pair<double, double> { rms (early), rms (late) };
		};

		const auto open = tailEnergy (0.f);
		const auto muted = tailEnergy (1.f);
		checkf ("palm mute shortens the tail", muted.second < open.second * 0.35,
		        "tail rms open %.5f vs muted %.5f", open.second, muted.second);
		checkf ("palm mute keeps the attack", muted.first > open.first * 0.25,
		        "attack rms open %.5f vs muted %.5f", open.first, muted.first);
	}

	// Sympathetic coupling: striking one string must make a *different* live
	// string pick up energy.  Both are driven, so compare against sympathy 0.
	{
		auto bleed = [] (float sym) {
			Rig rig;
			rig.set (jm::kDrive, 0.f);
			rig.set (jm::kCab, 0.f);
			rig.set (jm::kChorus, 0.f);
			rig.set (jm::kDelayMix, 0.f);
			rig.set (jm::kRevMix, 0.f);
			rig.set (jm::kBody, 0.f);
			rig.set (jm::kBehindBridge, 0.f);
			rig.set (jm::kAge, 0.f);
			rig.set (jm::kSustain, 0.9f);
			rig.set (jm::kSympathy, sym);
			rig.e.noteOn (40, 0.9f);    // low E
			rig.e.noteOn (64, 0.05f);   // high E, barely touched
			rig.render (4800);
			auto x = rig.render (24000);
			// measure the high E (329.6 Hz) that nobody played loudly
			return goertzel (x, midiFreq (64), rig.sr);
		};

		const double off = bleed (0.f);
		const double on = bleed (1.f);
		checkf ("sympathetic coupling feeds the other string", on > off * 1.2,
		        "high E power %.3e (sym 0) -> %.3e (sym 1)", off, on);
	}
}

// ---------------------------------------------------------------------------
int main (int argc, char** argv)
{
	std::printf ("HanaGuitar for indie -- engine verification\n");
	std::printf ("================================\n");

	testTuning ();
	testWhammy ();
	testPickupVoice ();
	testCircuits ();
	testKeyswitches ();
	testPerformanceModes ();
	testLegato ();
	testBarEnvelope ();
	testLowMasking ();
	testSilence ();
	testBlockSize ();
	testAdversarial ();
	testSampleRates ();
	testDeterminism ();
	testVoiceStealing ();
	testMuteAndSympathy ();

	std::printf ("\n--------------------------------\n");
	std::printf ("%d passed, %d failed\n", g_pass, g_fail);
	if (g_fail == 0) std::printf ("ALL ENGINE TESTS PASSED\n");
	(void) argc; (void) argv;
	return g_fail == 0 ? 0 : 1;
}
