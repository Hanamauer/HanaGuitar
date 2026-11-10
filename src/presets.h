// ===========================================================================
//  HanaGuitar for indie -- factory presets, shared by the editor and the offline renderer
//
//  These live outside the editor so the panel and tools/render_demo.cpp apply
//  exactly the same numbers.  A preset that sounds one way in the GUI and
//  another way in a bounce is a bug that is very hard to see.
//
//  A preset is a full parameter vector: defaults first, then these overrides.
// ===========================================================================
#pragma once

#include "params.h"

namespace jm {

struct PresetDef
{
	const char* name;
	const char* blurb;
	struct KV { int id; float plain; };
	int n;
	KV kv[22];
};

inline const PresetDef kPresets[4] = {
	{ "SHOEGAZE", "wall of glide", 19, {
		{ kSustain, 0.86f }, { kBrightness, 0.62f }, { kSympathy, 0.42f },
		{ kPickupSel, 1.f }, { kPuHeight, 0.58f }, { kAge, 0.24f },
		{ kBody, 0.72f }, { kBehindBridge, 0.46f },
		{ kCircuit, 0.f }, { kVolume, 1.f }, { kTone, 0.52f },
		{ kBarAtk, 0.45f }, { kBarDec, 0.6f }, { kBarSus, 0.55f }, { kBarRel, 1.1f }, { kBarDepth, 0.30f },
		{ kDrive, 0.74f }, { kDriveChar, 2.f }, { kMid, -2.5f },
		{ kChorus, 0.66f }, { kRevMix, 0.52f } } },

	{ "MIDWEST EMO", "tapped and clean", 19, {
		{ kSustain, 0.64f }, { kBrightness, 0.74f }, { kSympathy, 0.24f },
		{ kPickupSel, 1.f }, { kPuHeight, 0.52f }, { kAge, 0.12f },
		{ kBody, 0.50f }, { kBehindBridge, 0.30f },
		{ kCircuit, 0.f }, { kVolume, 1.f }, { kTone, 0.80f },
		{ kDrive, 0.32f }, { kDriveChar, 1.f }, { kMid, 3.f },
		{ kChorus, 0.14f }, { kRevMix, 0.24f } } },

	{ "GLIDE LEAD", "the bar is the instrument", 19, {
		{ kSustain, 0.80f }, { kBrightness, 0.58f }, { kSympathy, 0.20f },
		{ kPickupSel, 0.f }, { kPuHeight, 0.50f }, { kAge, 0.10f },
		{ kBody, 0.58f }, { kBehindBridge, 0.28f },
		{ kCircuit, 0.f }, { kVolume, 1.f }, { kTone, 0.42f },
		{ kBarAtk, 0.20f }, { kBarDec, 0.4f }, { kBarSus, 0.35f }, { kBarRel, 0.9f }, { kBarDepth, 0.40f },
		{ kDrive, 0.44f }, { kDriveChar, 1.f }, { kMid, 1.f },
		{ kChorus, 0.32f }, { kRevMix, 0.38f } } },

	{ "CLEAN JM", "the reference", 0, { { 0, 0.f } } },
};

constexpr int kNumPresets = 4;

template <typename Sink>
inline void applyPresetTo (Sink& sink, int idx)
{
	if (idx < 0) idx = 0;
	if (idx >= kNumPresets) idx = kNumPresets - 1;

	for (int i = 0; i < (int) kNumKnobs; ++i)
		sink.setParam (i, kParamDefs[i].def);

	const PresetDef& p = kPresets[idx];
	for (int i = 0; i < p.n; ++i)
		sink.setParam (p.kv[i].id, p.kv[i].plain);

	sink.setParam (kPreset, (float) idx);
}

} // namespace jm
