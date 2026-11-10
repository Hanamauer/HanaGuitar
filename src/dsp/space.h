// HanaGuitar for indie -- space: chorus, delay and reverb
//
// The two effects that define the sound this instrument is built for are both
// here, and both are deliberately modulated in the same way a Jazzmaster's
// tremolo arm modulates pitch: slowly, continuously, and without a hard LFO
// edge.  A shoegaze chorus that steps is a chorus that sounds like a preset.
#pragma once

#include "common.h"

namespace jm {

// ---------------------------------------------------------------- chorus
class Chorus
{
public:
	void prepare (float sampleRate)
	{
		fs = sampleRate;
		const int n = (int) (fs * 0.06f);
		dl.resize (n + 64);
		dl.clear ();
		phase = 0.f;
		phase2 = 0.31f;
		lpL.reset (); lpR.reset ();
		lpL.setLowpass (6000.f, 0.7f, fs);
		lpR.setLowpass (6000.f, 0.7f, fs);
	}

	void reset ()
	{
		dl.clear ();
		phase = 0.f;
		phase2 = 0.31f;
		lpL.reset (); lpR.reset ();
	}

	// amount 0..1.  Rate and depth are fixed to the "always on, barely there"
	// setting that shoegaze uses: ~0.45 Hz, wide, slow.
	inline void process (float& l, float& r, float amount)
	{
		const float rate = 0.45f;
		phase += rate / fs;
		if (phase >= 1.f) phase -= 1.f;
		phase2 += rate * 1.618f / fs;
		if (phase2 >= 1.f) phase2 -= 1.f;

		// two LFOs at an irrational ratio, so the pair never repeats audibly
		const float m1 = std::sin (kTwoPi * phase);
		const float m2 = std::sin (kTwoPi * phase2 + 0.9f);

		const float baseMs = 14.f;
		const float depthMs = 5.5f;
		const float dL = (baseMs + depthMs * m1) * 0.001f * fs;
		const float dR = (baseMs + depthMs * m2) * 0.001f * fs;

		const float wl = readHermite (dl, dL);
		const float wr = readHermite (dl, dR);

		dl.write (0.5f * (l + r));

		// the wet path is darker than the dry one, like a bucket brigade
		const float wetL = lpL.process (wl);
		const float wetR = lpR.process (wr);

		const float a = clampf (amount, 0.f, 1.f) * 0.62f;
		l += wetL * a;
		r += wetR * a;
	}

private:
	DelayLine dl;
	Biquad lpL, lpR;
	float phase = 0.f, phase2 = 0.31f;
	float fs = 48000.f;
};

// ---------------------------------------------------------------- delay
class Delay
{
public:
	void prepare (float sampleRate)
	{
		fs = sampleRate;
		const int n = (int) (fs * 2.2f);
		dlL.resize (n + 64);
		dlR.resize (n + 64);
		dlL.clear (); dlR.clear ();
		lpL.reset (); lpR.reset ();
		lpL.setLowpass (3800.f, 0.7f, fs);
		lpR.setLowpass (3800.f, 0.7f, fs);
		hpL.reset (); hpR.reset ();
		hpL.setHighpass (180.f, 0.7f, fs);
		hpR.setHighpass (180.f, 0.7f, fs);
		phase = 0.f;
	}

	void reset ()
	{
		dlL.clear (); dlR.clear ();
		lpL.reset (); lpR.reset (); hpL.reset (); hpR.reset ();
		phase = 0.f;
	}

	// Panic: drop the repeats.  A delay that keeps echoing after all-notes-off
	// is a stuck note with a longer memory.
	void clearTail () { dlL.clear (); dlR.clear (); }

	inline void process (float& l, float& r, float timeMs, float mix)
	{
		const float d = clampf (timeMs, 20.f, 2000.f) * 0.001f * fs;

		// Cross feedback: the left repeat goes into the right delay line.  On a
		// wide stereo patch this is what stops the repeats from stacking in the
		// middle and turning the image to mush.
		const float tapL = readHermite (dlL, d);
		const float tapR = readHermite (dlR, d);

		float fbL = hpL.process (lpL.process (tapL));
		float fbR = hpR.process (lpR.process (tapR));

		// a slow drift on the feedback path, so long trails shimmer instead of
		// sitting perfectly still
		phase += 0.13f / fs;
		if (phase >= 1.f) phase -= 1.f;
		const float drift = 1.f + 0.035f * std::sin (kTwoPi * phase);

		dlL.write (0.5f * (l + r) + fbR * 0.52f * drift);
		dlR.write (0.5f * (l + r) + fbL * 0.52f * drift);

		const float m = clampf (mix, 0.f, 1.f);
		l += tapL * m;
		r += tapR * m;
	}

private:
	DelayLine dlL, dlR;
	Biquad lpL, lpR, hpL, hpR;
	float phase = 0.f;
	float fs = 48000.f;
};

// ---------------------------------------------------------------- reverb
// A small feedback delay network.  Four delay lines, a Hadamard mixing matrix,
// and damping in the loop.  Size sets the line lengths; mode changes which of
// the two damping curves is used (bright room vs. dark hall vs. plate vs. a
// gated reverse-ish decay).
class Reverb
{
public:
	void prepare (float sampleRate)
	{
		fs = sampleRate;
		for (int i = 0; i < kLines; ++i)
		{
			line[i].resize ((int) (fs * 0.19f) + 64);
			line[i].clear ();
			damp[i].reset ();
			hp[i].reset ();
			hp[i].setHighpass (120.f, 0.7f, fs);
		}
		setMode (1);
	}

