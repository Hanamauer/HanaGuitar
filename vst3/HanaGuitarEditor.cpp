// ===========================================================================
//  HanaGuitarEditor.cpp -- the HanaGuitar for indie panel
//
//  The left half is the artwork the instrument is voiced after: the red offset
//  body, tortoiseshell guard, P90 and humbucker.  The right half is the
//  instrument's *electrics*: string, pickups, the two circuits, amp and space.
//
//  There is no tremolo-arm animation.  The bar is played from CC, so the panel
//  shows it as a number ("BAR -2.4 st"), not as a moving part.
//
//  THREADING
//      UI thread       -> wndProc              -> IParamSink only
//      audio thread    -> engine               -> never touches this file
//      host/automation -> setParamNormalized() -> posts WM_APP_PARAM
//
//  RENDERING
//      Everything is drawn into a 32-bit top-down DIB at the design resolution
//      and stretched into the host window with one StretchBlt.
// ===========================================================================

#include "HanaGuitarEditor.h"

#include <windows.h>
#include <windowsx.h>
#include <propidl.h>
#include <gdiplus.h>

#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "../src/params.h"
#include "../src/presets.h"
#include "../src/keyswitch.h"
#include "hero_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace Steinberg;
using namespace Gdiplus;
using namespace Gdiplus::DllExports;

namespace jm {
namespace {

const UINT WM_APP_PARAM = WM_APP + 1;

// If the panel ever comes up black in a host, this file says why: the client
// size the host gave us and whether GDI+ came up.
void uiLog (const char* msg, int a = 0, int b = 0)
{
	char path[MAX_PATH] = { 0 };
	DWORD n = GetTempPathA (MAX_PATH, path);
	if (n == 0 || n >= MAX_PATH) return;
	std::strncat (path, "HanaGuitar-ui.log", MAX_PATH - std::strlen (path) - 1);
	std::FILE* f = std::fopen (path, "a");
	if (f)
	{
		std::fprintf (f, "%s %d %d\n", msg, a, b);
		std::fclose (f);
	}
}

// ---- layout ---------------------------------------------------------------
const int kW = 1120;
const int kH = 680;

const int kImageW = 509;          // artwork occupies the left edge, full height
const int kX0 = 528;              // controls start here
const int kRight = 1104;          // controls end here
const int kKnobX = 618;           // knob area starts here (labels to the left)
const int kRowY0 = 112;
const int kRowH = 78;
const int kPresetY = 68;
const int kPresetH = 30;
const int kLegendY = 592;

// ---- palette --------------------------------------------------------------
struct Rgb { int r, g, b; };

const Rgb kBg       {   6,   9,  18 };
const Rgb kPanel    {  11,  16,  30 };
const Rgb kPanelHi  {  18,  26,  46 };
const Rgb kInk      { 226, 236, 250 };
const Rgb kDim      { 118, 136, 166 };
const Rgb kLine     {  34,  48,  76 };
const Rgb kAccent   {  86, 208, 255 };
const Rgb kAccent2  { 255, 106, 128 };
const Rgb kLamp     { 126, 246, 176 };

inline ARGB argb (const Rgb& c, int a = 255)
{
	return (ARGB) (((DWORD) a << 24) | ((DWORD) c.r << 16) |
	               ((DWORD) c.g << 8) | (DWORD) c.b);
}
inline ARGB argb (int r, int g, int b, int a = 255)
{
	return (ARGB) (((DWORD) a << 24) | ((DWORD) (r & 255) << 16) |
	               ((DWORD) (g & 255) << 8) | (DWORD) (b & 255));
}

struct RectI
{
	int x = 0, y = 0, w = 0, h = 0;
	bool has (int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
	float cx () const { return (float) x + 0.5f * (float) w; }
	float cy () const { return (float) y + 0.5f * (float) h; }
};

struct Knob
{
	int id = -1;
	RectI r;
	int row = 0;
};

// Row titles, in the order the rows are laid out.
struct RowDef
{
	const char* label;
	int first;       // first parameter id in the row
	int count;       // number of knobs
	int extra;       // row 2 starts with the circuit switch
};

const RowDef kRows[6] = {
	{ "STRING",  kSustain,    3, 0 },
	{ "PICKUP",  kPickupSel,  5, 0 },
	{ "CIRCUIT", kVolume,     3, 1 },   // the switch comes first
	{ "AMP",     kDrive,      7, 0 },
	{ "SPACE",   kChorus,     6, 0 },
	{ "MASTER",  kTremRate,   7, 0 },
};

const int kNumRows = 6;

// ---------------------------------------------------------------------------
// IPlugView has no message channel.  The editor registers its window against
// the view pointer, and the plugin -- which may call from the host thread --
// looks the window up and posts.  A global FindWindow would post to the wrong
// instance as soon as two copies of the plugin are open.
struct ViewMap
{
	struct Entry { const IPlugView* view; HWND hwnd; };
	CRITICAL_SECTION cs;
	Entry e[16];
	int n = 0;

	ViewMap () { InitializeCriticalSection (&cs); }
	~ViewMap () { DeleteCriticalSection (&cs); }

	void add (const IPlugView* v, HWND h)
	{
		EnterCriticalSection (&cs);
		for (int i = 0; i < n; ++i) if (e[i].view == v) { e[i].hwnd = h; LeaveCriticalSection (&cs); return; }
		if (n < 16) e[n++] = { v, h };
		LeaveCriticalSection (&cs);
	}
	void remove (const IPlugView* v)
	{
		EnterCriticalSection (&cs);
		for (int i = 0; i < n; ++i)
			if (e[i].view == v) { e[i] = e[--n]; break; }
		LeaveCriticalSection (&cs);
	}
	HWND find (const IPlugView* v)
	{
		HWND h = nullptr;
		EnterCriticalSection (&cs);
		for (int i = 0; i < n; ++i) if (e[i].view == v) { h = e[i].hwnd; break; }
		LeaveCriticalSection (&cs);
		return h;
	}
};

ViewMap& viewMap () { static ViewMap m; return m; }


// ---------------------------------------------------------------------------
// The view
// ---------------------------------------------------------------------------
class HanaGuitarEditor : public IPlugView, public IPlugViewContentScaleSupport
{
public:
	explicit HanaGuitarEditor (IParamSink* sink) : sink_ (sink) { buildLayout (); }
	~HanaGuitarEditor ();

	tresult PLUGIN_API queryInterface (const TUID iid, void** obj) SMTG_OVERRIDE
	{
		if (std::memcmp (iid, IPlugView::iid, 16) == 0 ||
		    std::memcmp (iid, FUnknown::iid, 16) == 0)
		{
			*obj = static_cast<IPlugView*> (this);
			addRef ();
			return kResultOk;
		}
		if (std::memcmp (iid, IPlugViewContentScaleSupport::iid, 16) == 0)
		{
			*obj = static_cast<IPlugViewContentScaleSupport*> (this);
			addRef ();
			return kResultOk;
		}
		*obj = nullptr;
		return kNoInterface;
	}

