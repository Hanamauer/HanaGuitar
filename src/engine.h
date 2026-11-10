// ===========================================================================
//  HanaGuitar for indie -- engine
//
//  Six strings, each a waveguide.  The whammy moves every ringing string's
//  bridge termination by the same ratio, which is the whole physical point: a
//  tremolo changes the tension of the speaking length, so all six go out of
//  tune together and come back together.
//
//  Signal flow per sample
//
//      strings ─┬─> P90 (neck)      ─�?//               └─> humbucker (bridge) ─┴─> circuit (lead / rhythm)
//                    �?                       �?//                    └─> sympathetic bank     └─> body ─> amp ─> space ─> out
//
//  The two pickups are summed *before* the resonant filter and the circuit,
//  because that is the electrical order: one inductance, one volume pot, one
//  tone pot.  Putting the circuit after the pickup sum is what makes rolling
//  the volume down darken the sound instead of just lowering it.
//
//  THREADING
//      process()          audio thread only
//      setParam()         any thread; writes atomics, never touches the graph
//      noteOn/noteOff     audio thread (from the event queue)
//
//  Nothing here allocates after prepare(), and nothing here blocks.
// ===========================================================================
#pragma once

#include "params.h"
#include "keyswitch.h"
#include "dsp/common.h"
#include "dsp/string.h"
#include "dsp/whammy.h"
#include "dsp/pickup.h"
#include "dsp/body.h"
#include "dsp/circuit.h"
#include "dsp/amp.h"
#include "dsp/space.h"
#include "dsp/feedback.h"

#include <atomic>

namespace jm {

constexpr int kMaxStrings = 6;
constexpr int kNumStrings = 6;
constexpr int kNumVoices = 6;

// Tuning presets as absolute open-string MIDI notes, low to high.
constexpr int kNumTunings = 7;
static const int kTunings[kNumTunings][6] = {
	{ 40, 45, 50, 55, 59, 64 },   // Standard   E2 A2 D3 G3 B3 E4
	{ 38, 45, 50, 55, 59, 64 },   // Drop D     D2 A2 D3 G3 B3 E4
	{ 39, 44, 49, 54, 58, 63 },   // Eb standard
	{ 38, 45, 50, 55, 57, 62 },   // DADGAD     D2 A2 D3 G3 A3 D4
	{ 40, 47, 52, 56, 59, 64 },   // Open E     E2 B2 E3 G#3 B3 E4
	{ 41, 45, 48, 55, 60, 64 },   // FACGCE     F2 A2 C3 G3 C4 E4
	{ 38, 45, 52, 57, 61, 64 },   // DAEAC#E    D2 A2 E3 A3 C#4 E4
};

struct ParamState
{
	std::atomic<float> v[kNumParams];

	ParamState ()
	{
		for (int i = 0; i < (int) kNumParams; ++i)
			v[i].store (kParamDefs[i].def, std::memory_order_relaxed);
	}
};

// ---------------------------------------------------------------------------
// Sympathetic resonance without a feedback loop.
//
// Injecting the sum of the strings back into every string is a positive
// feedback delay network, not a bridge: its loop gain can exceed unity and it
// grows out of the noise floor until the amp saturates.  A bank of decaying
// resonators tuned to the open strings gives the audible behaviour (open
// strings bloom when a chord leaves those pitches free) and is stable.
class SympatheticBank
{
public:
	void prepare (float sampleRate)
	{
		fs = sampleRate;
		tuned = -1;
		retune (0);
	}
	void retune (int tuning)
	{
		if (tuning == tuned) return;
		tuned = tuning;
		for (int i = 0; i < 6; ++i)
		{
			const float f = 440.f * std::pow (2.f, (float) (kTunings[tuning][i] - 69) / 12.f);
			r[i].set (f, 0.9f, fs);
			r[i].reset ();
		}
	}
	void reset () { for (int i = 0; i < 6; ++i) r[i].reset (); hpZ = 0.f; }

