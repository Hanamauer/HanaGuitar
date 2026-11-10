// HanaGuitar for indie -- amp, tone stack and cabinet
//
// Four characters, because the two scenes this instrument is for live on
// opposite ends of the same dial:
//
//   Clean       the Jazzmaster into a blackface-ish amp: lots of headroom, the
//               2 kHz spike of the pickups intact, breaks up only when hit hard.
//   Overdrive   mid-forward, the "midwest emo" clean-that-isn't: you can still
//               hear every string of a tapped arpeggio.
//   Fuzz        the shoegaze wall: asymmetric, octave-ish, compresses hard so
//               strummed chords turn into a single roaring block.
//   Muff        a big-muff-shaped curve: scooped mids, very long sustain, the
//               sound that makes a bend disappear into a chord.
//
// The cabinet is not an IR.  It is two resonant low passes plus a high pass --
// the four things a 1x12 actually does to a guitar signal -- and it is
// deliberately rate independent so the same numbers hold at 44.1 and 96 kHz.
#pragma once

#include "common.h"

namespace jm {

class Amp
{
public:
	void prepare (float sampleRate)
	{
		fs = sampleRate;
		rebuild ();
		reset ();
	}

	void reset ()
	{
		hpL.reset (); hpR.reset ();
		preL.reset (); preR.reset ();
		toneBL.reset (); toneBR.reset ();
		toneML.reset (); toneMR.reset ();
		toneHL.reset (); toneHR.reset ();
		presL.reset (); presR.reset ();
		cab1L.reset (); cab1R.reset ();
		cab2L.reset (); cab2R.reset ();
		cabHpL.reset (); cabHpR.reset ();
		sagEnv = 1.f;
		dcL = dcR = 0.f;
	}

	void setParams (float drive, int character, float bassDb, float midDb,
	                float trebleDb, float presence, float cabAmount)
	{
		drive_ = clampf (drive, 0.f, 1.f);
		char_ = character < 0 ? 0 : (character > 3 ? 3 : character);
		bassDb_ = clampf (bassDb, -12.f, 12.f);
		midDb_ = clampf (midDb, -12.f, 12.f);
		trebleDb_ = clampf (trebleDb, -12.f, 12.f);
		presence_ = clampf (presence, 0.f, 1.f);
		cab_ = clampf (cabAmount, 0.f, 1.f);
		rebuild ();
	}

	// pre-gain in dB, exposed for the drive display
	float driveGainDb () const
	{
		switch (char_)
		{
		case 0: return 2.f + drive_ * 14.f;
		case 1: return 6.f + drive_ * 26.f;
		case 2: return 14.f + drive_ * 30.f;
		default: return 12.f + drive_ * 32.f;
		}
	}

	inline void process (float& l, float& r)
	{
		// input coupling: keeps DC and sub-bass out of the clipper
		l = hpL.process (l);
		r = hpR.process (r);

		// pre-emphasis: a real amp's first stage tilts the input
		l = preL.process (l);
		r = preR.process (r);

		const float g = dbToGain (driveGainDb ());

		// sag: the supply droops under sustained drive and recovers.  This is
		// what makes a held chord breathe instead of sitting still.
		const float target = 1.f / (1.f + 0.55f * drive_ * std::fabs (l + r) * 0.5f);
		sagEnv += (target - sagEnv) * 0.00035f;

		l *= g * (0.72f + 0.28f * sagEnv);
		r *= g * (0.72f + 0.28f * sagEnv);

		switch (char_)
		{
		case 0:   // clean: soft, symmetric, almost no harmonic generation
			l = softSat (l * 0.55f) * 1.8f;
			r = softSat (r * 0.55f) * 1.8f;
			break;

		case 1:   // overdrive: asymmetric, mostly 2nd order
		{
			l = odShape (l);
			r = odShape (r);
			break;
		}

		case 2:   // fuzz: hard, asymmetric, with a gate-ish squaring
		{
			const float l2 = softSat (l * 1.6f);
			const float r2 = softSat (r * 1.6f);
			l = fuzzShape (l2);
			r = fuzzShape (r2);
			break;
		}

		default:  // muff: symmetric, heavy compression, scooped afterwards
		{
			l = softSat (softSat (l * 1.1f) * 2.6f);
			r = softSat (softSat (r * 1.1f) * 2.6f);
			break;
		}
		}

		// tone stack
		l = toneBL.process (l); r = toneBR.process (r);
		l = toneML.process (l); r = toneMR.process (r);
		l = toneHL.process (l); r = toneHR.process (r);
		l = presL.process (l);  r = presR.process (r);

		// cabinet: roll off the fizz, add the two box resonances
		if (cab_ > 0.0005f)
		{
			const float cl = cabHpL.process (l);
			const float cr = cabHpR.process (r);
			const float c1l = cab1L.process (cl);
			const float c1r = cab1R.process (cr);
			const float c2l = cab2L.process (c1l);
			const float c2r = cab2R.process (c1r);
			l = l + (c2l * 1.6f - l) * cab_;
			r = r + (c2r * 1.6f - r) * cab_;
		}

		// output DC blocker
		dcL += 0.0006f * (l - dcL);
		dcR += 0.0006f * (r - dcR);
		l -= dcL;
		r -= dcR;

		// Output trim.  Every character saturates to roughly the same internal
		// level, but a six-string chord is 3..6 dB hotter than the single note
		// the stage was voiced on, and the channel used to run +8..+11 dBFS into
		// the master limiter -- i.e. the master was clipping on every chord and
		// the "clean" amp was not clean.  These trims put a full chord at
		// roughly -1 dBFS with Output at 0, leaving the limiter for accidents.
		l *= kOutTrim[char_];
		r *= kOutTrim[char_];

		flushDenorm (dcL); flushDenorm (dcR); flushDenorm (sagEnv);
	}

