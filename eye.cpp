// eye.cpp - realistic ASCII eye animation (ANSI escape codes, no dependencies)
// Build: g++ eye.cpp -o eye     Run: ./eye  (green mode: ./eye green)     Quit: Ctrl+C
#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

// Small platform layer: sleeping, terminal size, ANSI support on Windows.
// (MinGW builds often lack std::thread, so Windows uses Sleep() instead.)
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <thread>
#if defined(__has_include)
#if __has_include(<sys/ioctl.h>)
#include <sys/ioctl.h>
#define HAVE_IOCTL 1
#endif
#endif
#endif

namespace {
volatile std::sig_atomic_t running = 1;
void onSignal(int) { running = 0; }

constexpr int W = 61, H = 23;                            // canvas size in cells
constexpr double CX = (W - 1) / 2.0, CY = (H - 1) / 2.0;
constexpr double PI = 3.14159265358979323846;
constexpr double HALF_W = 29, UP = 17, LOW = 14, IRIS_R = 12;  // eye geometry (1 row = 2 units)
bool green = false;

double clamp01(double v) { return std::min(1.0, std::max(0.0, v)); }

// Brightness (0..1) -> character density and colour (grayscale or green ramp).
char glyph(double b) { return " .:-=+*#%@"[(int)(clamp01(b) * 9.99)]; }
int tone(double b) {
    static const int g[] = {22, 28, 34, 40, 46, 83, 120, 157, 194, 231};
    return green ? g[(int)(clamp01(b) * 9.99)] : 232 + (int)(clamp01(b) * 23.99);
}

int envInt(const char* n, int d) {
    const char* s = std::getenv(n);
    int v = s ? std::atoi(s) : 0;
    return v > 0 ? v : d;
}

void sleepMs(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
}

void termSize(int& cols, int& rows) {
    cols = envInt("COLUMNS", 80);
    rows = envInt("LINES", 24);
#if defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        cols = info.srWindow.Right - info.srWindow.Left + 1;
        rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    }
#elif defined(HAVE_IOCTL)
    winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }
#endif
}
}  // namespace

int main(int argc, char** argv) {
    green = argc > 1 && std::string(argv[1]) == "green";
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (GetConsoleMode(hOut, &mode)) SetConsoleMode(hOut, mode | 0x0004);  // enable ANSI escapes
#endif
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::mt19937 rng(std::random_device{}());
    auto rnd = [&](double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); };

    std::fputs("\x1b[?1049h\x1b[?25l\x1b[2J", stdout);  // alt screen, hide cursor, clear

    using clk = std::chrono::steady_clock;
    const auto t0 = clk::now();
    double gx = 0, gy = 0, tx = 0, ty = 0;               // current / target gaze offset
    double nextLook = 1.0, nextBlink = 2.5, blinkT = -1, prevT = 0;
    int lastL = -1, lastT = -1;

    while (running) {
        const double t = std::chrono::duration<double>(clk::now() - t0).count();
        const double dt = t - prevT;
        prevT = t;

        // Gaze: pick a new target now and then, ease toward it.
        if (t > nextLook) {
            tx = rnd(-12, 12);
            ty = rnd(-4, 4);
            nextLook = t + rnd(1.2, 3.0);
        }
        const double k = 1 - std::exp(-dt * 9);
        gx += (tx - gx) * k;
        gy += (ty - gy) * k;

        // Blink: openness 1 -> 0 -> 1 over ~0.26s, sometimes twice in a row.
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

        const double icx = CX + gx + 1.5 * std::sin(t * 1.3);   // iris centre (x cells)
        const double icy = gy + std::cos(t * 1.1);              // iris centre (y units)
        const double rp = 4.5 + 0.8 * std::sin(t * 1.7);        // pupil radius

        // Centre on screen.
        int cols, rows;
        termSize(cols, rows);
        const int left = std::max(1, (cols - W) / 2 + 1);
        const int top0 = std::max(1, (rows - H) / 2 + 1);

        std::string out;
        int last = -1;
        auto emit = [&](char c, int col) {
            if (c != ' ' && col != last) { out += "\x1b[38;5;" + std::to_string(col) + "m"; last = col; }
            out += c;
        };
        if (left != lastL || top0 != lastT) { out += "\x1b[2J"; lastL = left; lastT = top0; }

        for (int y = 0; y < H; ++y) {
            out += "\x1b[" + std::to_string(top0 + y) + ";" + std::to_string(left) + "H";
            const double py = (y - CY) * 2;
            for (int x = 0; x < W; ++x) {
                char ch = ' ';
                double b = 0;
                const double u = (x - CX) / HALF_W;

                if (std::fabs(u) <= 1) {
                    // Lid curves: the upper lid moves, the lower lid barely does.
                    const double s = std::pow(1 - u * u, 0.75);
                    const double top = -UP * s * open + 0.6 * LOW * s * (1 - open) + 0.5 * gy;
                    const double bot = LOW * s * (0.6 + 0.4 * open);
                    const double dT = py - top, dB = bot - py;

                    const double tol = open < 0.3 ? 1.0 : 0.0;   // keep a closed lid visible
                    if (dT >= -tol && dB >= -tol) {
                        if (dB < 2 && open < 0.3) {                          // closed-eye line (blink only)
                            ch = std::fabs(u) > 0.93 ? (x < CX ? '<' : '>') : '=';
                            b = 0.95;
                        } else {                                             // eyeball
                            const double shade = (0.10 + 0.90 * std::min(1.0, dT / 12)) *   // soft fade at the top
                                                 (0.10 + 0.90 * std::min(1.0, dB / 10)) *   // soft fade at the bottom
                                                 (1 - 0.5 * u * u);                         // round-eye falloff
                            const double dx = x - icx, dy = py - icy, r = std::hypot(dx, dy);
                            if (r < IRIS_R) {
                                const double a = std::atan2(dy, dx) + t * 0.25;
                                const double fib = 0.5 + 0.5 * std::sin(a * 14 + std::sin(a * 5) * 1.5);
                                double v = 0.28 + 0.32 * fib;                                // radial fibres
                                v += 0.25 * std::exp(-std::pow((r - rp * 1.7) / 2.0, 2));   // bright ring round pupil
                                if (r > IRIS_R - 3) v *= 1 - 0.75 * clamp01((r - (IRIS_R - 3)) / 3);  // dark outer ring
                                b = v * (0.5 + 0.5 * shade);
                                if (r < rp + 1) b *= 0.3;
                                if (r < rp) b = 0;                                           // pupil
                                if (std::hypot(dx + 4, dy + 6) < 2.2) b = 1.0;               // highlight
                                else if (std::hypot(dx - 4, dy - 5) < 1.3) b = 0.7;          // soft reflection
                            } else {
                                b = 0.9 * shade;                                             // white of the eye
                            }
                            ch = glyph(b);
                            if (b >= 0.99) ch = '@';
                        }
                    }
                }
                emit(ch, tone(b));
            }
        }
        out += "\x1b[0m";

        std::fwrite(out.data(), 1, out.size(), stdout);
        std::fflush(stdout);
        sleepMs(33);
    }

    std::fputs("\x1b[0m\x1b[?25h\x1b[?1049l", stdout);  // reset colours, show cursor, leave alt screen
    std::fflush(stdout);
    return 0;
}