	void reset ()
	{
		for (int i = 0; i < kLines; ++i) { line[i].clear (); damp[i].reset (); hp[i].reset (); }
	}

	void clearTail ()
	{
		for (int i = 0; i < kLines; ++i) line[i].clear ();
	}

	void setMode (int m)
	{
		mode = m < 0 ? 0 : (m > 4 ? 4 : m);
		// per-mode damping cutoff and feedback trim.  Mode 4 is the shoegaze
		// reverse setting: the wet signal swells in and cuts, instead of
		// decaying from the attack.
		static const float cut[5] = { 5200.f, 3200.f, 6200.f, 2200.f, 4200.f };
		static const float fb[5] = { 0.72f, 0.80f, 0.76f, 0.62f, 0.72f };
		for (int i = 0; i < kLines; ++i)
		{
			damp[i].setLowpass (cut[mode], 0.7f, fs);
			fbBase[i] = fb[mode];
		}
	}

	inline void process (float& l, float& r, float size, float mix)
	{
		const float s = clampf (size, 0.f, 1.f);

		// prime-ish line lengths, scaled by size.  Small room -> short lines.
		static const float baseMs[kLines] = { 29.7f, 37.1f, 41.9f, 53.3f };
		float d[kLines];
		for (int i = 0; i < kLines; ++i)
			d[i] = (baseMs[i] * (0.45f + 0.75f * s)) * 0.001f * fs;

		float tap[kLines];
		for (int i = 0; i < kLines; ++i)
			tap[i] = readHermite (line[i], d[i]);

		// Hadamard 4x4 (unnormalised, scaled by 0.5 = 1/sqrt(4))
		const float a = tap[0], b = tap[1], c = tap[2], e = tap[3];
		float m0 = (a + b + c + e) * 0.5f;
		float m1 = (a - b + c - e) * 0.5f;
		float m2 = (a + b - c - e) * 0.5f;
		float m3 = (a - b - c + e) * 0.5f;

		float out[kLines] = { m0, m1, m2, m3 };

		const float inMono = 0.5f * (l + r);
		for (int i = 0; i < kLines; ++i)
		{
			float y = out[i] * fbBase[i] + inMono * 0.35f;
			y = damp[i].process (y);
			y = hp[i].process (y);
			line[i].write (y);
		}

		// stereo taps: two lines per side, deliberately different lengths
		float wetL = readHermite (line[0], d[0]) * 0.60f +
		             readHermite (line[2], d[2]) * 0.40f;
		float wetR = readHermite (line[1], d[1]) * 0.60f +
		             readHermite (line[3], d[3]) * 0.40f;

		// Reverse mode: every attack retriggers a 0.9 s swell that rises and
		// then cuts, so the reverb blooms *into* the note instead of decaying
		// out of it.  Real reverse reverb needs lookahead; this is the
		// real-time approximation the shoegaze records actually use (a
		// reverse-mode reverb pedal), and it behaves the same way under the
		// fingers: the wet only exists after a pick.
		if (mode == 4)
		{
			const float in = std::fabs (inMono);
			if (in > 0.02f && revTimer <= 0.f) revTimer = 0.9f * fs;
			if (revTimer > 0.f) revTimer -= 1.f;
			const float prog = 1.f - std::max (0.f, revTimer) / (0.9f * fs);
			const float e = std::sin (3.14159265f * clampf (prog, 0.f, 1.f));
			wetL *= e;
			wetR *= e;
		}

		const float m = clampf (mix, 0.f, 1.f);
		l += wetL * m;
		r += wetR * m;
	}

private:
	static constexpr int kLines = 4;
	DelayLine line[kLines];
	Biquad damp[kLines];
	Biquad hp[kLines];
	float fbBase[kLines] {};
	float revTimer = 0.f;
	int mode = 1;
	float fs = 48000.f;
};

} // namespace jm
