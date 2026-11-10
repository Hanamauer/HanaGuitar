// HanaGuitar for indie -- waveguide string with dynamic nut/bridge terminations
//
// A single delay loop, but read the way a real string is read:
//
//   y(x, t) = f(t - xT/2) - f(t - (T - xT/2))
//
// where f is the travelling wave stored in the loop and x is the position along
// the string as a fraction of the speaking length (0 = bridge, 1 = nut).  The
// second term is the same wave one nut-reflection later.  Reading the loop at
// those two delays is what makes a pickup at 27 % of the scale sound different
// from one at 7 % -- the comb is a consequence of the geometry, not a filter
// somebody bolted on.
//
// The whammy moves the *bridge*, so it changes the speaking length.  That is
// modelled by changing the loop period, which is also why a dive flattens the
// note and sharpens the behind-the-bridge segment at the same time.
#pragma once

#include "common.h"

namespace jm {

class WaveString
{
public:
	void prepare (float sampleRate, int maxDelaySamples)
	{
		fs = sampleRate;
		maxD = (float) maxDelaySamples;
		delay.resize (maxDelaySamples + 64);
		clear ();
	}

	void clear ()
	{
		delay.clear ();
		lp1 = lp2 = 0.f;
		dispX = dispY = 0.f;
		muteZ = 0.f;
		energy = 0.f;
		age = 0.f;
		delaySmooth = delayTarget;
		nutRef = 0.9985f;
		bridgeRef = 0.9985f;
		gainNow = loopGain;
	}

	// ---- tuning ---------------------------------------------------------
	// Everything inside the loop steals phase, and how much it steals depends on
	// frequency.  The delay line is therefore shortened by exactly the phase
	// delay of the filters at the note's fundamental -- forget this and every
	// note sits a few cents sharp, and the error grows with the note.
	void setFrequency (float freq, float dispersion, float hfDamping)
	{
		freq = clampf (freq, 20.f, fs * 0.45f);
		disp = clampf (dispersion, 0.f, 0.70f);
		hf = clampf (hfDamping, 0.002f, 0.30f);
		baseFreq = freq;

		// The loop is: read at delay D, filter, write.  Reading at D returns the
		// sample written D+1 samples ago (the write head advances after the
		// read), so the loop period is D+1 plus the filters.  Forgetting that
		// one sample leaves every note flat, and the error grows with pitch
		// (4 cents at 110 Hz, 12 cents at 330 Hz).
		const float w = kTwoPi * freq / fs;
		const float dLp = onePolePhaseDelay (hf, w) + onePolePhaseDelay (hf * 0.6f, w);
		const float dAp = allpass1PhaseDelay (disp, w);

		const float period = fs / freq;
		delayTarget = clampf (period - 1.f - dLp - dAp, 4.f, maxD);
		if (!(delaySmooth > 1.f) || delaySmooth > maxD) delaySmooth = delayTarget;
		updateLoopGain ();
	}

	void setDecay (float t60Seconds, float hfDamping)
	{
		t60 = clampf (t60Seconds, 0.05f, 30.f);
		hf = clampf (hfDamping, 0.002f, 0.30f);
		updateLoopGain ();
	}

	// Note-off / panic.  A real string does not stop because the *voice* was
	// released; the finger or the damper has to touch it.  Lowering t60 is that
	// contact: the loop loses its energy over the release time instead of
	// ringing on.  gainNow smooths the change so the release is a fade, not a
	// click.
	void damp (float t60Seconds)
	{
		t60 = clampf (t60Seconds, 0.005f, 30.f);
		updateLoopGain ();
	}

