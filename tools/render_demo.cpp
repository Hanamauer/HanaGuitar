// ===========================================================================
//  render_demo -- offline bounce of the HanaGuitar for indie engine
//
//  No host, no audio device, no installer: this renders the three things the
//  instrument is sold on, straight from src/engine.h, and writes 24-bit WAVs so
//  the result can be listened to and measured instead of described.
//
//    shoegaze.wav   fuzz, big space, a chord strummed and then bent with the
//                   arm while it rings -- the "glide guitar" test
//    midwest.wav    clean FACGCE tapping, the tuning and the articulation the
//                   midwest-emo preset exists for
//    glide.wav      one note, slow dive, spring release: the arm model alone
//
//  Build:  g++ -std=c++17 -O2 -o build\render_demo.exe tools\render_demo.cpp
//  Run:    build\render_demo.exe out
// ===========================================================================

#include "../src/engine.h"
#include "../src/presets.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace {

constexpr int kRate = 48000;

bool g_dry = false;

struct Stats
{
	double peak = 0.0;
	double sumSq = 0.0;
	long long n = 0;
	long long clipped = 0;
	double dc = 0.0;

	void add (float l, float r)
	{
		const double a = std::max (std::fabs ((double) l), std::fabs ((double) r));
		if (a > peak) peak = a;
		if (a >= 0.9995) ++clipped;
		sumSq += (double) l * l + (double) r * r;
		dc += 0.5 * ((double) l + r);
		n += 2;
	}
};

// ---------------------------------------------------------------- WAV writer
void writeWav (const std::string& path, const std::vector<float>& l,
               const std::vector<float>& r)
{
	const int n = (int) std::min (l.size (), r.size ());
	const int channels = 2;
	const int bits = 24;
	const int bytesPerSample = bits / 8;
	const uint32_t dataBytes = (uint32_t) n * channels * bytesPerSample;

	std::FILE* f = std::fopen (path.c_str (), "wb");
	if (!f) { std::printf ("cannot write %s\n", path.c_str ()); return; }

	auto u32 = [&] (uint32_t v) { std::fwrite (&v, 4, 1, f); };
	auto u16 = [&] (uint16_t v) { std::fwrite (&v, 2, 1, f); };

	std::fwrite ("RIFF", 1, 4, f);
	u32 (36u + dataBytes);
	std::fwrite ("WAVEfmt ", 1, 8, f);
	u32 (16);
	u16 (1);                       // PCM
	u16 ((uint16_t) channels);
	u32 ((uint32_t) kRate);
	u32 ((uint32_t) (kRate * channels * bytesPerSample));
	u16 ((uint16_t) (channels * bytesPerSample));
	u16 ((uint16_t) bits);
	std::fwrite ("data", 1, 4, f);
	u32 (dataBytes);

	for (int i = 0; i < n; ++i)
	{
		for (int c = 0; c < channels; ++c)
		{
			double v = (c == 0) ? l[i] : r[i];
			v = std::max (-1.0, std::min (1.0, v));
			int32_t s = (int32_t) std::lround (v * 8388607.0);
			uint8_t b[3] = { (uint8_t) (s & 0xFF),
			                 (uint8_t) ((s >> 8) & 0xFF),
			                 (uint8_t) ((s >> 16) & 0xFF) };
			std::fwrite (b, 1, 3, f);
		}
	}
	std::fclose (f);
}

// ---------------------------------------------------------------- transport
class Bounce
{
public:
	Bounce (int preset) : preset_ (preset)
	{
		e.prepare (kRate);
		jm::applyPresetTo (e, preset);
		if (g_dry) { e.setParam (jm::kChorus, 0.f); e.setParam (jm::kDelayMix, 0.f); e.setParam (jm::kRevMix, 0.f); }
		l.reserve (kRate * 12);
		r.reserve (kRate * 12);
	}

	// Render up to `until` seconds, calling tick() once per block.
	template <typename Fn>
	void run (double until, Fn&& tick)
	{
		const int block = 64;
		float bl[64], br[64];
		while ((double) pos / kRate < until)
		{
			tick ((double) pos / kRate);
			const int n = std::min (block, (int) (until * kRate) - pos);
			if (n <= 0) break;
			e.process (bl, br, n);
			for (int i = 0; i < n; ++i)
			{
				l.push_back (bl[i]);
				r.push_back (br[i]);
				stats.add (bl[i], br[i]);
			}
			pos += n;
		}
	}