	inline float process (float x)
	{
		// High-pass the drive: a low E2 must not pump the E2 resonator, or the
		// whole low end blooms and masks everything played after it.
		const float hp = x - hpZ;
		hpZ += 0.02f * hp;
		float s = 0.f;
		for (int i = 0; i < 6; ++i) s += r[i].process (hp);
		return s;
	}

private:
	Resonator r[6];
	float hpZ = 0.f;
	float fs = 48000.f;
	int tuned = -1;
};

class Engine
{
public:
	Engine () {}

	// ---- lifecycle ------------------------------------------------------
	void prepare (double sampleRate)
	{
		fs = (float) sampleRate;
		const int maxDelay = (int) (fs / 20.f) + 128;
		for (int i = 0; i < kNumStrings; ++i)
		{
			str[i].prepare (fs, maxDelay);
			str[i].setDecay (4.f, 0.04f);
		}
		whammy.prepare (sampleRate);
		// One arm setup.  The bar is played from CC, not from a knob, so its
		// spring and range are fixed to what a Jazzmaster does: about four
		// semitones of dive, a little pull-up, a hand-damped spring.
		whammy.setParams (4.0f, 2.4f, 0.30f, 0.0f, 0.0f);
		pickup.prepare (fs);
		body.prepare (fs);
		symBank.prepare (fs);
		circuit.prepare (fs);
		amp.prepare (fs);
		chorus.prepare (fs);
		delay.prepare (fs);
		reverb.prepare (fs);
		ageLp.setLowpass (5500.f, 0.7f, fs);
		feedback.prepare (fs);
		for (int i = 0; i < kNumVoices; ++i) voice[i] = -1;
		reset ();
	}

	void reset ()
	{
		for (int i = 0; i < kNumStrings; ++i) { str[i].clear (); voice[i] = -1; }
		whammy.reset ();
		pickup.reset ();
		body.reset ();
		symBank.reset ();
		circuit.reset ();
		amp.reset ();
		chorus.reset ();
		delay.reset ();
		reverb.reset ();
		ageLp.reset ();
		feedback.reset ();
		technique.reset ();
		for (int i = 0; i < kNumStrings; ++i) { sustained_[i] = false; swellEnv_[i] = 1.f; }
		lastNote_ = -1;
		lastVel_ = 0.f;
		tremCounter_ = 0;
		for (int i = 0; i < kNumStrings; ++i) { stackCount_[i] = 0; voice[i] = -1; }
		lastSlot_ = -1;
		sustainPedal_ = false;
		expression_ = expressionTarget_ = 1.f;
		scopeW = 0;
		peak = 0.f;
		bend_ = 0.f;
		aftertouch_ = 0.f;
		modWheel_ = 0.f;
		pendingCount = 0;
	}

	// ---- parameters -----------------------------------------------------
	void setParam (int id, float plainValue)
	{
		if (id < 0 || id >= (int) kNumParams) return;
		p_.v[id].store (plainValue, std::memory_order_relaxed);
	}

	float getParam (int id) const
	{
		if (id < 0 || id >= (int) kNumParams) return 0.f;
		return p_.v[id].load (std::memory_order_relaxed);
	}

	// ---- keyswitches ----------------------------------------------------
	// Returns true if the note was a technique key and must not sound.
	// Pickup / circuit switches are *parameters*, so the wrapper handles those
	// (it has to notify the host); everything that is a hand movement is here.
	bool keyswitch (int note, bool on)
	{
		switch (note)
		{
		case kKsPalmMute: technique.palmMute = on; return true;
		case kKsHarmonic: technique.harmonic = on; return true;
		case kKsTap:      technique.tap = on;      return true;
		case kKsChoke:    if (on) choke ();        return true;
		case kKsPickBridge: if (on) technique.pickPos = 0; return true;
		case kKsPickMiddle: if (on) technique.pickPos = 1; return true;
		case kKsPickNeck:   if (on) technique.pickPos = 2; return true;
		case kKsSwell:      technique.swell = on; return true;
		case kKsTremPick:   technique.tremPick = on; return true;
		case kKsFeedback:   technique.feedback = on; return true;
		default: return false;
		}
	}

