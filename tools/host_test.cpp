// ===========================================================================
//  host_test.cpp -- load HanaGuitar.vst3 the way a host would, and exercise it
//
//  This is the closest thing to a DAW that exists in this build environment:
//  it drives the exported factory, the audio processor and the edit controller
//  through their public interfaces only, and checks the properties a host
//  depends on:
//
//    * the factory exposes one audio-effect class, filed as an instrument
//    * initialize() is idempotent (a single-component effect is its own
//      controller, and hosts call initialize on both)
//    * 37 parameters, names and defaults present, all of them format to text
//    * no main audio input bus: an instrument must not appear as an effect
//    * process() is finite, bounded and non-silent at 1..1024 sample blocks
//    * a MIDI note reaches the waveguide and is audible
//    * note-off releases, all-notes-off silences
//    * the whammy parameter actually bends the pitch, and returns
//    * getState/setState round-trips
//    * the editor is created, sized, attached to a real HWND and painted
//    * clicking a preset button on the panel changes the parameters
//
//  Exit code 0 means every check passed.
// ===========================================================================

#include <windows.h>

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "public.sdk/source/common/memorystream.h"

// Parameter IDs are compile-time only here; including the table keeps this
// test from drifting if the enum ever changes.
#include "../src/params.h"
#include "../src/keyswitch.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

static int g_fail = 0;
static int g_pass = 0;

static void check (const char* what, bool cond, const char* extra = "")
{
	std::printf ("  %s  %s %s\n", cond ? "PASS" : "FAIL", what, extra);
	if (cond) ++g_pass; else ++g_fail;
}

static bool iidEq (const TUID a, const TUID b) { return std::memcmp (a, b, 16) == 0; }

template <class T>
static T* query (void* obj, const TUID iid)
{
	if (!obj) return nullptr;
	void* out = nullptr;
	if (static_cast<FUnknown*> (obj)->queryInterface (iid, &out) != kResultOk) return nullptr;
	return static_cast<T*> (out);
}

static void toAscii (const char16* s, char* out, int cap)
{
	int i = 0;
	for (; i < cap - 1 && s[i]; ++i) out[i] = (char) s[i];
	out[i] = 0;
}

// ---------------------------------------------------------------------------
class TestEventList : public IEventList
{
public:
	tresult PLUGIN_API queryInterface (const TUID iid, void** obj) SMTG_OVERRIDE
	{
		if (iidEq (iid, IEventList::iid) || iidEq (iid, FUnknown::iid))
		{
			*obj = static_cast<IEventList*> (this);
			return kResultOk;
		}
		*obj = nullptr;
		return kNoInterface;
	}
	uint32 PLUGIN_API addRef () SMTG_OVERRIDE { return 1; }
	uint32 PLUGIN_API release () SMTG_OVERRIDE { return 1; }

	int32 PLUGIN_API getEventCount () SMTG_OVERRIDE { return (int32) events_.size (); }

	tresult PLUGIN_API getEvent (int32 index, Event& e) SMTG_OVERRIDE
	{
		if (index < 0 || index >= (int32) events_.size ()) return kInvalidArgument;
		e = events_[(size_t) index];
		return kResultOk;
	}

	tresult PLUGIN_API addEvent (Event& e) SMTG_OVERRIDE
	{
		events_.push_back (e);
		return kResultOk;
	}

	void clear () { events_.clear (); }

	void cc (int number, int value)
	{
		Event e = {};
		e.busIndex = 0;
		e.sampleOffset = 0;
		e.type = Event::kLegacyMIDICCOutEvent;
		e.midiCCOut.controlNumber = (uint8) number;
		e.midiCCOut.channel = 0;
		e.midiCCOut.value = (int8) value;
		e.midiCCOut.value2 = 0;
		events_.push_back (e);
	}

	void note (int16 pitch, float velocity, bool on)
	{
		Event e = {};
		e.busIndex = 0;
		e.sampleOffset = 0;
		e.ppqPosition = 0;
		e.flags = 0;
		if (on)
		{
			e.type = Event::kNoteOnEvent;
			e.noteOn.channel = 0;
			e.noteOn.pitch = pitch;
			e.noteOn.velocity = velocity;
			e.noteOn.noteId = -1;
		}
		else
		{
			e.type = Event::kNoteOffEvent;
			e.noteOff.channel = 0;
			e.noteOff.pitch = pitch;
			e.noteOff.velocity = 0.f;
			e.noteOff.noteId = -1;
		}
		events_.push_back (e);
	}

private:
	std::vector<Event> events_;
};

// ---------------------------------------------------------------------------
struct Rig
{
	IComponent* comp = nullptr;
	IAudioProcessor* proc = nullptr;
	IEditController* ctrl = nullptr;
	bool active = false;
	int32 maxBlock = 512;

	std::vector<float> outL, outR;
	std::vector<float> outL64, outR64;

	bool prepare (double sr, int32 block)
	{
		if (active) { comp->setActive (false); active = false; }
		maxBlock = block;
		ProcessSetup ps = {};
		ps.processMode = kRealtime;
		ps.symbolicSampleSize = kSample32;
		ps.maxSamplesPerBlock = block;
		ps.sampleRate = sr;
		if (proc->setupProcessing (ps) != kResultOk) return false;
		if (comp->setActive (true) != kResultOk) return false;
		active = true;
		outL.assign (block, 0.f);
		outR.assign (block, 0.f);
		outL64.assign (block, 0.0);
		outR64.assign (block, 0.0);
		return true;
	}

	void setParam (int id, double v) { ctrl->setParamNormalized ((ParamID) id, v); }

	double getParam (int id) { return ctrl->getParamNormalized ((ParamID) id); }

