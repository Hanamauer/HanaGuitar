// ===========================================================================
//  HanaGuitarVST3.cpp -- VST3 wrapper for the HanaGuitar for indie engine
//
//  One translation unit, same shape as the rest of this workspace: the part
//  that cannot be tested without a host stays small enough to read in one
//  sitting, and touches as few SDK symbols as possible.
//
//  THREADING
//      audio thread    -> process()            -> engine + atomics
//      UI thread       -> window proc          -> IParamSink only
//      host/automation -> setParamNormalized() -> atomics + a window message
//
//  The editor never touches the engine and the audio thread never touches the
//  editor.  The only shared state is the parameter atomics plus the scope ring
//  and the peak meter, which are read-only display feeds.
//
//  PARAMETER PATH
//  The host talks in *normalised* 0..1 values, the engine talks in plain units
//  (Hz, semitones, dB).  Conversion happens in exactly one place -- applyParams()
//  -- so a wrong scaling cannot hide in the middle of the DSP.
// ===========================================================================

#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "public.sdk/source/main/pluginfactory.h"

#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/gui/iplugview.h"
#include "base/source/fstreamer.h"

#include "HanaGuitarEditor.h"
#include "../src/engine.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

void widen (char16* dst, const char* src, int cap)
{
	if (cap <= 0) return;
	int i = 0;
	for (; i < cap - 1 && src && src[i]; ++i)
		dst[i] = static_cast<char16> (static_cast<unsigned char> (src[i]));
	for (; i < cap; ++i) dst[i] = 0;
}

const char16* busName (const char* s)
{
	// Static per call site: the SDK stores the pointer, so it must outlive the
	// call.  The strings are literals, so a small ring of buffers is enough.
	static char16 buf[8][64];
	static int next = 0;
	char16* b = buf[next];
	next = (next + 1) & 7;
	widen (b, s, 64);
	return b;
}

// ---------------------------------------------------------------------------
class HanaGuitarVST3 : public SingleComponentEffect, public jm::IParamSink
{
public:
	HanaGuitarVST3 ()
	{
		// p_ holds *normalised* values (it mirrors what the host sends), while
		// jm::ParamState's own defaults are plain units.  Seed it correctly so
		// a host that never touches a parameter still gets the documented
		// defaults rather than a mis-scaled 0.55 Hz reverb.
		for (int i = 0; i < (int) jm::kNumParams; ++i)
			p_.v[i].store (jm::normFromPlain (jm::kParamDefs[i], jm::kParamDefs[i].def),
			               std::memory_order_relaxed);
	}
	// Parameter objects are owned by SingleComponentEffect's ParameterContainer:
	// addParameter() adopts them through IPtr<Parameter> (addRef = false), so
	// terminate()'s removeAll() -- or ~ParameterContainer() when terminate() was
	// never called -- releases and destroys them.  Deleting them here as well is
	// a double free: that was the access violation on unload.
	~HanaGuitarVST3 () override = default;

	static FUnknown* createInstance (void*) { return (IAudioProcessor*) new HanaGuitarVST3 (); }

	static const FUID uid;
	// ---- lifecycle -------------------------------------------------------
	tresult PLUGIN_API initialize (FUnknown* context) SMTG_OVERRIDE
	{
		tresult r = SingleComponentEffect::initialize (context);
		if (r != kResultOk) return r;

		// A single-component effect is both the processor and the controller,
		// so a host that calls initialize() through both interfaces initialises
		// the same object twice.  Without this guard every bus and parameter is
		// registered twice (37 parameters become 74, one output becomes two).
		if (initialized_) return kResultOk;

		// An instrument: one stereo output, one MIDI input, no audio input.
		addAudioOutput (busName ("Stereo Out"), SpeakerArr::kStereo);
		addEventInput (busName ("MIDI In"), 16);

		for (int i = 0; i < (int) jm::kNumParams; ++i)
		{
			JmParam* p = new JmParam (jm::kParamDefs[i]);
			params_[i] = p;
			parameters.addParameter (p);
			p_.v[i].store (p->getNormalized (), std::memory_order_relaxed);
		}

		initialized_ = true;
		return kResultOk;
	}