	int techniqueMask () const { return technique.mask (); }

	void choke ()
	{
		for (int i = 0; i < kNumStrings; ++i)
		{
			voice[i] = -1;
			stackCount_[i] = 0;
			str[i].damp (0.03f);
		}
		pendingCount = 0;
		lastSlot_ = -1;
	}

	// ---- notes ----------------------------------------------------------
	void noteOn (int midiNote, float velocity, bool retrigger = false)
	{
		// The host may set parameters and send the note in the same block; the
		// block-rate mirrors would still hold the previous values and the note
		// would be voiced with stale sustain / brightness.
		updateFromParams ();

		const int tune = std::min (kNumTunings - 1,
		                           std::max (0, (int) (getParam (kTuning) + 0.5f)));
		int best = 0;
		for (int i = 5; i >= 0; --i)
			if (midiNote >= kTunings[tune][i]) { best = i; break; }

		// If the note is already held on a string, this is a re-strike of that
		// note, not a new allocation.
		int slot = -1;
		for (int i = 0; i < kNumStrings; ++i)
			if (stackCount_[i] > 0 && noteStack_[i][stackCount_[i] - 1] == midiNote)
			{ slot = i; break; }

		// Legato first: a note arriving while the previous one is still held and
		// within a hand's reach belongs on the *same* string -- that is what
		// makes a hammer-on a hammer-on instead of a second string.
		if (slot < 0 && technique.tap && lastSlot_ >= 0 && stackCount_[lastSlot_] > 0)
		{
			const int top = noteStack_[lastSlot_][stackCount_[lastSlot_] - 1];
			const int opened = kTunings[tune][std::max (0, voice[lastSlot_])];
			if (top != midiNote && std::abs (midiNote - top) <= 7 &&
			    midiNote >= opened && midiNote <= opened + 19)
				slot = lastSlot_;
		}

		if (slot < 0)
		{
			for (int i = 0; i < kNumStrings; ++i)
				if (voice[i] == best && str[i].isAlive ()) { slot = i; break; }
		}
		if (slot < 0)
		{
			if (voice[best] < 0 || !str[best].isAlive ()) slot = best;
			else
				for (int i = 0; i < kNumStrings; ++i)
					if (voice[i] < 0 || !str[i].isAlive ()) { slot = i; break; }
		}
		if (slot < 0)
		{
			float minE = 1e9f;
			slot = 0;
			for (int i = 0; i < kNumStrings; ++i)
				if (str[i].getEnergy () < minE) { minE = str[i].getEnergy (); slot = i; }
			stackCount_[slot] = 0;                 // stealing kills the old note
		}

		voice[slot] = best;

		const int open = kTunings[tune][best];
		const int semis = midiNote - open;
		const float fretRatio = std::pow (2.f, -(float) semis / 12.f);
		const float f = 440.f * std::pow (2.f, ((float) midiNote - 69.f) / 12.f);

		// ---- technique -> excitation ------------------------------------
		float pickPos = 0.16f;
		if (technique.pickPos == 0) pickPos = 0.085f;
		else if (technique.pickPos == 2) pickPos = 0.30f;
		if (technique.harmonic) pickPos = 0.5f;

		float hardness = 0.45f + 0.55f * clampf (velocity, 0.f, 1.f);
		float vel = clampf (velocity, 0.02f, 1.f);
		float brightness = getParam (kBrightness);

		if (technique.tap)
		{
			// A tapped note is not picked: the fingertip hammers the string
			// against the fret and the note starts from that contact.
			hardness = 0.04f;
			vel *= 0.80f;
			brightness = clampf (brightness + 0.10f, 0.f, 1.f);
		}
		if (technique.harmonic)
		{
			hardness *= 0.6f;
			brightness = clampf (brightness + 0.25f, 0.f, 1.f);
		}

		const int prevTop = stackCount_[slot] > 0 ? noteStack_[slot][stackCount_[slot] - 1] : -1;

		if (stackCount_[slot] < kStackMax)
			noteStack_[slot][stackCount_[slot]++] = midiNote;
		else
			noteStack_[slot][kStackMax - 1] = midiNote;

		str[slot].setFrequency (f, dispFromBrightness (), hfFromBrightness (brightness));
		str[slot].setDecay (decaySeconds (), hfFromBrightness (brightness));
		str[slot].setMute (technique.palmMute ? 1.f : 0.f);
		str[slot].setSympathetic (0.f);
		str[slot].setRatio (whammy.ratio ());

		if (prevTop >= 0 && prevTop != midiNote && !retrigger)
		{
			// Hammer-on: the string is already sounding, so only the speaking
			// length changes.  A small contact burst stands in for the finger
			// hitting the fret -- no pick, no reset of the loop.
			str[slot].hammer (0.08f + 0.22f * vel, pickPos, brightness, rng);
			body.excite (vel * 0.5f, 0.4f);
		}
		else
		{
			str[slot].pluck (vel * 0.85f, pickPos, hardness, brightness, 0.f, rng);
			body.excite (vel, hardness);
		}

		whammy.addEnergy (vel * 0.35f);

		lastNote_ = midiNote;
		if (!retrigger) lastVel_ = clampf (velocity, 0.02f, 1.f);
		lastFreq_ = f;
		tremCounter_ = (int) (fs / 14.f);

		swellEnv_[slot] = technique.swell ? 0.f : 1.f;

		fret[slot] = fretRatio;
		lastSlot_ = slot;
	}