	void save (const std::string& path)
	{
		writeWav (path, l, r);
		std::printf ("  %-14s %6.2f s  peak %6.2f dBFS  rms %6.2f dBFS  %s\n",
		             path.c_str (), (double) l.size () / kRate,
		             20.0 * std::log10 (std::max (1e-9, stats.peak)),
		             10.0 * std::log10 (std::max (1e-12, stats.sumSq / std::max (1LL, stats.n))),
		             stats.clipped > 0 ? "LIMITED" : "clean");
	}

	jm::Engine e;
	Stats stats;
	std::vector<float> l, r;
	int pos = 0;
	int preset_ = 0;
};

double dbToLin (double db) { return std::pow (10.0, db / 20.0); }
int ms (double v) { return (int) std::lround (v * 0.001 * kRate); }

// ---------------------------------------------------------------- 1. shoegaze
void renderShoegaze (const std::string& dir)
{
	std::printf ("[shoegaze] fuzz + space + a chord bent with the arm\n");
	Bounce b (0);

	// Em across all six strings: E2 B2 E3 G3 B3 E4
	const int chord[6] = { 40, 47, 52, 55, 59, 64 };
	const float vel[6] = { 0.92f, 0.85f, 0.88f, 0.80f, 0.86f, 0.90f };

	bool strummed = false, released = false;

	auto armCurve = [] (double t) -> float
	{
		// hand on the bar: slow push down 1.1 .. 1.9 s, hold, slow release with
		// a little overshoot past the rest position at the end
		if (t < 1.1) return 0.f;
		if (t < 1.9) return (float) ((t - 1.1) / 0.8);
		if (t < 2.7) return 1.f;
		if (t < 3.3) return (float) (1.0 - (t - 2.7) / 0.6);
		if (t < 3.6) return (float) (-0.06 * std::sin ((t - 3.3) / 0.3 * 3.14159));
		return 0.f;
	};

	b.run (9.0, [&] (double t)
	{
		if (!strummed && t >= 0.10)
		{
			strummed = true;
			for (int i = 0; i < 6; ++i)
				b.e.noteOnDelayed (chord[i], vel[i], ms (i * 9.0));
		}
		if (!released && t >= 6.4)
		{
			released = true;
			for (int i = 0; i < 6; ++i) b.e.noteOff (chord[i]);
		}
		b.e.setModWheel (armCurve (t));
	});

	b.save (dir + (g_dry ? "/shoegaze_dry.wav" : "/shoegaze.wav"));
}

// ---------------------------------------------------------------- 2. midwest
void renderMidwest (const std::string& dir)
{
	std::printf ("[midwest] clean FACGCE tapping\n");
	Bounce b (1);
	b.e.setParam (jm::kTuning, 5.f);          // FACGCE

	// A tapped figure in FACGCE: open strings plus hammered notes, the shape
	// that makes this tuning worth having.  Strings low->high F2 A2 C3 G3 C4 E4.
	struct Note { double t; int pitch; float vel; double dur; };
	const Note seq[] = {
		{ 0.15, 53, 0.62f, 0.40 },   // F3
		{ 0.42, 60, 0.58f, 0.40 },   // C4
		{ 0.69, 67, 0.60f, 0.40 },   // G4
		{ 0.96, 60, 0.55f, 0.40 },
		{ 1.23, 65, 0.60f, 0.45 },   // F4
		{ 1.50, 64, 0.66f, 0.45 },   // E4 open
		{ 1.77, 60, 0.58f, 0.40 },
		{ 2.04, 55, 0.62f, 0.45 },   // G3 open
		{ 2.31, 62, 0.56f, 0.40 },   // D4
		{ 2.58, 67, 0.60f, 0.40 },
		{ 2.85, 64, 0.64f, 0.50 },
		{ 3.30, 41, 0.70f, 1.80 },   // low F2, let ring
		{ 3.30, 48, 0.66f, 1.80 },   // C3
		{ 3.30, 55, 0.62f, 1.80 },   // G3
		{ 3.30, 60, 0.60f, 1.80 },   // C4
		{ 5.40, 64, 0.62f, 0.60 },
		{ 5.40, 67, 0.58f, 0.60 },
		{ 6.20, 60, 0.55f, 2.00 },
	};

	std::vector<char> on (sizeof (seq) / sizeof (seq[0]), 0);
	std::vector<char> off (on.size (), 0);

	b.run (9.0, [&] (double t)
	{
		for (size_t i = 0; i < on.size (); ++i)
		{
			if (!on[i] && t >= seq[i].t)
			{
				on[i] = 1;
				b.e.noteOn (seq[i].pitch, seq[i].vel);
			}
			if (on[i] && !off[i] && t >= seq[i].t + seq[i].dur)
			{
				off[i] = 1;
				b.e.noteOff (seq[i].pitch);
			}
		}
	});

	b.save (dir + "/midwest.wav");
}

