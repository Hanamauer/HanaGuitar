// HanaGuitar for indie -- body, bridge and behind-the-bridge resonance
//
// Three things a Jazzmaster does that a plain waveguide does not:
//
//  1. The offset body is a thin slab with a big rout under the pickguard.  It
//     rings at low frequencies and that ring is a large part of what people
//     call the "JM thump".
//  2. The bridge has a set of long, un-damped string lengths behind it.  They
//     are short enough to be audible as a high, metallic zing that follows the
//     note, and they are why an offset chimes on chords where a Les Paul does
//     not.
//  3. The saddles are threaded, so the string sits on threads.  Light playing
//     produces a subtle rattle; hard playing produces the buzz that offset
//     players either fight or lean into.
#pragma once

#include "common.h"

namespace jm {

// ---------------------------------------------------------------- resonator
// A two-pole resonator with a settable T60, normalised so that an impulse
// through it produces a predictable peak.  Cheaper and far more stable than
// trying to build a modal body out of tuned delay lines.
struct Resonator
{
	float b0 = 0.f, a1 = 0.f, a2 = 0.f;
	float z1 = 0.f, z2 = 0.f;

	void set (float freq, float t60, float sr)
	{
		const float f = clampf (freq, 20.f, sr * 0.45f);
		const float w = kTwoPi * f / sr;
		// r is the per-sample pole radius that yields the requested T60
		const float r = clampf (std::pow (10.f, -3.f / std::max (0.005f, t60 * sr)), 0.f, 0.999999f);
		a1 = -2.f * r * std::cos (w);
		a2 = r * r;
		// gain such that a unit impulse gives roughly unit peak
		b0 = (1.f - r) * std::sqrt (std::max (0.02f, 1.f - r * r));
	}

	inline float process (float x)
	{
		const float y = b0 * x + z1;
		z1 = -a1 * y + z2;
		z2 = -a2 * y;
		flushDenorm (z1);
		flushDenorm (z2);
		return y;
	}

	void reset () { z1 = z2 = 0.f; }
};

class BodyModel
{
public:
	void prepare (float sampleRate)
	{
		fs = sampleRate;
		boxA.setPeak (240.f, 1.15f, 5.0f, fs);
		boxB.setPeak (430.f, 1.00f, 3.0f, fs);
		updateModes ();
		reset ();
	}

	void reset ()
	{
		for (int i = 0; i < kModes; ++i) mode[i].reset ();
		boxA.reset ();
		boxB.reset ();
		btb1 = btb2 = 0.f;
		buzzLp = 0.f;
		buzzEnv = 0.f;
	}

	void setAmount (float amt) { bodyAmt = clampf (amt, 0.f, 1.f); updateModes (); }
	void setBehindBridge (float amt) { btbAmt = clampf (amt, 0.f, 1.f); }
	void setBuzz (float amt) { buzzAmt = clampf (amt, 0.f, 1.f); }

	// Driven by the bridge force: the harder the attack, the more the body and
	// the saddle rattle are excited.
	inline void excite (float amplitude, float hardness)
	{
		const float drive = amplitude * (0.30f + 0.70f * clampf (hardness, 0.f, 1.f));
		exciteLevel = drive;
		buzzEnv = std::max (buzzEnv, drive * buzzAmt);
	}

	inline float process (float in)
	{
		float out = in;

		if (bodyAmt > 0.0005f)
		{
			// Two things make the offset body audible:
			//
			//  1. a pair of fixed cavity resonances (an EQ, not a feedback path),
			//     which is the hollow "box" colour around 240 / 430 Hz; and
			//  2. the modal ring from the pluck, which decays on its own.
			//
			// The earlier version drove the modal bank continuously from the
			// string sum: at the fundamental the summed modes arrived out of
			// phase and *cancelled* the dry signal, so turning Body up made the
			// guitar quieter.  EQ adds energy at the cavity frequencies without
			// touching the phase of everything else.
			const float ca = boxA.process (in);
			const float cb = boxB.process (in);
			out += ((ca - in) * 0.9f + (cb - in) * 0.6f) * bodyAmt;

			if (exciteLevel > 1e-6f)
			{
				float ring = 0.f;
				for (int i = 0; i < kModes; ++i)
					ring += mode[i].process (exciteLevel * amp[i]) * outAmp[i];
				out += ring * bodyAmt;
				exciteLevel = 0.f;
			}
		}

		if (btbAmt > 0.0005f)
		{
			// Two short taps behind the bridge.  Their lengths are fixed, so the
			// zing does not transpose with the note -- it sounds like a separate
			// piece of metal, which is exactly what it is.
			const float tapA = btb1 * 0.60f + in * 0.35f;
			const float tapB = btb2 * 0.45f + btb1 * 0.35f;
			btb2 = btb1;
			btb1 = tapA;
			const float zing = tapB - tapA * 0.40f;
			out += zing * btbAmt * 0.45f;
		}

		if (buzzEnv > 0.0005f)
		{
			const float n = rng.bipolar () * buzzEnv;
			buzzEnv *= 0.9993f;
			const float hp = n - buzzLp;
			buzzLp += 0.30f * (n - buzzLp);
			out += softSat (hp * 3.0f) * 0.05f;
			flushDenorm (buzzLp);
			flushDenorm (buzzEnv);
		}

		return out;
	}

	Rng rng { 0x1F3D5B79u };

private:
	void updateModes ()
	{
		// A slab-body offset: the first mode is the plate thump, the next two
		// are the neck/body coupling that gives the low-mid push.
		static const float f[kModes] = { 132.f, 205.f, 318.f, 470.f, 690.f, 980.f, 238.f, 356.f };
		static const float t[kModes] = { 0.55f, 0.38f, 0.26f, 0.18f, 0.12f, 0.09f, 1.15f, 0.85f };
		static const float a[kModes] = { 1.00f, 0.66f, 0.46f, 0.31f, 0.21f, 0.14f, 0.55f, 0.38f };
		for (int i = 0; i < kModes; ++i)
		{
			freq[i] = f[i];
			amp[i] = a[i];
			mode[i].set (f[i], t[i], fs);
			// A resonator's impulse peak is roughly b0/(1-r); undo that so the
			// relative mode amplitudes above are what actually come out.
			const float r = std::sqrt (std::max (1e-9f, mode[i].a2));
			outAmp[i] = clampf (1.f / std::max (0.02f, mode[i].b0 / (1.f - r)), 0.f, 40.f) * 0.26f;
		}
	}

	static constexpr int kModes = 8;
	Resonator mode[kModes];
	float freq[kModes] {};
	float amp[kModes] {};
	float outAmp[kModes] {};

	float fs = 48000.f;
	Biquad boxA, boxB;
	float bodyAmt = 0.55f;
	float btbAmt = 0.30f;
	float buzzAmt = 0.15f;
	float exciteLevel = 0.f;
	float btb1 = 0.f, btb2 = 0.f;
	float buzzLp = 0.f, buzzEnv = 0.f;
};

} // namespace jm
