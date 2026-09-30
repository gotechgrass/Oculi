// eye_widget.cpp - a tiny floating ASCII "terminal" eye that lives in a corner of your screen.
// No border, always on top, follows your mouse, blinks. Made of real text characters.
//
// Build: g++ eye_widget.cpp -o eye_widget -mwindows -lgdi32 -luser32
// Run:   eye_widget                options: gray | box | a size number 3..8 (4 = default, 3 = smaller)
//        eye_widget 3 gray         "box" adds a dark backing so it reads well on bright windows
// Drag it with the left mouse button. Right-click the eye to close it.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>

namespace {
constexpr double PI = 3.14159265358979323846;
constexpr double HALF_W = 29, UP = 17, LOW = 14, IRIS_R = 12;   // eye geometry (units)
constexpr int GW = 37, GH = 13;                                 // character grid
constexpr double USTEP = 61.0 / GW, RSTEP = 2 * USTEP;          // units per column / per row

struct Rgb { int r, g, b; };
double clamp01(double v) { return std::min(1.0, std::max(0.0, v)); }

HWND hwnd;
HDC memDC;
HFONT font;
uint32_t* pixels = nullptr;
int CW = 4, CH = 8, WW, HH;   // character cell size in pixels, window size
bool gray = false, box = false;
char cells[GH][GW];
Rgb colors[GH][GW];

using clk = std::chrono::steady_clock;
clk::time_point t0, tPrev;
std::mt19937 rng;
double gx = 0, gy = 0, nextBlink = 2.5, blinkT = -1;
int frame = 0;

double rnd(double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); }

char glyph(double b) { return " .:-=+*#%@"[(int)(clamp01(b) * 9.99)]; }

Rgb colorFor(double b) {   // brightness -> terminal-like colour (gray, or green fading to white)
    b = clamp01(b);
    if (gray) { int c = (int)(45 + 210 * b); return {c, c, c}; }
    const int g = (int)(70 + 185 * b), w = b > 0.6 ? (int)((b - 0.6) / 0.4 * 215) : 0;
    return {w, g, std::min(255, w + (int)(0.15 * g))};
}

void computeGrid(double t, double dt, double eyeCx, double eyeCy) {
    // Gaze: look toward the mouse pointer.
    POINT cur;
    GetCursorPos(&cur);
    const double ddx = cur.x - eyeCx, ddy = cur.y - eyeCy, dist = std::hypot(ddx, ddy);
    double tx = 0, ty = 0;
    if (dist > 1) {
        const double reach = clamp01(dist / 300.0);
        tx = ddx / dist * 12 * reach;
        ty = ddy / dist * 5 * reach;
    }
    const double k = 1 - std::exp(-dt * 10);
    gx += (tx - gx) * k;
    gy += (ty - gy) * k;

    // Blink.
    double open = 0.96 + 0.04 * std::sin(t * 1.5);
    if (blinkT < 0 && t > nextBlink) blinkT = t;
    if (blinkT >= 0) {
        const double p = (t - blinkT) / 0.26;
        if (p >= 1) {
            blinkT = -1;
            nextBlink = t + (rnd(0, 1) < 0.25 ? 0.12 : rnd(2.5, 6.0));
        } else {
            open = 0.5 + 0.5 * std::cos(2 * PI * p);
        }
    }

    const double icx = gx + 1.5 * std::sin(t * 1.3), icy = gy + std::cos(t * 1.1);
    const double rp = 4.5 + 0.8 * std::sin(t * 1.7);

    for (int y = 0; y < GH; ++y) {
        const double py = (y + 0.5 - GH / 2.0) * RSTEP;
        for (int x = 0; x < GW; ++x) {
            const double ux = (x + 0.5 - GW / 2.0) * USTEP, u = ux / HALF_W;
            char ch = ' ';
            double b = 0;
            if (std::fabs(u) < 1) {
                const double s = std::pow(1 - u * u, 0.75);
                const double top = -UP * s * open + 0.6 * LOW * s * (1 - open) + 0.5 * gy;
                const double bot = LOW * s * (0.6 + 0.4 * open);
                const double dT = py - top, dB = bot - py;
                const double tol = open < 0.3 ? RSTEP * 0.6 : 0.0;   // keep a closed eye visible
                if (dT >= -tol && dB >= -tol) {
                    if (dB < RSTEP && open < 0.3) {                   // closed-eye line (blink only)
                        ch = '=';
                        b = 0.95;
                    } else {
                        const double shade = (0.10 + 0.90 * std::min(1.0, std::max(0.0, dT) / 12)) *
                                             (0.10 + 0.90 * std::min(1.0, std::max(0.0, dB) / 10)) * (1 - 0.5 * u * u);
                        const double dx = ux - icx, dy = py - icy, r = std::hypot(dx, dy);
                        if (r < IRIS_R) {
                            const double a = std::atan2(dy, dx) + t * 0.25;
                            const double fib = 0.5 + 0.5 * std::sin(a * 9 + std::sin(a * 4) * 1.5);
                            double v = 0.28 + 0.32 * fib;
                            v += 0.25 * std::exp(-std::pow((r - rp * 1.7) / 2.5, 2));
                            if (r > IRIS_R - 3) v *= 1 - 0.75 * clamp01((r - (IRIS_R - 3)) / 3);
                            b = v * (0.5 + 0.5 * shade) * 1.2;
                            if (r < rp + 1) b *= 0.3;
                            if (r < rp) b = 0;                                        // pupil
                            if (std::hypot(dx + 4, dy + 6) < 3.0) b = 1.0;           // highlight
                            else if (std::hypot(dx - 4, dy - 5) < 2.0) b = 0.7;      // soft reflection
                        } else {
                            b = 0.9 * shade;                                          // white of the eye
                        }
                        ch = glyph(b);
                        if (b >= 0.99) ch = '@';
                    }
                }
            }
            cells[y][x] = ch;
            colors[y][x] = colorFor(b);
        }
    }
}