	void noteOff (int midiNote)
	{
		const int tune = std::min (kNumTunings - 1,
		                           std::max (0, (int) (getParam (kTuning) + 0.5f)));
		for (int i = 0; i < kNumStrings; ++i)
		{
			if (stackCount_[i] == 0) continue;

			// find the note in the stack
			int at = -1;
			for (int k = stackCount_[i] - 1; k >= 0; --k)
				if (noteStack_[i][k] == midiNote) { at = k; break; }
			if (at < 0) continue;

			for (int k = at; k < stackCount_[i] - 1; ++k)
				noteStack_[i][k] = noteStack_[i][k + 1];
			--stackCount_[i];

			if (stackCount_[i] > 0)
			{
				// Pull-off: the finger lifts and the note underneath is left
				// sounding.  Retune the string to it; no pick, no damper.
				const int back = noteStack_[i][stackCount_[i] - 1];
				const float f = 440.f * std::pow (2.f, ((float) back - 69.f) / 12.f);
				const int open = kTunings[tune][std::max (0, voice[i])];
				const float ratio = std::pow (2.f, -(float) (back - open) / 12.f);
				str[i].setFrequency (f, dispFromBrightness (), hfFromBrightness (getParam (kBrightness)));
				str[i].setRatio (whammy.ratio ());
				str[i].hammer (0.05f + 0.10f, 0.16f, getParam (kBrightness), rng);
				fret[i] = ratio;
			}
			else
			{
				voice[i] = -1;
				if (sustainPedal_)
					sustained_[i] = true;
				else
					str[i].damp (releaseT60 ());
			}
		}
	}

	void allNotesOff ()
	{
		for (int i = 0; i < kNumStrings; ++i)
		{
			voice[i] = -1;
			stackCount_[i] = 0;
			str[i].damp (0.06f);
		}
		lastSlot_ = -1;
		delay.clearTail ();
		reverb.clearTail ();
		pendingCount = 0;
	}

	// A strummed chord is not six notes at the same instant.  The host delivers
	// all six note-ons in one block, so the wrapper staggers them and this queue
	// fires each pluck on its own sample.
	void noteOnDelayed (int midiNote, float velocity, int delaySamples)
	{
		if (pendingCount >= kMaxPending) { noteOn (midiNote, velocity); return; }
		if (delaySamples < 0) delaySamples = 0;
		pending[pendingCount++] = { midiNote, velocity, delaySamples };
	}

