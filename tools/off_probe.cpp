// off_probe -- does note-off match a note that arrived through the strum queue?
#include "../src/engine.h"
#include <cstdio>
#include <cmath>
#include <vector>

int main ()
{
	jm::Engine e;
	e.prepare (48000);
	e.setParam (jm::kSustain, 0.9f);
	const int chord[3] = { 40, 52, 55 };
	for (int i = 0; i < 3; ++i) e.noteOnDelayed (chord[i], 0.85f, i * 672);

	std::vector<float> l (48000), r (l.size ());
	for (size_t i = 0; i < l.size (); i += 64) e.process (&l[i], &r[i], 64);
	std::printf ("after 1s: strings %.0f\n", e.activeStrings ());

	for (int i = 0; i < 3; ++i) e.noteOff (chord[i]);
	for (size_t i = 0; i < l.size (); i += 64) e.process (&l[i], &r[i], 64);

	double a = 0;
	for (int i = 40000; i < 48000; ++i) a += (double) l[i] * l[i];
	std::printf ("after off + 1s: strings %.0f  rms %.6f\n", e.activeStrings (), std::sqrt (a / 8000));
	return 0;
}