	tresult PLUGIN_API terminate () SMTG_OVERRIDE
	{
		prepared_ = false;
		editor_ = nullptr;
		outL_.clear (); outR_.clear ();
		// Base terminate() calls parameters.removeAll(), which releases -- and
		// therefore destroys -- every Parameter added in initialize().  Keeping
		// the convenience pointers would leave them dangling, and keeping the
		// init guard would make a later initialize() silently skip registering
		// busses and parameters.  A host that terminates and re-initialises
		// (some do, on device change) must get a clean second birth.
		for (int i = 0; i < (int) jm::kNumParams; ++i) params_[i] = nullptr;
		initialized_ = false;
		return SingleComponentEffect::terminate ();
	}

	tresult PLUGIN_API canProcessSampleSize (int32 n) SMTG_OVERRIDE
	{
		return (n == kSample32 || n == kSample64) ? kResultTrue : kResultFalse;
	}

	// An instrument with a reverb on the end has a tail, and telling the host
	// "no tail" makes it truncate the last chord of a shoegaze wash.
	uint32 PLUGIN_API getTailSamples () SMTG_OVERRIDE { return 48000u; }

	tresult PLUGIN_API setProcessing (TBool state) SMTG_OVERRIDE
	{
		// Some hosts call setProcessing(false) on stop and expect the next
		// start to be clean; others never call it at all.
		if (!state) engine_.allNotesOff ();
		return kResultOk;
	}

	tresult PLUGIN_API setupProcessing (ProcessSetup& s) SMTG_OVERRIDE
	{
		int32 maxBlock = s.maxSamplesPerBlock;
		if (maxBlock < 1) maxBlock = 1;
		if (maxBlock > (1 << 20)) maxBlock = 1 << 20;

		try
		{
			const size_t n = (size_t) maxBlock;
			outL_.assign (n, 0.f);
			outR_.assign (n, 0.f);
		}
		catch (...)
		{
			prepared_ = false;
			return kResultFalse;
		}

		hostRate_ = s.sampleRate;
		engine_.prepare (s.sampleRate);
		prepared_ = true;
		return SingleComponentEffect::setupProcessing (s);
	}

	tresult PLUGIN_API setActive (TBool state) SMTG_OVERRIDE
	{
		if (state)
		{
			engine_.reset ();
			applyParams ();
		}
		else
		{
			// A host that deactivates us must not leave the strings ringing into
			// the next activation.
			engine_.allNotesOff ();
		}
		return SingleComponentEffect::setActive (state);
	}

	// ---- parameters ------------------------------------------------------
	tresult PLUGIN_API setParamNormalized (ParamID tag, ParamValue value) SMTG_OVERRIDE
	{
		if (tag < (ParamID) jm::kNumParams)
		{
			if (!(value == value)) value = 0.0;              // reject NaN
			value = value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
			p_.v[tag].store ((float) value, std::memory_order_relaxed);
			if (editor_) jm::notifyEditorParamChanged (editor_, (int) tag);
		}
		return SingleComponentEffect::setParamNormalized (tag, value);
	}

	tresult PLUGIN_API getParamStringByValue (ParamID tag, ParamValue value,
	                                          String128 string) SMTG_OVERRIDE
	{
		if (tag >= (ParamID) jm::kNumParams) return kResultFalse;
		const jm::ParamDef& d = jm::kParamDefs[tag];
		const float plain = jm::plainFromNorm (d, (float) value);
		char buf[64];
		if (d.items && d.numItems > 0)
		{
			int idx = (int) (plain + 0.5f);
			if (idx < 0) idx = 0;
			if (idx >= d.numItems) idx = d.numItems - 1;
			std::snprintf (buf, sizeof (buf), "%s", d.items[idx]);
		}
		else if (d.unit && d.unit[0])
			std::snprintf (buf, sizeof (buf), "%.2f %s", plain, d.unit);
		else
			std::snprintf (buf, sizeof (buf), "%.2f", plain);
		widen (string, buf, 128);
		return kResultOk;
	}

	// ---- editor ----------------------------------------------------------
	IPlugView* PLUGIN_API createView (FIDString name) SMTG_OVERRIDE
	{
		if (name && std::strcmp (name, "editor") != 0) return nullptr;
		return jm::createHanaGuitarEditor (this);
	}

	void setActiveEditor (IPlugView* v) SMTG_OVERRIDE { editor_ = v; }

