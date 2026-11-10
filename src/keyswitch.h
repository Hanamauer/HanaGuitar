// HanaGuitar for indie -- keyswitch map
//
// The panel has no technique knobs.  Palm mute, harmonics, tapping, the picking
// position, the choke, the pickup / circuit switches and the shoegaze
// performance modes are played from the keyboard's lowest keys, the way a
// guitarist moves a hand: hold a key while you play, or tap a key to flip a
// switch.
#pragma once

namespace jm {

// C0 .. D#1 (MIDI 24 .. 39).  The lowest playable note in every tuning is E2
// (40), so the whole map sits below the instrument and can never collide with a
// played note.
enum Keyswitch
{
	kKsPalmMute = 24,     // hold
	kKsHarmonic = 25,     // hold
	kKsTap      = 26,     // hold
	kKsChoke    = 27,     // one-shot
	kKsPickBridge = 28,   // hold
	kKsPickMiddle = 29,   // hold
	kKsPickNeck   = 30,   // hold
	kKsRhythm   = 31,     // toggle
	kKsLead     = 32,     // toggle
	kKsPickupP90  = 33,   // toggle
	kKsPickupBoth = 34,   // toggle
	kKsPickupHB   = 35,   // toggle
	kKsSwell    = 36,     // toggle: volume-pedal swells (no attack)
	kKsTremPick = 37,     // hold:   tremolo picking at 14 notes/s
	kKsFeedback = 38,     // hold:   controlled feedback layer
	kKsReverse  = 39,     // toggle: reverse-reverb character

	kKsFirst = kKsPalmMute,
	kKsLast  = kKsReverse
};

struct TechniqueState
{
	bool palmMute = false;
	bool harmonic = false;
	bool tap = false;
	bool swell = false;
	bool tremPick = false;
	bool feedback = false;
	int pickPos = 1;          // 0 = bridge, 1 = middle, 2 = neck

	void reset ()
	{
		palmMute = harmonic = tap = false;
		swell = tremPick = feedback = false;
		pickPos = 1;
	}

	// bit 0 palm mute, bit 1 harmonics, bit 2 tap, bit 3 swell, bit 4 trem,
	// bit 5 feedback -- for the panel's lamps
	int mask () const
	{
		return (palmMute ? 1 : 0) | (harmonic ? 2 : 0) | (tap ? 4 : 0) |
		       (swell ? 8 : 0) | (tremPick ? 16 : 0) | (feedback ? 32 : 0);
	}
};

} // namespace jm
