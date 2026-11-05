// peek.cpp - the Peek tweak: hold the mouse button on a link for 400 ms (or click it with the chosen
// keys) to preview it in a card over the page, without opening a tab. The look and motion follow Zen
// Browser's Glance (src/zen/glance/ZenGlanceManager.mjs): the card flies out of the link along a slight arc
// with a small overshoot (350 ms), the page behind darkens, buttons sit beside the card's top right corner
// (close, open as tab), and closing flies the page back into the link.
//
// The preview is Dia's own page: Dia's engine, your profile, sign-ins and extensions.
//   * Dia opens the link the way it does for Shift+click (in a new window). That window is caught as Dia
//     creates it (a CBT hook on the UI thread: all of Dia's windows live on it). It is never seen until its
//     page is ready: made far below the screen (WinUI draws a default frame for a new window, whatever
//     its region), clipped to an empty window region, without frame or shadow, and the hiding is put back
//     after every message WinUI handles for it (a thread hook) and whenever it's shown. It's held at the
//     size that puts its page exactly on the card, kept from maximizing and resizing.
//   * Dia draws a page inside its own WinUI window (its Chromium windows only take input: the pixels are
//     composed into Dia's window), so the page can't be moved out: the window is the preview. Once its page
//     has its title, the window comes onto the screen (still clipped to nothing, so Dia draws the page),
//     and a moment later it's shown on the card: clipped to the page with rounded corners (its sidebar,
//     toolbar and window buttons cut away), owned by the backdrop (above it, no taskbar button).
//   * The animation (backdrop, card, buttons) runs on a thread of its own, paced to the display: Dia's UI
//     thread is busy making the window and page meanwhile and would make it stutter. When the preview
//     closes, a picture of its page goes on the card, Dia's window is hidden and the card flies back into
//     the link; Dia's window (and its tab) is closed after that.
// Close: Esc, the close button, a click outside. Keep ("open as tab", as Zen's): the preview stays where it
// is (a picture of it), its tab moves into the window you're in as it is, laid out at the page's size
// first (Dia's "Move to Window", from its tab menu, under the picture), and the picture and the dim fade,
// showing the tab full size.
//
// A link is told from the rest by Dia's own hover text: the address it shows at the bottom left of the
// window while the pointer is over a link (read from its XAML by ui.cpp). Dia sets it the moment the
// pointer reaches a link (the bubble fades in later) and clears it when the pointer leaves. (Chromium's
// accessibility can't be used: Dia's pages give it an empty tree.)
// Hold: a plain press on the page is held back while that check runs. Not on a link: it goes straight on
// to the page. On a link: it waits; released or moved past the drag distance first, it goes on to the page
// (a click, a drag, a selection); still down after 400 ms: the preview opens, and since the page never saw
// the press, the link isn't followed.

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <d3d11.h>
#include <dcomp.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "engine.h"

#define Log Engine_Log

using Microsoft::WRL::ComPtr;

namespace {

// Messages to the UI thread's message window.
const UINT WM_PEEK_CLICK = WM_APP + 0x4a1, WM_PEEK_ESC = WM_APP + 0x4a3;
const UINT WM_PEEK_PRESS = WM_APP + 0x4a5, WM_PEEK_REPLAY = WM_APP + 0x4a6;
const UINT WM_PEEK_DISMISS = WM_APP + 0x4a7;  // wp: 1 = keep (open as tab)
const UINT WM_PEEK_CARD = WM_APP + 0x4a8;     // the card has flown out: the page may show
const UINT WM_PEEK_DONE = WM_APP + 0x4a9;     // the card has flown back (wp 0) or filled the page (wp 1)
const UINT WM_PEEK_REVEAL = WM_APP + 0x4aa;   // "open as tab": the card has (almost) filled the page
// Messages to the animation thread's window.
const UINT GM_OPEN = WM_APP + 0x4c1, GM_SHOWN = WM_APP + 0x4c2, GM_CLOSE = WM_APP + 0x4c3, GM_MOVE = WM_APP + 0x4c4,
           GM_END = WM_APP + 0x4c5, GM_FADE = WM_APP + 0x4c6, GM_REVEALED = WM_APP + 0x4c7,
           GM_PATCH = WM_APP + 0x4c8, GM_COVER = WM_APP + 0x4c9;

const UINT_PTR kTimerHold = 4, kTimerDia = 5, kTimerInject = 6, kTimerPress = 7, kTimerTab = 8;
const int kFadeMs = 90;   // "open as tab": the card goes once the tab is under it (the same picture)
const int kGrowMs = 300;  // "open as tab": the card grows to the page (as Arc's Peek)
const int kRevealMs = 140;  // "open as tab": the card fades off the live page under it
const int kSwapMs = 160;  // "open as tab": the page at your page's size blends in over the preview's picture
const ULONGLONG kHoverTextMs = 120;  // a press right after a move: how long Dia's hover text may take
const UINT kHoldMs = 400;
const ULONGLONG kDiaTimeoutMs = 5000;
const ULONGLONG kNoWindowMs = 1500;  // Dia opened no window for the click (not a link after all)
const ULONGLONG kSettleMs = 100, kRevealByMs = 2500;  // page in place this long before it's shown; at the latest
const int kParkY = 30000;  // Dia's window is made this far below the screen (see above)
const ULONG_PTR kReplayTag = 0x4454504B;  // our own injected input ("DTPK"), which the hooks let through

// Zen's Glance.
const double kCardWidth = 0.85;   // of the page width (Zen: 80% of the tab area)
const double kMarginDip = 40;     // above and below the card
const int kGlanceMs = 350;        // zen.glance.animation-duration
const int kBackCloseMs = 233;     // the page behind comes back faster (duration / 1.5)
const double kDim = 0.55;         // the page behind darkens (Zen fades it to 30%)
const double kArcRatio = 0.4, kMaxArcDip = 30;  // the path bows: 40% of the distance, at most 30 px
const double kButtonDip = 36, kButtonGapDip = 8;
const double kLinkWDip = 80, kLinkHDip = 20;  // the card starts as a link-sized box at the link
const double kCloseScale = 0.12;  // closing: the card shrinks to this much of its size at the link, fading out

enum Phase { kLoading, kOpen, kClosing, kExpanding };

struct Peek {
    HWND dia = nullptr, page = nullptr;
    HWND backdrop = nullptr;  // (the animation thread's: Dia's window is owned by it)
    RECT target{};            // where the preview's page goes, screen pixels
    RECT pageArea{};          // the page you're in
    POINT origin{};
    std::wstring url;
    Phase phase = kLoading;
    bool destroying = false;
    bool cardDone = false;  // the card has flown out
    bool ready = false;     // Dia's page is in place, waiting for the card
    // Dia's side
    ULONGLONG started = 0;   // when the link was handed to Dia
    int injectStep = 0;      // Shift+click sent in steps: 1 = Shift down, 2 = clicked, 3 = Shift up
    HWND win = nullptr;      // the window Dia opened for the link
    HWND widget = nullptr;   // its page's Chromium window, once shown
    int aligns = 0;          // tries to land its page on the target
    bool pageSeen = false;
    bool parked = true;      // below the screen (see kParkY)
    ULONGLONG placedAt = 0;  // when its page landed on the target
};

enum Trigger { kHold, kShift, kCtrlShift, kAlt };
std::atomic<int> g_trigger{kHold};
enum HoldState { kIdle, kChecking, kOnLink };
std::atomic<int> g_hold{kIdle};
std::atomic<bool> g_holdFired{false};
POINT g_holdPt{};
HWND g_holdWidget = nullptr;
std::wstring g_holdUrl;  // the link held on
std::atomic<bool> g_shift{false}, g_ctrl{false}, g_alt{false}, g_win{false};

DWORD g_thread = 0;  // the UI thread that owns the hooks and the preview
HWND g_msg = nullptr;
HHOOK g_mouse = nullptr, g_keys = nullptr, g_cbt = nullptr, g_after = nullptr;
HWINEVENTHOOK g_showHook = nullptr;
std::atomic<bool> g_on{false};
std::atomic<bool> g_swallowUp{false}, g_upSwallowed{false};
POINT g_clickPt{};
HWND g_clickWidget = nullptr;
std::atomic<ULONGLONG> g_lastMove{0};  // when the pointer last moved

Peek* g_peek = nullptr;
// Where the page sat in the last Dia window shown: from its left and top edges, and how much smaller than
// the window it was. A new window is made that much bigger than the card, so its page lands on the card at
// once (no second layout).
struct { int dx = 0, dy = 0, dw = 0, dh = 0; } g_offset;

// ---------------------------------------------------------------- helpers

bool ClassIs(HWND h, const wchar_t* name) {
    wchar_t cls[64];
    return h && GetClassNameW(h, cls, 64) && wcscmp(cls, name) == 0;
}

bool OurWindow(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid == GetCurrentProcessId();
}

bool DiaIsForeground() { return OurWindow(GetForegroundWindow()); }

// Dia's page window (Chrome_RenderWidgetHostHWND) under a screen point, or null. The click lands on
// Dia's WinUI window (Microsoft.UI.Content.DesktopChildSiteBridge, which passes it on) and the GPU
// process lays "Intermediate D3D Window" over the page, so this looks for a page window whose area holds
// the point inside the Dia window at the point.
HWND PageWidgetAt(POINT pt) {
    HWND h = WindowFromPoint(pt);
    if (!h) return nullptr;
    if (ClassIs(h, L"Chrome_RenderWidgetHostHWND")) return OurWindow(h) ? h : nullptr;
    HWND root = GetAncestor(h, GA_ROOT);
    if (!root || !OurWindow(root) || !ClassIs(root, L"WinUIDesktopWin32WindowClass")) return nullptr;
    struct Ctx { POINT pt; HWND found; } ctx{pt, nullptr};
    EnumChildWindows(root, [](HWND c, LPARAM lp) -> BOOL {
        auto& x = *reinterpret_cast<Ctx*>(lp);
        RECT r;
        if (ClassIs(c, L"Chrome_RenderWidgetHostHWND") && IsWindowVisible(c) && GetWindowRect(c, &r) &&
            PtInRect(&r, x.pt)) {
            x.found = c;
            return FALSE;
        }
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.found;
}

bool StartsWithI(const std::wstring& s, const wchar_t* prefix) {
    size_t n = wcslen(prefix);
    return s.size() >= n && _wcsnicmp(s.c_str(), prefix, n) == 0;
}

std::wstring HostOf(const std::wstring& url) {
    size_t p = url.find(L"://");
    if (p == std::wstring::npos) return url;
    std::wstring rest = url.substr(p + 3);
    rest = rest.substr(0, rest.find_first_of(L"/?#"));
    if (size_t at = rest.rfind(L'@'); at != std::wstring::npos) rest = rest.substr(at + 1);
    if (StartsWithI(rest, L"www.")) rest = rest.substr(4);
    return rest;
}

// Dia's hover text names an address (not, say, a loading message).
bool LooksLikeAddress(const std::wstring& t) {
    if (t.empty() || t.find(L' ') != std::wstring::npos) return false;
    return t.find(L'.') != std::wstring::npos || t.find(L"://") != std::wstring::npos;
}

// The link the pointer is on in a page window's Dia window, from Dia's hover text, or empty.
std::wstring HoveredLink(HWND widget) {
    if (!IsWindow(widget)) return L"";
    std::wstring t = Ui_HoveredLink(GetAncestor(widget, GA_ROOT));
    if (!LooksLikeAddress(t)) return L"";
    return t.find(L"://") == std::wstring::npos ? L"https://" + t : t;
}

bool LightTheme() {
    DWORD v = 1, size = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v != 0;
}

COLORREF CardColor() { return LightTheme() ? RGB(243, 243, 243) : RGB(32, 32, 32); }
COLORREF TextColor() { return LightTheme() ? RGB(26, 26, 26) : RGB(255, 255, 255); }
COLORREF HoverColor() { return LightTheme() ? RGB(226, 226, 226) : RGB(52, 52, 52); }

bool ReduceMotion() {
    BOOL anim = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
    return !anim;
}

double Scale(HWND h) { return (h ? GetDpiForWindow(h) : 96) / 96.0; }

// Where the card goes over a page window (screen pixels), or false if the page is too small.
bool CardRectFor(HWND page, HWND dia, RECT& out, RECT* pageOut = nullptr) {
    RECT pr;
    if (!GetWindowRect(page, &pr)) return false;
    double s = Scale(dia);
    int w = pr.right - pr.left, h = pr.bottom - pr.top;
    if (w < 320 * s || h < 240 * s) return false;
    // DiaTweaks': 9% in from each side, 1.2% (at least 10) from the top and bottom.
    int dx = (int)std::lround(w * 0.09), dy = (int)std::lround((std::max)(10 * s, h * 0.012));
    out.left = pr.left + dx;
    out.right = pr.right - dx;
    out.top = pr.top + dy;
    out.bottom = pr.bottom - dy;
    if (pageOut) *pageOut = pr;
    return true;
}

void SendShift(bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = VK_SHIFT;
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
}

// ================================================================ the animation thread
// Backdrop, card, controls and the motion live here; the UI thread sends it what to do (GM_*) and hears
// back when the card is out (WM_PEEK_CARD), when a control or the backdrop was clicked (WM_PEEK_DISMISS),
// when the live page may show for "open as tab" (WM_PEEK_REVEAL) and when it's done (WM_PEEK_DONE).
//
// The motion follows DiaTweaks' Peek for macOS (JaceThings/DiaTweaks#130), itself after iOS's app open:
//   * Springs, critically damped, response 0.24 s at 90% speed. The dim rides a slower one (x 0.314/0.24).
//   * Open and close: the "genie". The card's top, middle and bottom rows each ride their own spring between
//     the link and the card (the row farther from the link leads out, the nearer one leads back in), corners
//     4 -> 10, the shadow growing with it; closing fades over the last 40%.
//   * Open as tab: the card grows to the page on the same spring, the dim going; the page in it is live.
//   * Controls: dark circles right of the card that pop in (spring 0.32 s, damping 0.7, 50 ms apart),
//     grow 12% under the pointer, and after a second show a hint with the shortcut.
// It's all handed to Windows' compositor (DirectComposition) as animation curves, which DWM plays at the
// display's rate whatever this thread or Dia are doing; this thread only keeps time.

struct GlanceOpen {
    HWND owner;  // the Dia window you're in
    RECT pageArea, target, link;
    double scale;
    HWND backdrop;  // (back to the UI thread)
};

struct GlanceClose {
    HBITMAP picture;  // the page, at the target's size (or null: the card's color)
    bool expand;      // fill the page instead of flying back
    RECT area;        // where things may show (Dia's window), screen pixels
    HBITMAP behind;   // expand: your page as it is under the dim (pageArea), or null
};

struct GlancePatch {  // a picture over a screen rectangle (null bitmap: none)
    HBITMAP bmp;
    RECT r;
};

enum GPhase { gIdle, gOpening, gOpen, gClosing, gHolding, gGrowing, gHeld, gFading };

const double kResponse = 0.24 / 0.9;          // seconds (iOS's 0.24 at DiaTweaks' default speed, 90%)
const double kDimResponse = kResponse * 0.314 / 0.24;
const double kDarkness = 0.45;
const double kCornerDip = 10, kCornerFromDip = 4;
const double kShadowBlurDip = 24, kShadowYDip = 6, kShadowAlpha = 0.35;
const int kStrips = 32;  // the genie's slices

struct Glance {
    HWND ctl = nullptr;  // this thread's message window
    HWND backdrop = nullptr, card = nullptr, buttons[3]{}, hint = nullptr;
    RECT pageArea{}, target{}, link{};
    double scale = 1;
    GPhase phase = gIdle;
    double start = 0, dur = 0;  // seconds (Now)
    double dimFrom = 0, dimTo = 0, dimStart = 0;
    bool fx = false;
    bool revealed = false;  // expand: the live page is under the card
    // controls
    double controlsAt = -1;  // when they started popping in (-1: hidden)
    int hover = -1, pressed = -1;
    double hoverAt[3]{-1, -1, -1}, hoverFrom[3]{1, 1, 1}, hoverScale[3]{1, 1, 1};
    int hintFor = -1;
    double hintAt = 0;
} G;

HWND g_ctl = nullptr;  // the animation thread's message window (for the UI thread)

double Now() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER v; QueryPerformanceFrequency(&v); return v; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / f.QuadPart;
}
double NowMs() { return Now() * 1000; }

double Clamp01(double v) { return (std::max)(0.0, (std::min)(1.0, v)); }
double EaseInOut(double t) { return t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2; }
double EaseOut(double t) { return 1 - (1 - t) * (1 - t); }
double Lerp(double a, double b, double t) { return a + (b - a) * t; }

// How far a critically damped spring with this response has left to go after t seconds (1 -> 0).
double Remaining(double t, double response) {
    if (t <= 0) return 1;
    double w = 2 * 3.14159265358979 / response;
    return (1 + w * t) * std::exp(-w * t);
}
// About when it has settled (within 0.1%), as Core Animation's settlingDuration.
double Settling(double response) { return 9.23 * response / (2 * 3.14159265358979); }
// An underdamped spring from 0 to 1 (the controls' pop).
double SpringTo1(double t, double response, double damping) {
    if (t <= 0) return 0;
    double w = 2 * 3.14159265358979 / response, wd = w * std::sqrt(1 - damping * damping);
    return 1 - std::exp(-damping * w * t) * (std::cos(wd * t) + damping * w / wd * std::sin(wd * t));
}

struct DRect {
    double l, t, r, b;
    double w() const { return r - l; }
    double h() const { return b - t; }
};
DRect D(const RECT& r) { return {(double)r.left, (double)r.top, (double)r.right, (double)r.bottom}; }
RECT Round(const DRect& d) {
    return {(LONG)std::lround(d.l), (LONG)std::lround(d.t), (LONG)std::lround(d.r), (LONG)std::lround(d.b)};
}
DRect LerpRect(const DRect& a, const DRect& b, double t) {
    return {Lerp(a.l, b.l, t), Lerp(a.t, b.t, t), Lerp(a.r, b.r, t), Lerp(a.b, b.b, t)};
}

void SetDimAlpha(double a) {
    if (G.backdrop) SetLayeredWindowAttributes(G.backdrop, 0, (BYTE)std::lround(255 * kDarkness * Clamp01(a)), LWA_ALPHA);
}
double DimNow() {
    double e = 1 - Remaining(Now() - G.dimStart, kDimResponse);
    return Lerp(G.dimFrom, G.dimTo, e);
}
void DimTo(double to) {
    G.dimFrom = DimNow();
    G.dimTo = to;
    G.dimStart = Now();
}

// ---------------------------------------------------------------- the compositor (DirectComposition)

struct Fx {
    ComPtr<ID3D11Device> d3d;
    ComPtr<IDCompositionDevice> dev;
    HWND win = nullptr;
    POINT origin{};  // the window's top left, screen pixels
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> root, scene, cover, patch;
    ComPtr<IDCompositionEffectGroup> coverFx;
    ComPtr<IDCompositionSurface> behind;  // expand: your page, for around the growing card
    SIZE behindSize{};
} X;

LRESULT CALLBACK FxProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, msg, wp, lp);
}