	// ---- jm::IParamSink ---------------------------------------------------
	void onBeginEdit (int id) SMTG_OVERRIDE { beginEdit ((ParamID) id); }
	void onPerformEdit (int id, double v) SMTG_OVERRIDE
	{
		setParamNormalized ((ParamID) id, v);
		performEdit ((ParamID) id, v);
	}
	void onEndEdit (int id) SMTG_OVERRIDE { endEdit ((ParamID) id); }

	double getNormalized (int id) SMTG_OVERRIDE
	{
		if (id < 0 || id >= (int) jm::kNumParams) return 0.0;
		return p_.v[id].load (std::memory_order_relaxed);
	}

	void copyScope (float* dst, int n) SMTG_OVERRIDE { engine_.copyScope (dst, n); }
	float takePeak () SMTG_OVERRIDE { return engine_.takePeak (); }
	double sampleRate () SMTG_OVERRIDE { return hostRate_ > 1.0 ? hostRate_ : 48000.0; }
	float barSemitones () SMTG_OVERRIDE { return engine_.barSemis (); }
	int techniqueMask () SMTG_OVERRIDE { return engine_.techniqueMask (); }

	// ---- state -----------------------------------------------------------
	tresult PLUGIN_API getState (IBStream* state) SMTG_OVERRIDE
	{
		if (!state) return kInvalidArgument;
		IBStreamer s (state, kLittleEndian);
		if (!s.writeInt32u (1)) return kResultFalse;         // format version
		for (int i = 0; i < (int) jm::kNumParams; ++i)
			if (!s.writeFloat (p_.v[i].load (std::memory_order_relaxed)))
				return kResultFalse;
		return kResultOk;
	}

	tresult PLUGIN_API setState (IBStream* state) SMTG_OVERRIDE
	{
		if (!state) return kInvalidArgument;
		IBStreamer s (state, kLittleEndian);
		uint32 version = 0;
		if (!s.readInt32u (version)) return kResultFalse;
		if (version != 1) return kResultFalse;

		// Read the whole vector before touching anything: a truncated stream
		// must be rejected outright, not leave the plugin half-loaded with a
		// mixture of the old patch and the new one.
		float vals[jm::kNumParams];
		for (int i = 0; i < (int) jm::kNumParams; ++i)
		{
			if (!s.readFloat (vals[i])) return kResultFalse;
			if (!(vals[i] == vals[i]))
				vals[i] = (float) jm::normFromPlain (jm::kParamDefs[i], jm::kParamDefs[i].def);
			vals[i] = vals[i] < 0.f ? 0.f : (vals[i] > 1.f ? 1.f : vals[i]);
		}

		for (int i = 0; i < (int) jm::kNumParams; ++i)
		{
			p_.v[i].store (vals[i], std::memory_order_relaxed);
			Parameter* prm = parameters.getParameter ((ParamID) i);
			if (prm) prm->setNormalized (vals[i]);
			if (editor_) jm::notifyEditorParamChanged (editor_, i);
		}
		applyParams ();
		return kResultOk;
	}

	// ---- audio -----------------------------------------------------------
	tresult PLUGIN_API process (ProcessData& data) SMTG_OVERRIDE
	{
		// 1. automation.  Only the last point of each queue is applied: the
		// engine snapshots parameters per block, so intermediate points inside
		// one block would be discarded anyway.
		if (data.inputParameterChanges)
		{
			const int32 nq = data.inputParameterChanges->getParameterCount ();
			for (int32 i = 0; i < nq; ++i)
			{
				IParamValueQueue* q = data.inputParameterChanges->getParameterData (i);
				if (!q) continue;
				const int32 points = q->getPointCount ();
				if (points <= 0) continue;
				ParamValue v = 0.0;
				int32 offset = 0;
				if (q->getPoint (points - 1, offset, v) == kResultOk)
				{
					const ParamID id = q->getParameterId ();
					if (id < (ParamID) jm::kNumParams && v == v)
						p_.v[id].store ((float) (v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v)),
						                 std::memory_order_relaxed);
				}
			}
		}
		applyParams ();

