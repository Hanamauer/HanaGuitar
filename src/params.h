// HanaGuitar for indie - Physical offset-body guitar (P90 neck + humbucker bridge)
// Parameter definitions
//
// Everything that is a *playing technique* is gone from this table: palm mute,
// harmonics, tapping, picking position, choke and the pickup/circuit switches
// are keyswitches (see keyswitch.h) or, for the whammy bar, CC.  What remains
// here is the instrument: pickups, the two circuits, the amp and the space.
#pragma once

#include "pluginterfaces/vst/vsttypes.h"
#include "pluginterfaces/base/ustring.h"
#include "public.sdk/source/vst/vstparameters.h"

#include <cstdio>

namespace jm {

using Steinberg::Vst::ParamID;
using Steinberg::Vst::ParamValue;

enum P : ParamID
{
	// --- column 1: STRING
	kSustain = 0,
	kBrightness,
	kSympathy,
	// --- column 2: PICKUP
	kPickupSel,
	kPuHeight,
	kAge,
	kBody,
	kBehindBridge,
	// --- column 3: CIRCUIT (the Jazzmaster's two wiring harnesses)
	kCircuit,
	kVolume,
	kTone,
	kRhythmTone,
	// --- column 4: AMP
	kDrive,
	kDriveChar,
	kBass,
	kMid,
	kTreble,
	kPresence,
	kCab,
	// --- column 5: SPACE
	kChorus,
	kDelayTime,
	kDelayMix,
	kRevSize,
	kRevMix,
	kWidth,
	// --- column 6: MASTER
	kTremRate,
	kTremDepth,
	kBarLfoRate,
	kBarLfoDepth,
	kTuning,
	kRevMode,
	kOutput,
	// --- preset selector
	kPreset,
	// --- the bar itself, exposed for host automation (no panel knob: it is
	// played from CC1 / pitch bend, but a DAW that does not forward CC can
	// still automate this parameter, and the panel's BAR readout is draggable)
	kBar,