	static constexpr float kOutTrim[4] = { 0.26f, 0.24f, 0.26f, 0.30f };

private:
	// Asymmetric overdrive: a diode-ish knee on the positive side, a softer one
	// on the negative side.  The asymmetry is what puts even harmonics in, and
	// even harmonics are most of the difference between "distorted" and "loud".
	//
	// An asymmetric shaper has a DC term: raw(0) is not 0.  Feeding that back
	// into a loop, or just leaving it in the output, means the amp hums when
	// nothing is playing.  The offset is removed explicitly rather than left to
	// the DC blocker, so silence really is silence.
	static inline float odShape (float x)
	{
		return odRaw (x) - odRaw (0.f);
	}

	static inline float odRaw (float x)
	{
		const float pos = softSat (x * 1.35f + 0.10f);
		const float neg = softSat (x * 1.10f - 0.06f);
		return (pos + neg) * 0.55f;
	}

	// The fuzz stage squares the signal, and a square term has a DC offset
	// (x^2 >= 0).  Removed the same way as the overdrive's.
	static inline float fuzzShape (float x)
	{
		return softSat (softSat (x * 2.2f) * 1.5f + 0.18f * (x * x - 0.33f));
	}

	void rebuild ()
	{
		// Input: 6 dB/oct below ~70 Hz, plus the 1st-stage tilt.
		hpL.setHighpass (70.f, 0.7f, fs);
		hpR.setHighpass (70.f, 0.7f, fs);
		preL.setHighShelf (1400.f, 3.0f, fs);
		preR.setHighShelf (1400.f, 3.0f, fs);

		// Tone stack.  Bass and treble are shelves; mid is a peak.  The muff
		// character starts with the mids already scooped, which is why its mid
		// control reads differently.
		const float midBase = (char_ == 3) ? -7.f : 0.f;
		toneBL.setLowShelf (110.f, bassDb_, fs);
		toneBR.setLowShelf (110.f, bassDb_, fs);
		toneML.setPeak (700.f, 1.1f, midDb_ + midBase, fs);
		toneMR.setPeak (700.f, 1.1f, midDb_ + midBase, fs);
		toneHL.setHighShelf (3000.f, trebleDb_, fs);
		toneHR.setHighShelf (3000.f, trebleDb_, fs);

		// Presence: a narrow lift right where the Jazzmaster's pickups peak.
		const float presDb = presence_ * 7.f;
		presL.setPeak (2200.f, 1.6f, presDb, fs);
		presR.setPeak (2200.f, 1.6f, presDb, fs);

		// Cabinet: 1x12.  High pass at 85, two low passes (cone + dust cap) and
		// a resonance at 115 Hz that gives the box its weight.
		cabHpL.setHighpass (85.f, 0.8f, fs);
		cabHpR.setHighpass (85.f, 0.8f, fs);
		cab1L.setLowpass (4600.f, 0.9f, fs);
		cab1R.setLowpass (4600.f, 0.9f, fs);
		cab2L.setLowpass (6800.f, 0.7f, fs);
		cab2R.setLowpass (6800.f, 0.7f, fs);
	}

	float fs = 48000.f;
	float drive_ = 0.22f;
	int   char_ = 1;
	float bassDb_ = 0.f, midDb_ = 0.f, trebleDb_ = 0.f;
	float presence_ = 0.4f, cab_ = 0.7f;
	float sagEnv = 1.f;
	float dcL = 0.f, dcR = 0.f;

	Biquad hpL, hpR, preL, preR;
	Biquad toneBL, toneBR, toneML, toneMR, toneHL, toneHR;
	Biquad presL, presR;
	Biquad cab1L, cab1R, cab2L, cab2R, cabHpL, cabHpR;
};

} // namespace jm
