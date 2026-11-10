// HanaGuitar for indie -- the two circuits
//
// A Jazzmaster is not one circuit.  It has a lead circuit (both pickups, a 1M
// volume and a 1M tone with a 0.033 uF cap, which is why it stays bright when
// you roll the volume down) and a rhythm circuit on the upper bout: neck pickup
// only, 50k pots and a 0.02 uF cap, which is dark, quiet and woody.
//
// The audible consequences that matter here:
//
//   * lead, tone down     -> a first-order-ish treble cut from ~6 kHz to ~600 Hz
//   * lead, volume down   -> the cable capacitance loads the pot: the sound gets
//                            duller as well as quieter (no treble bleed)
//   * rhythm              -> neck pickup only, ~1.2..2.6 kHz ceiling, lower level,
//                            a little extra low-mid weight
//
// This is a filter/level model of that network, not a SPICE netlist, but every
// control does what the real one does.
#pragma once

#include "common.h"

namespace jm {

class Circuit
{
public:
	void prepare (float sampleRate)
	{
		fs = sampleRate;
		reset ();
		rebuild (1.f, 0.6f, 0.55f, false);
	}

	void reset ()
	{
		toneLp.reset ();
		loadLp.reset ();
		bodyLp.reset ();
	}

	void setParams (float volume, float tone, float rhythmTone, bool rhythm)
	{
		volume = clampf (volume, 0.f, 1.f);
		tone = clampf (tone, 0.f, 1.f);
		rhythmTone = clampf (rhythmTone, 0.f, 1.f);
		if (std::fabs (volume - v_) > 2e-4f || std::fabs (tone - t_) > 2e-4f ||
		    std::fabs (rhythmTone - rt_) > 2e-4f || rhythm != rhythm_)
			rebuild (volume, tone, rhythmTone, rhythm);
	}

	inline float process (float x)
	{
		x = bodyLp.process (x);
		x = toneLp.process (x);
		x = loadLp.process (x);
		return x * gain_;
	}

private:
	void rebuild (float volume, float tone, float rhythmTone, bool rhythm)
	{
		v_ = volume; t_ = tone; rt_ = rhythmTone; rhythm_ = rhythm;

		if (rhythm)
		{
			// 50k pots and a 0.02 uF cap: a much lower ceiling, and the pot
			// still works as a treble control inside that narrow window.
			gain_ = 0.42f * std::pow (volume, 1.2f);
			toneLp.setLowpass (900.f + 1700.f * rhythmTone, 0.85f, fs);
			loadLp.setLowpass (9000.f, 0.7f, fs);
			bodyLp.setLowShelf (190.f, 3.5f, fs);
		}
		else
		{
			gain_ = 0.95f * std::pow (volume, 1.35f);
			toneLp.setLowpass (550.f + 5400.f * std::pow (tone, 1.5f), 0.75f, fs);
			// 1M pot + cable capacitance: full up is open, rolled down is dark.
			loadLp.setLowpass (800.f + 16000.f * volume * volume, 0.7f, fs);
			bodyLp.identity ();
		}
	}

	float fs = 48000.f;
	float gain_ = 1.f;
	float v_ = -1.f, t_ = -1.f, rt_ = -1.f;
	bool rhythm_ = false;
	Biquad toneLp, loadLp, bodyLp;
};

} // namespace jm
