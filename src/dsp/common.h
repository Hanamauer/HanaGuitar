// HanaGuitar for indie -- DSP primitives
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>

namespace jm {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

inline float clampf (float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline float lerpf (float a, float b, float t) { return a + (b - a) * t; }
inline float dbToGain (float db) { return std::pow (10.f, db * 0.05f); }
inline float gainToDb (float g) { return 20.f * std::log10 (g > 1e-9f ? g : 1e-9f); }
inline float smoothStep (float t)
{
	t = clampf (t, 0.f, 1.f);
	return t * t * (3.f - 2.f * t);
}

// Pade [3/2] approximation of tanh, saturates exactly at +/-1.
inline float softSat (float x)
{
	if (x >= 3.f) return 1.f;
	if (x <= -3.f) return -1.f;
	float x2 = x * x;
	return x * (27.f + x2) / (27.f + 9.f * x2);
}

inline void flushDenorm (float& x)
{
	if (std::fabs (x) < 1e-25f) x = 0.f;
}

// ---------------------------------------------------------------------------
// Phase delay of the loop filters.
//
// A waveguide only stays in tune if the *total* phase delay around the loop is
// exactly one period. Every filter inside the loop steals a few samples, and
// the amount it steals depends on frequency -- so the delay line has to be
// shortened by exactly that much at the note's fundamental. These are the
// closed forms for the two filter types the string uses.
//
//   one-pole lowpass   y = (1-a)x + a*y   ->  a is the *feedback* coefficient
//   first-order allpass y = c*x + x1 - c*y1
//
// At w -> 0 both reduce to their DC group delay, which is what a naive
// implementation gets wrong: a/ (1-a) is not small, and forgetting it puts
// every note a few cents sharp.
// ---------------------------------------------------------------------------
inline float onePolePhaseDelay (float a, float w)
{
	if (w < 1e-6f) return a / std::max (1e-6f, 1.f - a);
	return std::atan2 (a * std::sin (w), 1.f - a * std::cos (w)) / w;
}

inline float allpass1PhaseDelay (float c, float w)
{
	if (w < 1e-6f) return (1.f - c) / std::max (1e-6f, 1.f + c);
	float num = std::atan2 (-std::sin (w), c + std::cos (w));
	float den = std::atan2 (-c * std::sin (w), 1.f + c * std::cos (w));
	return -(num - den) / w;
}

struct Rng
{
	uint32_t s = 0x9E3779B9u;
	explicit Rng (uint32_t seed = 0x9E3779B9u) : s (seed ? seed : 1u) {}
	inline uint32_t nextu ()
	{
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return s;
	}
	inline float next () { return (float) (nextu () >> 8) * (1.f / 16777216.f); }
	inline float bipolar () { return next () * 2.f - 1.f; }
};

// ---------------------------------------------------------------- Biquad (RBJ)
struct Biquad
{
	float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
	float z1 = 0, z2 = 0;

	inline float process (float x)
	{
		float y = b0 * x + z1;
		z1 = b1 * x - a1 * y + z2;
		z2 = b2 * x - a2 * y;
		flushDenorm (z1);
		flushDenorm (z2);
		return y;
	}
	void reset () { z1 = z2 = 0; }
	void identity () { b0 = 1; b1 = b2 = a1 = a2 = 0; }

	void setCoeffs (float nb0, float nb1, float nb2, float na1, float na2)
	{
		b0 = nb0; b1 = nb1; b2 = nb2; a1 = na1; a2 = na2;
	}

	void setLowpass (float fc, float q, float sr)
	{
		float w = kTwoPi * clampf (fc, 10.f, sr * 0.49f) / sr;
		float cw = std::cos (w), sw = std::sin (w);
		float alpha = sw / (2.f * std::max (0.05f, q));
		float a0 = 1.f + alpha;
		setCoeffs ((1.f - cw) * 0.5f / a0, (1.f - cw) / a0, (1.f - cw) * 0.5f / a0,
		           -2.f * cw / a0, (1.f - alpha) / a0);
	}

	void setHighpass (float fc, float q, float sr)
	{
		float w = kTwoPi * clampf (fc, 10.f, sr * 0.49f) / sr;
		float cw = std::cos (w), sw = std::sin (w);
		float alpha = sw / (2.f * std::max (0.05f, q));
		float a0 = 1.f + alpha;
		setCoeffs ((1.f + cw) * 0.5f / a0, -(1.f + cw) / a0, (1.f + cw) * 0.5f / a0,
		           -2.f * cw / a0, (1.f - alpha) / a0);
	}

	void setBandpass (float fc, float q, float sr)
	{
		float w = kTwoPi * clampf (fc, 10.f, sr * 0.49f) / sr;
		float cw = std::cos (w), sw = std::sin (w);
		float alpha = sw / (2.f * std::max (0.05f, q));
		float a0 = 1.f + alpha;
		setCoeffs (alpha / a0, 0.f, -alpha / a0, -2.f * cw / a0, (1.f - alpha) / a0);
	}

	void setNotch (float fc, float q, float sr)
	{
		float w = kTwoPi * clampf (fc, 10.f, sr * 0.49f) / sr;
		float cw = std::cos (w), sw = std::sin (w);
		float alpha = sw / (2.f * std::max (0.05f, q));
		float a0 = 1.f + alpha;
		setCoeffs (1.f / a0, -2.f * cw / a0, 1.f / a0, -2.f * cw / a0, (1.f - alpha) / a0);
	}

	void setPeak (float fc, float q, float gainDb, float sr)
	{
		float w = kTwoPi * clampf (fc, 10.f, sr * 0.49f) / sr;
		float cw = std::cos (w), sw = std::sin (w);
		float A = std::pow (10.f, gainDb / 40.f);
		float alpha = sw / (2.f * std::max (0.05f, q));
		float a0 = 1.f + alpha / A;
		setCoeffs ((1.f + alpha * A) / a0, (-2.f * cw) / a0, (1.f - alpha * A) / a0,
		           (-2.f * cw) / a0, (1.f - alpha / A) / a0);
	}

	void setLowShelf (float fc, float gainDb, float sr)
	{
		float w = kTwoPi * clampf (fc, 10.f, sr * 0.49f) / sr;
		float cw = std::cos (w), sw = std::sin (w);
		float A = std::pow (10.f, gainDb / 40.f);
		float alpha = sw * 0.5f * std::sqrt ((A + 1.f / A) * (1.f / 1.f - 1.f) + 2.f);
		float k = 2.f * std::sqrt (A) * alpha;
		float a0 = (A + 1.f) + (A - 1.f) * cw + k;
		setCoeffs (A * ((A + 1.f) - (A - 1.f) * cw + k) / a0,
		           2.f * A * ((A - 1.f) - (A + 1.f) * cw) / a0,
		           A * ((A + 1.f) - (A - 1.f) * cw - k) / a0,
		           -2.f * ((A - 1.f) + (A + 1.f) * cw) / a0,
		           ((A + 1.f) + (A - 1.f) * cw - k) / a0);
	}

	void setHighShelf (float fc, float gainDb, float sr)
	{
		float w = kTwoPi * clampf (fc, 10.f, sr * 0.49f) / sr;
		float cw = std::cos (w), sw = std::sin (w);
		float A = std::pow (10.f, gainDb / 40.f);
		float alpha = sw * 0.5f * std::sqrt ((A + 1.f / A) * (1.f / 1.f - 1.f) + 2.f);
		float k = 2.f * std::sqrt (A) * alpha;
		float a0 = (A + 1.f) - (A - 1.f) * cw + k;
		setCoeffs (A * ((A + 1.f) + (A - 1.f) * cw + k) / a0,
		           -2.f * A * ((A - 1.f) + (A + 1.f) * cw) / a0,
		           A * ((A + 1.f) + (A - 1.f) * cw - k) / a0,
		           2.f * ((A - 1.f) - (A + 1.f) * cw) / a0,
		           ((A + 1.f) - (A - 1.f) * cw - k) / a0);
	}
};

// ---------------------------------------------------------------- One pole
struct OnePole
{
	float a = 0.f;   // feedback coefficient
	float z = 0.f;
	inline float process (float x)
	{
		z = x * (1.f - a) + a * z;
		flushDenorm (z);
		return z;
	}
	void setCutoff (float fc, float sr)
	{
		a = std::exp (-kTwoPi * clampf (fc, 1.f, sr * 0.49f) / sr);
	}
	float phaseDelay (float freq) const
	{
		float w = kTwoPi * freq / 48000.f;
		if (w < 1e-6f) return a / (1.f - a);
		return std::atan2 (a * std::sin (w), 1.f - a * std::cos (w)) / w;
	}
	void reset () { z = 0.f; }
};

// ---------------------------------------------------------------- Delay line
class DelayLine
{
public:
	void resize (int n)
	{
		int size = 4;
		while (size < n) size <<= 1;
		buf.assign ((size_t) size, 0.f);
		mask = size - 1;
		w = 0;
	}
	void clear () { std::fill (buf.begin (), buf.end (), 0.f); w = 0; }

	inline void write (float x)
	{
		buf[(size_t) w] = x;
		w = (w + 1) & mask;
	}
	// `w` points at the next slot to be written, so buf[w-1] is the sample
	// written most recently and buf[w-1-D] is the sample from D samples ago.
	inline float readInt (int d) const
	{
		if (d < 0) d = 0;
		return buf[(size_t) ((w - 1 - d) & mask)];
	}

	// linear interpolation, read only (safe for side taps)
	inline float readLin (float d) const
	{
		if (d < 0.f) d = 0.f;
		const int di = (int) d;
		const float f = d - (float) di;
		const float x0 = buf[(size_t) ((w - 1 - di) & mask)];
		const float x1 = buf[(size_t) ((w - 2 - di) & mask)];
		return x0 + f * (x1 - x0);
	}

	int size () const { return mask + 1; }
	int writePos () const { return w; }
	float* data () { return buf.data (); }
	float& atOffset (int n) { return buf[(size_t) ((w + n) & mask)]; }

	int mask = 0;
	int w = 0;
	std::vector<float> buf;
};

// Catmull-Rom (4 point) fractional read.
//
// Same indexing convention as readInt(): `delay` is measured back from the most
// recently written sample.  The polynomial is evaluated as
//   y = p1 + 0.5*f*(p2-p0 + f*(2p0-5p1+4p2-p3 + f*(3(p1-p2)+p3-p0)))
// which is the standard Catmull-Rom form and is exact at f = 0 and f = 1.
inline float readHermite (const DelayLine& d, float delay)
{
	if (delay < 1.f) delay = 1.f;
	const int di = (int) delay;
	const float f = delay - (float) di;
	const int m = d.mask;
	const float p0 = d.buf[(size_t) ((d.w - 1 - di + 1) & m)];
	const float p1 = d.buf[(size_t) ((d.w - 1 - di) & m)];
	const float p2 = d.buf[(size_t) ((d.w - 1 - di - 1) & m)];
	const float p3 = d.buf[(size_t) ((d.w - 1 - di - 2) & m)];
	const float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
	const float b = p0 - 2.5f * p1 + 2.f * p2 - 0.5f * p3;
	const float c = -0.5f * p0 + 0.5f * p2;
	return ((a * f + b) * f + c) * f + p1;
}

// ---------------------------------------------------------------- 4x oversampler
class Oversampler4
{
public:
	static constexpr int kPhases = 4;
	static constexpr int kTapsPerPhase = 16;
	static constexpr int kTaps = kPhases * kTapsPerPhase;   // 64
	static constexpr int kHist = kTapsPerPhase + 1;

	void init ()
	{
		double h[kTaps];
		double sum = 0.0;
		const double fc = 0.11;   // cycles / sample at 4x rate (slightly under 0.125)
		for (int i = 0; i < kTaps; ++i)
		{
			double t = (double) i - (kTaps - 1) * 0.5;
			double s = (std::fabs (t) < 1e-9) ? 2.0 * fc
			                                  : std::sin (2.0 * 3.141592653589793 * fc * t) /
			                                        (3.141592653589793 * t);
			double win = 0.42 - 0.5 * std::cos (2.0 * 3.141592653589793 * i / (kTaps - 1)) +
			             0.08 * std::cos (4.0 * 3.141592653589793 * i / (kTaps - 1));
			h[i] = s * win;
			sum += h[i];
		}
		for (int i = 0; i < kTaps; ++i) h[i] /= sum;

		for (int p = 0; p < kPhases; ++p)
			for (int k = 0; k < kTapsPerPhase; ++k)
				ph[p][k] = (float) (h[p + kPhases * k] * kPhases);

		for (int i = 0; i < kTaps; ++i) dn[i] = (float) h[i];

		reset ();
	}

	inline void up (float x, float* out)
	{
		for (int i = kHist - 1; i > 0; --i) xh[i] = xh[i - 1];
		xh[0] = x;
		for (int p = 0; p < kPhases; ++p)
		{
			float acc = 0.f;
			for (int k = 0; k < kTapsPerPhase; ++k) acc += ph[p][k] * xh[k];
			out[p] = acc;
		}
	}

	inline float down (const float* in)
	{
		for (int i = kTaps - 1; i >= kPhases; --i) uh[i] = uh[i - kPhases];
		uh[3] = in[3];
		uh[2] = in[2];
		uh[1] = in[1];
		uh[0] = in[0];
		float acc = 0.f;
		for (int i = 0; i < kTaps; ++i) acc += dn[i] * uh[i];
		return acc;
	}

	void reset ()
	{
		std::fill (xh, xh + kHist, 0.f);
		std::fill (uh, uh + kTaps, 0.f);
	}

	float ph[kPhases][kTapsPerPhase] {};
	float dn[kTaps] {};
	float xh[kHist] {};
	float uh[kTaps] {};
};

} // namespace jm