	uint32 PLUGIN_API addRef () SMTG_OVERRIDE { return ++refs_; }
	uint32 PLUGIN_API release () SMTG_OVERRIDE
	{
		const uint32 n = --refs_;
		if (n == 0) delete this;
		return n;
	}

	tresult PLUGIN_API isPlatformTypeSupported (FIDString type) SMTG_OVERRIDE
	{
		return (type && std::strcmp (type, kPlatformTypeHWND) == 0) ? kResultTrue
		                                                          : kResultFalse;
	}

	tresult PLUGIN_API attached (void* parent, FIDString type) SMTG_OVERRIDE;
	tresult PLUGIN_API removed () SMTG_OVERRIDE;

	tresult PLUGIN_API getSize (ViewRect* size) SMTG_OVERRIDE
	{
		if (!size) return kInvalidArgument;
		size->left = 0;
		size->top = 0;
		size->right = (int32) (kW * scale_ + 0.5f);
		size->bottom = (int32) (kH * scale_ + 0.5f);
		return kResultOk;
	}

	tresult PLUGIN_API onSize (ViewRect* r) SMTG_OVERRIDE
	{
		if (!r) return kInvalidArgument;
		const int pxW = (int) (r->right - r->left);
		const int pxH = (int) (r->bottom - r->top);
		if (pxW >= 8 && pxH >= 8) layoutToWindow (pxW, pxH);
		else uiLog ("onSize-degenerate", pxW, pxH);
		return kResultOk;
	}

	tresult PLUGIN_API setContentScaleFactor (float f) SMTG_OVERRIDE
	{
		if (f <= 0.f) return kInvalidArgument;
		scaleHint_ = std::min (3.f, std::max (1.f, f));
		if (frame_)
		{
			ViewRect r{ 0, 0, (int32) (kW * scaleHint_ + 0.5f),
			            (int32) (kH * scaleHint_ + 0.5f) };
			frame_->resizeView (this, &r);
		}
		return kResultOk;
	}

	tresult PLUGIN_API canResize () SMTG_OVERRIDE { return kResultTrue; }

	tresult PLUGIN_API checkSizeConstraint (ViewRect* r) SMTG_OVERRIDE
	{
		if (!r) return kInvalidArgument;
		const int w = (int) (r->right - r->left);
		const int h = (int) (r->bottom - r->top);
		const float aspect = (float) kW / (float) kH;
		int nw = w, nh = h;
		if (nw < 560) nw = 560;
		if (nw > 3360) nw = 3360;
		nh = (int) ((float) nw / aspect + 0.5f);
		if (nh < 340) { nh = 340; nw = (int) (nh * aspect + 0.5f); }
		if (nw != w || nh != h)
		{
			r->right = r->left + (int32) nw;
			r->bottom = r->top + (int32) nh;
		}
		return kResultTrue;
	}

	tresult PLUGIN_API onFocus (TBool) SMTG_OVERRIDE { return kResultOk; }
	tresult PLUGIN_API setFrame (IPlugFrame* f) SMTG_OVERRIDE { frame_ = f; return kResultOk; }
	tresult PLUGIN_API onWheel (float) SMTG_OVERRIDE { return kResultFalse; }
	tresult PLUGIN_API onKeyDown (char16, int16, int16) SMTG_OVERRIDE { return kResultFalse; }
	tresult PLUGIN_API onKeyUp (char16, int16, int16) SMTG_OVERRIDE { return kResultFalse; }

	void onParamChanged (int) { if (hwnd_) PostMessageW (hwnd_, WM_APP_PARAM, 0, 0); }

private:
	// ---- setup ------------------------------------------------------------
	void buildLayout ();
	bool initGdi ();
	void shutdownGdi ();
	bool ensureSurface ();
	void layoutToWindow (int w, int h);
	void syncToParent ();
	bool makeWindow (HWND parent);
	void designFromWindow (int wx, int wy, int& dx, int& dy) const;

	static LRESULT CALLBACK thunk (HWND h, UINT m, WPARAM w, LPARAM l)
	{
		HanaGuitarEditor* self =
		    reinterpret_cast<HanaGuitarEditor*> (GetWindowLongPtrW (h, GWLP_USERDATA));
		return self ? self->wndProc (h, m, w, l) : DefWindowProcW (h, m, w, l);
	}
	LRESULT wndProc (HWND h, UINT m, WPARAM w, LPARAM l);

	// ---- painting ---------------------------------------------------------
	void paint (HDC dc);
	void drawBackground (GpGraphics* g);
	void drawArtwork (GpGraphics* g);
	void drawHeader (GpGraphics* g);
	void drawPresets (GpGraphics* g);
	void drawKnobs (GpGraphics* g);
	void drawKnob (GpGraphics* g, const Knob& k);
	void drawCircuitSwitch (GpGraphics* g);
	void drawLegend (GpGraphics* g);
	void drawFooter (GpGraphics* g);

	// ---- primitives -------------------------------------------------------
	void fillRect (GpGraphics* g, GpBrush* b, int x, int y, int w, int h);
	void fillRound (GpGraphics* g, GpBrush* b, int x, int y, int w, int h, int r);
	void line (GpGraphics* g, GpPen* p, int x0, int y0, int x1, int y1);
	void ellipse (GpGraphics* g, GpBrush* b, int x, int y, int w, int h);
	void txt (GpGraphics* g, GpBrush* b, GpFont* f, const char* s,
	          int x, int y, int w, int h, int align = 0);

	// ---- interaction ------------------------------------------------------
	int knobAt (int x, int y) const;
	void beginDrag (int id, int x, int y);
	void dragTo (int x, int y);
	void endDrag ();
	void nudge (int id, double delta);
	void cycleDiscrete (int id, int dir);
	double valueOf (int id) const;
	void setValue (int id, double norm, bool gesture);
	void applyPreset (int idx);
	int presetIndex () const;
	void toggleCircuit ();

	static void formatValue (int id, double norm, char* out, int n);

	IParamSink* sink_ = nullptr;
	uint32 refs_ = 1;

	HWND hwnd_ = nullptr;
	HWND parent_ = nullptr;
	IPlugFrame* frame_ = nullptr;
	float scale_ = 1.f;
	float scaleHint_ = 1.f;
	int winW_ = kW, winH_ = kH;
	float scaleXY_ = 1.f;
	int offX_ = 0, offY_ = 0;

	Knob knobs[kNumKnobs];
	int knobCount_ = 0;
	RectI circuitSwitch_;
	RectI presetBtn_[4];
	RectI barRect_;