// ---------------------------------------------------------------- 3. glide
void renderGlide (const std::string& dir)
{
	std::printf ("[glide] one note, slow dive, spring release\n");
	Bounce b (2);

	auto armCurve = [] (double t) -> float
	{
		if (t < 0.6) return 0.f;
		if (t < 2.6) return (float) ((t - 0.6) / 2.0);       // a very slow push
		if (t < 3.0) return 1.f;
		return 0.f;                                          // hand off: spring
	};

	bool on = false;
	b.run (7.0, [&] (double t)
	{
		if (!on && t >= 0.05) { on = true; b.e.noteOn (59, 0.9f); }   // B3
		b.e.setModWheel (armCurve (t));
	});

	b.save (dir + "/glide.wav");
}


// ------------------------------------------------------- 4. performance modes
void renderTechniques (const std::string& dir)
{
	std::printf ("[techniques] swell -> tremolo picking -> feedback (reverse reverb)\n");
	Bounce b (0);
	b.e.setParam (jm::kDrive, 0.22f);
	b.e.setParam (jm::kDriveChar, 1.f);
	b.e.setParam (jm::kRevMode, 4.f);        // reverse
	b.e.setParam (jm::kRevMix, 0.32f);
	b.e.setParam (jm::kSustain, 0.92f);

	bool chord = false, trem = false, fb = false, done = false;
	const int em[4] = { 40, 52, 55, 64 };

	b.run (12.0, [&] (double t)
	{
		// 1. volume swell + reverse reverb: no pick attack at all
		if (!chord && t >= 0.10)
		{
			chord = true;
			b.e.keyswitch (jm::kKsSwell, true);
			for (int i = 0; i < 4; ++i) b.e.noteOnDelayed (em[i], 0.85f, ms (i * 12.0));
		}
		if (chord && t >= 3.10)
		{
			chord = false;
			b.e.keyswitch (jm::kKsSwell, false);
			for (int i = 0; i < 4; ++i) b.e.noteOff (em[i]);
			b.e.setParam (jm::kRevMix, 0.05f);   // dry for the rest of the demo
			b.e.setParam (jm::kRevMode, 1.f);
		}

		// 2. tremolo picking on one note
		if (!trem && t >= 3.80)
		{
			trem = true;
			b.e.keyswitch (jm::kKsTremPick, true);
			b.e.noteOn (64, 0.9f);
		}
		if (trem && t >= 6.00)
		{
			trem = false;
			b.e.keyswitch (jm::kKsTremPick, false);
			b.e.noteOff (64);
		}

		// 3. controlled feedback under a held chord
		if (!fb && t >= 6.40)
		{
			fb = true;
			b.e.keyswitch (jm::kKsFeedback, true);
			for (int i = 0; i < 3; ++i) b.e.noteOnDelayed (em[i], 0.85f, ms (i * 14.0));
		}
		if (fb && t >= 10.6)
		{
			fb = false;
			b.e.keyswitch (jm::kKsFeedback, false);
			for (int i = 0; i < 4; ++i) b.e.noteOff (em[i]);
			b.e.setParam (jm::kDelayMix, 0.f);
			b.e.setParam (jm::kRevMix, 0.f);
			b.e.setParam (jm::kChorus, 0.f);
			b.e.allNotesOff ();
			b.e.setParam (jm::kOutput, -40.f);   // arrange a clean ending
			done = true;
		}
		(void) done;
	});

	std::printf ("  end mask=%d strings=%.0f\n", b.e.techniqueMask (), b.e.activeStrings ());
	b.save (dir + "/techniques.wav");
}

} // namespace

int main (int argc, char** argv)
{
	const std::string dir = (argc > 1) ? argv[1] : ".";
	g_dry = (argc > 2 && std::strcmp (argv[2], "dry") == 0);
	std::printf ("HanaGuitar for indie -- offline bounce @ %d Hz\n\n", kRate);
	renderShoegaze (dir);
	renderMidwest (dir);
	renderGlide (dir);
	renderTechniques (dir);
	std::printf ("\ndone\n");
	return 0;
}