void render() {
    const auto now = clk::now();
    const double t = std::chrono::duration<double>(now - t0).count();
    const double dt = std::chrono::duration<double>(now - tPrev).count();
    tPrev = now;

    RECT wr;
    GetWindowRect(hwnd, &wr);
    computeGrid(t, dt, (wr.left + wr.right) / 2.0, (wr.top + wr.bottom) / 2.0);

    std::memset(pixels, 0, (size_t)WW * HH * 4);
    for (int y = 0; y < GH; ++y)
        for (int x = 0; x < GW; ++x) {
            if (cells[y][x] == ' ') continue;
            const Rgb c = colors[y][x];
            SetTextColor(memDC, RGB(c.r, c.g, c.b));
            TextOutA(memDC, x * CW, y * CH, &cells[y][x], 1);
        }
    GdiFlush();

    // Text is drawn on black; turn brightness into transparency (black = see-through).
    for (int i = 0; i < WW * HH; ++i) {
        const uint32_t p = pixels[i];
        const uint32_t m = std::max({(p >> 16) & 255, (p >> 8) & 255, p & 255});
        uint32_t a = std::min<uint32_t>(255, m * 2);
        if (box) a = std::max<uint32_t>(a, 170);
        pixels[i] = (a << 24) | (p & 0x00FFFFFF);
    }

    POINT src = {0, 0};
    SIZE sz = {WW, HH};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, nullptr, nullptr, &sz, memDC, &src, 0, &bf, ULW_ALPHA);

    if (++frame % 60 == 0)  // stay on top of everything
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_TIMER: render(); return 0;
        case WM_NCHITTEST: return HTCAPTION;               // drag with the left mouse button
        case WM_NCRBUTTONUP: DestroyWindow(h); return 0;   // right-click closes it
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, m, w, l);
}
}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmd, int) {
    char args[256];
    std::strncpy(args, cmd ? cmd : "", sizeof args - 1);
    args[sizeof args - 1] = 0;
    for (char* tok = std::strtok(args, " "); tok; tok = std::strtok(nullptr, " ")) {
        if (!std::strcmp(tok, "gray") || !std::strcmp(tok, "grey")) gray = true;
        else if (!std::strcmp(tok, "box")) box = true;
        else if (std::atoi(tok) >= 3 && std::atoi(tok) <= 8) CW = std::atoi(tok);
    }
    CH = CW * 2;
    WW = GW * CW;
    HH = GH * CH;

    WNDCLASS wc = {};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "OculiWidget";
    RegisterClass(&wc);

    RECT work;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);   // bottom-right corner, above the taskbar
    hwnd = CreateWindowEx(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "OculiWidget", "Oculi", WS_POPUP,
                          work.right - WW - 20, work.bottom - HH - 20, WW, HH, nullptr, nullptr, inst, nullptr);

    memDC = CreateCompatibleDC(nullptr);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = WW;
    bmi.bmiHeader.biHeight = -HH;   // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    HBITMAP dib = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, (void**)&pixels, nullptr, 0);
    SelectObject(memDC, dib);
    font = CreateFontA(-(int)(CW * 1.8 + 0.5), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    SelectObject(memDC, font);
    SetBkMode(memDC, TRANSPARENT);

    t0 = tPrev = clk::now();
    rng.seed((unsigned)t0.time_since_epoch().count());
    render();
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetTimer(hwnd, 1, 33, nullptr);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    DeleteObject(font);
    DeleteObject(dib);
    DeleteDC(memDC);
    return 0;
}