ComPtr<IDCompositionSurface> FxSurface(const UINT* px, int w, int h, bool alpha = false) {
    ComPtr<IDCompositionSurface> s;
    if (!X.dev || w <= 0 || h <= 0 ||
        FAILED(X.dev->CreateSurface(w, h, DXGI_FORMAT_B8G8R8A8_UNORM,
                                    alpha ? DXGI_ALPHA_MODE_PREMULTIPLIED : DXGI_ALPHA_MODE_IGNORE, &s)))
        return nullptr;
    ComPtr<ID3D11Texture2D> tex;
    POINT off{};
    if (FAILED(s->BeginDraw(nullptr, IID_PPV_ARGS(&tex), &off))) return nullptr;
    ComPtr<ID3D11DeviceContext> ctx;
    X.d3d->GetImmediateContext(&ctx);
    D3D11_BOX box{(UINT)off.x, (UINT)off.y, 0, (UINT)(off.x + w), (UINT)(off.y + h), 1};
    ctx->UpdateSubresource(tex.Get(), 0, &box, px, w * 4, 0);
    s->EndDraw();
    return s;
}

ComPtr<IDCompositionSurface> FxSurfaceOf(HBITMAP bmp, SIZE& size) {
    BITMAP bm{};
    if (!bmp || !GetObject(bmp, sizeof(bm), &bm)) return nullptr;
    int w = bm.bmWidth, h = bm.bmHeight;
    std::vector<UINT> px((size_t)w * h, 0);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    HDC dc = GetDC(nullptr);
    int got = GetDIBits(dc, bmp, 0, h, px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (!got) return nullptr;
    size = {w, h};
    return FxSurface(px.data(), w, h);
}

ComPtr<IDCompositionSurface> FxColor(COLORREF c) {
    UINT px = 0xFF000000 | (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c);
    return FxSurface(&px, 1, 1);
}

// A soft shadow for a rounded box of this size (scaled down: it's blurred anyway), with its margin.
struct Shadow {
    ComPtr<IDCompositionSurface> surface;
    int w = 0, h = 0;      // the bitmap
    double margin = 0;     // screen pixels around the box
    double down = 1;       // bitmap pixels per screen pixel
};

Shadow MakeShadow(double boxW, double boxH, double radius) {
    Shadow s;
    s.down = 0.25;
    s.margin = kShadowBlurDip * G.scale * 1.5;
    int bw = (std::max)(4, (int)std::lround(boxW * s.down)), bh = (std::max)(4, (int)std::lround(boxH * s.down));
    int m = (int)std::lround(s.margin * s.down);
    s.w = bw + 2 * m, s.h = bh + 2 * m;
    std::vector<float> a((size_t)s.w * s.h, 0), b(a.size());
    double r = radius * s.down;
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) {
            double dx = (std::max)({0.0, r - x - 0.5, x + 0.5 - (bw - r)}), dy = (std::max)({0.0, r - y - 0.5, y + 0.5 - (bh - r)});
            double d = std::sqrt(dx * dx + dy * dy);
            a[(size_t)(y + m) * s.w + x + m] = (float)Clamp01(r - d + 0.5 + (r <= 0 ? 1 : 0));
        }
    // Three box blurs ~ a gaussian (NSShadow's blur radius is about two sigmas).
    int k = (std::max)(1, (int)std::lround(kShadowBlurDip * G.scale * s.down / 2 * 0.9));
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < s.h; y++) {  // horizontal
            float acc = 0;
            for (int x = -k; x <= k; x++) acc += a[(size_t)y * s.w + (std::max)(0, (std::min)(s.w - 1, x))];
            for (int x = 0; x < s.w; x++) {
                b[(size_t)y * s.w + x] = acc / (2 * k + 1);
                acc += a[(size_t)y * s.w + (std::min)(s.w - 1, x + k + 1)] - a[(size_t)y * s.w + (std::max)(0, x - k)];
            }
        }
        for (int x = 0; x < s.w; x++) {  // vertical
            float acc = 0;
            for (int y = -k; y <= k; y++) acc += b[(size_t)(std::max)(0, (std::min)(s.h - 1, y)) * s.w + x];
            for (int y = 0; y < s.h; y++) {
                a[(size_t)y * s.w + x] = acc / (2 * k + 1);
                acc += b[(size_t)(std::min)(s.h - 1, y + k + 1) * s.w + x] - b[(size_t)(std::max)(0, y - k) * s.w + x];
            }
        }
    }
    std::vector<UINT> px(a.size());
    for (size_t i = 0; i < a.size(); i++) px[i] = (UINT)std::lround(Clamp01(a[i]) * kShadowAlpha * 255) << 24;
    s.surface = FxSurface(px.data(), s.w, s.h, true);
    return s;
}

// A value's course over dur seconds as a DWM animation curve: f(t), t in seconds, followed by cubic pieces.
ComPtr<IDCompositionAnimation> Curve(double dur, const std::function<double(double)>& f) {
    ComPtr<IDCompositionAnimation> a;
    if (!X.dev || FAILED(X.dev->CreateAnimation(&a))) return nullptr;
    if (dur <= 0) {
        a->AddCubic(0, (float)f(0), 0, 0, 0);
        return a;
    }
    int n = (std::max)(24, (int)std::ceil(dur * 120));
    double h = dur / n;
    auto slope = [&](double t) {
        double e = h / 8, t0 = (std::max)(0.0, t - e), t1 = (std::min)(dur, t + e);
        return (f(t1) - f(t0)) / (t1 - t0);
    };
    for (int i = 0; i < n; i++) {
        double t0 = i * h, t1 = t0 + h;
        double p0 = f(t0), p1 = f(t1), m0 = slope(t0), m1 = slope(t1);
        double c = (3 * (p1 - p0) / h - 2 * m0 - m1) / h, d = (2 * (p0 - p1) / h + m0 + m1) / (h * h);
        a->AddCubic(t0, (float)p0, (float)m0, (float)c, (float)d);
    }
    a->End(dur, (float)f(dur));
    return a;
}

void FxEnd() {
    if (X.win) DestroyWindow(X.win);
    X.win = nullptr;
    X.target.Reset();
    X.root.Reset(), X.scene.Reset(), X.cover.Reset(), X.patch.Reset(), X.coverFx.Reset(), X.behind.Reset();
    if (X.dev) X.dev->Commit();
}

// The overlay over Dia's window (shown with FxShow). False: no GPU composition here.
bool FxBegin(const RECT& area) {
    FxEnd();
    if (!X.dev) {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     nullptr, 0, D3D11_SDK_VERSION, &X.d3d, nullptr, nullptr)))
            return false;
        ComPtr<IDXGIDevice> dx;
        if (FAILED(X.d3d.As(&dx)) || FAILED(DCompositionCreateDevice(dx.Get(), IID_PPV_ARGS(&X.dev)))) {
            X.d3d.Reset();
            return false;
        }
    }
    X.origin = {area.left, area.top};
    X.win = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW |
                                WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                            L"DiaTweaksPeekFx", L"", WS_POPUP, area.left, area.top, area.right - area.left,
                            area.bottom - area.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    bool ok = X.win && SUCCEEDED(X.dev->CreateTargetForHwnd(X.win, TRUE, &X.target)) &&
              SUCCEEDED(X.dev->CreateVisual(&X.root)) && SUCCEEDED(X.dev->CreateVisual(&X.scene)) &&
              SUCCEEDED(X.dev->CreateVisual(&X.cover)) && SUCCEEDED(X.dev->CreateVisual(&X.patch)) &&
              SUCCEEDED(X.dev->CreateEffectGroup(&X.coverFx));
    if (ok) {
        X.cover->SetEffect(X.coverFx.Get());
        X.root->AddVisual(X.scene.Get(), FALSE, nullptr);
        X.root->AddVisual(X.cover.Get(), FALSE, nullptr);
        X.root->AddVisual(X.patch.Get(), FALSE, nullptr);
        ok = SUCCEEDED(X.target->SetRoot(X.root.Get()));
    }
    if (!ok) {
        FxEnd();
        X.dev.Reset(), X.d3d.Reset();  // (made again next time: it may have been lost)
        return false;
    }
    return true;
}

// Commits; with wait, returns once it's on the screen.
void FxCommit(bool wait = false) {
    if (!X.dev) return;
    X.dev->Commit();
    if (wait) {
        X.dev->WaitForCommitCompletion();
        DwmFlush();
    }
}