	kNumParams
};

static constexpr int kNumKnobs = kPreset;   // every knob except the preset button

struct ParamDef
{
	P id;
	const char* name;
	const char* group;
	const char* unit;
	float min, max, def;
	int steps;                   // 0 = continuous, >0 = discrete
	const char* const* items;    // for discrete
	int numItems;
};

// ---------------------------------------------------------------------------
// value tables
// ---------------------------------------------------------------------------
static const char* kPickupItems[] = { "P90", "Both", "HB" };
static const char* kDriveItems[] = { "Clean", "Overdrive", "Fuzz", "Muff" };
static const char* kCircuitItems[] = { "Lead", "Rhythm" };
static const char* kTuningItems[] = { "Standard", "Drop D", "Eb Standard", "DADGAD",
                                      "Open E", "FACGCE", "DAEAC#E" };
static const char* kRevItems[] = { "Room", "Hall", "Plate", "Gate", "Reverse" };
static const char* kPresetItems[] = { "Shoegaze", "Midwest Emo", "Glide Lead", "Clean JM" };

// ---------------------------------------------------------------------------
// parameter table. Order MUST match the enum above (ids are contiguous).
// ---------------------------------------------------------------------------
static const ParamDef kParamDefs[] = {
	// STRING
	{ kSustain,      "Sustain",     "STRING",  "",   0.f, 1.f,  0.65f, 0, nullptr, 0 },
	{ kBrightness,   "Brightness",  "STRING",  "",   0.f, 1.f,  0.66f, 0, nullptr, 0 },
	{ kSympathy,     "Sympathy",    "STRING",  "",   0.f, 1.f,  0.25f, 0, nullptr, 0 },
	// PICKUP
	{ kPickupSel,    "Pickup",      "PICKUP",  "",   0.f, 2.f,  1.f,   2, kPickupItems, 3 },
	{ kPuHeight,     "Height",      "PICKUP",  "",   0.f, 1.f,  0.50f, 0, nullptr, 0 },
	{ kAge,          "String Age",  "PICKUP",  "",   0.f, 1.f,  0.15f, 0, nullptr, 0 },
	{ kBody,         "Body",        "PICKUP",  "",   0.f, 1.f,  0.68f, 0, nullptr, 0 },
	{ kBehindBridge, "B.T.B.",      "PICKUP",  "",   0.f, 1.f,  0.30f, 0, nullptr, 0 },
	// CIRCUIT
	{ kCircuit,      "Circuit",     "CIRCUIT", "",   0.f, 1.f,  0.f,   1, kCircuitItems, 2 },
	{ kVolume,       "Volume",      "CIRCUIT", "",   0.f, 1.f,  1.00f, 0, nullptr, 0 },
	{ kTone,         "Tone",        "CIRCUIT", "",   0.f, 1.f,  0.60f, 0, nullptr, 0 },
	{ kRhythmTone,   "Rhythm Tone", "CIRCUIT", "",   0.f, 1.f,  0.55f, 0, nullptr, 0 },
	// AMP
	{ kDrive,        "Drive",       "AMP",     "",   0.f, 1.f,  0.08f, 0, nullptr, 0 },
	{ kDriveChar,    "Character",   "AMP",     "",   0.f, 3.f,  0.f,   3, kDriveItems, 4 },
	{ kBass,         "Bass",        "AMP",     "dB", -12.f, 12.f, 0.f, 0, nullptr, 0 },
	{ kMid,          "Mid",         "AMP",     "dB", -12.f, 12.f, 0.f, 0, nullptr, 0 },
	{ kTreble,       "Treble",      "AMP",     "dB", -12.f, 12.f, 0.f, 0, nullptr, 0 },
	{ kPresence,     "Presence",    "AMP",     "",   0.f, 1.f,  0.40f, 0, nullptr, 0 },
	{ kCab,          "Cabinet",     "AMP",     "",   0.f, 1.f,  0.70f, 0, nullptr, 0 },
	// SPACE
	{ kChorus,       "Chorus",      "SPACE",   "",   0.f, 1.f,  0.10f, 0, nullptr, 0 },
	{ kDelayTime,    "Delay Time",  "SPACE",   "ms", 20.f, 1200.f, 380.f, 0, nullptr, 0 },
	{ kDelayMix,     "Delay Mix",   "SPACE",   "",   0.f, 1.f,  0.18f, 0, nullptr, 0 },
	{ kRevSize,      "Reverb Size", "SPACE",   "",   0.f, 1.f,  0.62f, 0, nullptr, 0 },
	{ kRevMix,       "Reverb Mix",  "SPACE",   "",   0.f, 1.f,  0.20f, 0, nullptr, 0 },
	{ kWidth,        "Width",       "SPACE",   "",   0.f, 1.f,  0.60f, 0, nullptr, 0 },
	// MASTER
	{ kTremRate,     "Trem Rate",   "MASTER",  "Hz", 0.2f, 12.f, 3.50f, 0, nullptr, 0 },
	{ kTremDepth,    "Trem Depth",  "MASTER",  "",   0.f, 1.f,  0.00f, 0, nullptr, 0 },
	{ kBarLfoRate,   "Bar Rate",    "MASTER",  "Hz", 0.03f, 8.f, 0.25f, 0, nullptr, 0 },
	{ kBarLfoDepth,  "Bar Depth",   "MASTER",  "",   0.f, 1.f,  0.00f, 0, nullptr, 0 },
	{ kTuning,       "Tuning",      "MASTER",  "",   0.f, 6.f,  0.f,   6, kTuningItems, 7 },
	{ kRevMode,      "Reverb Mode", "MASTER",  "",   0.f, 4.f,  1.f,   4, kRevItems, 5 },
	{ kOutput,       "Output",      "MASTER",  "dB", -30.f, 12.f, -4.f, 0, nullptr, 0 },
	// preset
	{ kPreset,       "Preset",      "",        "",   0.f, 3.f,  3.f,   3, kPresetItems, 4 },
	{ kBar,          "Bar",         "WHAMMY",  "",   0.f, 1.f,  0.f,   0, nullptr, 0 },
};

static constexpr int kNumDefs = (int) (sizeof (kParamDefs) / sizeof (kParamDefs[0]));

inline const ParamDef& paramDef (int id)
{
	return kParamDefs[id];
}

inline float plainFromNorm (const ParamDef& d, float norm)
{
	norm = norm < 0.f ? 0.f : (norm > 1.f ? 1.f : norm);
	float v = d.min + norm * (d.max - d.min);
	if (d.steps > 0)
		v = (float) (int) (v + 0.5f);
	return v;
}

inline float normFromPlain (const ParamDef& d, float plain)
{
	if (d.max <= d.min)
		return 0.f;
	float n = (plain - d.min) / (d.max - d.min);
	return n < 0.f ? 0.f : (n > 1.f ? 1.f : n);
}

// Convert an ASCII string into a VST TChar buffer
inline void asciiToTChar (const char* s, Steinberg::Vst::TChar* out, int n)
{
	Steinberg::UString (out, n).fromAscii (s);
}

void initParameters (Steinberg::Vst::ParameterContainer& params);

} // namespace jm

