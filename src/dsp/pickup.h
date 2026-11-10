// HanaGuitar for indie -- pickup models
//
// Two pickups, because the guitar in the artwork has two very different ones:
//
//   neck   P90 soapbar.  One wide coil, high inductance, a strong upper-mid
//          peak and a fat low end.  It is the reason the rhythm circuit sounds
//          like a jazz box rather than a strat.
//   bridge humbucker.  Two narrower coils in series, 10 mm apart.  Higher
//          output, lower resonance, and the coil spacing puts a gentle comb on
//          top so it does not simply sound like a louder P90.
//
// The coil model is the same aperture average the old single-coil used: a
// pickup senses the string over the width of the coil, and that width is what
// puts the notches in the spectrum.  The resonant peak is a biquad on the
// *summed* string signal, which is where it belongs electrically -- the pickup
// sees all six strings through one inductance.
#pragma once

#include "common.h"
#include "string.h"

namespace jm {

class Pickup
{
public:
	void prepare (float sampleRate)
	{
		fs = sampleRate;
		// P90: peaky and bright-ish; humbucker: lower and broader.
		p90Res.setPeak (4600.f, 1.9f, 6.0f, fs);
		hbRes.setPeak (3250.f, 1.15f, 5.0f, fs);
		hbLp.setLowpass (4300.f, 0.7f, fs);
		hp.setHighpass (75.f, 0.7f, fs);
		reset ();
	}

	void reset ()
	{
		p90Res.reset ();
		hbRes.reset ();
		hbLp.reset ();
		hp.reset ();
	}

	// Displacement averaged over a coil of width 2*ap, at fraction xi of the
	// speaking length (0 = bridge).
	static inline float coil (const WaveString& s, float xi, float ap)
	{
		xi = clampf (xi, 0.f, 0.995f);
		const float a = clampf (xi - ap, 0.f, 0.995f);
		const float b = clampf (xi + ap, 0.f, 0.995f);
		return 0.5f * (s.readPos (a) + s.readPos (b));
	}

	static inline float p90 (const WaveString& s, float xi)
	{
		return coil (s, xi, 0.021f);
	}

	static inline float humbucker (const WaveString& s, float xi)
	{
		return coil (s, xi - 0.007f, 0.019f) + coil (s, xi + 0.007f, 0.019f);
	}

	inline float shapeP90 (float x) { return p90Res.process (x) * 0.66f; }
	inline float shapeHB (float x)  { return hbLp.process (hbRes.process (x)) * 0.58f; }
	inline float dcBlock (float x)  { return hp.process (x); }

	// Positions as a fraction of the open scale length, measured from the
	// bridge.  The neck P90 sits where a Jazzmaster neck pickup sits; the
	// bridge humbucker is a little closer to the bridge than the old single
	// coil was.
	static constexpr float kNeckPos = 0.275f;
	static constexpr float kBridgePos = 0.115f;

private:
	float fs = 48000.f;
	Biquad p90Res, hbRes, hbLp, hp;
};

} // namespace jm