	int dragId_ = -1;
	int dragY0_ = 0;
	double dragV0_ = 0.0;
	int presetHot_ = -1;
	int knobHot_ = -1;
	int presetIdx_ = 3;

	// GDI+
	ULONG_PTR gdiToken_ = 0;
	bool gdiOk_ = false;
	HDC memDc_ = nullptr;
	HBITMAP dib_ = nullptr;
	HBITMAP oldBmp_ = nullptr;
	void* bits_ = nullptr;
	int dibW_ = 0, dibH_ = 0;

	GpFontFamily* ff_ = nullptr;
	GpFontFamily* ffBold_ = nullptr;
	GpFont* fTitle_ = nullptr;
	GpFont* fLabel_ = nullptr;
	GpFont* fValue_ = nullptr;
	GpFont* fSmall_ = nullptr;
	GpFont* fTiny_ = nullptr;
	GpFont* fPreset_ = nullptr;
	GpStringFormat* fmtL_ = nullptr;
	GpStringFormat* fmtC_ = nullptr;
	GpStringFormat* fmtR_ = nullptr;
	GpBitmap* hero_ = nullptr;

	GpSolidFill* brBg_ = nullptr;
	GpSolidFill* brPanel_ = nullptr;
	GpSolidFill* brPanelHi_ = nullptr;
	GpSolidFill* brInk_ = nullptr;
	GpSolidFill* brDim_ = nullptr;
	GpSolidFill* brLine_ = nullptr;
	GpSolidFill* brAccent_ = nullptr;
	GpSolidFill* brAccent2_ = nullptr;
	GpSolidFill* brLamp_ = nullptr;
	GpLineGradient* gradRight_ = nullptr;
	GpLineGradient* gradBottom_ = nullptr;
	GpLineGradient* gradPanel_ = nullptr;
	GpPen* penLine_ = nullptr;
	GpPen* penAccent_ = nullptr;
	GpPen* penDim_ = nullptr;
	GpPen* penTrack_ = nullptr;