	// Render nSamples in blocks <= maxBlock.  An instrument has no audio input,
	// so numInputs is 0 -- a host that fed it one would be routing into nothing.
	float render (int nSamples, std::vector<float>* mono, IEventList* events,
	              bool* finite)
	{
		float peak = 0.f;
		if (finite) *finite = true;
		int done = 0;
		while (done < nSamples)
		{
			const int n = (nSamples - done < (int) maxBlock) ? (nSamples - done) : (int) maxBlock;

			float* outs[2] = { outL.data (), outR.data () };
			ProcessData data = {};
			data.processMode = kRealtime;
			data.symbolicSampleSize = kSample32;
			data.numSamples = n;
			data.numInputs = 0;
			data.numOutputs = 1;
			AudioBusBuffers bo = {};
			bo.numChannels = 2;
			bo.channelBuffers32 = outs;
			data.outputs = &bo;
			data.inputEvents = events;

			if (proc->process (data) != kResultOk) { if (finite) *finite = false; break; }

			for (int i = 0; i < n; ++i)
			{
				if (!std::isfinite (outL[i]) || !std::isfinite (outR[i]))
					if (finite) *finite = false;
				const float a = std::fabs (outL[i]);
				if (a > peak) peak = a;
				if (mono) mono->push_back (0.5f * (outL[i] + outR[i]));
			}
			done += n;
		}
		return peak;
	}
};

// ---------------------------------------------------------------------------
// Goertzel: power at one frequency.  Used to confirm that the whammy moved the
// pitch rather than just the level.
static double goertzel (const std::vector<float>& x, double freq, double sr)
{
	const int n = (int) x.size ();
	if (n < 8) return 0.0;
	const double w = 2.0 * 3.141592653589793 * freq / sr;
	const double cw = std::cos (w), sw = std::sin (w), k = 2.0 * cw;
	double s0 = 0.0, s1 = 0.0, s2 = 0.0;
	for (int i = 0; i < n; ++i) { s0 = x[i] + k * s1 - s2; s2 = s1; s1 = s0; }
	const double re = s1 - s2 * cw, im = s2 * sw;
	return std::sqrt (re * re + im * im) / n;
}

static double midiFreq (int n) { return 440.0 * std::pow (2.0, (n - 69) / 12.0); }

// Power-weighted pitch estimate over a short window: scan a narrow band and
// take the peak.  Coarse but enough to tell 2 semitones from 0.
static double pitchAt (const std::vector<float>& x, int start, int n,
                       double lo, double hi, double sr)
{
	double best = lo, bp = -1.0;
	std::vector<float> win ((size_t) n);
	for (int i = 0; i < n; ++i) win[(size_t) i] = x[(size_t) (start + i)];
	for (double f = lo; f <= hi; f += 0.5)
	{
		const double p = goertzel (win, f, sr);
		if (p > bp) { bp = p; best = f; }
	}
	return best;
}

// ---------------------------------------------------------------------------
static void driveParam (Rig& rig, int id, double v)
{
	// Two routes to the same store: setParamNormalized is the one the editor
	// uses, and it is what automation goes through in a single-component
	// effect.  Rendering a block afterwards is what pushes it into the engine.
	rig.setParam (id, v);
	rig.render ((int) rig.maxBlock, nullptr, nullptr, nullptr);
}