namespace Steinberg {
namespace Vst {

// ---------------------------------------------------------------------------
// Parameter subclass that shows plain units and accepts typed input.
// ---------------------------------------------------------------------------
class JmParam : public Parameter
{
public:
	JmParam (const jm::ParamDef& d)
	: def (d)
	{
		UString (info.title, USTRINGSIZE (info.title)).fromAscii (d.name);
		UString (info.shortTitle, USTRINGSIZE (info.shortTitle)).fromAscii (d.name);
		if (d.unit && d.unit[0])
			UString (info.units, USTRINGSIZE (info.units)).fromAscii (d.unit);
		info.stepCount = d.steps;
		info.flags = ParameterInfo::kCanAutomate;
		info.id = d.id;
		info.unitId = kRootUnitId;
		info.defaultNormalizedValue = jm::normFromPlain (d, d.def);
		setNormalized (info.defaultNormalizedValue);
	}

	void toString (ParamValue normValue, String128 string) const SMTG_OVERRIDE
	{
		char text[64];
		const float v = jm::plainFromNorm (def, (float) normValue);
		if (def.items && def.numItems > 0)
		{
			int idx = (int) (v + 0.5f);
			if (idx < 0) idx = 0;
			if (idx >= def.numItems) idx = def.numItems - 1;
			std::snprintf (text, sizeof (text), "%s", def.items[idx]);
		}
		else if (def.unit && def.unit[0])
			std::snprintf (text, sizeof (text), "%.2f %s", v, def.unit);
		else
			std::snprintf (text, sizeof (text), "%.2f", v);
		UString (string, 128).fromAscii (text);
	}

	bool fromString (const TChar* string, ParamValue& normValue) const SMTG_OVERRIDE
	{
		UString wrapper ((TChar*) string, -1);
		if (def.items && def.numItems > 0)
		{
			char ascii[64] = { 0 };
			wrapper.toAscii (ascii, 64);
			for (int i = 0; i < def.numItems; ++i)
			{
				if (strcasecmp_ (ascii, def.items[i]) == 0)
				{
					normValue = jm::normFromPlain (def, (float) i);
					return true;
				}
			}
		}
		double tmp = 0.0;
		if (wrapper.scanFloat (tmp))
		{
			normValue = jm::normFromPlain (def, (float) tmp);
			return true;
		}
		return false;
	}

	float plainValue (ParamValue normValue) const
	{
		return jm::plainFromNorm (def, (float) normValue);
	}

	const jm::ParamDef& definition () const { return def; }

private:
	static int strcasecmp_ (const char* a, const char* b)
	{
		for (;; ++a, ++b)
		{
			int ca = (unsigned char) *a, cb = (unsigned char) *b;
			if (ca >= 'A' && ca <= 'Z') ca += 32;
			if (cb >= 'A' && cb <= 'Z') cb += 32;
			if (ca != cb) return ca - cb;
			if (!ca) return 0;
		}
	}
	jm::ParamDef def;
};

} // Vst
} // Steinberg