	float peakShow_ = 0.f;
	int techMask_ = 0;
	float barShow_ = 0.f;
};

// ---------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------
void HanaGuitarEditor::buildLayout ()
{
	knobCount_ = 0;
	for (int r = 0; r < kNumRows; ++r)
	{
		const RowDef& row = kRows[r];
		const int total = row.count + row.extra;
		const int cellW = (kRight - kKnobX) / total;
		const int y = kRowY0 + r * kRowH;

		int slot = 0;
		if (row.extra)
		{
			circuitSwitch_.x = kKnobX + 6;
			circuitSwitch_.y = y + 18;
			circuitSwitch_.w = 84;
			circuitSwitch_.h = 36;
			++slot;
		}
		for (int i = 0; i < row.count; ++i)
		{
			Knob& k = knobs[knobCount_++];
			k.id = row.first + i;
			k.row = r;
			k.r.x = kKnobX + slot * cellW;
			k.r.y = y;
			k.r.w = cellW;
			k.r.h = kRowH;
			++slot;
		}
	}

	barRect_ = { kRight - 152, 4, 152, 30 };

	for (int i = 0; i < 4; ++i)
	{
		const int bw = (kRight - kX0 - 3 * 8) / 4;
		presetBtn_[i].x = kX0 + i * (bw + 8);
		presetBtn_[i].y = kPresetY;
		presetBtn_[i].w = bw;
		presetBtn_[i].h = kPresetH;
	}
}

// ---------------------------------------------------------------------------
// GDI+ setup
// ---------------------------------------------------------------------------
bool HanaGuitarEditor::initGdi ()
{
	GdiplusStartupInput in;
	if (GdiplusStartup (&gdiToken_, &in, nullptr) != Ok) return false;
	gdiOk_ = true;

	auto family = [] (const WCHAR* name, GpFontFamily** out) {
		if (GdipCreateFontFamilyFromName (name, nullptr, out) != Ok)
			GdipCreateFontFamilyFromName (L"Segoe UI", nullptr, out);
	};
	family (L"Segoe UI", &ff_);
	family (L"Segoe UI Semibold", &ffBold_);

	GdipCreateFont (ffBold_, 27.f, FontStyleRegular, UnitPixel, &fTitle_);
	GdipCreateFont (ffBold_, 10.f, FontStyleRegular, UnitPixel, &fLabel_);
	GdipCreateFont (ff_, 11.f, FontStyleRegular, UnitPixel, &fValue_);
	GdipCreateFont (ff_, 10.f, FontStyleRegular, UnitPixel, &fSmall_);
	GdipCreateFont (ff_, 8.5f, FontStyleRegular, UnitPixel, &fTiny_);
	GdipCreateFont (ffBold_, 11.f, FontStyleRegular, UnitPixel, &fPreset_);

	GdipCreateStringFormat (0, 0, &fmtL_);
	GdipSetStringFormatAlign (fmtL_, StringAlignmentNear);
	GdipSetStringFormatLineAlign (fmtL_, StringAlignmentCenter);
	GdipCreateStringFormat (0, 0, &fmtC_);
	GdipSetStringFormatAlign (fmtC_, StringAlignmentCenter);
	GdipSetStringFormatLineAlign (fmtC_, StringAlignmentCenter);
	GdipCreateStringFormat (0, 0, &fmtR_);
	GdipSetStringFormatAlign (fmtR_, StringAlignmentFar);
	GdipSetStringFormatLineAlign (fmtR_, StringAlignmentCenter);

	GdipCreateSolidFill (argb (kBg), &brBg_);
	GdipCreateSolidFill (argb (kPanel), &brPanel_);
	GdipCreateSolidFill (argb (kPanelHi), &brPanelHi_);
	GdipCreateSolidFill (argb (kInk), &brInk_);
	GdipCreateSolidFill (argb (kDim), &brDim_);
	GdipCreateSolidFill (argb (kLine), &brLine_);
	GdipCreateSolidFill (argb (kAccent), &brAccent_);
	GdipCreateSolidFill (argb (kAccent2), &brAccent2_);
	GdipCreateSolidFill (argb (kLamp), &brLamp_);

	GdipCreatePen1 (argb (kLine), 1.f, UnitPixel, &penLine_);
	GdipCreatePen1 (argb (kAccent), 2.f, UnitPixel, &penAccent_);
	GdipCreatePen1 (argb (kDim), 1.f, UnitPixel, &penDim_);
	GdipCreatePen1 (argb (kLine), 3.f, UnitPixel, &penTrack_);

	{
		RectF rr ((float) (kImageW - 64), 0.f, 64.f, (float) kH);
		GdipCreateLineBrushFromRectWithAngle (&rr, argb (kPanel, 0), argb (kPanel),
		                                      0.f, TRUE, WrapModeTile, &gradRight_);
		RectF rb (0.f, (float) (kH - 96), (float) kImageW, 96.f);
		GdipCreateLineBrushFromRectWithAngle (&rb, argb (kBg, 0), argb (kBg),
		                                      90.f, TRUE, WrapModeTile, &gradBottom_);
		RectF rp ((float) (kImageW + 1), 2.f, (float) (kW - kImageW - 1), 150.f);
		GdipCreateLineBrushFromRectWithAngle (&rp, argb (24, 58, 96, 40), argb (24, 58, 96, 0),
		                                      90.f, TRUE, WrapModeTile, &gradPanel_);
	}

	// The artwork, decoded once from the embedded JPEG.
	IStream* stream = nullptr;
	HGLOBAL mem = GlobalAlloc (GMEM_MOVEABLE, jm::kHeroJpgSize);
	if (mem)
	{
		void* p = GlobalLock (mem);
		if (p)
		{
			std::memcpy (p, jm::kHeroJpg, jm::kHeroJpgSize);
			GlobalUnlock (mem);
			if (CreateStreamOnHGlobal (mem, TRUE, &stream) == S_OK && stream)
			{
				GdipCreateBitmapFromStream (stream, &hero_);
				stream->Release ();
			}
		}
		else GlobalFree (mem);
	}

	return true;
}

void HanaGuitarEditor::shutdownGdi ()
{
	if (!gdiOk_) return;
	if (hero_) { GdipDisposeImage (hero_); hero_ = nullptr; }
	if (fTitle_) GdipDeleteFont (fTitle_);
	if (fLabel_) GdipDeleteFont (fLabel_);
	if (fValue_) GdipDeleteFont (fValue_);
	if (fSmall_) GdipDeleteFont (fSmall_);
	if (fTiny_) GdipDeleteFont (fTiny_);
	if (fPreset_) GdipDeleteFont (fPreset_);
	if (ff_) GdipDeleteFontFamily (ff_);
	if (ffBold_) GdipDeleteFontFamily (ffBold_);
	if (fmtL_) GdipDeleteStringFormat (fmtL_);
	if (fmtC_) GdipDeleteStringFormat (fmtC_);
	if (fmtR_) GdipDeleteStringFormat (fmtR_);
	if (brBg_) GdipDeleteBrush (brBg_);
	if (brPanel_) GdipDeleteBrush (brPanel_);
	if (brPanelHi_) GdipDeleteBrush (brPanelHi_);
	if (brInk_) GdipDeleteBrush (brInk_);
	if (brDim_) GdipDeleteBrush (brDim_);
	if (brLine_) GdipDeleteBrush (brLine_);
	if (brAccent_) GdipDeleteBrush (brAccent_);
	if (brAccent2_) GdipDeleteBrush (brAccent2_);
	if (brLamp_) GdipDeleteBrush (brLamp_);
	if (gradRight_) GdipDeleteBrush (gradRight_);
	if (gradBottom_) GdipDeleteBrush (gradBottom_);
	if (gradPanel_) GdipDeleteBrush (gradPanel_);
	if (penLine_) GdipDeletePen (penLine_);
	if (penAccent_) GdipDeletePen (penAccent_);
	if (penDim_) GdipDeletePen (penDim_);
	if (penTrack_) GdipDeletePen (penTrack_);
	if (gdiOk_) { GdiplusShutdown (gdiToken_); gdiOk_ = false; }
}

bool HanaGuitarEditor::ensureSurface ()
{
	if (memDc_ && dibW_ == kW && dibH_ == kH) return true;
	if (memDc_) { SelectObject (memDc_, oldBmp_); DeleteObject (dib_); DeleteDC (memDc_); }
	HDC screen = GetDC (nullptr);
	memDc_ = CreateCompatibleDC (screen);
	ReleaseDC (nullptr, screen);
	if (!memDc_) return false;

	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = kW;
	bi.bmiHeader.biHeight = -kH;          // top-down
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	dib_ = CreateDIBSection (memDc_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
	if (!dib_) return false;
	oldBmp_ = (HBITMAP) SelectObject (memDc_, dib_);
	dibW_ = kW;
	dibH_ = kH;
	return true;
}

// ---------------------------------------------------------------------------
// window
// ---------------------------------------------------------------------------
bool HanaGuitarEditor::makeWindow (HWND parent)
{
	static bool registered = false;
	if (!registered)
	{
		WNDCLASSEXW wc = {};
		wc.cbSize = sizeof (wc);
		wc.style = CS_HREDRAW | CS_VREDRAW;
		wc.lpfnWndProc = thunk;
		wc.hInstance = GetModuleHandleW (nullptr);
		wc.hCursor = LoadCursorW (nullptr, IDC_ARROW);
		wc.lpszClassName = L"HanaGuitarPanel";
		if (!RegisterClassExW (&wc)) return false;
		registered = true;
	}
	hwnd_ = CreateWindowExW (0, L"HanaGuitarPanel", L"", WS_CHILD | WS_VISIBLE,
	                         0, 0, winW_, winH_, parent, nullptr,
	                         GetModuleHandleW (nullptr), this);
	if (!hwnd_) return false;
	SetWindowLongPtrW (hwnd_, GWLP_USERDATA, (LONG_PTR) this);
	return true;
}

tresult PLUGIN_API HanaGuitarEditor::attached (void* parent, FIDString type)
{
	if (!parent || !type || std::strcmp (type, kPlatformTypeHWND) != 0)
		return kResultFalse;
	parent_ = (HWND) parent;
	{
		RECT prc = {};
		GetClientRect (parent_, &prc);
		const int pw = (int) (prc.right - prc.left);
		const int ph = (int) (prc.bottom - prc.top);
		winW_ = pw > 16 ? pw : kW;
		winH_ = ph > 16 ? ph : kH;
	}
	if (!initGdi ()) { uiLog ("initGdi-failed", 0, 0); return kResultFalse; }
	if (!makeWindow (parent_)) return kResultFalse;
	layoutToWindow (winW_, winH_);
	syncToParent ();
	uiLog ("attached", winW_, winH_);
	viewMap ().add (this, hwnd_);
	SetTimer (hwnd_, 1, 50, nullptr);
	sink_->setActiveEditor (this);
	return kResultOk;
}

tresult PLUGIN_API HanaGuitarEditor::removed ()
{
	viewMap ().remove (this);
	sink_->setActiveEditor (nullptr);
	if (hwnd_)
	{
		KillTimer (hwnd_, 1);
		DestroyWindow (hwnd_);
		hwnd_ = nullptr;
	}
	shutdownGdi ();
	if (memDc_) { SelectObject (memDc_, oldBmp_); DeleteObject (dib_); DeleteDC (memDc_); }
	memDc_ = nullptr; dib_ = nullptr; oldBmp_ = nullptr;
	return kResultOk;
}

HanaGuitarEditor::~HanaGuitarEditor ()
{
	removed ();
}

void HanaGuitarEditor::layoutToWindow (int w, int h)
{
	winW_ = w; winH_ = h;
	const float sx = (float) w / (float) kW;
	const float sy = (float) h / (float) kH;
	scaleXY_ = std::min (sx, sy);
	offX_ = (int) ((w - kW * scaleXY_) * 0.5f);
	offY_ = (int) ((h - kH * scaleXY_) * 0.5f);
	if (hwnd_) MoveWindow (hwnd_, 0, 0, w, h, TRUE);
}

// The host is the authority on how big the editor is.  Some hosts (FL in
// particular) resize the container window and never call onSize; others
// reparent the view when the plugin is refreshed.  Both cases end with a child
// window that is 0x0 or the wrong size, which reads as a black panel.  This
// makes the child follow its *current* parent, whatever happened to it.
void HanaGuitarEditor::syncToParent ()
{
	if (!hwnd_) return;

	HWND parent = GetParent (hwnd_);
	if (!parent) return;
	parent_ = parent;

	RECT prc = {};
	if (!GetClientRect (parent, &prc)) return;
	const int pw = (int) (prc.right - prc.left);
	const int ph = (int) (prc.bottom - prc.top);
	if (pw < 8 || ph < 8) return;

	RECT rc = {};
	GetClientRect (hwnd_, &rc);
	const int cw = (int) (rc.right - rc.left);
	const int ch = (int) (rc.bottom - rc.top);

	if (cw < 8 || ch < 8 || std::abs (cw - pw) > 2 || std::abs (ch - ph) > 2)
	{
		layoutToWindow (pw, ph);
		InvalidateRect (hwnd_, nullptr, TRUE);
	}
}

void HanaGuitarEditor::designFromWindow (int wx, int wy, int& dx, int& dy) const
{
	dx = (int) ((wx - offX_) / std::max (0.01f, scaleXY_));
	dy = (int) ((wy - offY_) / std::max (0.01f, scaleXY_));
}

// ---------------------------------------------------------------------------
// primitives
// ---------------------------------------------------------------------------
void HanaGuitarEditor::fillRect (GpGraphics* g, GpBrush* b, int x, int y, int w, int h)
{
	if (b && w > 0 && h > 0) GdipFillRectangleI (g, b, x, y, w, h);
}

void HanaGuitarEditor::fillRound (GpGraphics* g, GpBrush* b, int x, int y, int w, int h, int r)
{
	if (!b || w <= 0 || h <= 0) return;
	GpGraphics* save = nullptr;
	(void) save;
	GpPath* path = nullptr;
	GdipCreatePath (FillModeWinding, &path);
	const float fw = (float) w, fh = (float) h, fr = (float) r;
	GdipAddPathArc (path, (float) x, (float) y, fr * 2, fr * 2, 180.f, 90.f);
	GdipAddPathArc (path, (float) x + fw - fr * 2, (float) y, fr * 2, fr * 2, 270.f, 90.f);
	GdipAddPathArc (path, (float) x + fw - fr * 2, (float) y + fh - fr * 2, fr * 2, fr * 2, 0.f, 90.f);
	GdipAddPathArc (path, (float) x, (float) y + fh - fr * 2, fr * 2, fr * 2, 90.f, 90.f);
	GdipClosePathFigure (path);
	GdipFillPath (g, b, path);
	GdipDeletePath (path);
}

void HanaGuitarEditor::line (GpGraphics* g, GpPen* p, int x0, int y0, int x1, int y1)
{
	GdipDrawLine (g, p, (float) x0, (float) y0, (float) x1, (float) y1);
}

void HanaGuitarEditor::ellipse (GpGraphics* g, GpBrush* b, int x, int y, int w, int h)
{
	if (b && w > 0 && h > 0)
		GdipFillEllipse (g, b, (float) x, (float) y, (float) w, (float) h);
}

void HanaGuitarEditor::txt (GpGraphics* g, GpBrush* b, GpFont* f, const char* s,
                          int x, int y, int w, int h, int align)
{
	if (!s || !*s || !f) return;
	WCHAR wide[256];
	const int n = MultiByteToWideChar (CP_ACP, 0, s, -1, wide, 255);
	if (n <= 0) return;
	RectF rect ((float) x, (float) y, (float) w, (float) h);
	GpStringFormat* fmt = (align == 1) ? fmtC_ : (align == 2 ? fmtR_ : fmtL_);
	GdipDrawString (g, wide, n - 1, f, &rect, fmt, b);
}

// ---------------------------------------------------------------------------
// paint
// ---------------------------------------------------------------------------
void HanaGuitarEditor::paint (HDC dc)
{
	// Never trust the size the host last passed to onSize: some hosts send a
	// 0x0 rect while creating the view and then resize the HWND directly.  The
	// client rect is the only authority here.
	RECT rc = {};
	GetClientRect (hwnd_, &rc);
	int cw = (int) (rc.right - rc.left);
	int ch = (int) (rc.bottom - rc.top);
	if (cw < 8 || ch < 8)
	{
		// Last resort: the host may have zeroed us and the timer has not run
		// yet.  Ask the parent once more before giving up on this paint.
		syncToParent ();
		GetClientRect (hwnd_, &rc);
		cw = (int) (rc.right - rc.left);
		ch = (int) (rc.bottom - rc.top);
		if (cw < 8 || ch < 8)
		{
			uiLog ("degenerate-client", cw, ch);
			return;
		}
	}
	scaleXY_ = std::min ((float) cw / (float) kW, (float) ch / (float) kH);
	offX_ = (int) ((cw - kW * scaleXY_) * 0.5f);
	offY_ = (int) ((ch - kH * scaleXY_) * 0.5f);

	if (!gdiOk_)
	{
		// GDI+ failed to start: still paint something readable, so the panel is
		// never mistaken for a dead plugin.
		HBRUSH b = CreateSolidBrush (RGB (10, 14, 26));
		FillRect (dc, &rc, b);
		DeleteObject (b);
		SetBkMode (dc, TRANSPARENT);
		SetTextColor (dc, RGB (220, 230, 250));
		DrawTextW (dc, L"HanaGuitar for indie - GDI+ failed to initialise", -1, &rc,
		           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		uiLog ("gdiplus-failed", cw, ch);
		return;
	}

	if (!ensureSurface ()) { uiLog ("surface-failed", cw, ch); return; }

	GpGraphics* g = nullptr;
	GdipCreateFromHDC (memDc_, &g);
	if (!g) return;
	GdipSetSmoothingMode (g, SmoothingModeAntiAlias);
	GdipSetTextRenderingHint (g, TextRenderingHintClearTypeGridFit);
	GdipSetPixelOffsetMode (g, PixelOffsetModeHalf);

	drawBackground (g);
	drawArtwork (g);
	drawHeader (g);
	drawPresets (g);
	drawKnobs (g);
	drawLegend (g);
	drawFooter (g);

	GdipDeleteGraphics (g);
	StretchBlt (dc, offX_, offY_, (int) (kW * scaleXY_), (int) (kH * scaleXY_),
	            memDc_, 0, 0, kW, kH, SRCCOPY);
}

void HanaGuitarEditor::drawBackground (GpGraphics* g)
{
	fillRect (g, brBg_, 0, 0, kW, kH);
	fillRect (g, brPanel_, kImageW, 0, kW - kImageW, kH);

	// A faint vertical separator, then the panel's top highlight.
	fillRect (g, brLine_, kImageW, 0, 1, kH);
	fillRect (g, brPanelHi_, kImageW + 1, 0, kW - kImageW - 1, 2);

	// A soft top highlight, so the knob area is not a flat rectangle.
	if (gradPanel_) fillRect (g, gradPanel_, kImageW + 1, 2, kW - kImageW - 1, 150);
}

void HanaGuitarEditor::drawArtwork (GpGraphics* g)
{
	if (hero_) GdipDrawImageRectI (g, (GpImage*) hero_, 0, 0, kImageW, kH);
	else fillRect (g, brPanelHi_, 0, 0, kImageW, kH);

	// Fade the artwork's right edge into the panel so the join is not a seam,
	// and its bottom into the footer.  Both are cached gradient brushes: the
	// old version built 146 solid fills per frame.
	if (gradRight_) fillRect (g, gradRight_, kImageW - 64, 0, 64, kH);
	if (gradBottom_) fillRect (g, gradBottom_, 0, kH - 96, kImageW, 96);
}

void HanaGuitarEditor::drawHeader (GpGraphics* g)
{
	txt (g, brInk_, fTitle_, "HANA GUITAR", kX0, 12, 300, 34);
	txt (g, brDim_, fSmall_, "for indie  |  P90 + humbucker  |  lead / rhythm", kX0, 44, 250, 16);

	char bar[32];
	std::snprintf (bar, sizeof (bar), "BAR %+.1f st", barShow_);
	txt (g, brAccent_, fValue_, bar, kRight - 150, 10, 150, 18, 2);

	// level meter
	const float pk = peakShow_;
	const int mx = kRight - 150, my = 32, mw = 150, mh = 6;
	fillRound (g, brLine_, mx, my, mw, mh, 3);
	const int fw = (int) (mw * std::min (1.f, pk * 1.25f));
	if (fw > 0)
	{
		GpSolidFill* c = nullptr;
		GdipCreateSolidFill (argb (pk > 0.98f ? kAccent2 : kAccent), &c);
		fillRound (g, c, mx, my, fw, mh, 3);
		GdipDeleteBrush (c);
	}

	// technique lamps
	const int lampY = 48;
	const char* names[6] = { "PM", "HARM", "TAP", "SWL", "TRM", "FB" };
	for (int i = 0; i < 6; ++i)
	{
		const bool on = (techMask_ & (1 << i)) != 0;
		const int lx = kRight - 322 + i * 52;
		ellipse (g, on ? brLamp_ : brLine_, lx, lampY, 7, 7);
		txt (g, on ? brLamp_ : brDim_, fTiny_, names[i], lx + 11, lampY - 6, 40, 18);
	}
}
void HanaGuitarEditor::drawPresets (GpGraphics* g)
{
	for (int i = 0; i < 4; ++i)
	{
		const bool sel = (presetIndex () == i);
		const bool hot = (presetHot_ == i);
		GpSolidFill* bg = nullptr;
		GdipCreateSolidFill (sel ? argb (kAccent, 46) : (hot ? argb (34, 48, 74) : argb (kPanelHi)), &bg);
		fillRound (g, bg, presetBtn_[i].x, presetBtn_[i].y, presetBtn_[i].w, presetBtn_[i].h, 5);
		if (sel)
		{
			GpPen* p = nullptr;
			GdipCreatePen1 (argb (kAccent), 1.f, UnitPixel, &p);
			GpPath* path = nullptr;
			GdipCreatePath (FillModeWinding, &path);
			GdipAddPathRectangle (path, (float) presetBtn_[i].x + 0.5f,
			                      (float) presetBtn_[i].y + 0.5f,
			                      (float) presetBtn_[i].w - 1.f, (float) presetBtn_[i].h - 1.f);
			GdipDrawPath (g, p, path);
			GdipDeletePath (path);
			GdipDeletePen (p);
		}
		txt (g, sel ? brAccent_ : brInk_, fPreset_, kPresets[i].name,
		     presetBtn_[i].x, presetBtn_[i].y + 7, presetBtn_[i].w, 18, 1);
		GdipDeleteBrush (bg);
	}
}

void HanaGuitarEditor::drawKnobs (GpGraphics* g)
{
	for (int r = 0; r < kNumRows; ++r)
	{
		const int y = kRowY0 + r * kRowH;
		txt (g, brDim_, fLabel_, kRows[r].label, kX0, y + 30, 80, 16);
	}
	drawCircuitSwitch (g);
	for (int i = 0; i < knobCount_; ++i) drawKnob (g, knobs[i]);
}

void HanaGuitarEditor::drawKnob (GpGraphics* g, const Knob& k)
{
	const float cx = k.r.cx ();
	const float cy = k.r.y + 38.f;
	const float rad = 20.f;

	char label[64];
	std::snprintf (label, sizeof (label), "%s", kParamDefs[k.id].name);
	txt (g, brDim_, fLabel_, label, k.r.x, k.r.y + 2, k.r.w, 14, 1);

	// track
	GdipDrawArc (g, penTrack_, cx - rad, cy - rad, rad * 2, rad * 2, 135.f, 270.f);

	const double v = valueOf (k.id);
	const bool active = (knobHot_ == k.id || dragId_ == k.id);
	GpPen* arc = nullptr;
	GdipCreatePen1 (active ? argb (kAccent) : argb (kAccent.r, kAccent.g, kAccent.b, 210),
	                 active ? 2.5f : 2.f, UnitPixel, &arc);
	GdipDrawArc (g, arc, cx - rad, cy - rad, rad * 2, rad * 2,
	             135.f, (float) (270.0 * v));
	GdipDeletePen (arc);

	// cap
	ellipse (g, brPanelHi_, (int) (cx - 13), (int) (cy - 13), 26, 26);

	// pointer
	const float ang = (float) (135.0 + 270.0 * v) * 3.14159265f / 180.f;
	const float px = cx + std::cos (ang) * 16.f;
	const float py = cy + std::sin (ang) * 16.f;
	line (g, penAccent_, (int) cx, (int) cy, (int) px, (int) py);

	// value
	char val[48];
	formatValue (k.id, v, val, sizeof (val));
	txt (g, brInk_, fValue_, val, k.r.x, k.r.y + 58, k.r.w, 16, 1);
}

void HanaGuitarEditor::drawCircuitSwitch (GpGraphics* g)
{
	const RectI& r = circuitSwitch_;
	const bool rhythm = valueOf (kCircuit) > 0.5;
	fillRound (g, brPanel_, r.x, r.y, r.w, r.h, 5);
	if (rhythm)
	{
		GpSolidFill* c = nullptr;
		GdipCreateSolidFill (argb (kAccent2, 60), &c);
		fillRound (g, c, r.x + 1, r.y + 1, r.w / 2 - 1, r.h - 2, 4);
		GdipDeleteBrush (c);
	}
	else
	{
		GpSolidFill* c = nullptr;
		GdipCreateSolidFill (argb (kAccent, 60), &c);
		fillRound (g, c, r.x + r.w / 2, r.y + 1, r.w / 2 - 1, r.h - 2, 4);
		GdipDeleteBrush (c);
	}
	txt (g, rhythm ? brDim_ : brAccent_, fTiny_, "LEAD",
	     r.x, r.y + r.h / 2 - 7, r.w / 2, 14, 1);
	txt (g, rhythm ? brAccent2_ : brDim_, fTiny_, "RHY",
	     r.x + r.w / 2, r.y + r.h / 2 - 7, r.w / 2, 14, 1);
}

void HanaGuitarEditor::drawLegend (GpGraphics* g)
{
	fillRect (g, brLine_, kX0, kLegendY, kRight - kX0, 1);
	txt (g, brDim_, fLabel_, "KEY SWITCHES  (C0..D#1, below the played range)", kX0, kLegendY + 4, 380, 14);

	struct Ent { const char* key; const char* what; };
	static const Ent ents[16] = {
		{ "C0",  "palm mute" },  { "C#0", "harmonics" }, { "D0", "tap" },      { "D#0", "choke" },
		{ "E0",  "pick bridge" }, { "F0",  "pick middle" }, { "F#0", "pick neck" }, { "G0",  "rhythm" },
		{ "G#0", "lead" },       { "A0",  "P90" },        { "A#0", "both" },     { "B0",  "humbucker" },
		{ "C1",  "swell" },      { "C#1", "trem picking" }, { "D1", "feedback" }, { "D#1", "reverse verb" },
	};

	const int colW = (kRight - kX0) / 4;
	for (int i = 0; i < 16; ++i)
	{
		const int col = i / 4;
		const int row = i % 4;
		const int x = kX0 + col * colW;
		const int y = kLegendY + 16 + row * 12;
		txt (g, brAccent_, fTiny_, ents[i].key, x, y, 28, 12);
		txt (g, brDim_, fTiny_, ents[i].what, x + 28, y, colW - 32, 12);
	}
}

void HanaGuitarEditor::drawFooter (GpGraphics* g)
{
	txt (g, brDim_, fTiny_, "BAR: drag readout / CC1 / pitch bend", kX0, kH - 14, 300, 12);
	txt (g, brDim_, fTiny_, "HanaGuitar 1.3", kRight - 120, kH - 14, 120, 12, 2);
}

// ---------------------------------------------------------------------------
// values / interaction
// ---------------------------------------------------------------------------
double HanaGuitarEditor::valueOf (int id) const
{
	return sink_ ? sink_->getNormalized (id) : 0.0;
}

void HanaGuitarEditor::formatValue (int id, double norm, char* out, int n)
{
	const ParamDef& d = kParamDefs[id];
	const float v = plainFromNorm (d, (float) norm);
	if (d.items && d.numItems > 0)
	{
		int idx = (int) (v + 0.5f);
		if (idx < 0) idx = 0;
		if (idx >= d.numItems) idx = d.numItems - 1;
		std::snprintf (out, n, "%s", d.items[idx]);
	}
	else if (d.unit && d.unit[0])
		std::snprintf (out, n, "%.1f %s", v, d.unit);
	else
		std::snprintf (out, n, "%.2f", v);
}

int HanaGuitarEditor::knobAt (int x, int y) const
{
	for (int i = 0; i < knobCount_; ++i)
	{
		const RectI& r = knobs[i].r;
		if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
			return knobs[i].id;
	}
	return -1;
}

void HanaGuitarEditor::beginDrag (int id, int x, int y)
{
	if (!sink_ || id < 0) return;
	dragId_ = id;
	dragY0_ = y;
	dragV0_ = valueOf (id);
	sink_->onBeginEdit (id);
	(void) x;
}

void HanaGuitarEditor::dragTo (int x, int y)
{
	if (dragId_ < 0 || !sink_) return;
	const double dy = (double) (dragY0_ - y);
	const double v = std::min (1.0, std::max (0.0, dragV0_ + dy / 150.0));
	setValue (dragId_, v, false);
	(void) x;
	if (hwnd_) InvalidateRect (hwnd_, nullptr, FALSE);
}

void HanaGuitarEditor::endDrag ()
{
	if (dragId_ >= 0 && sink_) sink_->onEndEdit (dragId_);
	dragId_ = -1;
}

void HanaGuitarEditor::nudge (int id, double delta)
{
	if (id < 0 || !sink_) return;
	const ParamDef& d = kParamDefs[id];
	const double step = (d.steps > 0) ? 1.0 : 0.01;
	sink_->onBeginEdit (id);
	sink_->onPerformEdit (id, std::min (1.0, std::max (0.0, valueOf (id) + delta * step)));
	sink_->onEndEdit (id);
}

void HanaGuitarEditor::cycleDiscrete (int id, int dir)
{
	if (id < 0 || !sink_) return;
	const ParamDef& d = kParamDefs[id];
	if (d.steps <= 0) return;
	const int cur = (int) (valueOf (id) * d.steps + 0.5);
	int next = cur + dir;
	if (next < 0) next = d.steps;
	if (next > d.steps) next = 0;
	sink_->onBeginEdit (id);
	sink_->onPerformEdit (id, (double) normFromPlain (d, (float) next));
	sink_->onEndEdit (id);
}

void HanaGuitarEditor::setValue (int id, double norm, bool gesture)
{
	if (!sink_) return;
	if (gesture) sink_->onBeginEdit (id);
	const ParamDef& d = kParamDefs[id];
	if (d.steps > 0)
		norm = (double) normFromPlain (d, (float) plainFromNorm (d, (float) norm));
	sink_->onPerformEdit (id, norm);
	if (gesture) sink_->onEndEdit (id);
}

void HanaGuitarEditor::toggleCircuit ()
{
	if (!sink_) return;
	sink_->onBeginEdit (kCircuit);
	sink_->onPerformEdit (kCircuit, valueOf (kCircuit) > 0.5 ? 0.0 : 1.0);
	sink_->onEndEdit (kCircuit);
}

void HanaGuitarEditor::applyPreset (int idx)
{
	if (!sink_ || idx < 0 || idx >= 4) return;
	for (int i = 0; i < (int) kNumKnobs; ++i)
		setValue (i, (double) normFromPlain (kParamDefs[i], kParamDefs[i].def), true);

	const PresetDef& p = kPresets[idx];
	for (int i = 0; i < p.n; ++i)
		setValue (p.kv[i].id, (double) normFromPlain (kParamDefs[p.kv[i].id], p.kv[i].plain), true);

	setValue (kPreset, (double) idx / 3.0, true);
	presetIdx_ = idx;
}

int HanaGuitarEditor::presetIndex () const
{
	if (!sink_) return presetIdx_;
	const double n = sink_->getNormalized (kPreset);
	int i = (int) (n * 3.0 + 0.5);
	return std::min (3, std::max (0, i));
}

// ---------------------------------------------------------------------------
// window proc
// ---------------------------------------------------------------------------
LRESULT HanaGuitarEditor::wndProc (HWND h, UINT m, WPARAM w, LPARAM l)
{
	switch (m)
	{
	case WM_ERASEBKGND:
		return 1;

	case WM_PRINTCLIENT:
		paint ((HDC) w);
		return 0;

	case WM_SIZE:
		InvalidateRect (h, nullptr, FALSE);
		return 0;

	case WM_SHOWWINDOW:
		if (w) { syncToParent (); InvalidateRect (h, nullptr, TRUE); }
		return 0;

	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC dc = BeginPaint (h, &ps);
		paint (dc);
		EndPaint (h, &ps);
		return 0;
	}

	case WM_APP_PARAM:
		if (sink_)
		{
			peakShow_ = sink_->takePeak ();
			techMask_ = sink_->techniqueMask ();
			barShow_ = sink_->barSemitones ();
		}
		InvalidateRect (h, nullptr, FALSE);
		return 0;

	case WM_TIMER:
		syncToParent ();
		{
			RECT rc = {}, prc = {};
			GetClientRect (h, &rc);
			if ((rc.right < 8 || rc.bottom < 8) && parent_ && GetClientRect (parent_, &prc) &&
			    prc.right > 16 && prc.bottom > 16)
			{
				MoveWindow (h, 0, 0, prc.right, prc.bottom, TRUE);
				layoutToWindow ((int) prc.right, (int) prc.bottom);
				InvalidateRect (h, nullptr, TRUE);
				uiLog ("adopted-parent", (int) prc.right, (int) prc.bottom);
			}
		}
		if (sink_)
		{
			peakShow_ = std::max (peakShow_ * 0.80f, sink_->takePeak ());
			techMask_ = sink_->techniqueMask ();
			barShow_ = sink_->barSemitones ();
		}
		InvalidateRect (h, nullptr, FALSE);
		return 0;

	case WM_MOUSEMOVE:
	{
		int dx, dy;
		designFromWindow (GET_X_LPARAM (l), GET_Y_LPARAM (l), dx, dy);
		if (dragId_ >= 0)
		{
			dragTo (dx, dy);
			return 0;
		}
		const int id = knobAt (dx, dy);
		if (id != knobHot_) { knobHot_ = id; InvalidateRect (h, nullptr, FALSE); }

		int hot = -1;
		for (int i = 0; i < 4; ++i) if (presetBtn_[i].has (dx, dy)) hot = i;
		if (hot != presetHot_) { presetHot_ = hot; InvalidateRect (h, nullptr, FALSE); }

		if (dx < kImageW) SetCursor (LoadCursorW (nullptr, IDC_ARROW));
		return 0;
	}

	case WM_LBUTTONDOWN:
	{
		int dx, dy;
		designFromWindow (GET_X_LPARAM (l), GET_Y_LPARAM (l), dx, dy);
		SetCapture (h);

		for (int i = 0; i < 4; ++i)
			if (presetBtn_[i].has (dx, dy)) { applyPreset (i); return 0; }

		if (circuitSwitch_.has (dx, dy)) { toggleCircuit (); return 0; }

		if (barRect_.has (dx, dy)) { beginDrag (jm::kBar, dx, dy); return 0; }

		const int id = knobAt (dx, dy);
		if (id >= 0)
		{
			if (kParamDefs[id].steps > 0)
			{
				// discrete knobs: click cycles forward, right click back
				cycleDiscrete (id, 1);
				return 0;
			}
			beginDrag (id, dx, dy);
			InvalidateRect (h, nullptr, FALSE);
		}
		return 0;
	}

	case WM_LBUTTONUP:
		if (GetCapture () == h) ReleaseCapture ();
		endDrag ();
		InvalidateRect (h, nullptr, FALSE);
		return 0;

	case WM_RBUTTONDOWN:
	{
		int dx, dy;
		designFromWindow (GET_X_LPARAM (l), GET_Y_LPARAM (l), dx, dy);
		const int id = knobAt (dx, dy);
		if (id >= 0 && kParamDefs[id].steps > 0) cycleDiscrete (id, -1);
		return 0;
	}

	case WM_LBUTTONDBLCLK:
	{
		int dx, dy;
		designFromWindow (GET_X_LPARAM (l), GET_Y_LPARAM (l), dx, dy);
		const int id = knobAt (dx, dy);
		if (barRect_.has (dx, dy))
			setValue (jm::kBar, 0.0, true);
		else if (id >= 0)
			setValue (id, (double) normFromPlain (kParamDefs[id], kParamDefs[id].def), true);
		InvalidateRect (h, nullptr, FALSE);
		return 0;
	}

	case WM_MOUSEWHEEL:
	{
		POINT pt{ GET_X_LPARAM (l), GET_Y_LPARAM (l) };
		ScreenToClient (h, &pt);
		int dx, dy;
		designFromWindow (pt.x, pt.y, dx, dy);
		const int id = knobAt (dx, dy);
		if (id >= 0)
		{
			const int dir = (GET_WHEEL_DELTA_WPARAM (w) > 0) ? 1 : -1;
			if (kParamDefs[id].steps > 0) cycleDiscrete (id, dir);
			else nudge (id, dir * ((GetKeyState (VK_SHIFT) & 0x8000) ? 0.001 : 0.02));
		}
		return 0;
	}

	case WM_SETCURSOR:
		return DefWindowProcW (h, m, w, l);

	default:
		return DefWindowProcW (h, m, w, l);
	}
}


} // namespace

// Exported at namespace scope: the plugin calls this by name.
void notifyEditorParamChanged (IPlugView* view, int)
{
	if (!view) return;
	if (HWND h = viewMap ().find (view)) PostMessageW (h, WM_APP_PARAM, 0, 0);
}

IPlugView* createHanaGuitarEditor (IParamSink* sink)
{
	return new HanaGuitarEditor (sink);
}

} // namespace jm