void FxShow() {
    FxCommit(true);
    ShowWindow(X.win, SW_SHOWNOACTIVATE);
    DwmFlush();
}

void FxClearScene() {
    if (X.scene) X.scene->RemoveAllVisuals();
}

ComPtr<IDCompositionVisual> NewVisual() {
    ComPtr<IDCompositionVisual> v;
    X.dev->CreateVisual(&v);
    return v;
}

// A picture (or a color, stretched) placed by animated scale and offset to fill rect(t), in overlay space.
void PlaceContent(IDCompositionVisual* v, double dur, double cw, double ch, const std::function<DRect(double)>& rect) {
    ComPtr<IDCompositionScaleTransform> scale;
    ComPtr<IDCompositionTranslateTransform> move;
    ComPtr<IDCompositionTransform> group;
    X.dev->CreateScaleTransform(&scale);
    X.dev->CreateTranslateTransform(&move);
    double ox = X.origin.x, oy = X.origin.y;
    if (auto a = Curve(dur, [=](double t) { return rect(t).w() / cw; })) scale->SetScaleX(a.Get());
    if (auto a = Curve(dur, [=](double t) { return rect(t).h() / ch; })) scale->SetScaleY(a.Get());
    if (auto a = Curve(dur, [=](double t) { return rect(t).l - ox; })) move->SetOffsetX(a.Get());
    if (auto a = Curve(dur, [=](double t) { return rect(t).t - oy; })) move->SetOffsetY(a.Get());
    IDCompositionTransform* steps[] = {scale.Get(), move.Get()};
    X.dev->CreateTransformGroup(steps, 2, &group);
    v->SetTransform(group.Get());
}

// A rectangle clip following rect(t) (overlay space), with these corner radii (screen pixels) over time.
ComPtr<IDCompositionRectangleClip> ClipTo(double dur, const std::function<DRect(double)>& rect,
                                          const std::function<double(double)>& radius, bool top = true,
                                          bool bottom = true) {
    ComPtr<IDCompositionRectangleClip> c;
    X.dev->CreateRectangleClip(&c);
    double ox = X.origin.x, oy = X.origin.y;
    if (auto a = Curve(dur, [=](double t) { return rect(t).l - ox; })) c->SetLeft(a.Get());
    if (auto a = Curve(dur, [=](double t) { return rect(t).t - oy; })) c->SetTop(a.Get());
    if (auto a = Curve(dur, [=](double t) { return rect(t).r - ox; })) c->SetRight(a.Get());
    if (auto a = Curve(dur, [=](double t) { return rect(t).b - oy; })) c->SetBottom(a.Get());
    if (radius) {
        auto rad = [=](double t) {
            DRect r = rect(t);
            return (std::max)(0.0, (std::min)({radius(t), r.w() / 2, r.h() / 2}));
        };
        if (top)
            for (int i = 0; i < 4; i++)
                if (auto a = Curve(dur, rad))
                    i == 0 ? c->SetTopLeftRadiusX(a.Get()) : i == 1 ? c->SetTopLeftRadiusY(a.Get())
                    : i == 2 ? c->SetTopRightRadiusX(a.Get()) : c->SetTopRightRadiusY(a.Get());
        if (bottom)
            for (int i = 0; i < 4; i++)
                if (auto a = Curve(dur, rad))
                    i == 0 ? c->SetBottomLeftRadiusX(a.Get()) : i == 1 ? c->SetBottomLeftRadiusY(a.Get())
                    : i == 2 ? c->SetBottomRightRadiusX(a.Get()) : c->SetBottomRightRadiusY(a.Get());
    }
    return c;
}

void FadeTo(IDCompositionEffectGroup* e, double dur, const std::function<double(double)>& f) {
    if (auto a = Curve(dur, f)) e->SetOpacity(a.Get());
}

// ---------------------------------------------------------------- the genie

// The rows' springs: the top, middle and bottom of the card (DiaTweaks' DPPeekTravel).
struct Genie {
    DRect card, link;
    double rows[3];
    double dur;
    bool opening;
    // How far row r (0 top, 1 middle, 2 bottom) still is from the card, toward the link, at t.
    double Away(int r, double t) const {
        double left = Remaining(t, rows[r]);
        return opening ? left : 1 - left;
    }
    // The card's left and right edges and its y, at height v (0 top .. 1 bottom), at t.
    void At(double v, double t, double& l, double& r, double& y) const {
        int r0 = v < 0.5 ? 0 : 1;
        double s = (v - r0 * 0.5) * 2;
        double l0, rr0, y0, l1, rr1, y1;
        Row(r0, t, l0, rr0, y0);
        Row(r0 + 1, t, l1, rr1, y1);
        l = Lerp(l0, l1, s), r = Lerp(rr0, rr1, s), y = Lerp(y0, y1, s);
    }
    void Row(int r, double t, double& l, double& rr, double& y) const {
        double a = Away(r, t), v = r / 2.0;
        l = Lerp(card.l, link.l, a);
        rr = Lerp(card.r, link.r, a);
        y = Lerp(card.t + v * card.h(), link.t + v * link.h(), a);
    }
    DRect Bounds(double t) const {
        DRect b{1e9, 1e9, -1e9, -1e9};
        for (int r = 0; r < 3; r++) {
            double l, rr, y;
            Row(r, t, l, rr, y);
            b.l = (std::min)(b.l, l), b.r = (std::max)(b.r, rr), b.t = (std::min)(b.t, y), b.b = (std::max)(b.b, y);
        }
        return b;
    }
    double Scale(double t) const {  // the middle row's size against the card's
        double a = Away(1, t);
        return Clamp01((std::min)(Lerp(1, link.w() / card.w(), a), Lerp(1, link.h() / card.h(), a)));
    }
};

Genie MakeGenie(bool opening) {
    Genie g;
    g.card = D(G.target);
    g.link = D(G.link);
    g.opening = opening;
    double ay = 2 * Clamp01(((G.link.top + G.link.bottom) / 2.0 - G.pageArea.top) /
                            (std::max)(1L, G.pageArea.bottom - G.pageArea.top)) - 1;
    double k = ay >= 0 ? ay : 0.75 * ay;
    if (!opening) k = -k;
    g.dur = 0;
    for (int r = 0; r < 3; r++) {
        double v = r / 2.0, n = k < 0 ? 1 - std::sqrt(1 - v) : std::sqrt(v);
        g.rows[r] = kResponse * (1 - 0.5 * k + k * n);
        g.dur = (std::max)(g.dur, Settling(g.rows[r]));
    }
    return g;
}

// The card flying between the link and its place: the picture (or the card's color) cut in slices that
// each take the card's width and place at their height, under a shadow, corners and fade as DiaTweaks'.
void PlayGenie(const Genie& g, HBITMAP picture) {
    double dur = ReduceMotion() ? 0 : g.dur;
    double radius = kCornerDip * G.scale, from = kCornerFromDip * G.scale;
    auto bounds = [g](double t) { return g.Bounds(t); };
    auto corner = [g, radius, from](double t) { return Lerp(radius, from, g.Away(1, t)); };
    ComPtr<IDCompositionVisual> all = NewVisual(), body = NewVisual(), shadow = NewVisual();
    ComPtr<IDCompositionEffectGroup> fade, shade;
    X.dev->CreateEffectGroup(&fade);
    X.dev->CreateEffectGroup(&shade);
    // The shadow: around the box the slices fill, as strong as the card is big.
    Shadow sh = MakeShadow(g.card.w(), g.card.h(), radius);
    if (sh.surface) {
        shadow->SetContent(sh.surface.Get());
        shadow->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
        double m = sh.margin, dy = kShadowYDip * G.scale;
        PlaceContent(shadow.Get(), dur, sh.w, sh.h, [g, m, dy](double t) {
            DRect b = g.Bounds(t);
            double s = b.w() / g.card.w(), sy = b.h() / g.card.h();
            return DRect{b.l - m * s, b.t - m * sy + dy * sy, b.r + m * s, b.b + m * sy + dy * sy};
        });
        shadow->SetEffect(shade.Get());
        FadeTo(shade.Get(), dur, [g](double t) { return g.Scale(t); });
        all->AddVisual(shadow.Get(), FALSE, nullptr);
    }
    // The slices, inside the rounded box around them.
    SIZE size{1, 1};
    ComPtr<IDCompositionSurface> content = picture ? FxSurfaceOf(picture, size) : nullptr;
    if (!content) content = FxColor(CardColor()), size = {1, 1};
    body->SetClip(ClipTo(dur, bounds, corner).Get());
    for (int i = 0; i < kStrips; i++) {
        double v0 = (double)i / kStrips, v1 = (double)(i + 1) / kStrips;
        ComPtr<IDCompositionVisual> slice = NewVisual(), pic = NewVisual();
        // The slice's place: the edges at its middle, from its top to its bottom (a hair over, so no seams).
        auto place = [g, v0, v1](double t) {
            double l, r, y0, y1, la, ra, lb, rb;
            g.At((v0 + v1) / 2, t, l, r, y0);
            g.At(v0, t, la, ra, y0);
            g.At(v1, t, lb, rb, y1);
            return DRect{l, y0, r, y1 + 0.75};
        };
        slice->SetClip(ClipTo(dur, place, nullptr).Get());
        pic->SetContent(content.Get());
        pic->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
        double pw = size.cx, ph = size.cy;
        // The whole picture placed so that its rows v0..v1 land on the slice.
        PlaceContent(pic.Get(), dur, pw, ph, [place, v0, v1](double t) {
            DRect s = place(t);
            double h = (s.b - 0.75 - s.t) / (v1 - v0);
            return DRect{s.l, s.t - v0 * h, s.r, s.t - v0 * h + h};
        });
        slice->AddVisual(pic.Get(), FALSE, nullptr);
        body->AddVisual(slice.Get(), FALSE, nullptr);
    }
    all->AddVisual(body.Get(), FALSE, nullptr);
    all->SetEffect(fade.Get());
    if (!g.opening)
        FadeTo(fade.Get(), dur, [g](double t) {
            double away = (std::min)({g.Away(0, t), g.Away(1, t), g.Away(2, t)});
            double left = Clamp01((1 - away) / 0.4);
            return left * left * (3 - 2 * left);
        });
    X.scene->AddVisual(all.Get(), FALSE, nullptr);
}

// ---------------------------------------------------------------- open as tab: the card grows to the page

// Your page around the card, dimmed, with the card's shadow on it; the card itself is a hole the live page
// shows through (it's under this window). The hole grows to the page on the spring, the dim going.
void PlayGrow(double dur, double dimFrom) {
    DRect from = D(G.target), to = D(G.pageArea);
    double response = kResponse;
    auto hole = [from, to, response, dur](double t) {
        double e = dur > 0 ? 1 - Remaining(t, response) : 1;
        if (t >= dur) e = 1;
        return LerpRect(from, to, e);
    };
    ComPtr<IDCompositionSurface> page = X.behind;
    ComPtr<IDCompositionSurface> black = FxColor(RGB(0, 0, 0));
    double radius = kCornerDip * G.scale;
    Shadow sh = MakeShadow(from.w(), from.h(), radius);
    DRect area = to;
    // Four bands around the hole: above, below, left, right.
    for (int side = 0; side < 4; side++) {
        auto band = [hole, area, side](double t) {
            DRect h = hole(t);
            switch (side) {
            case 0: return DRect{area.l, area.t, area.r, h.t};
            case 1: return DRect{area.l, h.b, area.r, area.b};
            case 2: return DRect{area.l, h.t, h.l, h.b};
            default: return DRect{h.r, h.t, area.r, h.b};
            }
        };
        ComPtr<IDCompositionVisual> v = NewVisual(), pic = NewVisual(), dim = NewVisual(), shadow = NewVisual();
        v->SetClip(ClipTo(dur, band, nullptr).Get());
        if (page) {
            pic->SetContent(page.Get());
            pic->SetOffsetX((float)(area.l - X.origin.x));
            pic->SetOffsetY((float)(area.t - X.origin.y));
            v->AddVisual(pic.Get(), FALSE, nullptr);
        }
        dim->SetContent(black.Get());
        PlaceContent(dim.Get(), 0, 1, 1, [area](double) { return area; });
        ComPtr<IDCompositionEffectGroup> dimFx;
        X.dev->CreateEffectGroup(&dimFx);
        dim->SetEffect(dimFx.Get());
        double from0 = dimFrom;
        FadeTo(dimFx.Get(), dur * 1.4, [from0](double t) { return kDarkness * from0 * Remaining(t, kDimResponse); });
        v->AddVisual(dim.Get(), FALSE, nullptr);
        if (sh.surface) {
            shadow->SetContent(sh.surface.Get());
            double m = sh.margin, dy = kShadowYDip * G.scale;
            PlaceContent(shadow.Get(), dur, sh.w, sh.h, [hole, m, dy](double t) {
                DRect b = hole(t);
                return DRect{b.l - m, b.t - m + dy, b.r + m, b.b + m + dy};
            });
            v->AddVisual(shadow.Get(), FALSE, nullptr);
        }
        X.scene->AddVisual(v.Get(), FALSE, nullptr);
    }
    // The hole's rounded corners: the dimmed page shows in them (a ring of it, clipped round, over the hole).
    // (Small: drawn as four corner pieces of the band picture.)
    (void)radius;
}

// ---------------------------------------------------------------- the controls