		// 2. notes.
		if (data.inputEvents)
		{
			struct NoteEv { int pitch; float velocity; int32 offset; };
			NoteEv notes[64];
			int numNotes = 0;

			const int32 ne = data.inputEvents->getEventCount ();
			for (int32 i = 0; i < ne; ++i)
			{
				Event e = {};
				if (data.inputEvents->getEvent (i, e) != kResultOk) continue;
				switch (e.type)
				{
				case Event::kNoteOnEvent:
				{
					const int pitch = e.noteOn.pitch;

					// Technique keys live below the playable range and never
					// sound.  Held keys (palm mute, harmonics, tap, picking
					// position) go to the engine; switch keys change a
					// parameter, so the host sees it and the panel follows.
					if (engine_.keyswitch (pitch, true)) break;
					if (pitch >= jm::kKsRhythm && pitch <= jm::kKsReverse)
					{
						applySwitchKey (pitch);
						break;
					}

					// MIDI 1.0 expresses note-off as note-on with velocity 0.
					if (e.noteOn.velocity <= 0.f)
						engine_.noteOff (pitch);
					else if (numNotes < 64)
						notes[numNotes++] = { pitch, e.noteOn.velocity, e.sampleOffset };
					break;
				}
				case Event::kNoteOffEvent:
					if (engine_.keyswitch (e.noteOff.pitch, false)) break;
					engine_.noteOff (e.noteOff.pitch);
					break;
				case Event::kPolyPressureEvent:
					// Channel pressure is often delivered as poly pressure.
					engine_.setAftertouch ((float) e.polyPressure.pressure);
					break;
				case Event::kLegacyMIDICCOutEvent:
				{
					// VST3 has no dedicated pitch-bend event type.  The bar is
					// played from CC1 / CC11 (or the wheel); the spring model
					// turns the stepped controller into a physical dive.
					const auto& cc = e.midiCCOut;
					if (cc.controlNumber == ControllerNumbers::kPitchBend)
					{
						const int v = (int) (uint8) cc.value | ((int) (uint8) cc.value2 << 7);
						engine_.setPitchBend ((float) (v - 8192) / 8192.0f);
					}
					else if (cc.controlNumber == ControllerNumbers::kAfterTouch)
						engine_.setAftertouch ((float) (uint8) cc.value / 127.0f);
					else if (cc.controlNumber == ControllerNumbers::kCtrlModWheel)
						engine_.setModWheel ((float) (uint8) cc.value / 127.0f);
					else if (cc.controlNumber == 11)         // expression = volume pedal
						engine_.setExpression ((float) (uint8) cc.value / 127.0f);
					else if (cc.controlNumber == 64)         // sustain = let ring
						engine_.setSustainPedal ((uint8) cc.value >= 64);
					break;
				}
				case Event::kDataEvent:
					if (e.data.type == DataEvent::kMidiSysEx)
					{
						// All-notes-off / all-sound-off, so a stuck note cannot
						// outlive the host's panic.
						const uint8* d = e.data.bytes;
						const uint32 len = e.data.size;
						if (len >= 2 && (d[0] & 0xF0) == 0xB0 && (d[1] == 120 || d[1] == 123))
							engine_.allNotesOff ();
					}
					break;
				default:
					break;
				}
			}

			// A chord arrives as several note-ons at the same sample offset.
			// Six strings hit on the same sample is not a strum, it is a
			// machine: stagger them across the strings and alternate the
			// direction, so a held keyboard chord behaves like a hand.
			std::sort (notes, notes + numNotes,
			           [] (const NoteEv& a, const NoteEv& b)
			           {
				           if (a.offset != b.offset) return a.offset < b.offset;
				           return a.pitch < b.pitch;
			           });

			const int stride = std::max (1, (int) (hostRate_ * 0.009));
			for (int i = 0; i < numNotes; )
			{
				int j = i;
				while (j < numNotes && notes[j].offset == notes[i].offset) ++j;
				const int count = j - i;

				if (count > 1)
				{
					strumUp_ = !strumUp_;
					for (int k = 0; k < count; ++k)
					{
						const int rank = strumUp_ ? k : (count - 1 - k);
						engine_.noteOnDelayed (notes[i + k].pitch, notes[i + k].velocity,
						                       notes[i].offset + rank * stride);
					}
				}
				else
				{
					engine_.noteOnDelayed (notes[i].pitch, notes[i].velocity, notes[i].offset);
				}
				i = j;
			}
		}

		// 3. structural sanity.
		if (data.numSamples <= 0) return kResultOk;
		if (!prepared_ || outL_.empty ()) return kResultOk;
		if (data.numOutputs <= 0 || data.outputs[0].numChannels <= 0) return kResultOk;