	// The bar is played from CC (and pitch bend).  The spring model turns a
	// stepped controller into a physical dive and return.
	// CC11: the volume pedal.  A swell played with the pedal is the same
	// gesture as CC11 rising; it changes the level the amp sees, so a swell
	// also cleans up the distortion as it opens.
	void setExpression (float v) { expressionTarget_ = clampf (v, 0.f, 1.f); }

	// CC64: hold notes that have been released.
	void setSustainPedal (bool on)
	{
		if (sustainPedal_ && !on)
		{
			for (int i = 0; i < kNumStrings; ++i)
				if (sustained_[i]) { sustained_[i] = false; str[i].damp (releaseT60 ()); }
		}
		sustainPedal_ = on;
	}

	void setPitchBend (float v) { bend_ = clampf (v, -1.f, 1.f); }
	void setAftertouch (float v) { aftertouch_ = clampf (v, 0.f, 1.f); }
	void setModWheel (float v) { modWheel_ = clampf (v, 0.f, 1.f); }
	float pitchBend () const { return bend_; }

	// ---- audio ----------------------------------------------------------
	void process (float* outL, float* outR, int n)
	{
		updateFromParams ();

		for (int i = 0; i < n; ++i)
		{
			for (int q = 0; q < pendingCount; )
			{
				if (--pending[q].delay <= 0)
				{
					noteOn (pending[q].note, pending[q].velocity);
					pending[q] = pending[--pendingCount];
				}
				else ++q;
			}

			// tremolo picking: re-strike the held note 14 times a second while
			// the key is down, in place on the same string.
			if (technique.tremPick && lastNote_ >= 0)
			{
				if (--tremCounter_ <= 0)
				{
					noteOn (lastNote_, lastVel_ * 0.9f, true);
					tremCounter_ = (int) (fs / 14.f);
				}
			}

			barLfoPhase_ += barLfoRate_ / fs;
			if (barLfoPhase_ >= 1.f) barLfoPhase_ -= 1.f;
			const float barLfo = std::sin (kTwoPi * barLfoPhase_) * barLfoDepth_ * 0.25f;
			whammy.setTarget (barParam_ + barLfo - bend_ + modWheel_ + aftertouch_ * 0.9f);
			whammy.tick ();

			// ---- strings + pickups ---------------------------------------
			float neckRaw = 0.f;
			float hbRaw = 0.f;
			for (int s = 0; s < kNumStrings; ++s)
			{
				if (!str[s].isAlive ()) continue;
				str[s].setRatio (whammy.ratio ());
				str[s].process ();
				const float invFret = 1.f / std::max (0.06f, fret[s]);
				swellEnv_[s] += (1.f - swellEnv_[s]) * 0.00006f;
				const float sw = swellEnv_[s];
				neckRaw += Pickup::p90 (str[s], Pickup::kNeckPos * invFret) * sw;
				hbRaw += Pickup::humbucker (str[s], Pickup::kBridgePos * invFret) * sw;
			}

			const float p90 = pickup.shapeP90 (neckRaw);
			const float hb = pickup.shapeHB (hbRaw);

			// ---- circuit -------------------------------------------------
			float mono;
			if (circuitRhythm)
				mono = p90;                       // the rhythm circuit is neck-only
			else if (pickupSel == 0)
				mono = p90;
			else if (pickupSel == 2)
				mono = hb;
			else
				mono = 0.52f * (p90 + hb);        // both, with the usual loading loss

			mono *= 0.62f + 0.85f * puHeight;
			if (age > 0.001f)
				mono += (ageLp.process (mono) - mono) * age;
			mono = pickup.dcBlock (mono);

			// Controlled feedback rides on the string, locked to the last note.
			feedback.setPitch (lastFreq_);
			mono += feedback.process (mono, technique.feedback);

			mono = circuit.process (mono);

			float l = mono * 0.62f;
			float r = l;

			if (sympathy > 0.001f && l != 0.f)
				l += symBank.process (l) * sympathy * 2.2f;

			// ---- body, amp, space ---------------------------------------
			{
				const float bodyOut = body.process (l);
				l = bodyOut;
				r = bodyOut;
			}
			amp.process (l, r);
			chorus.process (l, r, chorusAmt);
			delay.process (l, r, delayMs, delayMix);
			reverb.process (l, r, revSize, revMix);

			const float mid = 0.5f * (l + r);
			const float side = 0.5f * (l - r) * width * 1.6f;
			l = mid + side;
			r = mid - side;

			if (tremDepth > 0.0005f)
			{
				tremPhase += tremRate / fs;
				if (tremPhase >= 1.f) tremPhase -= 1.f;
				const float m = 1.f - tremDepth * (0.5f + 0.5f * std::sin (kTwoPi * tremPhase));
				l *= m;
				r *= m;
			}

			// CC11 volume pedal, smoothed: a swell must not zipper.
			expression_ += (expressionTarget_ - expression_) * 0.0004f;
			l *= outGain * expression_;
			r *= outGain * expression_;
			l = sanitize (l);
			r = sanitize (r);

			outL[i] = l;
			outR[i] = r;

			scope[scopeW & (kScope - 1)] = 0.5f * (l + r);
			scopeW = (scopeW + 1) & (kScope - 1);
			const float a = std::fabs (l) > std::fabs (r) ? std::fabs (l) : std::fabs (r);
			if (a > peak) peak = a;
		}
	}