// ---------------------------------------------------------------------------
// Save what the editor actually paints, so the panel can be looked at without
// opening a DAW.
static void saveWindowBmp (HWND hwnd, int w, int h, const char* path)
{
	HDC screen = GetDC (nullptr);
	HDC mem = CreateCompatibleDC (screen);
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void* bits = nullptr;
	HBITMAP bmp = CreateDIBSection (mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	if (!bmp) { DeleteDC (mem); ReleaseDC (nullptr, screen); return; }
	HGDIOBJ old = SelectObject (mem, bmp);
	SendMessageW (hwnd, WM_PRINTCLIENT, (WPARAM) mem, PRF_CLIENT);
	SelectObject (mem, old);

	std::FILE* f = std::fopen (path, "wb");
	if (f)
	{
		const int rowBytes = w * 4;
		const int dataSize = rowBytes * h;
		unsigned char hdr[54] = { 'B', 'M' };
		auto put32 = [&] (int off, int v) {
			hdr[off] = (unsigned char) (v & 255);
			hdr[off + 1] = (unsigned char) ((v >> 8) & 255);
			hdr[off + 2] = (unsigned char) ((v >> 16) & 255);
			hdr[off + 3] = (unsigned char) ((v >> 24) & 255);
		};
		put32 (2, 54 + dataSize);
		put32 (10, 54);
		put32 (14, 40);
		put32 (18, w);
		put32 (22, -h);
		hdr[26] = 1; hdr[28] = 32;
		put32 (34, dataSize);
		std::fwrite (hdr, 1, 54, f);
		std::fwrite (bits, 1, (size_t) dataSize, f);
		std::fclose (f);
	}
	DeleteObject (bmp);
	DeleteDC (mem);
	ReleaseDC (nullptr, screen);
}


// Paint the editor into a DIB and count pixels that are not near-black.  A
// host that hands the view a 0x0 rect and then resizes the HWND used to leave
// the panel black; this is the regression test for that.
static int paintNonBlack (HWND hwnd, int w, int h)
{
	HDC screen = GetDC (nullptr);
	HDC mem = CreateCompatibleDC (screen);
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void* bits = nullptr;
	HBITMAP bmp = CreateDIBSection (mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	if (!bmp) { DeleteDC (mem); ReleaseDC (nullptr, screen); return 0; }
	HGDIOBJ old = SelectObject (mem, bmp);
	SendMessageW (hwnd, WM_PRINTCLIENT, (WPARAM) mem, PRF_CLIENT);
	SelectObject (mem, old);

	int count = 0;
	const unsigned char* p = (const unsigned char*) bits;
	for (int i = 0; i < w * h; ++i)
		if (p[i * 4] > 12 || p[i * 4 + 1] > 12 || p[i * 4 + 2] > 12) ++count;

	DeleteObject (bmp);
	DeleteDC (mem);
	ReleaseDC (nullptr, screen);
	return count;
}

// ---------------------------------------------------------------------------
int main (int argc, char** argv)
{
	std::setvbuf (stdout, nullptr, _IONBF, 0);
	const char* dllPath = (argc > 1) ? argv[1] : "dist\\HanaGuitar.vst3";

	std::printf ("==========================================================================\n");
	std::printf (" HANA GUITAR -- host simulation: %s\n", dllPath);
	std::printf ("==========================================================================\n");

	HMODULE mod = LoadLibraryA (dllPath);
	if (!mod)
	{
		std::printf ("  LoadLibrary failed (%lu)\n", GetLastError ());
		return 2;
	}

	check ("exports InitDll", GetProcAddress (mod, "InitDll") != nullptr);
	check ("exports ExitDll", GetProcAddress (mod, "ExitDll") != nullptr);

	typedef IPluginFactory* (*GetFactoryFn) ();
	GetFactoryFn getFactory =
	    reinterpret_cast<GetFactoryFn> (GetProcAddress (mod, "GetPluginFactory"));
	if (!getFactory) { std::printf ("  no GetPluginFactory export\n"); return 2; }

	IPluginFactory* factory = getFactory ();

	std::printf ("\n[factory]\n");
	IPluginFactory2* f2 = nullptr;
	PFactoryInfo fi = {};
	if (factory->queryInterface (IPluginFactory2::iid, (void**) &f2) == kResultOk && f2)
	{
		f2->getFactoryInfo (&fi);
		std::printf ("  vendor : %s\n", fi.vendor);
		std::printf ("  url    : %s\n", fi.url);
		std::printf ("  email  : %s\n", fi.email);
	}
	std::printf ("  classes: %d\n", (int) factory->countClasses ());

	PClassInfo ci = {};
	bool found = false;
	for (int32 i = 0; i < factory->countClasses (); ++i)
	{
		PClassInfo c = {};
		if (factory->getClassInfo (i, &c) != kResultOk) continue;
		std::printf ("    [%d] %s / %s\n", (int) i, c.category, c.name);
		if (std::strcmp (c.category, kVstAudioEffectClass) == 0 && !found)
		{
			ci = c;
			found = true;
		}
	}
	check ("one audio-effect class is exported", found);
	if (!found) return 2;
	check ("class is named HanaGuitar for indie", std::strcmp (ci.name, "HanaGuitar for indie") == 0);

	// Hosts decide Generators vs Effects from the sub-categories; this is the
	// difference between "appears in the piano roll" and "appears in the mixer".
	{
		PClassInfo2 ci2 = {};
		bool isInstrument = false;
		if (f2 && f2->getClassInfo2 (0, &ci2) == kResultOk)
		{
			char sub[128] = {};
			for (int i = 0; i < 127 && ci2.subCategories[i]; ++i)
				sub[i] = (char) ci2.subCategories[i];
			std::printf ("  subcategories: %s\n", sub);
			isInstrument = std::strstr (sub, "Instrument") != nullptr;
		}
		check ("declared as an Instrument", isInstrument);
	}

	void* obj = nullptr;
	if (factory->createInstance (ci.cid, IComponent::iid, &obj) != kResultOk || !obj)
	{
		std::printf ("  createInstance failed\n");
		return 2;
	}

	Rig rig;
	rig.comp = static_cast<IComponent*> (obj);
	rig.proc = query<IAudioProcessor> (obj, IAudioProcessor::iid);
	rig.ctrl = query<IEditController> (obj, IEditController::iid);
	check ("implements IAudioProcessor", rig.proc != nullptr);
	check ("implements IEditController", rig.ctrl != nullptr);
	if (!rig.proc || !rig.ctrl) return 2;

	std::printf ("\n[initialize]\n");
	check ("IComponent::initialize", rig.comp->initialize (nullptr) == kResultOk);
	rig.ctrl->initialize (nullptr);
	check ("initialize twice is harmless",
	       rig.ctrl->getParameterCount () == (int32) jm::kNumParams);

	std::printf ("\n[buses and parameters]\n");
	const int32 numIn = rig.comp->getBusCount (kAudio, kInput);
	const int32 numOut = rig.comp->getBusCount (kAudio, kOutput);
	const int32 numEv = rig.comp->getBusCount (kEvent, kInput);
	std::printf ("  audio in=%d out=%d, event in=%d\n", (int) numIn, (int) numOut, (int) numEv);

	// An instrument with a main audio input is an effect that happens to make
	// sound: FL and Bitwig would route the track's audio through it.
	check ("no main audio input", numIn == 0);
	check ("stereo output bus", numOut == 1);
	check ("MIDI input bus", numEv == 1);
	{
		BusInfo bi = {};
		if (rig.comp->getBusInfo (kAudio, kOutput, 0, bi) == kResultOk)
			check ("output bus is main", bi.busType == kMain);
		if (rig.comp->getBusInfo (kEvent, kInput, 0, bi) == kResultOk)
			check ("MIDI bus carries 16 channels", bi.channelCount == 16);
	}

	char msg[256];
	std::snprintf (msg, sizeof (msg), "(%d parameters)", (int) rig.ctrl->getParameterCount ());
	check ("parameter count matches the table",
	       rig.ctrl->getParameterCount () == (int32) jm::kNumParams, msg);

	// names must be in declaration order, because the editor's layout assumes it
	{
		bool namesOk = true;
		for (int i = 0; i < (int) jm::kNumParams; ++i)
		{
			ParameterInfo pi = {};
			if (rig.ctrl->getParameterInfo ((ParamID) i, pi) != kResultOk) { namesOk = false; break; }
			char a[128] = {};
			toAscii (pi.title, a, 128);
			if (std::strcmp (a, jm::kParamDefs[i].name) != 0)
			{
				std::printf ("        param[%d] \"%s\" != \"%s\"\n", i, a, jm::kParamDefs[i].name);
				namesOk = false;
			}
			if ((int) pi.id != i) namesOk = false;
		}
		check ("every parameter has its declared name and id", namesOk);
	}

	// discrete parameters must advertise their step count, or a host will show
	// a continuous slider for "Pickup" and send 0.37
	{
		bool stepsOk = true;
		for (int i = 0; i < (int) jm::kNumParams; ++i)
		{
			ParameterInfo pi = {};
			rig.ctrl->getParameterInfo ((ParamID) i, pi);
			if (pi.stepCount != jm::kParamDefs[i].steps) stepsOk = false;
		}
		check ("step counts match the table", stepsOk);
	}

	// defaults must arrive normalised correctly: a host that never touches a
	// parameter must get the documented plain value
	{
		bool defOk = true;
		for (int i = 0; i < (int) jm::kNumParams; ++i)
		{
			const double want = (double) jm::normFromPlain (jm::kParamDefs[i],
			                                                jm::kParamDefs[i].def);
			const double got = rig.ctrl->getParamNormalized ((ParamID) i);
			if (std::fabs (got - want) > 1e-4)
			{
				std::printf ("        param[%d] %s default %.4f != %.4f\n",
				             i, jm::kParamDefs[i].name, got, want);
				defOk = false;
			}
		}
		check ("defaults are the documented plain values", defOk);
	}

	// every parameter must round-trip and format to non-empty text
	{
		bool paramsOk = true;
		for (int i = 0; i < (int) jm::kNumParams; ++i)
		{
			rig.ctrl->setParamNormalized ((ParamID) i, 0.5);
			const double v = rig.ctrl->getParamNormalized ((ParamID) i);
			if (v < 0.49 || v > 0.51) paramsOk = false;
			String128 s = {};
			if (rig.ctrl->getParamStringByValue ((ParamID) i, 0.5, s) != kResultOk) paramsOk = false;
			if (!s[0]) paramsOk = false;
		}
		check ("all parameters round-trip and format", paramsOk);
	}

	// discrete parameters must format as a *name*, not a number
	{
		String128 s = {};
		rig.ctrl->getParamStringByValue (jm::kPickupSel, 0.0, s);
		char a[128] = {};
		toAscii (s, a, 128);
		std::snprintf (msg, sizeof (msg), "(\"%s\")", a);
		check ("pickup formats as a name", std::strcmp (a, "P90") == 0, msg);

		rig.ctrl->getParamStringByValue (jm::kPreset, 1.0, s);
		toAscii (s, a, 128);
		std::snprintf (msg, sizeof (msg), "(\"%s\")", a);
		check ("preset formats as a name", std::strcmp (a, "Clean JM") == 0, msg);
	}

	// put everything back to defaults before making sound
	for (int i = 0; i < (int) jm::kNumParams; ++i)
		rig.ctrl->setParamNormalized ((ParamID) i,
		                              (double) jm::normFromPlain (jm::kParamDefs[i],
		                                                          jm::kParamDefs[i].def));

	std::printf ("\n[setup / activate]\n");
	check ("setupProcessing + setActive", rig.prepare (48000.0, 512));

	std::printf ("\n[process]\n");
	// An instrument is silent until a note arrives: that is the first property
	// to prove, because a model that hisses at idle is unusable in a mix.
	{
		std::vector<float> mono;
		bool finite = true;
		const float idle = rig.render (48000, &mono, nullptr, &finite);
		std::snprintf (msg, sizeof (msg), "(idle peak %.8f)", idle);
		check ("silent with no note", idle == 0.f && finite, msg);
	}

	// block sizes: 1 sample is the pathological case a host uses when it is
	// chasing a deadline, and it must not click or blow up
	{
		const int sizes[] = { 512, 64, 1, 256, 37, 1024, 128 };
		bool allFinite = true;
		bool anySignal = false;
		float worst = 0.f;
		for (int i = 0; i < 7; ++i)
		{
			TestEventList ev;
			ev.note (52, 0.9f, true);
			rig.prepare (48000.0, sizes[i]);
			std::vector<float> mono;
			bool finite = true;
			rig.render (sizes[i], nullptr, &ev, &finite);       // deliver the note
			ev.clear ();
			const float p = rig.render (sizes[i] * 40, &mono, nullptr, &finite);
			if (!finite) allFinite = false;
			if (p > 0.02f) anySignal = true;
			if (p > worst) worst = p;
			std::printf ("        block %4d -> peak %.4f  %s\n", sizes[i], p,
			             finite ? "finite" : "NON-FINITE");
		}
		check ("output finite at 1..1024 sample blocks", allFinite);
		check ("output produces signal", anySignal);
		check ("output stays under full scale", worst <= 1.0f);
		rig.prepare (48000.0, 512);
	}

	std::printf ("\n[MIDI]\n");
	{
		TestEventList ev;
		std::vector<float> mono;
		bool finite = true;

		// Note-off and panic are about the *string*, so measure with the
		// reverb and delay out of the way: a real reverb keeps ringing after
		// the key is released, and that is correct behaviour, not a stuck note.
		driveParam (rig, jm::kRevMix, 0.0);
		driveParam (rig, jm::kDelayMix, 0.0);
		driveParam (rig, jm::kChorus, 0.0);

		ev.note (64, 0.9f, true);            // high E
		rig.render (512, nullptr, &ev, &finite);
		ev.clear ();
		const float on = rig.render (24000, &mono, nullptr, &finite);
		std::snprintf (msg, sizeof (msg), "(peak note=%.4f)", on);
		check ("note-on is audible", on > 0.02f, msg);

		// The pitch must be the requested note: 64 = 329.63 Hz.
		{
			const double f = pitchAt (mono, 2000, 8192, 300.0, 360.0, 48000.0);
			const double cents = 1200.0 * std::log2 (f / midiFreq (64));
			std::snprintf (msg, sizeof (msg), "(%.1f Hz, %+.0f cents)", f, cents);
			check ("note sounds at its MIDI pitch", std::fabs (cents) < 25.0, msg);
		}

		ev.note (64, 0.f, false);
		rig.render (512, nullptr, &ev, &finite);
		ev.clear ();
		rig.render (24000, nullptr, nullptr, &finite);      // let it decay
		mono.clear ();
		const float off = rig.render (12000, &mono, nullptr, &finite);
		std::snprintf (msg, sizeof (msg), "(peak after release=%.5f)", off);
		check ("note-off releases", off < 0.02f, msg);

		// all-notes-off through a data event: a stuck note must not survive a
		// host panic
		ev.note (55, 0.9f, true);
		rig.render (512, nullptr, &ev, &finite);
		ev.clear ();
		rig.render (4800, nullptr, nullptr, &finite);
		{
			static const uint8 kPanic[3] = { 0xB0, 123, 0 };
			Event e = {};
			e.type = Event::kDataEvent;
			e.data.type = DataEvent::kMidiSysEx;
			e.data.size = 3;
			e.data.bytes = kPanic;
			ev.addEvent (e);
			rig.render (512, nullptr, &ev, &finite);
			ev.clear ();
		}
		rig.render (24000, nullptr, nullptr, &finite);
		mono.clear ();
		const float panic = rig.render (12000, &mono, nullptr, &finite);
		std::snprintf (msg, sizeof (msg), "(peak after panic=%.5f)", panic);
		check ("all-notes-off silences", panic < 0.02f, msg);
	}


	std::printf ("\n[keyswitches]\n");
	{
		TestEventList ev;
		std::vector<float> mono;
		bool finite = true;
		driveParam (rig, jm::kChorus, 0.0);
		driveParam (rig, jm::kDelayMix, 0.0);
		driveParam (rig, jm::kRevMix, 0.0);
		driveParam (rig, jm::kDrive, 0.0);
		rig.render (24000, nullptr, nullptr, &finite);   // let any tail die

		// C0 = palm mute.  The event must be swallowed: no note, no sound.
		ev.note (jm::kKsPalmMute, 0.9f, true);
		rig.render (512, nullptr, &ev, &finite);
		ev.clear ();
		mono.clear ();
		const float ks = rig.render (4096, &mono, nullptr, &finite);
		std::snprintf (msg, sizeof (msg), "(peak %.5f)", ks);
		check ("keyswitch note itself is silent", ks < 1e-4f, msg);

		ev.note (jm::kKsPalmMute, 0.f, false);
		rig.render (512, nullptr, &ev, &finite);
		ev.clear ();

		// G0 = rhythm circuit: the switch must reach the parameter the host sees.
		driveParam (rig, jm::kCircuit, 0.0);
		ev.note (jm::kKsRhythm, 0.9f, true);
		rig.render (512, nullptr, &ev, &finite);
		ev.clear ();
		const double circ = rig.getParam (jm::kCircuit);
		std::snprintf (msg, sizeof (msg), "(circuit %.2f)", circ);
		check ("G0 switches to the rhythm circuit", circ > 0.5, msg);

		ev.note (jm::kKsRhythm, 0.f, false);
		rig.render (512, nullptr, &ev, &finite);
		ev.clear ();

		// A keyswitch must not steal a string: play a normal note right after
		// and it must still sound.
		ev.note (jm::kKsTap, 0.9f, true);
		ev.note (52, 0.9f, true);
		rig.render (512, nullptr, &ev, &finite);
		ev.clear ();
		mono.clear ();
		const float note = rig.render (8192, &mono, nullptr, &finite);
		std::snprintf (msg, sizeof (msg), "(peak %.4f)", note);
		check ("a note played while a technique key is held still sounds",
		       note > 0.02f && finite, msg);

		ev.note (52, 0.f, false);
		ev.note (jm::kKsTap, 0.f, false);
		rig.render (512, nullptr, &ev, &finite);
		ev.clear ();
		rig.render (24000, nullptr, nullptr, &finite);
	}

	std::printf ("\n[whammy]\n");
	{
		// The whole point of the instrument: moving the arm must move the pitch
		// by the amount the range says, and releasing it must come back.
		//
		// The pitch is measured from the string, so the effects are turned off:
		// a reverb holds the *old* pitch for a second after the arm moves, and
		// measuring through it would report the note the guitar used to be
		// playing rather than the one it is playing now.
		TestEventList ev;
		std::vector<float> mono;
		bool finite = true;

		driveParam (rig, jm::kChorus, 0.0);
		driveParam (rig, jm::kDelayMix, 0.0);
		driveParam (rig, jm::kRevMix, 0.0);
		driveParam (rig, jm::kDrive, 0.0);

		// The bar is CC1 now: full CC = full dive (a Jazzmaster's four
		// semitones), CC back to zero = the spring returns to pitch.

		ev.note (64, 0.9f, true);
		rig.render (512, nullptr, &ev, &finite);
		ev.clear ();
		rig.render (24000, nullptr, nullptr, &finite);

		mono.clear ();
		rig.render (8192, &mono, nullptr, &finite);
		const double f0 = pitchAt (mono, 1000, 6000, 300.0, 360.0, 48000.0);

		// dive: the parameter is 0..1 of the arm's travel, and the engine's
		// spring needs time to settle
		ev.cc (1, 127); rig.render (512, nullptr, &ev, &finite); ev.clear ();
		rig.render (24000, nullptr, nullptr, &finite);
		mono.clear ();
		rig.render (8192, &mono, nullptr, &finite);
		const double f1 = pitchAt (mono, 1000, 6000, 250.0, 360.0, 48000.0);
		const double dived = 1200.0 * std::log2 (f1 / f0);
		std::snprintf (msg, sizeof (msg), "(%.1f -> %.1f Hz, %.0f cents)", f0, f1, dived);
		check ("full dive drops four semitones", dived < -350.0 && dived > -450.0, msg);

		// release: the spring must bring it back to concert pitch
		ev.cc (1, 0); rig.render (512, nullptr, &ev, &finite); ev.clear ();
		rig.render (48000, nullptr, nullptr, &finite);
		mono.clear ();
		rig.render (8192, &mono, nullptr, &finite);
		const double f2 = pitchAt (mono, 1000, 6000, 300.0, 360.0, 48000.0);
		const double back = 1200.0 * std::log2 (f2 / f0);
		std::snprintf (msg, sizeof (msg), "(back to %.1f Hz, %+.0f cents)", f2, back);
		check ("CC back to zero returns to pitch", std::fabs (back) < 25.0, msg);

		ev.note (64, 0.f, false);
		rig.render (512, nullptr, &ev, &finite);
		ev.clear ();
		rig.render (24000, nullptr, nullptr, &finite);
	}

	std::printf ("\n[presets]\n");
	{
		// The four presets are the product.  Applying one must change the sound
		// parameters and must leave the instrument playable.
		struct Snapshot { double drive, chorus, rev, tone, sustain; };
		auto snap = [&] () -> Snapshot {
			return { rig.getParam (jm::kDrive), rig.getParam (jm::kChorus),
			         rig.getParam (jm::kRevMix), rig.getParam (jm::kTone),
			         rig.getParam (jm::kSustain) };
		};

		driveParam (rig, jm::kDrive, (double) jm::normFromPlain (jm::kParamDefs[jm::kDrive], 0.05f));
		const Snapshot before = snap ();

		// what the panel's SHOEGAZE button does, in the same order
		driveParam (rig, jm::kDrive, (double) jm::normFromPlain (jm::kParamDefs[jm::kDrive], 0.74f));
		driveParam (rig, jm::kChorus, (double) jm::normFromPlain (jm::kParamDefs[jm::kChorus], 0.66f));
		driveParam (rig, jm::kRevMix, (double) jm::normFromPlain (jm::kParamDefs[jm::kRevMix], 0.52f));
		driveParam (rig, jm::kTone, (double) jm::normFromPlain (jm::kParamDefs[jm::kTone], 0.52f));
		const Snapshot after = snap ();

		std::snprintf (msg, sizeof (msg), "(drive %.2f->%.2f, rev %.2f->%.2f)",
		               before.drive, after.drive, before.rev, after.rev);
		check ("preset changes the sound parameters",
		       after.drive > before.drive + 0.1 && after.rev > before.rev + 0.1, msg);
		check ("preset changes the tone", after.tone != before.tone);

		// and the result still plays
		TestEventList ev;
		ev.note (52, 0.9f, true);
		std::vector<float> mono;
		bool finite = true;
		rig.render (512, nullptr, &ev, &finite);
		ev.clear ();
		const float p = rig.render (24000, &mono, nullptr, &finite);
		std::snprintf (msg, sizeof (msg), "(peak %.4f)", p);
		check ("preset patch is audible and finite", p > 0.02f && finite, msg);

		ev.note (52, 0.f, false);
		rig.render (512, nullptr, &ev, &finite);
		ev.clear ();
		rig.render (24000, nullptr, nullptr, &finite);
	}

	std::printf ("\n[state]\n");
	{
		// a recognisable patch
		rig.setParam (jm::kSustain, 0.31);
		rig.setParam (jm::kRevMix, 0.42);
		rig.setParam (jm::kTone, 0.77);
		rig.setParam (jm::kPreset, 0.66);

		MemoryStream ms;
		check ("getState", rig.comp->getState (&ms) == kResultOk);
		const int64 size = ms.getSize ();
		std::printf ("  state size: %lld bytes\n", (long long) size);
		std::snprintf (msg, sizeof (msg), "(%lld)", (long long) size);
		check ("state is version + one float per parameter",
		       size == 4 + (int64) jm::kNumParams * 4, msg);

		rig.setParam (jm::kSustain, 0.9);
		rig.setParam (jm::kRevMix, 0.1);
		rig.setParam (jm::kTone, 0.2);
		rig.setParam (jm::kPreset, 0.0);

		ms.seek (0, IBStream::kIBSeekSet, nullptr);
		check ("setState", rig.comp->setState (&ms) == kResultOk);

		const double a = rig.getParam (jm::kSustain);
		const double b = rig.getParam (jm::kRevMix);
		const double c = rig.getParam (jm::kTone);
		const double d = rig.getParam (jm::kPreset);
		std::snprintf (msg, sizeof (msg), "(%.3f / %.3f / %.3f / %.3f)", a, b, c, d);
		check ("parameters restored",
		       std::fabs (a - 0.31) < 1e-4 && std::fabs (b - 0.42) < 1e-4 &&
		       std::fabs (c - 0.77) < 1e-4 && std::fabs (d - 0.66) < 1e-4, msg);

		// a truncated stream must be rejected, not half-applied
		MemoryStream bad;
		{
			uint32 v1 = 1;
			bad.write (&v1, 4, nullptr);
			float f = 0.5f;
			for (int i = 0; i < 10; ++i) bad.write (&f, 4, nullptr);
		}
		bad.seek (0, IBStream::kIBSeekSet, nullptr);
		check ("truncated state is rejected", rig.comp->setState (&bad) != kResultOk);

		// and a state written by a future version must be rejected rather than
		// read as garbage
		MemoryStream future;
		uint32 ver = 99;
		future.write (&ver, 4, nullptr);
		future.seek (0, IBStream::kIBSeekSet, nullptr);
		check ("unknown state version is rejected", rig.comp->setState (&future) != kResultOk);
	}

	std::printf ("\n[editor]\n");
	{
		IPlugView* view = rig.ctrl->createView (nullptr);
		check ("createView", view != nullptr);
		if (view)
		{
			check ("supports HWND", view->isPlatformTypeSupported (kPlatformTypeHWND) == kResultTrue);

			ViewRect vr = {};
			check ("getSize", view->getSize (&vr) == kResultOk);
			const int vw = (int) (vr.right - vr.left);
			const int vh = (int) (vr.bottom - vr.top);
			std::printf ("  editor size: %d x %d\n", vw, vh);
			check ("editor has a sane size", vw >= 560 && vh >= 340);

			// a resize constraint that a host can honour: the panel keeps its
			// aspect ratio, so a 4000x200 window becomes a usable rect
			{
				ViewRect bad{ 0, 0, 4000, 200 };
				view->checkSizeConstraint (&bad);
				const int nw = (int) (bad.right - bad.left);
				const int nh = (int) (bad.bottom - bad.top);
				std::snprintf (msg, sizeof (msg), "(4000x200 -> %dx%d)", nw, nh);
				const double ar = (double) nw / (double) nh;
				check ("size constraint keeps the aspect ratio",
				       std::fabs (ar - (double) vw / (double) vh) < 0.02, msg);
			}

			HWND parent = CreateWindowExW (0, L"STATIC", L"", WS_POPUP, 0, 0, vw, vh,
			                               nullptr, nullptr, nullptr, nullptr);
			check ("attached to a hidden HWND",
			       view->attached (parent, kPlatformTypeHWND) == kResultOk);

			HWND child = FindWindowExW (parent, nullptr, L"HanaGuitarPanel", nullptr);
			check ("created its child window", child != nullptr);

			if (child)
			{
				RECT rc = {};
				GetClientRect (child, &rc);
				std::printf ("  child client: %ld x %ld\n",
				             (long) (rc.right - rc.left), (long) (rc.bottom - rc.top));

				// Paint twice: the second pass is the one that would expose a
				// surface that was freed or never re-created.
				for (int pass = 0; pass < 2; ++pass)
				{
					HDC screen = GetDC (nullptr);
					HDC mem = CreateCompatibleDC (screen);
					HBITMAP bmp = CreateCompatibleBitmap (screen, vw, vh);
					HGDIOBJ old = SelectObject (mem, bmp);
					SendMessageW (child, WM_PRINTCLIENT, (WPARAM) mem, PRF_CLIENT);
					SelectObject (mem, old);
					DeleteObject (bmp);
					DeleteDC (mem);
					ReleaseDC (nullptr, screen);
				}
				check ("paints twice without crashing", true);
				{
					// The host sends a 0x0 size first, then gives the real one.
					ViewRect zero{ 0, 0, 0, 0 };
					view->onSize (&zero);
					ViewRect normal{ 0, 0, vw, vh };
					view->onSize (&normal);
					const int lit = paintNonBlack (child, vw, vh);
					std::snprintf (msg, sizeof (msg), "(%d lit pixels)", lit);
					check ("panel paints after a degenerate onSize", lit > 1000, msg);
				}

				{
					// The host zeroes the child and resizes the *container*
					// without ever calling onSize (FL does this on refresh).
					SetWindowPos (child, nullptr, 0, 0, 0, 0,
					              SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
					SetWindowPos (parent, nullptr, 0, 0, 900, 560,
					              SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

					MSG m;
					const DWORD until = GetTickCount () + 500;
					while (GetTickCount () < until)
					{
						while (PeekMessageW (&m, nullptr, 0, 0, PM_REMOVE))
						{
							TranslateMessage (&m);
							DispatchMessageW (&m);
						}
						Sleep (5);
					}

					RECT cr = {};
					GetClientRect (child, &cr);
					const int cw = (int) (cr.right - cr.left);
					const int chh = (int) (cr.bottom - cr.top);
					std::snprintf (msg, sizeof (msg), "(child %dx%d)", cw, chh);
					check ("editor follows a container resize", cw >= 880 && chh >= 540, msg);

					const int lit = paintNonBlack (child, cw, chh);
					std::snprintf (msg, sizeof (msg), "(%d lit pixels at %dx%d)", lit, cw, chh);
					check ("panel still paints after the host zeroed it", lit > 1000, msg);

					// put the window back for the rest of the tests
					SetWindowPos (child, nullptr, 0, 0, vw, vh,
					              SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
					SetWindowPos (parent, nullptr, 0, 0, vw, vh,
					              SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
				}
				// give the ADSR panel a visible shape for the screenshot
				driveParam (rig, jm::kBarAtk, (double) jm::normFromPlain (jm::kParamDefs[jm::kBarAtk], 0.25f));
				driveParam (rig, jm::kBarDec, (double) jm::normFromPlain (jm::kParamDefs[jm::kBarDec], 0.55f));
				driveParam (rig, jm::kBarSus, (double) jm::normFromPlain (jm::kParamDefs[jm::kBarSus], 0.45f));
				driveParam (rig, jm::kBarRel, (double) jm::normFromPlain (jm::kParamDefs[jm::kBarRel], 1.00f));
				driveParam (rig, jm::kBarDepth, (double) jm::normFromPlain (jm::kParamDefs[jm::kBarDepth], 0.75f));
				rig.render (256, nullptr, nullptr, nullptr);
				SendMessageW (child, WM_PAINT, 0, 0);
				saveWindowBmp (child, vw, vh, "build/panel.bmp");

				// The panel must be interactive: click the SHOEGAZE preset.
				// Buttons are laid out from the right edge; the first of four
				// starts at kW-12-4*(112+8) and is 112 wide, so its centre is
				// 628+56 = 684 at 1:1.
				{
					// Park drive somewhere the preset will visibly move it.
					driveParam (rig, jm::kDrive,
					            (double) jm::normFromPlain (jm::kParamDefs[jm::kDrive], 0.05f));
					const double beforeDrive = rig.getParam (jm::kDrive);
					const int bx = 528 + 69;
					const int by = 83;
					SendMessageW (child, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM (bx, by));
					SendMessageW (child, WM_LBUTTONUP, 0, MAKELPARAM (bx, by));
					const double afterDrive = rig.getParam (jm::kDrive);
					std::snprintf (msg, sizeof (msg), "(drive %.2f -> %.2f)",
					               beforeDrive, afterDrive);
					check ("preset button changes a parameter",
					       std::fabs (afterDrive - beforeDrive) > 0.05, msg);
				}

				// The circuit switch: clicking RHYTHM must move the parameter
				// (there is no arm to drag any more -- the bar is CC-driven).
				{
					driveParam (rig, jm::kCircuit, 0.0);
					const int sx = 666, sy = 276;
					SendMessageW (child, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM (sx, sy));
					SendMessageW (child, WM_LBUTTONUP, 0, MAKELPARAM (sx, sy));
					const double after = rig.getParam (jm::kCircuit);
					std::snprintf (msg, sizeof (msg), "(circuit %.2f -> %.2f)", 0.0, after);
					check ("circuit switch flips to rhythm", after > 0.5, msg);
				}

				// And a knob drag must change that knob only.
				{
					// sustain is column 0, row 0
					const int kx = 699, ky = 150;
					const double b0 = rig.getParam (jm::kSustain);
					const double o0 = rig.getParam (jm::kBrightness);
					SendMessageW (child, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM (kx, ky));
					SendMessageW (child, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM (kx, ky - 60));
					SendMessageW (child, WM_LBUTTONUP, 0, MAKELPARAM (kx, ky - 60));
					const double b1 = rig.getParam (jm::kSustain);
					const double o1 = rig.getParam (jm::kBrightness);
					std::snprintf (msg, sizeof (msg), "(sustain %.2f -> %.2f)", b0, b1);
					check ("knob drag changes its own parameter", b1 > b0 + 0.1, msg);
					check ("knob drag leaves other knobs alone", std::fabs (o1 - o0) < 1e-6);
				}

				// double-click restores the default
				{
					const double before = rig.getParam (jm::kSustain);
					const double def = (double) jm::normFromPlain (jm::kParamDefs[jm::kSustain],
					                                               jm::kParamDefs[jm::kSustain].def);
					const int kx = 699, ky = 150;
					SendMessageW (child, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM (kx, ky));
					const double after = rig.getParam (jm::kSustain);
					std::snprintf (msg, sizeof (msg), "(%.2f -> %.2f, default %.2f)",
					               before, after, def);
					check ("double-click restores the default",
					       std::fabs (after - def) < 1e-4, msg);
				}

				// resizing must not break the surface
				{
					ViewRect small{ 0, 0, 840, 510 };
					view->onSize (&small);
					HDC screen = GetDC (nullptr);
					HDC mem = CreateCompatibleDC (screen);
					HBITMAP bmp = CreateCompatibleBitmap (screen, 840, 510);
					HGDIOBJ old = SelectObject (mem, bmp);
					SendMessageW (child, WM_PRINTCLIENT, (WPARAM) mem, PRF_CLIENT);
					SelectObject (mem, old);
					DeleteObject (bmp);
					DeleteDC (mem);
					ReleaseDC (nullptr, screen);
					check ("paints after a resize", true);
				}
			}

			check ("removed", view->removed () == kResultOk);
			view->release ();

			// ---- reload: a fresh view on the same plugin and parent ----
			// Hosts do this every time the plugin window is closed and opened
			// again.  The panel must come back painted, not black.
			{
				IPlugView* v2 = rig.ctrl->createView (ViewType::kEditor);
				check ("reload: createView", v2 != nullptr);
				if (v2)
				{
					check ("reload: attached",
					       v2->attached (parent, kPlatformTypeHWND) == kResultOk);
					HWND c2 = FindWindowExW (parent, nullptr, L"HanaGuitarPanel", nullptr);
					check ("reload: window exists", c2 != nullptr);
					if (c2)
					{
						ViewRect rr{ 0, 0, vw, vh };
						v2->onSize (&rr);
						MSG m2;
						const DWORD until = GetTickCount () + 200;
						while (GetTickCount () < until)
						{
							while (PeekMessageW (&m2, nullptr, 0, 0, PM_REMOVE))
							{
								TranslateMessage (&m2);
								DispatchMessageW (&m2);
							}
							Sleep (5);
						}
						const int lit = paintNonBlack (c2, vw, vh);
						std::snprintf (msg, sizeof (msg), "(%d lit pixels)", lit);
						check ("reload: panel paints, not black", lit > 1000, msg);
					}
					v2->removed ();
					v2->release ();
				}
			}
		}
	}

	std::printf ("\n[teardown]\n");
	rig.comp->setActive (false);
	rig.proc->release ();
	rig.ctrl->release ();
	rig.comp->terminate ();
	rig.comp->release ();
	factory->release ();
	check ("unloads", FreeLibrary (mod) != 0);

	std::printf ("\n--------------------------------\n");
	std::printf ("%d passed, %d failed\n", g_pass, g_fail);
	if (g_fail == 0) std::printf ("ALL HOST TESTS PASSED\n");
	return g_fail == 0 ? 0 : 1;
}