	// Palm mute: the edge of the hand rests on the string just in front of the
	// bridge, so it does two things at once -- it kills the loop gain (short
	// sustain) and it adds a low pass at the contact point (dull attack).  Both
	// are applied here rather than at note-on, so turning the knob while a
	// chord rings behaves like moving the hand.
	void setMute (float amount)
	{
		mute = clampf (amount, 0.f, 1.f);
		// 0 -> 0.9995 (open), 1 -> 0.985 (choked, but the note still speaks)
		muteGain = 1.f - 0.0145f * mute * mute;
		// Contact damping.  Even fully muted a palm mute is not a low pass at
		// 700 Hz -- that would swallow the pick attack and the note would lose
		// its definition.  The corner stays above 1 kHz so the "chug" keeps its
		// click; what the mute really removes is sustain, via muteGain.
		muteLp = mute > 0.001f ? (1.f - std::exp (-kTwoPi * (3200.f - 2000.f * mute) / fs)) : 1.f;
	}

	// ratio > 1 raises the pitch (a pull-up), < 1 lowers it (a dive)
	void setRatio (float ratio)
	{
		if (!(ratio > 0.f)) ratio = 1.f;
		ratio = clampf (ratio, 0.25f, 4.f);
		const float f = clampf (baseFreq * ratio, 20.f, fs * 0.45f);
		// Same compensation as setFrequency(), evaluated at the *bent* pitch.
		// If the filters' phase delay were ignored here the interval of the
		// bend would be wrong: a two-semitone dive would land somewhere else.
		const float w = kTwoPi * f / fs;
		const float dLp = onePolePhaseDelay (hf, w) + onePolePhaseDelay (hf * 0.6f, w);
		const float dAp = allpass1PhaseDelay (disp, w);
		// The same one-sample loop term as setFrequency(): the read happens
		// before the write, so a delay of D yields a period of D + 1.
		delayTarget = clampf (fs / f - 1.f - dLp - dAp, 4.f, maxD);
	}

	// ---- excitation -----------------------------------------------------
	//
	// A pluck is not noise.  It is an initial *displacement* of the string: a
	// triangle with its apex at the pick position.  Decomposed into the string's
	// modes that is
	//
	//     A_n = (2h / (n^2 pi^2)) * sin(n pi p) / (p (1-p))
	//
	// so the fundamental is inherently the loudest partial and the pick
	// position appears as an exact sin(n pi p) comb -- a real notch, not the
	// near-unity feed-forward filter that was boosting the 4th harmonic before.
	//
	// The loop stores the travelling wave f, and a fixed-fixed string is
	//     u(x) = f(D - xD/2) - f(xD/2)
	// Choosing f so that u is the triangle gives, for buffer index i in [0, D),
	//     f(i) = -0.5 * tri(2i/D)          i <= D/2
	//     f(i) = +0.5 * tri(2 - 2i/D)      i >  D/2
	// which is what is written below.  A short noise burst is layered on top for
	// the pick's own scrape, scaled by brightness and hardness.
	void pluck (float amplitude, float pickPos, float pickHard, float brightness,
	            float fretBuzz, Rng& rng)
	{
		const int n = delay.size ();
		std::fill (delay.data (), delay.data () + n, 0.f);
		lp1 = lp2 = 0.f;
		dispX = dispY = 0.f;

		pickPos = clampf (pickPos, 0.02f, 0.5f);

		int D = (int) (delayTarget + 0.5f);
		if (D < 8) D = 8;
		if (D > n - 64) D = n - 64;

		// The excitation occupies exactly one loop period, ending at the sample
		// just behind the write head.  Read back at delay D it is played out in
		// order over the first period, which is the string's initial state.
		const int base = delay.w - 1 - D;

		float* buf = delay.data ();

		// --- triangle displacement -> travelling wave -----------------------
		const float h = amplitude;
		const float p = pickPos;
		auto tri = [&] (float x) -> float
		{
			if (x <= 0.f) return 0.f;
			if (x >= 1.f) return 0.f;
			return (x < p) ? (h * x / p) : (h * (1.f - x) / (1.f - p));
		};

		const float halfD = 0.5f * (float) D;
		for (int i = 0; i < D; ++i)
		{
			float f;
			if ((float) i <= halfD)
				f = -0.5f * tri (2.f * (float) i / (float) D);
			else
				f = +0.5f * tri (2.f - 2.f * (float) i / (float) D);
			buf[(size_t) ((base + i) & delay.mask)] = f;
		}

		// --- pick transient: the broadband edge of the attack --------------
		// A perfect triangular displacement has a 1/n^2 spectrum, and 1/n^2 is
		// a thuddy, blanket-over-the-amp guitar: the third harmonic already
		// sits 19 dB under the fundamental.  A real pick releases the string
		// over a finite contact and leaves a short broadband transient; that
		// transient is what supplies the 1/n slope and the "chiff".  It is
		// injected at the pluck point and decays over a few milliseconds, so it
		// colours the onset without turning the sustain into noise.
		{
			const float noiseAmt = (0.16f + 0.40f * clampf (brightness, 0.f, 1.f)) *
			                       (0.40f + 0.60f * clampf (pickHard, 0.f, 1.f)) *
			                       amplitude;
			int len = std::min (D / 2, (int) (fs * 0.008f));
			if (len < 8) len = 8;
			const float a = 0.18f + 0.55f * clampf (brightness, 0.f, 1.f);
			float lp = 0.f;
			const int start = base + (int) (p * (float) D);
			for (int i = 0; i < len; ++i)
			{
				const float env = std::exp (-6.5f * (float) i / (float) len);
				const float n = rng.bipolar ();
				lp += (1.f - a) * (n - lp);
				buf[(size_t) ((start + i) & delay.mask)] += lp * env * noiseAmt * 4.0f;
			}
		}

		if (fretBuzz > 0.001f)
		{
			buzzAcc = buzzAcc * 0.90f + rng.bipolar () * 0.9f;
			buf[(size_t) ((base + (int) (p * (float) D)) & delay.mask)] +=
				softSat (buzzAcc * (1.f + fretBuzz * 10.f)) * fretBuzz * 0.12f * amplitude;
		}

		energy = amplitude;
		age = 0.f;
		delaySmooth = delayTarget;
		gainNow = loopGain;
	}