	// ---- display --------------------------------------------------------
	void copyScope (float* dst, int n) const
	{
		const int cnt = n < kScope ? n : kScope;
		for (int i = 0; i < cnt; ++i)
			dst[i] = scope[(scopeW - cnt + i) & (kScope - 1)];
	}

	float takePeak ()
	{
		const float p = peak;
		peak = 0.f;
		return p;
	}

	float barSemis () const { return whammy.semitones (); }
	float barAngle () const { return whammy.barAngle (); }
	float activeStrings () const
	{
		int c = 0;
		for (int i = 0; i < kNumStrings; ++i) if (str[i].isAlive ()) ++c;
		return (float) c;
	}

	Rng rng { 0xC0FFEE11u };

private:
	static constexpr int kScope = 1024;

	float releaseT60 () const { return 0.30f; }

	static inline float sanitize (float x)
	{
		if (!(x == x)) return 0.f;
		const float a = std::fabs (x);
		if (a <= 0.94f) return x;
		const float over = a - 0.94f;
		const float shaped = 0.94f + over / (1.f + 17.f * over);
		return x < 0.f ? -shaped : shaped;
	}

	float dispFromBrightness () const { return 0.06f + 0.22f * brightness_; }

	float hfFromBrightness (float b) const
	{
		// The loop filter runs once per period, so this coefficient is tiny by
		// design: 0.05 is already a -3.7 dB/s tilt at 2 kHz.
		return 0.02f + 0.06f * clampf (b, 0.f, 1.f);
	}

	float decaySeconds () const { return 0.5f + 19.5f * sustain_; }

