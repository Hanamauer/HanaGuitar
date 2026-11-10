// HanaGuitar for indie -- controlled feedback
//
// A guitar feeding back is not a distortion pedal: the string keeps ringing,
// the amp keeps pushing it, and the note slowly trades its fundamental for a
// harmonic while the pitch floats a few cents.  The model here is a small bank
// of harmonics locked to the note that is sounding, driven by an envelope that
// follows the string, with slow drift so it never sounds like an oscillator
// held at a fixed pitch.  It is a layer on top of the strings, not a replacement
// for them, so palm mutes and note-offs still work.
#pragma once

#include "common.h"

namespace jm {

class FeedbackModel
{
public:
	void prepare (float sampleRate)
	{
		fs = sampleRate;
		reset ();
	}

	void reset ()
	{
		for (int i = 0; i < 3; ++i) phase[i] = 0.f;
		env = 0.f;
		gain = 0.f;
		drift = 0.f;
		driftTarget = 0.f;
		pitch = 110.f;
	}

	void setPitch (float f) { pitch = clampf (f, 30.f, 2000.f); }

	void setActive (bool on) { active = on; }

	inline float process (float input, bool on)
	{
		active = on;

		// envelope: fast to open, slow to close -- the amp takes a moment to
		// take over, and it lets go slowly.
		const float level = std::fabs (input);
		const float a = (level > env) ? 0.0025f : 0.00018f;
		env += (level - env) * a;

		// Once the amp has taken over it keeps the string moving by itself: the
		// layer holds its level while the key is down instead of following the
		// string down, which is exactly what makes feedback a *sustain*.
		const float target = on ? 0.72f : 0.f;
		gain += (target - gain) * (on ? 0.00045f : 0.00030f);

		// slow pitch drift (+/- 6 cents) and a slow trade from the fundamental
		// to the second and third harmonic
		drift += (driftTarget - drift) * 0.00002f;
		if (rng.next () < 0.00002f) driftTarget = rng.bipolar () * 0.006f;

		const float f = pitch * (1.f + drift);
		const float w = kTwoPi * f / fs;
		float s = 0.f;
		const float harm[3] = { 1.f, 0.42f, 0.22f };
		for (int i = 0; i < 3; ++i)
		{
			phase[i] += f * (float) (i + 1) / fs;
			if (phase[i] >= 1.f) phase[i] -= 1.f;
			s += harm[i] * std::sin (kTwoPi * phase[i]);
		}
		return s * gain * 0.30f;
	}

	Rng rng { 0x2A17C3D5u };

private:
	float fs = 48000.f;
	float phase[3] = { 0.f, 0.f, 0.f };
	float env = 0.f;
	float gain = 0.f;
	float drift = 0.f, driftTarget = 0.f;
	float pitch = 110.f;
	bool active = false;
};

} // namespace jm