// Pixels for a layered window, premultiplied BGRA.
struct Canvas {
    int w, h;
    std::vector<UINT> px;
    Canvas(int w_, int h_) : w(w_), h(h_), px((size_t)w_ * h_, 0) {}
    void Over(int x, int y, double a, BYTE r, BYTE g, BYTE b) {  // a: 0..1 coverage x alpha
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
        UINT& d = px[(size_t)y * w + x];
        double da = (d >> 24) / 255.0, k = 1 - a;
        auto ch = [&](int shift, BYTE c) { return (UINT)std::lround(c * a + ((d >> shift) & 0xFF) * k); };
        d = (UINT)std::lround((a + da * k) * 255) << 24 | ch(16, r) << 16 | ch(8, g) << 8 | ch(0, b);
    }
    void RoundRect(double l, double t, double r, double b, double rad, double alpha, COLORREF c) {
        for (int y = (int)std::floor(t); y < (int)std::ceil(b); y++)
            for (int x = (int)std::floor(l); x < (int)std::ceil(r); x++) {
                double cx = x + 0.5, cy = y + 0.5;
                double dx = (std::max)({0.0, l + rad - cx, cx - (r - rad)}), dy = (std::max)({0.0, t + rad - cy, cy - (b - rad)});
                double inside = rad > 0 ? rad - std::sqrt(dx * dx + dy * dy) : 1e9;
                double edge = (std::min)({inside, cx - l, r - cx, cy - t, b - cy});
                Over(x, y, Clamp01(edge + 0.5) * alpha, GetRValue(c), GetGValue(c), GetBValue(c));
            }
    }
    // Text in a color, antialiased (drawn white on black, its brightness taken as coverage), centred in the box.
    void Text(const wchar_t* s, HFONT font, double l, double t, double r, double b, COLORREF c, bool center = true) {
        int bw = (int)std::ceil(r - l), bh = (int)std::ceil(b - t);
        if (bw <= 0 || bh <= 0) return;
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), bw, -bh, 1, 32, BI_RGB};
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ ob = SelectObject(dc, bmp), of = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT rc{0, 0, bw, bh};
        DrawTextW(dc, s, -1, &rc, DT_SINGLELINE | DT_VCENTER | (center ? DT_CENTER : DT_LEFT) | DT_NOPREFIX);
        GdiFlush();
        auto* p = static_cast<UINT*>(bits);
        int ox = (int)std::floor(l), oy = (int)std::floor(t);
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++) {
                UINT v = p[(size_t)y * bw + x];
                double a = (std::max)({(v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF}) / 255.0;
                Over(ox + x, oy + y, a, GetRValue(c), GetGValue(c), GetBValue(c));
            }
        SelectObject(dc, of);
        SelectObject(dc, ob);
        DeleteObject(bmp);
        DeleteDC(dc);
    }
    double TextWidth(const wchar_t* s, HFONT font) {
        HDC dc = CreateCompatibleDC(nullptr);
        HGDIOBJ of = SelectObject(dc, font);
        SIZE sz{};
        GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
        SelectObject(dc, of);
        DeleteDC(dc);
        return sz.cx;
    }
    // Onto a layered window at (x, y) on the screen, at this opacity.
    void Show(HWND win, int x, int y, double opacity) {
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
        void* bits = nullptr;
        HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        memcpy(bits, px.data(), px.size() * 4);
        HGDIOBJ ob = SelectObject(dc, bmp);
        POINT dst{x, y}, src{0, 0};
        SIZE sz{w, h};
        BLENDFUNCTION bf{AC_SRC_OVER, 0, (BYTE)std::lround(255 * Clamp01(opacity)), AC_SRC_ALPHA};
        UpdateLayeredWindow(win, screen, &dst, &sz, dc, &src, 0, &bf, ULW_ALPHA);
        SelectObject(dc, ob);
        DeleteObject(bmp);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
    }
};

HFONT Font(const wchar_t* face, double dip, int weight) {
    return CreateFontW(-(int)std::lround(dip * G.scale), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                       ANTIALIASED_QUALITY, 0, face);
}

const double kButtonSideDip = 28, kButtonGapDip8 = 8, kControlsXDip = 12, kControlsYDip = 4;
const int kButtons = 3;
const wchar_t* const kButtonGlyph[kButtons] = {L"\xE711", L"\xE740", L"\xE89F"};  // Cancel, FullScreen, OpenPane
const wchar_t* const kButtonHint[kButtons] = {L"Close", L"Expand", L"Create Split View"};
const wchar_t* const kButtonKey[kButtons] = {L"W", L"O", nullptr};

// Where control i's circle is centred, and its window (room for it to grow).
POINT ButtonCentre(int i) {
    double side = kButtonSideDip * G.scale, gap = kButtonGapDip8 * G.scale;
    return {(LONG)std::lround(G.target.right + kControlsXDip * G.scale + side / 2),
            (LONG)std::lround(G.target.top + kControlsYDip * G.scale + side / 2 + i * (side + gap))};
}
int ButtonBox() { return (int)std::ceil(kButtonSideDip * G.scale * 1.3); }

// Control i at this scale and opacity (DiaTweaks': a near-black circle, a white symbol).
void DrawButton(int i, double scale, double opacity) {
    HWND b = G.buttons[i];
    if (!b) return;
    int box = ButtonBox();
    Canvas c(box, box);
    double side = kButtonSideDip * G.scale * scale, cx = box / 2.0, r = side / 2;
    double alpha = G.pressed == i ? 0.7 : 0.9;
    c.RoundRect(cx - r, cx - r, cx + r, cx + r, r, alpha, RGB(20, 20, 20));
    HFONT f = Font(L"Segoe Fluent Icons", 12 * scale, FW_NORMAL);
    c.Text(kButtonGlyph[i], f, 0, 0, box, box, RGB(255, 255, 255));
    DeleteObject(f);
    POINT p = ButtonCentre(i);
    c.Show(b, p.x - box / 2, p.y - box / 2, opacity);
}

// The hint beside control i: its name and shortcut keys, in a dark pill (DiaTweaks' hint).
void DrawHint(int i, double opacity) {
    if (!G.hint) return;
    double s = G.scale;
    HFONT label = Font(L"Segoe UI", 12, FW_MEDIUM), keyFont = Font(L"Segoe UI", 11, FW_SEMIBOLD);
    Canvas probe(1, 1);
    double padX = 10 * s, capGap = 4 * s, labelGap = 6 * s, capH = 17 * s, height = 26 * s;
    double lw = probe.TextWidth(kButtonHint[i], label);
    const wchar_t* keys[2] = {L"Ctrl", kButtonKey[i]};
    int nk = kButtonKey[i] ? 2 : 0;
    double capW[2]{};
    for (int k = 0; k < nk; k++) capW[k] = (std::max)(capH, probe.TextWidth(keys[k], keyFont) + 8 * s);
    double w = padX + lw + (nk ? labelGap + capW[0] + capGap + capW[1] : 0) + padX;
    Canvas c((int)std::ceil(w), (int)std::ceil(height));
    c.RoundRect(0, 0, w, height, (std::min)(height / 2, 13 * s), 0.92, RGB(26, 26, 26));
    c.Text(kButtonHint[i], label, padX, 0, padX + lw + 2, height, RGB(255, 255, 255), false);
    double x = padX + lw + labelGap, ct = (height - capH) / 2;
    for (int k = 0; k < nk; k++) {
        c.RoundRect(x, ct, x + capW[k], ct + capH, 4 * s, 0.95, RGB(255, 255, 255));
        c.Text(keys[k], keyFont, x, ct, x + capW[k], ct + capH, RGB(20, 20, 20));
        x += capW[k] + capGap;
    }
    DeleteObject(label);
    DeleteObject(keyFont);
    POINT p = ButtonCentre(i);
    int left = (int)std::lround(p.x + kButtonSideDip * s / 2 + 10 * s);
    c.Show(G.hint, left, (int)std::lround(p.y - height / 2), opacity);
    ShowWindow(G.hint, SW_SHOWNOACTIVATE);
}

void HideControls() {
    G.controlsAt = -1;
    G.hover = G.pressed = G.hintFor = -1;
    for (HWND b : G.buttons)
        if (b) ShowWindow(b, SW_HIDE);
    if (G.hint) ShowWindow(G.hint, SW_HIDE);
}