	void updateFromParams ()
	{
		sustain_ = getParam (kSustain);
		brightness_ = getParam (kBrightness);
		sympathy = getParam (kSympathy);
		puHeight = getParam (kPuHeight);
		age = getParam (kAge);

		pickupSel = (int) (getParam (kPickupSel) + 0.5f);
		circuitRhythm = getParam (kCircuit) > 0.5f;
		circuit.setParams (getParam (kVolume), getParam (kTone),
		                   getParam (kRhythmTone), circuitRhythm);

		body.setAmount (getParam (kBody));
		body.setBehindBridge (getParam (kBehindBridge));
		body.setBuzz (technique.palmMute ? 0.f : 0.12f);

		const float mute = technique.palmMute ? 1.f : 0.f;
		for (int s = 0; s < kNumStrings; ++s)
		{
			str[s].setMute (mute);
			str[s].setSympathetic (0.f);
		}
		symBank.retune (std::min (kNumTunings - 1,
		                          std::max (0, (int) (getParam (kTuning) + 0.5f))));

		barParam_ = getParam (kBar);
		barLfoRate_ = getParam (kBarLfoRate);
		barLfoDepth_ = getParam (kBarLfoDepth);

		amp.setParams (getParam (kDrive), (int) (getParam (kDriveChar) + 0.5f),
		               getParam (kBass), getParam (kMid), getParam (kTreble),
		               getParam (kPresence), getParam (kCab));

		chorusAmt = getParam (kChorus);
		delayMs = getParam (kDelayTime);
		delayMix = getParam (kDelayMix);
		revSize = getParam (kRevSize);
		revMix = getParam (kRevMix);
		width = getParam (kWidth);

		tremRate = getParam (kTremRate);
		tremDepth = getParam (kTremDepth);
		outGain = dbToGain (getParam (kOutput));

		const int rm = (int) (getParam (kRevMode) + 0.5f);
		if (rm != revModeCur) { revModeCur = rm; reverb.setMode (rm); }
	}

	WaveString str[kNumStrings];
	WhammyModel whammy;
	Pickup pickup;
	BodyModel body;
	SympatheticBank symBank;
	Circuit circuit;
	FeedbackModel feedback;
	Amp amp;
	Chorus chorus;
	Delay delay;
	Reverb reverb;
	Biquad ageLp;

	TechniqueState technique;

	ParamState p_;
	int voice[kNumStrings] = { -1, -1, -1, -1, -1, -1 };
	float fret[kNumStrings] = { 1, 1, 1, 1, 1, 1 };

	// Legato / tapping: what is held on each string, bottom to top.  A note-on
	// over a ringing string is a hammer-on (the speaking length changes, the
	// string keeps its energy); the matching note-off is a pull-off back to the
	// note underneath, not a damper.
	static constexpr int kStackMax = 4;
	int noteStack_[kNumStrings][kStackMax] = {};
	int stackCount_[kNumStrings] = {};
	int lastSlot_ = -1;

	// block-rate mirrors
	float sustain_ = 0.55f, brightness_ = 0.6f;
	int   pickupSel = 1;
	bool  circuitRhythm = false;
	float puHeight = 0.5f, age = 0.15f;
	float chorusAmt = 0.1f, delayMs = 380.f, delayMix = 0.18f;
	float revSize = 0.62f, revMix = 0.2f, width = 0.6f;
	float tremRate = 3.5f, tremDepth = 0.f, outGain = 0.63f;
	float sympathy = 0.25f;
	float tremPhase = 0.f;
	int   revModeCur = -1;

	float bend_ = 0.f, aftertouch_ = 0.f, modWheel_ = 0.f, barParam_ = 0.f;
	// The bar's own LFO: an always-available vibrato that the spring model
	// turns into a hand-like wobble instead of a stepped sine.
	float barLfoPhase_ = 0.f, barLfoRate_ = 0.25f, barLfoDepth_ = 0.f;
	float expression_ = 1.f, expressionTarget_ = 1.f;
	bool sustainPedal_ = false;
	bool sustained_[kNumStrings] = { false, false, false, false, false, false };
	float swellEnv_[kNumStrings] = { 1.f, 1.f, 1.f, 1.f, 1.f, 1.f };
	int lastNote_ = -1;
	float lastVel_ = 0.f;
	float lastFreq_ = 110.f;
	int tremCounter_ = 0;

	struct PendingNote { int note; float velocity; int delay; };
	static constexpr int kMaxPending = 32;
	PendingNote pending[kMaxPending] {};
	int pendingCount = 0;

	float fs = 48000.f;
	float peak = 0.f;
	float scope[kScope] {};
	int   scopeW = 0;
};

} // namespace jm