	// Hammer-on / pull-off: add a short contact burst to a string that is
	// already sounding.  Unlike pluck() this does not clear the loop, so the
	// note keeps its energy and only the speaking length changes.
	void hammer (float amplitude, float pickPos, float brightness, Rng& rng)
	{
		const int D = (int) (delayTarget + 0.5f);
		if (D < 8) return;
		int start = delay.w - 1 - (int) (clampf (pickPos, 0.02f, 0.5f) * (float) D);
		int len = std::min (D / 5, (int) (fs * 0.004f));
		if (len < 6) len = 6;
		const float a = 0.28f + 0.42f * clampf (brightness, 0.f, 1.f);
		float lp = 0.f;
		for (int i = 0; i < len; ++i)
		{
			const float env = std::exp (-3.0f * (float) i / (float) len);
			lp += (1.f - a) * (rng.bipolar () - lp);
			delay.buf[(size_t) ((start + i) & delay.mask)] += lp * env * amplitude;
		}
		if (amplitude > energy) energy = amplitude;
	}

	// ---- audio ----------------------------------------------------------
	inline float process ()
	{
		// The delay follows its target with a one-pole, so a whammy move is a
		// glide and never a click.  The time constant is short enough to track a
		// fast dive (a few ms) and long enough to smooth the per-sample updates.
		delaySmooth += (delayTarget - delaySmooth) * 0.45f;

		const float out = readHermite (delay, delaySmooth);

		// stiffness: first-order allpass, gives inharmonic partials
		float y = out;
		if (disp > 0.0005f)
		{
			const float z = disp * y + dispX - disp * dispY;
			dispX = y;
			dispY = z;
			y = z;
		}

		// frequency dependent loss: highs die faster than lows
		gainNow += (loopGain - gainNow) * 0.002f;
		y *= gainNow * muteGain;
		// Frequency-dependent loss.  The filter runs once per *period*, not once
		// per sample of a continuous signal, so the coefficients look tiny: at
		// hf = 0.05 the 2 kHz loss is 0.015 dB per pass, which is already
		// -3.7 dB per second at 247 Hz.  The old 0.10..0.72 range was a
		// 1.5..4.4 kHz lowpass sitting inside the loop and killed the eighth
		// harmonic within milliseconds -- the "blanket over the amp" tone.
		lp1 += (1.f - hf) * (y - lp1);
		y = lp1;
		lp2 += (1.f - hf * 0.6f) * (y - lp2);
		y = lp2;

		// palm mute contact damping: one more pole, only when muted
		if (mute > 0.001f)
		{
			muteZ += muteLp * (y - muteZ);
			y = muteZ;
		}

		// independent terminations -- the bridge one is what the arm moves.
		// Both ends are fixed, so both reflect with an inversion; the two
		// inversions cancel and the loop gain is positive.  Making only one of
		// them negative doubles the period, which sounds an octave down and puts
		// all the energy on the odd harmonics of the wrong fundamental.
		y *= nutRef * bridgeRef;

		delay.write (y);

		energy = energy * 0.99990f + std::fabs (y) * 0.00010f;
		age += 1.f;
		flushDenorm (lp1);
		flushDenorm (lp2);
		flushDenorm (dispX);
		flushDenorm (dispY);
		flushDenorm (muteZ);
		flushDenorm (energy);
		return out;
	}

