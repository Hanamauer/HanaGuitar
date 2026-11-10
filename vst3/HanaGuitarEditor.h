// ===========================================================================
//  HanaGuitarEditor.h -- the only channel between the panel and the plugin
//
//  Deliberately tiny.  The audio thread never touches the editor and the editor
//  never touches the engine: knob moves leave as host edit gestures, and the
//  read-only display values (level, bar readout, technique lamps) are atomics
//  inside the engine.
// ===========================================================================
#pragma once

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/gui/iplugview.h"

namespace jm {

class IParamSink
{
public:
	virtual ~IParamSink () {}

	// Edit gestures, so automation records instead of being a silent write.
	virtual void onBeginEdit (int id) = 0;
	virtual void onPerformEdit (int id, double value) = 0;
	virtual void onEndEdit (int id) = 0;

	// Current value, for laying out controls and for host-driven updates.
	virtual double getNormalized (int id) = 0;

	// Read-only display feed.
	virtual void copyScope (float* dst, int n) = 0;
	virtual float takePeak () = 0;
	virtual double sampleRate () = 0;

	// The bar as the engine has it: the panel shows this as a pitch readout
	// (the arm itself is not drawn; the artwork is the guitar).
	virtual float barSemitones () = 0;

	// bit 0 palm mute, bit 1 harmonics, bit 2 tap -- the keyswitch lamps.
	virtual int techniqueMask () = 0;

	// Lifecycle.  The editor tells the plugin which view is live, so the plugin
	// can never hold a pointer to a view the host has already released.
	virtual void setActiveEditor (Steinberg::IPlugView* view) = 0;
};

Steinberg::IPlugView* createHanaGuitarEditor (IParamSink* sink);

// Safe from any thread: posts a window message and nothing else.
void notifyEditorParamChanged (Steinberg::IPlugView* view, int id);

} // namespace jm