void ShowControls() {
    G.controlsAt = Now();
    for (int i = 0; i < kButtons; i++) {
        G.hoverScale[i] = 1, G.hoverAt[i] = -1;
        DrawButton(i, 0.4, 0);
        if (G.buttons[i]) SetWindowPos(G.buttons[i], HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

// The controls' frame: popping in, growing under the pointer, the hint. Returns whether any still moves.
bool ControlsFrame() {
    if (G.controlsAt < 0) return false;
    double now = Now();
    bool moving = false;
    for (int i = 0; i < kButtons; i++) {
        double t = now - G.controlsAt - i * 0.05;
        double pop = t <= 0 ? 0.4 : Lerp(0.4, 1, SpringTo1(t, 0.32, 0.7));
        double fade = Clamp01(t / 0.14);
        bool popping = t < Settling(0.32) * 1.6;
        double hs = G.hoverScale[i];
        if (G.hoverAt[i] >= 0) {
            double ht = Clamp01((now - G.hoverAt[i]) / 0.12);
            hs = Lerp(G.hoverFrom[i], G.hover == i ? 1.12 : 1, EaseInOut(ht));
            G.hoverScale[i] = hs;
            if (ht >= 1) G.hoverAt[i] = -1;
            moving = true;
        }
        if (popping || G.hoverAt[i] >= 0 || true) DrawButton(i, pop * hs, fade);
        moving |= popping;
    }
    if (G.hover >= 0 && G.hintFor != G.hover && now - G.hintAt >= 1.0) {
        G.hintFor = G.hover;
        G.hintAt = now;
    }
    if (G.hintFor >= 0) {
        double a = Clamp01((now - G.hintAt) / 0.12);
        DrawHint(G.hintFor, a);
        moving |= a < 1;
    }
    if (G.hover >= 0 && G.hintFor < 0) moving = true;  // (counting to the hint)
    return moving;
}

LRESULT CALLBACK ButtonProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    int i = h == G.buttons[0] ? 0 : h == G.buttons[1] ? 1 : h == G.buttons[2] ? 2 : -1;
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST: {  // (only the circle)
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (i < 0) return HTNOWHERE;
        POINT c = ButtonCentre(i);
        double r = kButtonSideDip * G.scale / 2 + 1;
        return (p.x - c.x) * (p.x - c.x) + (p.y - c.y) * (p.y - c.y) <= r * r ? HTCLIENT : HTTRANSPARENT;
    }
    case WM_MOUSEMOVE:
        if (i >= 0 && G.hover != i) {
            G.hoverFrom[i] = G.hoverScale[i];
            G.hoverAt[i] = Now();
            G.hover = i;
            G.hintAt = Now();
            G.hintFor = -1;
            if (G.hint) ShowWindow(G.hint, SW_HIDE);
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
        }
        return 0;
    case WM_MOUSELEAVE:
        if (i >= 0 && G.hover == i) {
            G.hover = -1;
            G.hoverFrom[i] = G.hoverScale[i];
            G.hoverAt[i] = Now();
            G.hintFor = -1;
            if (G.hint) ShowWindow(G.hint, SW_HIDE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (i >= 0) G.pressed = i, DrawButton(i, G.hoverScale[i], 1);
        return 0;
    case WM_LBUTTONUP:
        if (i >= 0) {
            bool hit = G.pressed == i;
            G.pressed = -1;
            if (hit && G.phase == gOpen) PostMessageW(g_msg, WM_PEEK_DISMISS, i, 0);  // (0 close, 1 tab, 2 split)
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK BackdropProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        if (G.phase == gOpening || G.phase == gOpen) PostMessageW(g_msg, WM_PEEK_DISMISS, 0, 0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// The card at rest (under Dia's window, which shows the page on it): its color, rounded, with Windows'
// shadow.
LRESULT CALLBACK CardProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) return 0;
        break;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH b = CreateSolidBrush(CardColor());
        FillRect(dc, &rc, b);
        DeleteObject(b);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void PlaceCard(const RECT& r) {
    SetWindowPos(G.card, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
}

void EndGlance() {
    for (HWND& b : G.buttons)
        if (b) DestroyWindow(b), b = nullptr;
    if (G.hint) DestroyWindow(G.hint), G.hint = nullptr;
    if (G.card) DestroyWindow(G.card), G.card = nullptr;
    if (G.backdrop) DestroyWindow(G.backdrop), G.backdrop = nullptr;
    FxEnd();
    G.fx = false;
    G.revealed = false;
    G.controlsAt = -1;
    G.hover = G.pressed = G.hintFor = -1;
    G.phase = gIdle;
}

void StartGlance(GlanceOpen* o) {
    EndGlance();
    HINSTANCE inst = GetModuleHandleW(nullptr);
    G.pageArea = o->pageArea;
    G.target = o->target;
    G.link = o->link;
    G.scale = o->scale;
    const RECT& pr = o->pageArea;
    G.backdrop = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"DiaTweaksPeekBackdrop", L"",
                                 WS_POPUP, pr.left, pr.top, pr.right - pr.left, pr.bottom - pr.top, o->owner, nullptr,
                                 inst, nullptr);
    o->backdrop = G.backdrop;
    if (!G.backdrop) return;
    G.dimFrom = G.dimTo = 0;
    G.dimStart = Now();
    SetDimAlpha(0);
    G.card = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"DiaTweaksPeekCard", L"", WS_POPUP | WS_THICKFRAME,
                             0, 0, 1, 1, G.backdrop, nullptr, inst, nullptr);
    if (G.card) {
        DWORD corner = 2;  // DWMWCP_ROUND
        DwmSetWindowAttribute(G.card, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
        MARGINS m{0, 0, 1, 0};  // (gives it Windows' shadow)
        DwmExtendFrameIntoClientArea(G.card, &m);
        BOOL noAnim = TRUE;
        DwmSetWindowAttribute(G.card, 3 /*DWMWA_TRANSITIONS_FORCEDISABLED*/, &noAnim, sizeof(noAnim));
        PlaceCard(G.target);
    }
    for (auto& b : G.buttons)
        b = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"DiaTweaksPeekButton", L"", WS_POPUP, 0,
                            0, 1, 1, G.backdrop, nullptr, inst, nullptr);
    G.hint = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"DiaTweaksPeekShot",
                             L"", WS_POPUP, 0, 0, 1, 1, G.backdrop, nullptr, inst, nullptr);
    ShowWindow(G.backdrop, SW_SHOWNOACTIVATE);
    DimTo(1);
    // The card comes out of the link: the genie, on the GPU; at rest, the card window takes its place.
    G.fx = FxBegin(o->pageArea);
    Genie g = MakeGenie(true);
    if (G.fx) {
        PlayGenie(g, nullptr);
        FxShow();
    }
    G.phase = gOpening;
    G.start = Now();
    G.dur = G.fx && !ReduceMotion() ? g.dur : 0;
}

LRESULT CALLBACK GlanceCtlProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case GM_OPEN:
        StartGlance(reinterpret_cast<GlanceOpen*>(lp));
        return 0;
    case GM_SHOWN:  // the page is on the card: the controls pop in
        if (G.phase == gOpen) ShowControls();
        return 0;
    case GM_MOVE: {
        auto* o = reinterpret_cast<GlanceOpen*>(lp);
        G.pageArea = o->pageArea;
        G.target = o->target;
        if (G.backdrop)
            SetWindowPos(G.backdrop, nullptr, G.pageArea.left, G.pageArea.top, G.pageArea.right - G.pageArea.left,
                         G.pageArea.bottom - G.pageArea.top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (G.phase == gOpen) {
            PlaceCard(G.target);
            if (G.controlsAt >= 0)
                for (int i = 0; i < kButtons; i++) DrawButton(i, G.hoverScale[i], 1);
        }
        return 0;
    }
    case GM_CLOSE: {  // (sent: the picture is on the screen before Dia's window is hidden)
        auto* c = reinterpret_cast<GlanceClose*>(lp);
        if (G.phase == gIdle) {
            if (c->picture) DeleteObject(c->picture);
            if (c->behind) DeleteObject(c->behind);
            PostMessageW(g_msg, WM_PEEK_DONE, 0, 0);  // (nothing shown yet: just done)
            return 0;
        }
        HideControls();
        double dim = DimNow();
        if (G.phase == gOpening) {  // (from where it is: just put it in place)
            FxClearScene();
            PlaceCard(G.target);
        }
        G.fx = FxBegin(IsRectEmpty(&c->area) ? G.pageArea : c->area);
        if (c->expand) {
            // Your page, dimmed, around the preview's picture: as the screen is now. It holds until the live
            // page, laid out at your page's size, is under it (GM_REVEALED), then the hole grows.
            if (G.fx) {
                if (c->behind) X.behind = FxSurfaceOf(c->behind, X.behindSize);
                PlayGrow(0, dim);
                SIZE size{};
                if (auto s = c->picture ? FxSurfaceOf(c->picture, size) : FxColor(CardColor())) {
                    if (!c->picture) size = {1, 1};
                    X.cover->SetContent(s.Get());
                    RECT t = G.target;
                    PlaceContent(X.cover.Get(), 0, size.cx, size.cy, [t](double) { return D(t); });
                    X.cover->SetClip(ClipTo(0, [t](double) { return D(t); }, [](double) { return kCornerDip * G.scale; }).Get());
                }
                FxShow();
            }
            if (G.card) ShowWindow(G.card, SW_HIDE);
            if (G.backdrop) ShowWindow(G.backdrop, SW_HIDE);
            G.phase = gHolding;
            G.dimFrom = dim;  // (for the growth)
            G.start = Now();
            G.dur = 0;
        } else {
            if (G.fx) {
                PlayGenie(MakeGenie(false), c->picture);
                FxShow();
            }
            if (G.card) ShowWindow(G.card, SW_HIDE);
            DimTo(0);
            G.phase = gClosing;
            G.start = Now();
            G.dur = G.fx && !ReduceMotion() ? MakeGenie(false).dur : 0;
        }
        if (c->picture) DeleteObject(c->picture);
        if (c->behind) DeleteObject(c->behind);
        if (G.backdrop)  // (clicks go through: it's going)
            SetWindowLongPtrW(G.backdrop, GWL_EXSTYLE, GetWindowLongPtrW(G.backdrop, GWL_EXSTYLE) | WS_EX_TRANSPARENT);
        return 0;
    }
    case GM_END:
        EndGlance();
        return 0;
    case GM_REVEALED: {  // (sent) "open as tab": the live page is under the card: the card grows to the page
        if (G.phase != gHolding) return 0;
        G.revealed = true;
        if (G.fx) {
            double dim = G.dimFrom, dur = ReduceMotion() ? 0 : Settling(kResponse);
            FxClearScene();
            PlayGrow(dur, dim);
            X.cover->SetContent(nullptr);
            FxCommit(true);
            G.dur = dur;
        }
        G.phase = gGrowing;
        G.start = Now();
        return 0;
    }
    case GM_PATCH: {  // (sent) a picture over part of the screen, on it before this returns
        auto* pt = reinterpret_cast<GlancePatch*>(lp);
        if (!G.fx || !X.patch) return 0;
        SIZE size{};
        ComPtr<IDCompositionSurface> sfc = pt && pt->bmp ? FxSurfaceOf(pt->bmp, size) : nullptr;
        X.patch->SetContent(sfc.Get());
        if (sfc) X.patch->SetOffsetX((float)(pt->r.left - X.origin.x)), X.patch->SetOffsetY((float)(pt->r.top - X.origin.y));
        FxCommit(true);
        return sfc ? 1 : 0;
    }
    case GM_COVER: {  // (sent) the page, still, over the page area, on it before this returns (lp: HBITMAP)
        HBITMAP b = (HBITMAP)lp;
        if (G.phase == gIdle || !G.fx) {
            if (b) DeleteObject(b);
            return 0;
        }
        SIZE size{};
        ComPtr<IDCompositionSurface> s = FxSurfaceOf(b, size);
        if (b) DeleteObject(b);
        if (!s) return 0;
        X.cover->SetContent(s.Get());
        X.cover->SetTransform(nullptr);
        X.cover->SetClip(nullptr);
        X.cover->SetOffsetX((float)(G.pageArea.left - X.origin.x));
        X.cover->SetOffsetY((float)(G.pageArea.top - X.origin.y));
        X.coverFx->SetOpacity(1.f);
        FxCommit(true);
        return 1;
    }
    case GM_FADE:  // "open as tab": done (the tab is in your window, or it didn't move): what's left fades
        if (G.phase == gIdle) return 0;
        G.phase = gFading;
        G.start = Now();
        G.dur = ReduceMotion() ? 0 : kFadeMs / 1000.0;
        if (G.fx) {
            FadeTo(X.coverFx.Get(), G.dur, [](double t) { return 1 - EaseInOut(Clamp01(t / (kFadeMs / 1000.0))); });
            FxCommit();
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// One frame: the dim (a window's alpha), the controls, and the phases' ends. Returns whether anything moves.
bool GlanceFrame() {
    double now = Now(), t = now - G.start;
    bool done = t >= G.dur;
    bool moving = false;
    if (G.backdrop && IsWindowVisible(G.backdrop)) {
        double d = DimNow();
        SetDimAlpha(d);
        moving |= std::abs(d - G.dimTo) > 0.002;
    }
    switch (G.phase) {
    case gOpening:
        if (done) {  // the card window takes the genie's place (the same), then the genie goes
            PlaceCard(G.target);
            ShowWindow(G.card, SW_SHOWNOACTIVATE);
            DwmFlush();
            FxEnd();
            G.phase = gOpen;
            PostMessageW(g_msg, WM_PEEK_CARD, 0, 0);
        } else {
            moving = true;
        }
        break;
    case gClosing:
        if (done) {
            EndGlance();
            PostMessageW(g_msg, WM_PEEK_DONE, 0, 0);
            return false;
        }
        moving = true;
        break;
    case gGrowing:
        if (done) {  // the page is the live one now, full size: the overlay stays (empty) for the tab's move
            FxClearScene();
            FxCommit();
            G.phase = gHeld;
        } else {
            moving = true;
        }
        break;
    case gFading:
        if (done) {
            EndGlance();
            return false;
        }
        moving = true;
        break;
    default:
        break;
    }
    moving |= ControlsFrame();
    return moving;
}

DWORD WINAPI GlanceThread(LPVOID ready) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = BackdropProc;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"DiaTweaksPeekBackdrop";
    RegisterClassW(&wc);
    wc.lpfnWndProc = CardProc;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"DiaTweaksPeekCard";
    RegisterClassW(&wc);
    wc.lpfnWndProc = FxProc;
    wc.lpszClassName = L"DiaTweaksPeekFx";
    RegisterClassW(&wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"DiaTweaksPeekShot";
    RegisterClassW(&wc);
    wc.lpfnWndProc = ButtonProc;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.lpszClassName = L"DiaTweaksPeekButton";
    RegisterClassW(&wc);
    wc.lpfnWndProc = GlanceCtlProc;
    wc.hCursor = nullptr;
    wc.lpszClassName = L"DiaTweaksPeekGlance";
    RegisterClassW(&wc);
    G.ctl = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    g_ctl = G.ctl;
    SetEvent((HANDLE)ready);
    // Paced to the display: a frame, then wait for the next composition (DwmFlush); idle, just messages.
    bool moving = false;
    for (;;) {
        MSG m;
        if (!moving) {
            if (GetMessageW(&m, nullptr, 0, 0) <= 0) break;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) return 0;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        moving = G.phase != gIdle && GlanceFrame();
        if (moving) DwmFlush();
    }
    return 0;
}

void EnsureGlanceThread() {
    if (g_ctl) return;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE t = CreateThread(nullptr, 0, GlanceThread, ready, 0, nullptr);
    if (t) {
        WaitForSingleObject(ready, 2000);
        CloseHandle(t);
    }
    CloseHandle(ready);
}

// ================================================================ Dia's window as the preview

// The Chromium window of a Dia window, once it has a page.
HWND PageHostOf(HWND top) {
    if (!top) return nullptr;
    for (HWND c = FindWindowExW(top, nullptr, L"Chrome_WidgetWin_1", nullptr); c;
         c = FindWindowExW(top, c, L"Chrome_WidgetWin_1", nullptr)) {
        if ((GetWindowLongW(c, GWL_STYLE) & WS_VISIBLE) &&
            FindWindowExW(c, nullptr, L"Chrome_RenderWidgetHostHWND", nullptr))
            return c;
    }
    return nullptr;
}

// Nothing of Dia's window shows (and it takes no clicks) while it's clipped to an empty region.
void HideWindow(HWND w) { SetWindowRgn(w, CreateRectRgn(0, 0, 0, 0), TRUE); }

bool RegionEmpty(HWND w) {
    HRGN r = CreateRectRgn(0, 0, 0, 0);
    int type = GetWindowRgn(w, r);
    DeleteObject(r);
    return type == NULLREGION;
}

void Cloak(HWND w, bool on) {
    BOOL v = on;
    DwmSetWindowAttribute(w, 13 /*DWMWA_CLOAK*/, &v, sizeof(v));
}

// No frame of its own: no shadow, no border, no open animation.
void NoFrame(HWND w) {
    DWORD off = 1;  // DWMNCRP_DISABLED
    DwmSetWindowAttribute(w, 2 /*DWMWA_NCRENDERING_POLICY*/, &off, sizeof(off));
    COLORREF none = 0xFFFFFFFE;  // DWMWA_COLOR_NONE
    DwmSetWindowAttribute(w, 34 /*DWMWA_BORDER_COLOR*/, &none, sizeof(none));
    BOOL noAnim = TRUE;
    DwmSetWindowAttribute(w, 3 /*DWMWA_TRANSITIONS_FORCEDISABLED*/, &noAnim, sizeof(noAnim));
}

// Its frame drawn by Windows again (as Dia had it).
void DefaultFrame(HWND w) {
    DWORD policy = 0;  // DWMNCRP_USEWINDOWSTYLE
    DwmSetWindowAttribute(w, 2 /*DWMWA_NCRENDERING_POLICY*/, &policy, sizeof(policy));
}

// Nothing of Dia's window shows: clipped to nothing, no frame or shadow. (Not cloaked: Dia doesn't draw
// the page of a cloaked window, and doesn't start again when it's uncloaked. Not layered either: WinUI
// doesn't draw a window that was layered as it was made.)
void KeepHidden(HWND w) {
    static bool busy = false;  // (changing it sends messages back here)
    if (busy) return;
    busy = true;
    if (!RegionEmpty(w)) HideWindow(w);
    NoFrame(w);
    busy = false;
}

// The preview has a fixed size and place: Dia's window doesn't offer its edges for resizing, and isn't
// moved, sized or maximized from its system menu.
LRESULT CALLBACK FixedProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_NCHITTEST) {
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        return (r >= HTLEFT && r <= HTBOTTOMRIGHT) || r == HTCAPTION || r == HTMAXBUTTON || r == HTMINBUTTON ? HTCLIENT : r;
    }
    if (msg == WM_SYSCOMMAND) {
        UINT c = (UINT)(wp & 0xFFF0);
        if (c == SC_SIZE || c == SC_MOVE || c == SC_MAXIMIZE || c == SC_MINIMIZE) return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, FixedProc, 1);
    return DefSubclassProc(h, msg, wp, lp);
}

// Dia has the page's title: the page has started drawing.
bool HasTitle(HWND w) {
    wchar_t t[64] = L"";
    GetWindowTextW(w, t, 64);
    return t[0] && wcscmp(t, L"Dia") != 0 && wcscmp(t, L"New Tab") != 0 && wcscmp(t, L"WinUI Desktop") != 0;
}

// Moves and sizes Dia's window so that its page covers the target exactly (below the screen while
// parked). Dia lays the page out again after a resize (on a later message), so this measures, corrects,
// and says whether it's there yet.
bool AlignPage(Peek* p, HWND page) {
    RECT wr, pr;
    if (!GetWindowRect(p->win, &wr) || !GetWindowRect(page, &pr)) return false;
    RECT t = p->target;
    if (p->parked) OffsetRect(&t, 0, kParkY);
    if (std::abs(pr.left - t.left) <= 2 && std::abs(pr.top - t.top) <= 2 && std::abs(pr.right - t.right) <= 2 &&
        std::abs(pr.bottom - t.bottom) <= 2)
        return true;
    int dx = pr.left - wr.left, dy = pr.top - wr.top;
    int w = (wr.right - wr.left) + ((t.right - t.left) - (pr.right - pr.left));
    int h = (wr.bottom - wr.top) + ((t.bottom - t.top) - (pr.bottom - pr.top));
    SetWindowPos(p->win, nullptr, t.left - dx, t.top - dy, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    return false;
}

// Clips Dia's window to its page, with the card's rounded corners.
void ClipToPage(Peek* p) {
    RECT wr, pr;
    if (!IsWindow(p->win) || !p->widget || !GetWindowRect(p->win, &wr) || !GetWindowRect(p->widget, &pr)) return;
    int rad = (int)std::lround(16 * Scale(p->dia));
    SetWindowRgn(p->win,
                 CreateRoundRectRgn(pr.left - wr.left, pr.top - wr.top, pr.right - wr.left + 1, pr.bottom - wr.top + 1,
                                    rad, rad),
                 TRUE);
}

// Owned by the backdrop: above it and the card, no taskbar button, not in Alt+Tab. (WinUI resets an
// owner set at creation.)
void OwnWindow(Peek* p) {
    if (!p->backdrop || GetWindow(p->win, GW_OWNER) == p->backdrop) return;
    SetWindowLongPtrW(p->win, GWLP_HWNDPARENT, (LONG_PTR)p->backdrop);
    ComPtr<ITaskbarList> taskbar;
    if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar))) &&
        SUCCEEDED(taskbar->HrInit()))
        taskbar->DeleteTab(p->win);
}

// A picture of the preview's page (what's on the screen of it), at its size.
HBITMAP PicturePage(Peek* p) {
    RECT wr, pr;
    if (!IsWindow(p->win) || !p->widget || !GetWindowRect(p->win, &wr) || !GetWindowRect(p->widget, &pr)) return nullptr;
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top, pw = pr.right - pr.left, ph = pr.bottom - pr.top;
    HDC screen = GetDC(nullptr);
    HDC whole = CreateCompatibleDC(screen), part = CreateCompatibleDC(screen);
    HBITMAP all = CreateCompatibleBitmap(screen, ww, wh), pic = CreateCompatibleBitmap(screen, pw, ph);
    HGDIOBJ o1 = SelectObject(whole, all), o2 = SelectObject(part, pic);
    PrintWindow(p->win, whole, 2 /*PW_RENDERFULLCONTENT*/);
    BitBlt(part, 0, 0, pw, ph, whole, pr.left - wr.left, pr.top - wr.top, SRCCOPY);
    SelectObject(whole, o1);
    SelectObject(part, o2);
    DeleteObject(all);
    DeleteDC(whole);
    DeleteDC(part);
    ReleaseDC(nullptr, screen);
    return pic;
}

// A picture of a window's content in a screen rectangle (as drawn, including what Chromium composes in).
HBITMAP PictureOf(HWND w, const RECT& r) {
    RECT wr;
    if (!IsWindow(w) || !GetWindowRect(w, &wr)) return nullptr;
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top, pw = r.right - r.left, ph = r.bottom - r.top;
    if (pw <= 0 || ph <= 0) return nullptr;
    HDC screen = GetDC(nullptr);
    HDC whole = CreateCompatibleDC(screen), part = CreateCompatibleDC(screen);
    HBITMAP all = CreateCompatibleBitmap(screen, ww, wh), pic = CreateCompatibleBitmap(screen, pw, ph);
    HGDIOBJ o1 = SelectObject(whole, all), o2 = SelectObject(part, pic);
    PrintWindow(w, whole, 2 /*PW_RENDERFULLCONTENT*/);
    BitBlt(part, 0, 0, pw, ph, whole, r.left - wr.left, r.top - wr.top, SRCCOPY);
    SelectObject(whole, o1);
    SelectObject(part, o2);
    DeleteObject(all);
    DeleteDC(whole);
    DeleteDC(part);
    ReleaseDC(nullptr, screen);
    return pic;
}

// ---------------------------------------------------------------- open / show / close

void Destroy(bool keepCard = false);

void StopHiding() {
    if (g_showHook) UnhookWinEvent(g_showHook);
    g_showHook = nullptr;
    if (g_after) UnhookWindowsHookEx(g_after);
    g_after = nullptr;
}

// The page is ready and the card has flown out: the page shows on the card.
void RevealPage(Peek* p, HWND page) {
    p->widget = page;
    StopHiding();
    RECT wr, pr;
    if (GetWindowRect(p->win, &wr) && GetWindowRect(page, &pr)) {
        g_offset.dx = pr.left - wr.left;
        g_offset.dy = pr.top - wr.top;
        g_offset.dw = (wr.right - wr.left) - (pr.right - pr.left);
        g_offset.dh = (wr.bottom - wr.top) - (pr.bottom - pr.top);
    }
    OwnWindow(p);
    ClipToPage(p);
    SetWindowPos(p->win, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    DefaultFrame(p->win);
    Cloak(p->win, false);
    SetForegroundWindow(p->win);
    p->phase = kOpen;
    if (g_ctl && GetWindowRect(page, &pr) && !EqualRect(&pr, &p->target)) {  // (the card exactly under the page)
        GlanceOpen o{p->dia, p->pageArea, pr, {}, Scale(p->dia), nullptr};
        SendMessageW(g_ctl, GM_MOVE, 0, (LPARAM)&o);
    }
    if (g_ctl) PostMessageW(g_ctl, GM_SHOWN, 0, 0);
    Log("peek: page shown after %llu ms (%d tries to place it)", GetTickCount64() - p->started, p->aligns);
}

// "Open as tab", as Zen's: the preview's page takes your page's size at once (Dia lays it out again, live,
// under the card while the card grows), and once the card has nearly filled the page, the live page is
// shown there and the card fades off it: from then on it's the page itself, scrolled and clicked as usual.
// Then its tab moves into the window you're in, as it is (Dia's own "Move to Window": not loaded again, a
// plain tab): its tab row (in its window's sidebar, cut away) is let through for a right-click, under a
// picture of what's on the screen there, and the menu that opens is never shown (cloaked); "Move to Window"
// > your window is chosen in it. Just as the tab moves, a still picture of the page covers it for the few
// frames your window takes to show it.
struct {
    HWND win = nullptr, dia = nullptr;
    RECT pageArea{};  // your page (where the preview's page now is)
    std::wstring title;
    ULONGLONG started = 0, revealedAt = 0, landedAt = 0;
    int step = 0;  // 0 resize, 5 wait for the card, 6 shown: settle, 3 right-click, 1 menu, 2 moved?
    HWINEVENTHOOK popHook = nullptr;
    POINT pageOff{};  // where the page sits in its window (measured before the resize)
    std::vector<HWND> cloaked;
} g_move;
const ULONGLONG kMoveTimeoutMs = 3000, kShownSettleMs = 450, kLandMs = 80;  // (after the card has grown)
const ULONGLONG kLayoutMs = 140;  // the page laid out again at your page's size (and drawn) by then

// The preview window's page with rounded corners, in its window's coordinates.
HRGN PageRegion(HWND w) {
    RECT wr, pr;
    HWND host = PageHostOf(w);
    if (!host || !GetWindowRect(w, &wr) || !GetWindowRect(host, &pr)) return CreateRectRgn(0, 0, 0, 0);
    int rad = (int)std::lround(16 * Scale(w));
    return CreateRoundRectRgn(pr.left - wr.left, pr.top - wr.top, pr.right - wr.left + 1, pr.bottom - wr.top + 1, rad, rad);
}

// Windows Dia makes or shows while the tab moves (its tab menu and submenu) are never seen.
void CALLBACK OnMovePopup(HWINEVENTHOOK, DWORD, HWND w, LONG obj, LONG, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || !w || !g_move.win || w == g_move.win || w == g_move.dia) return;
    if (GetAncestor(w, GA_ROOT) != w) return;
    Cloak(w, true);
    if (std::find(g_move.cloaked.begin(), g_move.cloaked.end(), w) == g_move.cloaked.end()) g_move.cloaked.push_back(w);
}

void HidePopups(bool on) {
    if (on && !g_move.popHook) {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&OnMovePopup, &self);
        g_move.popHook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, self, OnMovePopup,
                                         GetCurrentProcessId(), GetCurrentThreadId(), WINEVENT_INCONTEXT);
    } else if (!on) {
        if (g_move.popHook) UnhookWinEvent(g_move.popHook);
        g_move.popHook = nullptr;
        for (HWND w : g_move.cloaked)
            if (IsWindow(w)) Cloak(w, false);
        g_move.cloaked.clear();
    }
}