	// Sympathetic drive: another string's vibration reaches this one through
	// the bridge and body.  A small amount of the summed string signal is
	// injected into the loop, which is what makes the open strings ring when a
	// chord leaves them free -- the shimmer at the end of a shoegaze phrase.
	inline void addSympathetic (float x)
	{
		if (symAmt < 1e-4f) return;
		delay.buf[(size_t) ((delay.w - 1) & delay.mask)] += x * symAmt;
	}

	// Displacement at a point along the string, for the pickup aperture.
	// xi = 0 at the bridge, 1 at the nut.
	inline float readPos (float xi) const
	{
		const float d = delaySmooth;
		const float a = clampf (xi, 0.f, 1.f) * 0.5f * d;
		const float b = d - a;
		return readHermite (delay, a) - readHermite (delay, b);
	}

	// ---- state ----------------------------------------------------------
	float targetDelay () const { return delayTarget; }
	float smoothedDelay () const { return delaySmooth; }
	bool isAlive () const { return energy > 1e-7f; }
	float getEnergy () const { return energy; }
	float fundamental () const { return baseFreq; }

	void setSympathetic (float amt) { symAmt = clampf (amt, 0.f, 0.25f); }

	float fs = 48000.f;
	float delayTarget = 100.f;
	float delaySmooth = 100.f;
	float loopGain = 0.999f;
	float hf = 0.3f;
	float disp = 0.f;
	float nutRef = 0.9985f, bridgeRef = 0.9985f;
	float lp1 = 0.f, lp2 = 0.f;
	float dispX = 0.f, dispY = 0.f;
	float energy = 0.f, age = 0.f;
	float baseFreq = 110.f;
	float t60 = 4.f;
	float gainNow = 0.999f;
	float buzzAcc = 0.f;

	// palm mute
	float mute = 0.f;
	float muteGain = 1.f;
	float muteLp = 1.f;
	float muteZ = 0.f;

	// sympathetic coupling
	float symAmt = 0.f;

	DelayLine delay;

private:
	void updateLoopGain ()
	{
		const float loopsPerSecond = fs / std::max (8.f, delayTarget);
		const float g = std::pow (10.f, -3.f / std::max (0.03f, t60 * loopsPerSecond));
		loopGain = clampf (g, 0.0001f, 0.99999f);
	}

	float maxD = 2000.f;
};

} // namespace jm
