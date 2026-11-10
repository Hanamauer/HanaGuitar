// HanaGuitar for indie -- tremolo arm ("whammy") physical model
//
// The Jazzmaster's floating tremolo is modelled as a damped 2nd order
// spring/mass system.  The performer does not set the arm angle directly;
// they apply a torque, and the arm accelerates, overshoots and settles.
// That is what gives the characteristic "springy" wobble and the natural
// return-to-pitch after a dive.  On top of that we add
//   * flutter  - the bar's own high frequency resonance, excited by playing
//   * glide    - a slow organic pitch drift (shoegaze "glide guitar")
#pragma once

#include "common.h"

namespace jm {

class WhammyModel
{
public:
	void prepare (double sampleRate)
	{
		fs = (float) sampleRate;
		dt = 1.f / fs;
		// flutter resonator coefficients are rate independent
		float wf = kTwoPi * 11.5f / fs;
		float r = 0.9965f;
		flutC1 = 2.f * r * std::cos (wf);
		flutC2 = -r * r;
		reset ();
	}

	void reset ()
	{
		target = 0.f;
		angle = 0.f;
		vel = 0.f;
		flut1 = flut2 = 0.f;
		flutterEnergy = 0.f;
		glidePhase = 0.f;
		glidePhase2 = 0.37f;
		outSemis = 0.f;
		outRatio = 1.f;
		outAngle = 0.f;
	}

	void setParams (float rangeSemis, float springHz, float flutterAmt,
	                float glideRateHz, float glideDepthAmt)
	{
		range = clampf (rangeSemis, 0.f, 12.f);
		spring = clampf (springHz, 0.5f, 12.f);
		flutter = clampf (flutterAmt, 0.f, 1.f);
		glideRate = clampf (glideRateHz, 0.03f, 4.f);
		glideDepth = clampf (glideDepthAmt, 0.f, 1.f);
	}

	// bar target: 0 = rest, 1 = full dive, negative = pull up
	void setTarget (float t) { target = clampf (t, -0.35f, 1.f); }
	void addEnergy (float e) { flutterEnergy += e; }

	inline void tick ()
	{
		// ---- spring / mass / damper -------------------------------------
		float w = kTwoPi * spring;
		// Damping ratio.  A bare Jazzmaster plate is lightly damped, but the
		// player's hand rests on the arm and the strings load it, so the real
		// thing settles with a modest overshoot rather than ringing like a
		// tuning fork.  zeta 0.32..0.52 gives ~20..12 % overshoot, which is
		// what a hand-damped arm actually does.  Stiffer springs (heavier
		// string gauges) settle faster, hence the scale with `spring`.
		float zeta = 0.32f + 0.20f * clampf (spring / 12.f, 0.f, 1.f);
		float acc = w * w * (target - angle) - 2.f * zeta * w * vel;
		vel += acc * dt;
		angle += vel * dt;

		// hard stops at both ends of the arm travel
		if (angle > 1.f)
		{
			angle = 1.f;
			if (vel > 0.f) vel *= -0.35f;
		}
		else if (angle < -0.35f)
		{
			angle = -0.35f;
			if (vel < 0.f) vel *= -0.35f;
		}

		// ---- bar flutter ------------------------------------------------
		flutterEnergy *= 0.99965f;
		if (flutterEnergy > 1.f) flutterEnergy = 1.f;
		float exc = rng.bipolar () * flutter * (0.0015f + flutterEnergy * 0.05f);
		float fy = exc + flutC1 * flut1 + flutC2 * flut2;
		flut2 = flut1;
		flut1 = fy;
		flushDenorm (flut1);
		flushDenorm (flut2);

		// ---- slow glide drift -------------------------------------------
		glidePhase += glideRate / fs;
		if (glidePhase >= 1.f) glidePhase -= 1.f;
		glidePhase2 += glideRate * 1.371f / fs;
		if (glidePhase2 >= 1.f) glidePhase2 -= 1.f;
		float glide = 0.f;
		if (glideDepth > 0.0001f)
			glide = glideDepth * range * 0.30f *
			        (0.62f * std::sin (kTwoPi * glidePhase) +
			         0.38f * std::sin (kTwoPi * glidePhase2 + 1.1f));

		outSemis = -range * angle + glide;
		outAngle = angle + flut1 * 0.35f;
		outRatio = std::pow (2.f, outSemis * (1.f / 12.f));
	}

	inline float semitones () const { return outSemis; }
	inline float ratio () const { return outRatio; }
	inline float barAngle () const { return outAngle; }
	inline float targetValue () const { return target; }

	// Damping ratio actually in use, so a test can compare the measured
	// overshoot against the closed form exp(-pi*zeta/sqrt(1-zeta^2)).
	inline float damping () const { return 0.32f + 0.20f * clampf (spring / 12.f, 0.f, 1.f); }

	Rng rng { 0x51ED270Bu };

private:
	float fs = 48000.f, dt = 1.f / 48000.f;
	float range = 2.5f, spring = 4.5f, flutter = 0.25f;
	float glideRate = 0.35f, glideDepth = 0.f;
	float target = 0.f, angle = 0.f, vel = 0.f;
	float flut1 = 0.f, flut2 = 0.f, flutC1 = 0.f, flutC2 = 0.f;
	float flutterEnergy = 0.f;
	float glidePhase = 0.f, glidePhase2 = 0.f;
	float outSemis = 0.f, outRatio = 1.f, outAngle = 0.f;
};

} // namespace jm