void EndMove(bool moved) {
    KillTimer(g_msg, kTimerTab);
    HidePopups(false);
    HWND w = g_move.win;
    g_move.win = nullptr;
    Ui_SetPeekWindow(nullptr);
    if (IsWindow(w)) {
        HideWindow(w);
        if (!moved) {
            Cloak(w, true);
            PostMessageW(w, WM_CLOSE, 0, 0);
        }
    }
    if (IsWindow(g_move.dia)) SetForegroundWindow(g_move.dia);
    if (g_ctl) PostMessageW(g_ctl, GM_FADE, moved ? 0 : 1, 0);
    Log("peek: open as tab: %s after %llu ms", moved ? "moved" : "gave up", GetTickCount64() - g_move.started);
}

void MoveTab(HWND win, HWND dia, const RECT& pageArea) {
    g_move.win = win;
    g_move.dia = dia;
    g_move.pageArea = pageArea;
    wchar_t t[256] = L"";
    GetWindowTextW(dia, t, 256);
    g_move.title = t;
    g_move.started = GetTickCount64();
    g_move.revealedAt = g_move.landedAt = 0;
    g_move.step = 0;
    SetWindowLongPtrW(win, GWLP_HWNDPARENT, 0);  // (not destroyed with the backdrop)
    Ui_SetPeekWindow(win);                       // (its toolbar stays as Dia lays it out)
    SetTimer(g_msg, kTimerTab, 15, nullptr);
    // At once: the page takes your page's size and place (laid out again while the card grows over it).
    if (HWND host = PageHostOf(win)) {
        RECT wr, pr;
        GetWindowRect(win, &wr);
        GetWindowRect(host, &pr);
        g_move.pageOff = {pr.left - wr.left, pr.top - wr.top};
        SetWindowPos(win, nullptr, pageArea.left - (pr.left - wr.left), pageArea.top - (pr.top - wr.top),
                     (wr.right - wr.left) + ((pageArea.right - pageArea.left) - (pr.right - pr.left)),
                     (wr.bottom - wr.top) + ((pageArea.bottom - pageArea.top) - (pr.bottom - pr.top)),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    g_move.step = 5;
}

// The card has nearly filled the page: the live page shows under it (and takes the keyboard).
void ShowMovedPage() {
    HWND w = g_move.win;
    if (!w || g_move.step != 5) return;
    if (!IsWindow(w)) return EndMove(false);
    // The page where it was put (Dia reports its new place a little later): square while the card grows
    // (its hole is).
    const RECT& a = g_move.pageArea;
    POINT o = g_move.pageOff;
    SetWindowRgn(w, CreateRectRgn(o.x, o.y, o.x + (a.right - a.left), o.y + (a.bottom - a.top)), TRUE);
    SetWindowPos(w, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    DwmFlush();  // (on the screen, under the card; it takes the keyboard once grown, see MoveTick)
    if (g_ctl) SendMessageW(g_ctl, GM_REVEALED, 0, 0);
    g_move.revealedAt = GetTickCount64();
    g_move.step = 6;
}

// Just before the tab moves (from ui.cpp, as "Move to Window" > your window is chosen): a still picture of
// the page covers it while it changes windows.
void BeforeMove() {
    if (!g_move.win || !g_ctl) return;
    if (HBITMAP b = PictureOf(g_move.win, g_move.pageArea)) SendMessageW(g_ctl, GM_COVER, 0, (LPARAM)b);
}

void MoveTick() {
    ULONGLONG age = GetTickCount64() - g_move.started;
    HWND w = g_move.win;
    if (!w) return KillTimer(g_msg, kTimerTab), void();
    if (g_move.step == 5) {  // laid out at your page's size (a moment after the resize): shown
        RECT pr{};
        HWND host = PageHostOf(w);
        if (host) GetWindowRect(host, &pr);
        const RECT& a = g_move.pageArea;
        bool sized = host && pr.right - pr.left == a.right - a.left && pr.bottom - pr.top == a.bottom - a.top;
        if ((sized && age >= kLayoutMs) || age > 1000) ShowMovedPage();
        return;
    }
    if (g_move.step == 6) {
        if (GetTickCount64() - g_move.revealedAt < kShownSettleMs) return;
        SetForegroundWindow(w);
        g_move.step = 3;
    }
    if (g_move.step == 3) {
        RECT row;
        if (!IsWindow(w) || !Ui_PeekTabRow(w, row)) {
            Log("peek: open as tab: no tab row");
            return EndMove(false);
        }
        // The part of the row left of the page (it lies over your sidebar), let through for a right-click,
        // under a picture of what's on the screen there.
        RECT under = row, page{};
        if (HWND host = PageHostOf(w)) GetWindowRect(host, &page);
        under.left = row.left + 2;
        under.right = (std::min)(row.right, page.left - 2);
        if (under.right - under.left < 4) {
            Log("peek: open as tab: the tab row is hidden");
            return EndMove(false);
        }
        HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, under.right - under.left, under.bottom - under.top);
        HGDIOBJ old = SelectObject(mem, bmp);
        BitBlt(mem, 0, 0, under.right - under.left, under.bottom - under.top, screen, under.left, under.top, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        GlancePatch patch{bmp, under};
        if (g_ctl) SendMessageW(g_ctl, GM_PATCH, 0, (LPARAM)&patch);
        DeleteObject(bmp);
        RECT wr;
        GetWindowRect(w, &wr);
        HRGN rgn = PageRegion(w), strip = CreateRectRgn(under.left - wr.left, under.top - wr.top, under.right - wr.left,
                                                          under.bottom - wr.top);
        CombineRgn(rgn, rgn, strip, RGN_OR);
        DeleteObject(strip);
        SetWindowRgn(w, rgn, TRUE);
        HidePopups(true);
        POINT at{(under.left + under.right) / 2, (under.top + under.bottom) / 2}, back;
        GetCursorPos(&back);
        auto abs = [](POINT pt, DWORD flags) {
            INPUT in{};
            in.type = INPUT_MOUSE;
            in.mi.dx = (LONG)std::lround((pt.x - GetSystemMetrics(SM_XVIRTUALSCREEN)) * 65535.0 /
                                         (GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1));
            in.mi.dy = (LONG)std::lround((pt.y - GetSystemMetrics(SM_YVIRTUALSCREEN)) * 65535.0 /
                                         (GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1));
            in.mi.dwFlags = flags | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;
            in.mi.dwExtraInfo = kReplayTag;
            return in;
        };
        INPUT in[4] = {abs(at, 0), abs(at, MOUSEEVENTF_RIGHTDOWN), abs(at, MOUSEEVENTF_RIGHTUP), abs(back, 0)};
        SendInput(4, in, sizeof(INPUT));
        g_move.step = 1;
        return;
    }
    if (g_move.step == 1) {
        int r = IsWindow(w) ? Ui_PeekMoveTab(w, g_move.dia) : -1;
        if (r == 1) {
            g_move.step = 2;
            return;
        }
        if (r < 0 || age > kMoveTimeoutMs) {
            Log("peek: open as tab: no Move to Window (%d)", r);
            return EndMove(false);
        }
        return;
    }
    // Moved: done once the preview's window is gone (Dia closes a window left without tabs) or yours shows
    // the tab (its title changes). Neither: it didn't move, and the preview's window is closed.
    wchar_t t[256] = L"";
    if (IsWindow(g_move.dia)) GetWindowTextW(g_move.dia, t, 256);
    bool moved = !IsWindow(w) || (t[0] && g_move.title != t);
    if (moved && !g_move.landedAt) {
        g_move.landedAt = GetTickCount64();  // (it shows a frame or two later)
        return;
    }
    if (moved && GetTickCount64() - g_move.landedAt < kLandMs) return;
    if (moved || age > kMoveTimeoutMs + 1500) EndMove(moved);
}

// Close (or, keep: fill the page and open as a tab). The card takes a picture of the page, Dia's window
// is hidden, and the card flies back into the link (or grows to the page); Dia closes its window only after
// that (heavy work on this thread, which would stall the animation if it ran meanwhile).
void Dismiss(int exit = 0) {  // 0 close, 1 open as tab, 2 split view
    Peek* p = g_peek;
    if (!p || p->phase == kClosing || p->phase == kExpanding) return;
    Log("peek: dismiss %d", exit);
    if (exit == 2) {
        // Split view: Dia opens a link Alt+clicked in a split beside your page. The preview goes at once and
        // the link is Alt+clicked where it was held.
        POINT at = p->origin, back;
        Destroy();
        GetCursorPos(&back);
        auto abs = [](POINT pt, DWORD flags) {
            INPUT in{};
            in.type = INPUT_MOUSE;
            in.mi.dx = (LONG)std::lround((pt.x - GetSystemMetrics(SM_XVIRTUALSCREEN)) * 65535.0 /
                                         (GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1));
            in.mi.dy = (LONG)std::lround((pt.y - GetSystemMetrics(SM_YVIRTUALSCREEN)) * 65535.0 /
                                         (GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1));
            in.mi.dwFlags = flags | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;
            in.mi.dwExtraInfo = kReplayTag;
            return in;
        };
        INPUT alt[2]{};
        alt[0].type = alt[1].type = INPUT_KEYBOARD;
        alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
        alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
        alt[0].ki.dwExtraInfo = alt[1].ki.dwExtraInfo = kReplayTag;
        INPUT in[6] = {alt[0], abs(at, 0), abs(at, MOUSEEVENTF_LEFTDOWN), abs(at, MOUSEEVENTF_LEFTUP), alt[1], abs(back, 0)};
        SendInput(6, in, sizeof(INPUT));
        return;
    }
    bool keep = exit == 1;
    p->phase = keep ? kExpanding : kClosing;
    KillTimer(g_msg, kTimerDia);
    GlanceClose c{p->widget ? PicturePage(p) : nullptr, keep, {}, keep ? PictureOf(p->dia, p->pageArea) : nullptr};
    if (IsWindow(p->dia)) GetWindowRect(p->dia, &c.area);
    if (g_ctl && p->backdrop) SendMessageW(g_ctl, GM_CLOSE, 0, (LPARAM)&c);
    else if (c.picture) DeleteObject(c.picture);
    if (IsWindow(p->win)) HideWindow(p->win);
    p->widget = nullptr;
    if (IsWindow(p->dia) && !keep) SetForegroundWindow(p->dia);
    if (keep && g_ctl && p->backdrop && IsWindow(p->win)) {
        // The tab moves at once, while the card fills the page (its window is kept: Destroy leaves it).
        HWND win = p->win, dia = p->dia;
        RECT area = p->pageArea;
        p->win = nullptr;
        Destroy(true);
        MoveTab(win, dia, area);
        return;
    }
    if (!g_ctl || !p->backdrop) Destroy();  // (the card isn't out yet: nothing to animate)
}

void Destroy(bool keepCard) {
    Peek* p = g_peek;
    if (!p || p->destroying) return;
    p->destroying = true;
    g_peek = nullptr;
    KillTimer(g_msg, kTimerDia);
    KillTimer(g_msg, kTimerInject);
    if (p->injectStep == 1 || p->injectStep == 2) SendShift(false);  // still holding Shift for Dia
    Ui_SetPeekWindow(nullptr);
    StopHiding();
    if (IsWindow(p->win)) {
        SetWindowLongPtrW(p->win, GWLP_HWNDPARENT, 0);  // (not destroyed with the backdrop)
        HideWindow(p->win);
        Cloak(p->win, true);
        PostMessageW(p->win, WM_CLOSE, 0, 0);  // Dia closes its window (and its tab)
    }
    if (g_ctl && !keepCard) SendMessageW(g_ctl, GM_END, 0, 0);
    if (IsWindow(p->dia)) SetForegroundWindow(p->dia);  // Dia puts focus back on its page
    delete p;
}

bool AwaitingDiaWindow() {
    return g_peek && !g_peek->win && g_peek->phase == kLoading && GetTickCount64() - g_peek->started < kDiaTimeoutMs;
}

// While a preview loads: after each message Dia's window handles (WinUI makes, frames, shows and moves it,
// and may replace its window procedure, so a subclass doesn't see all of it), it's sent back below the
// screen if moved up, and the hiding is put back, before the next frame is drawn.
LRESULT CALLBACK AfterHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_peek && g_peek->win && !g_peek->widget) {
        auto* m = reinterpret_cast<CWPRETSTRUCT*>(lp);
        if (m->hwnd == g_peek->win) {
            if (g_peek->parked && m->message == WM_WINDOWPOSCHANGED) {
                RECT wr;
                if (GetWindowRect(m->hwnd, &wr) && wr.top < kParkY / 2)
                    SetWindowPos(m->hwnd, nullptr, wr.left, wr.top + kParkY, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            switch (m->message) {
            case WM_CREATE:
            case WM_SHOWWINDOW:
            case WM_WINDOWPOSCHANGED:
            case WM_STYLECHANGED:
            case WM_NCCALCSIZE:
            case WM_ACTIVATE:
            case WM_DWMNCRENDERINGCHANGED:
            case WM_DWMCOMPOSITIONCHANGED:
            case WM_SETTEXT:
                KeepHidden(m->hwnd);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// The moment Windows shows or moves Dia's window (in this thread, inside the call): hide it right then.
void CALLBACK OnShow(HWINEVENTHOOK, DWORD, HWND w, LONG obj, LONG, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || !g_peek || w != g_peek->win || g_peek->widget) return;
    KeepHidden(w);
}

// Dia's new window for the link: made below the screen at the size that puts its page on the target,
// hidden as it's made (see AfterHook), kept from maximizing.
LRESULT CALLBACK CbtHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND && AwaitingDiaWindow()) {
        HWND h = (HWND)wp;
        auto* c = reinterpret_cast<CBT_CREATEWNDW*>(lp);
        if (!(c->lpcs->style & WS_CHILD) && ClassIs(h, L"WinUIDesktopWin32WindowClass")) {
            Peek* p = g_peek;
            p->win = h;
            if (!g_after) g_after = SetWindowsHookExW(WH_CALLWNDPROCRET, AfterHook, nullptr, GetCurrentThreadId());
            if (!g_showHook) {
                HMODULE self = nullptr;
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCWSTR)&OnShow, &self);
                g_showHook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE, self, OnShow,
                                             GetCurrentProcessId(), GetCurrentThreadId(), WINEVENT_INCONTEXT);
            }
            NoFrame(h);
            SetWindowSubclass(h, FixedProc, 1, 0);
            Ui_SetPeekWindow(h);  // (its toolbar is left as Dia lays it out: see ui.cpp)
            HideWindow(h);
            const RECT& t = p->target;
            c->lpcs->x = t.left - g_offset.dx;
            c->lpcs->y = t.top - g_offset.dy + kParkY;
            c->lpcs->cx = t.right - t.left + g_offset.dw;
            c->lpcs->cy = t.bottom - t.top + g_offset.dh;
            Log("peek: Dia window after %llu ms", GetTickCount64() - p->started);
        }
    } else if (code == HCBT_MINMAX && g_peek && (HWND)wp == g_peek->win) {
        int cmd = LOWORD(lp);
        if (cmd == SW_MAXIMIZE || cmd == SW_SHOWMAXIMIZED) return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void DiaTick() {
    Peek* p = g_peek;
    if (!p || p->phase == kClosing || p->phase == kExpanding || p->destroying) {
        KillTimer(g_msg, kTimerDia);
        return;
    }
    ULONGLONG age = GetTickCount64() - p->started;
    if (!p->win) {
        if (age > kNoWindowMs) {
            Log("peek: Dia opened no window for the link");
            MessageBeep(MB_OK);
            Dismiss();
        }
        return;
    }
    if (!IsWindow(p->win)) return Dismiss();  // closed from inside
    if (IsZoomed(p->win)) ShowWindow(p->win, SW_RESTORE);
    HWND page = PageHostOf(p->win);
    if (p->widget) {  // shown: keep it on the target (it may have moved: window resized)
        if (page && !AlignPage(p, page)) ClipToPage(p);
        return;
    }
    KeepHidden(p->win);
    OwnWindow(p);
    if (!page) {
        // Dia gives a new window its own size after making it: keep it at the size that puts its page on
        // the target (learned from the last one), so the page is laid out once, at its size.
        RECT t = p->target;
        if (p->parked) OffsetRect(&t, 0, kParkY);
        RECT want{t.left - g_offset.dx, t.top - g_offset.dy, t.right - g_offset.dx + g_offset.dw,
                  t.bottom - g_offset.dy + g_offset.dh}, wr;
        if (GetWindowRect(p->win, &wr) && !EqualRect(&wr, &want))
            SetWindowPos(p->win, nullptr, want.left, want.top, want.right - want.left, want.bottom - want.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        if (age > kDiaTimeoutMs) {
            Log("peek: Dia's window got no page");
            MessageBeep(MB_OK);
            Dismiss();
        }
        return;
    }
    if (!p->pageSeen) {
        p->pageSeen = true;
        Log("peek: page window after %llu ms", age);
    }
    p->aligns++;
    ULONGLONG now = GetTickCount64();
    if (p->parked && (HasTitle(p->win) || age > kRevealByMs) && AlignPage(p, page)) {
        // Past WinUI's default frame: onto the screen, still clipped to nothing, so the page draws there
        // (Dia doesn't draw a page it counts as off screen) before it's shown.
        p->parked = false;
        RECT wr;
        GetWindowRect(p->win, &wr);
        SetWindowPos(p->win, nullptr, wr.left, wr.top - kParkY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        p->placedAt = now;
        return;
    }
    if (p->parked) return;
    if (!AlignPage(p, page) && p->aligns <= 30) {
        p->placedAt = 0;
        return;
    }
    if (!p->placedAt) p->placedAt = now;
    // Shown once its page has been in place (no resize) for a moment: Dia has drawn it at its size by then
    // (earlier, it shows black or a blurred placeholder).
    if (now - p->placedAt >= kSettleMs || age > kRevealByMs) {
        if (p->cardDone) RevealPage(p, page);
        else p->ready = true;  // (shown when the card has flown out)
    }
}

// Shift+click for Dia in steps: Dia reads Shift when it handles the click, so Shift goes down first and
// stays down a moment. Then the card flies out (before, it would have caught the click meant for the link).
void InjectStep() {
    Peek* p = g_peek;
    if (!p || p->phase != kLoading) {
        KillTimer(g_msg, kTimerInject);
        return;
    }
    if (p->injectStep == 1) {
        INPUT in[2]{};
        in[0].type = in[1].type = INPUT_MOUSE;
        in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        in[0].mi.dwExtraInfo = in[1].mi.dwExtraInfo = kReplayTag;
        SendInput(2, in, sizeof(INPUT));
        p->injectStep = 2;
        return;
    }
    SendShift(false);
    p->injectStep = 3;
    KillTimer(g_msg, kTimerInject);
    double s = Scale(p->dia);
    int lw = (int)std::lround(kLinkWDip * s), lh = (int)std::lround(kLinkHDip * s);
    GlanceOpen o{p->dia, p->pageArea, p->target,
                 {p->origin.x - lw / 2, p->origin.y - lh / 2, p->origin.x + lw / 2, p->origin.y + lh / 2}, s, nullptr};
    SendMessageW(g_ctl, GM_OPEN, 0, (LPARAM)&o);
    p->backdrop = o.backdrop;
    if (!p->backdrop) Dismiss();
}

bool Open(HWND widget, POINT click, const std::wstring& url) {
    HWND page = GetAncestor(widget, GA_PARENT);
    HWND dia = GetAncestor(widget, GA_ROOT);
    if (!page || !dia || IsIconic(dia)) return false;
    RECT target, pr;
    if (!CardRectFor(page, dia, target, &pr)) return false;
    EnsureGlanceThread();
    if (!g_ctl) return false;
    if (!g_cbt) g_cbt = SetWindowsHookExW(WH_CBT, CbtHook, nullptr, GetCurrentThreadId());
    if (!g_cbt) return false;
    auto* p = new Peek;
    p->dia = dia;
    p->page = page;
    p->target = target;
    p->pageArea = pr;
    p->origin = click;
    p->url = url;
    g_peek = p;
    p->started = GetTickCount64();
    // Dia opens the link: Shift+click, the press and release at the link (the page never saw the real
    // press). The modifier-click triggers keep their keys down: those are released first.
    if (g_trigger != kHold) {
        INPUT up[3]{};
        int n = 0;
        if (g_ctrl) up[n].type = INPUT_KEYBOARD, up[n].ki.wVk = VK_CONTROL, up[n++].ki.dwFlags = KEYEVENTF_KEYUP;
        if (g_alt) up[n].type = INPUT_KEYBOARD, up[n].ki.wVk = VK_MENU, up[n++].ki.dwFlags = KEYEVENTF_KEYUP;
        if (n) SendInput(n, up, sizeof(INPUT));
    }
    SendShift(true);
    p->injectStep = 1;
    SetTimer(g_msg, kTimerInject, 25, nullptr);
    SetTimer(g_msg, kTimerDia, 15, nullptr);
    Log("peek: opening %ls", HostOf(url).c_str());
    return true;
}

// The page moved or changed size (window resized, sidebar toggled): follow it. Gone or minimized: close.
// Dia's own window activated (a click on its sidebar or toolbar): close.
void Follow() {
    Peek* p = g_peek;
    if (!p || p->destroying || p->phase == kClosing || p->phase == kExpanding) return;
    if (!IsWindow(p->page) || !IsWindowVisible(p->page) || !IsWindow(p->dia) || IsIconic(p->dia)) return Destroy();
    if (p->phase == kOpen && GetForegroundWindow() == p->dia) return Dismiss();  // Dia's own window clicked
    RECT target, pr;
    if (!CardRectFor(p->page, p->dia, target, &pr)) return Destroy();
    if (EqualRect(&target, &p->target)) return;
    p->target = target;
    p->pageArea = pr;
    GlanceOpen o{p->dia, pr, target, {}, Scale(p->dia), nullptr};
    if (g_ctl) SendMessageW(g_ctl, GM_MOVE, 0, (LPARAM)&o);
}

// ---------------------------------------------------------------- input

// Not a link after all: give the page its click back (the modifier keys are still held).
void ReplayClick() {
    bool upGone = g_upSwallowed.exchange(false);
    g_swallowUp = false;
    INPUT in[2]{};
    in[0].type = in[1].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    in[0].mi.dwExtraInfo = in[1].mi.dwExtraInfo = kReplayTag;
    SendInput(upGone ? 2 : 1, in, sizeof(INPUT));
}

void HandleClick() {
    std::wstring url = HoveredLink(g_clickWidget);
    if (url.empty()) return ReplayClick();
    g_swallowUp = false;
    g_upSwallowed = false;
    if (!Open(g_clickWidget, g_clickPt, url)) MessageBeep(MB_OK);
}

// Gives a held-back press (and its release, if that came already) back to the page.
void ReplayPress(bool withRelease) {
    KillTimer(g_msg, kTimerHold);
    INPUT in[2]{};
    in[0].type = in[1].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    in[0].mi.dwExtraInfo = in[1].mi.dwExtraInfo = kReplayTag;
    SendInput(withRelease ? 2 : 1, in, sizeof(INPUT));
}

// A plain press was held back: on a link, wait for the hold; anywhere else, give it back right away. Dia's
// hover text can lag the pointer: a press right after a move waits for it a moment.
void HandlePress() {
    if (g_hold != kChecking) return;
    std::wstring url = HoveredLink(g_holdWidget);
    if (url.empty() && IsWindow(g_holdWidget) && GetTickCount64() - g_lastMove < kHoverTextMs) {
        SetTimer(g_msg, kTimerPress, 30, nullptr);
        return;
    }
    int expected = kChecking;
    if (url.empty()) {
        if (g_hold.compare_exchange_strong(expected, kIdle)) ReplayPress(false);
        return;
    }
    g_holdUrl = url;
    if (g_hold.compare_exchange_strong(expected, kOnLink)) SetTimer(g_msg, kTimerHold, kHoldMs, nullptr);
}

// Held on a link for 400 ms: preview it. The page never saw the press, so nothing else happens.
void HandleHold() {
    int expected = kOnLink;
    if (!g_hold.compare_exchange_strong(expected, kIdle)) return;
    if (!IsWindow(g_holdWidget)) return ReplayPress(false);
    g_holdFired = true;  // its release is kept from the page
    if (!Open(g_holdWidget, g_holdPt, g_holdUrl)) {
        g_holdFired = false;
        MessageBeep(MB_OK);
        ReplayPress(false);
    }
}

LRESULT CALLBACK MsgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    try {
        if (msg == WM_PEEK_CLICK) {
            HandleClick();
        } else if (msg == WM_PEEK_ESC) {
            Dismiss((int)wp);
        } else if (msg == WM_PEEK_DISMISS) {
            Dismiss((int)wp);
        } else if (msg == WM_PEEK_CARD) {
            if (Peek* p = g_peek; p && p->phase == kLoading) {
                p->cardDone = true;
                if (p->ready)
                    if (HWND page = PageHostOf(p->win)) RevealPage(p, page);
            }
        } else if (msg == WM_PEEK_DONE) {
            if (wp) {  // (the card filled the page: the tab moves meanwhile, see Dismiss)
                if (g_peek) Destroy(true);
                if (!g_move.win && g_ctl) PostMessageW(g_ctl, GM_FADE, 0, 0);
            } else {
                Destroy();
            }
        } else if (msg == WM_TIMER && wp == kTimerTab) {
            MoveTick();
        } else if (msg == WM_PEEK_REVEAL) {
            ShowMovedPage();
        } else if (msg == WM_PEEK_PRESS) {
            HandlePress();
        } else if (msg == WM_TIMER && wp == kTimerPress) {
            KillTimer(h, kTimerPress);
            HandlePress();
        } else if (msg == WM_PEEK_REPLAY) {
            ReplayPress(wp != 0);
        } else if (msg == WM_TIMER && wp == kTimerDia) {
            DiaTick();
        } else if (msg == WM_TIMER && wp == kTimerInject) {
            InjectStep();
        } else if (msg == WM_TIMER && wp == kTimerHold) {
            KillTimer(h, kTimerHold);
            HandleHold();
        }
    } catch (...) {
    }
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_on) {
        auto* m = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        // (Our own injected input, and mouse input made from touch or pen, pass through untouched.)
        bool ours = m->dwExtraInfo == kReplayTag || (m->dwExtraInfo & 0xFFFFFF00) == 0xFF515700;
        if (wp == WM_MOUSEMOVE && !ours) g_lastMove = GetTickCount64();
        if (g_trigger == kHold && !ours) {
            int st = g_hold;
            if (wp == WM_LBUTTONDOWN) {
                if (st == kIdle && !g_peek && !g_shift && !g_ctrl && !g_alt && !g_win && DiaIsForeground()) {
                    if (HWND w = PageWidgetAt(m->pt)) {
                        g_holdPt = m->pt;
                        g_holdWidget = w;
                        g_holdFired = false;
                        g_hold = kChecking;
                        PostMessageW(g_msg, WM_PEEK_PRESS, 0, 0);
                        return 1;
                    }
                }
            } else if (wp == WM_LBUTTONUP) {
                if (g_holdFired.exchange(false)) return 1;
                if (st != kIdle && g_hold.exchange(kIdle) != kIdle) {
                    PostMessageW(g_msg, WM_PEEK_REPLAY, 1, 0);
                    return 1;
                }
            } else if (st != kIdle) {
                bool moved = wp == WM_MOUSEMOVE && (std::abs(m->pt.x - g_holdPt.x) > GetSystemMetrics(SM_CXDRAG) ||
                                                    std::abs(m->pt.y - g_holdPt.y) > GetSystemMetrics(SM_CYDRAG));
                if ((moved || wp != WM_MOUSEMOVE) && g_hold.exchange(kIdle) != kIdle)
                    PostMessageW(g_msg, WM_PEEK_REPLAY, 0, 0);
            }
        } else if (g_trigger != kHold && wp == WM_LBUTTONDOWN && !ours && !g_peek) {
            bool shift = g_shift, ctrl = g_ctrl, alt = g_alt;
            int t = g_trigger;
            bool match = !g_win && (t == kShift      ? shift && !ctrl && !alt
                                    : t == kCtrlShift ? shift && ctrl && !alt
                                                      : alt && !ctrl && !shift);
            if (match && DiaIsForeground()) {
                if (HWND w = PageWidgetAt(m->pt)) {
                    g_clickPt = m->pt;
                    g_clickWidget = w;
                    g_swallowUp = true;
                    g_upSwallowed = false;
                    PostMessageW(g_msg, WM_PEEK_CLICK, 0, 0);
                    return 1;
                }
            }
        } else if (g_trigger != kHold && wp == WM_LBUTTONUP && !ours && g_swallowUp) {
            g_swallowUp = false;
            g_upSwallowed = true;
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK KeyHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        bool injected = k->flags & LLKHF_INJECTED;
        DWORD vk = k->vkCode;
        std::atomic<bool>* mod = vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_SHIFT         ? &g_shift
                                 : vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_CONTROL ? &g_ctrl
                                 : vk == VK_LMENU || vk == VK_RMENU || vk == VK_MENU          ? &g_alt
                                 : vk == VK_LWIN || vk == VK_RWIN                             ? &g_win
                                                                                              : nullptr;
        if (mod && !(injected && (g_peek || k->dwExtraInfo == kReplayTag))) {  // (our own Shift isn't you)
            *mod = down;
        } else if (g_on && down && vk == VK_ESCAPE && g_peek && DiaIsForeground()) {
            PostMessageW(g_msg, WM_PEEK_ESC, 0, 0);
            return 1;
        } else if (g_on && down && (vk == 'W' || vk == 'O') && g_ctrl && !g_shift && !g_alt && g_peek &&
                   g_peek->phase == kOpen && DiaIsForeground()) {  // DiaTweaks' Close Ctrl+W, Expand Ctrl+O
            PostMessageW(g_msg, WM_PEEK_ESC, vk == 'O', 0);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void Install() {
    g_thread = GetCurrentThreadId();
    WNDCLASSW wc{};
    wc.lpfnWndProc = MsgProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DiaTweaksPeekMsg";
    RegisterClassW(&wc);
    g_msg = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&MouseHook, &self);
    g_mouse = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, self, 0);
    g_keys = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHook, self, 0);
    if (!g_mouse || !g_keys) Log("peek: hook failed %lu", GetLastError());
    else Log("peek: ready on thread %lu", g_thread);
}

}  // namespace

void Peek_BeforeMove() { BeforeMove(); }

// Called every engine tick on each UI thread. The hooks live on the first UI thread that sees the tweak on.
void Peek_Tick() {
    bool on = Engine_TweakOn(L"peek");
    g_on = on;
    std::wstring v = Engine_TweakValue(L"peek");
    g_trigger = v == L"alt" ? kAlt : v == L"ctrlShift" ? kCtrlShift : v == L"shift" ? kShift : kHold;
    // A key released while the hook missed it (lock screen, another desktop) must not stay "down".
    if (!g_peek) {
        if (!(GetAsyncKeyState(VK_SHIFT) & 0x8000)) g_shift = false;
        if (!(GetAsyncKeyState(VK_CONTROL) & 0x8000)) g_ctrl = false;
        if (!(GetAsyncKeyState(VK_MENU) & 0x8000)) g_alt = false;
        if (!(GetAsyncKeyState(VK_LWIN) & 0x8000) && !(GetAsyncKeyState(VK_RWIN) & 0x8000)) g_win = false;
    }
    if (!g_thread) {
        if (!on) return;
        Install();
    }
    if (GetCurrentThreadId() != g_thread) return;
    if (!on) {
        if (g_peek) Destroy();
        return;
    }
    Follow();
    static int n = 0;  // TEMP (research)
    if (++n % 10 == 0) {
        std::wstring f = Engine_Dir() + L"\\peek-probe.txt";
        if (GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(f.c_str());
            Ui_PeekProbe();
        }
    }
}