		AudioBusBuffers& out = data.outputs[0];
		const int32 nCh = std::min<int32> (out.numChannels, 2);
		out.silenceFlags = 0;

		const bool is32 = (data.symbolicSampleSize == kSample32);
		const int32 maxChunk = (int32) outL_.size ();
		int32 done = 0;

		while (done < data.numSamples)
		{
			const int32 n = std::min (maxChunk, data.numSamples - done);
			if (n <= 0) break;

			engine_.process (outL_.data (), outR_.data (), n);

			if (is32)
			{
				if (out.channelBuffers32[0])
					for (int32 i = 0; i < n; ++i)
						out.channelBuffers32[0][done + i] = outL_[i];
				if (nCh > 1 && out.channelBuffers32[1])
					for (int32 i = 0; i < n; ++i)
						out.channelBuffers32[1][done + i] = outR_[i];
			}
			else
			{
				if (out.channelBuffers64[0])
					for (int32 i = 0; i < n; ++i)
						out.channelBuffers64[0][done + i] = (double) outL_[i];
				if (nCh > 1 && out.channelBuffers64[1])
					for (int32 i = 0; i < n; ++i)
						out.channelBuffers64[1][done + i] = (double) outR_[i];
			}
			done += n;
		}
		return kResultOk;
	}

	OBJ_METHODS (HanaGuitarVST3, SingleComponentEffect)

private:
	void applyParams ()
	{
		for (int i = 0; i < (int) jm::kNumParams; ++i)
		{
			const float norm = p_.v[i].load (std::memory_order_relaxed);
			engine_.setParam (i, jm::plainFromNorm (jm::kParamDefs[i], norm));
		}
	}

	// A switch keyswitch: the pickup and the two circuits are parameters, not
	// hidden state, so the host's automation and the panel both stay in sync.
	void applySwitchKey (int pitch)
	{
		switch (pitch)
		{
		case jm::kKsRhythm:     setParamFromKey (jm::kCircuit, 1.f); break;
		case jm::kKsLead:       setParamFromKey (jm::kCircuit, 0.f); break;
		case jm::kKsPickupP90:  setParamFromKey (jm::kPickupSel, 0.f); break;
		case jm::kKsPickupBoth: setParamFromKey (jm::kPickupSel, 1.f); break;
		case jm::kKsPickupHB:   setParamFromKey (jm::kPickupSel, 2.f); break;
		case jm::kKsReverse:    setParamFromKey (jm::kRevMode, 4.f); break;
		default: break;
		}
	}

	void setParamFromKey (int id, float plain)
	{
		const float norm = jm::normFromPlain (jm::kParamDefs[id], plain);
		p_.v[id].store (norm, std::memory_order_relaxed);
		// The host reads the Parameter object, not our atomic: a keyswitch that
		// only moved the atomic would show the old value in the DAW.
		if (Parameter* prm = parameters.getParameter ((ParamID) id))
			prm->setNormalized (norm);
		if (editor_) jm::notifyEditorParamChanged (editor_, id);
	}

	jm::Engine engine_;

	// The plugin owns the Parameter objects so the editor can ask a single
	// object for both the normalised value and the plain-unit string.
	JmParam* params_[jm::kNumParams] {};

	jm::ParamState p_;
	IPlugView* editor_ = nullptr;

	std::vector<float> outL_, outR_;
	double hostRate_ = 48000.0;
	bool prepared_ = false;
	bool initialized_ = false;
	bool strumUp_ = false;
};

const FUID HanaGuitarVST3::uid (0x0FF5E701, 0x4A2C7B13, 0xB8D94062, 0x5C1E3F88);

} // anonymous namespace

// ---------------------------------------------------------------------------
BEGIN_FACTORY_DEF ("HanaGuitar for indie", "https://example.invalid/hanaguitar",
                   "nobody@example.invalid")

	DEF_CLASS2 (INLINE_UID_FROM_FUID (HanaGuitarVST3::uid),
	            PClassInfo::kManyInstances,
	            kVstAudioEffectClass,
	            "HanaGuitar for indie",
	            Vst::kDistributable,
	            Vst::PlugType::kInstrumentSynth,
	            "1.0.0",
	            kVstVersionString,
	            HanaGuitarVST3::createInstance)

END_FACTORY
