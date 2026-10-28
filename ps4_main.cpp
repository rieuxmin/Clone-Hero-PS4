// ============================================================================
// PS4 (OpenOrbis) Guitar Hero: Overdrive - 1920x1080P 60FPS Clone Hero Edition
// Modeled after Clone Hero 1080p60 UI & Gameplay (rWa7DXocU7E):
//   - Locked 60 FPS Software Rasterizer: CPU-Cached RAM Backbuffer + Pre-Baked
//     1080p Stage Templates + 1D Scanline-Span AA Ellipses + Pre-Baked Star LUTs
//     (Eliminates all uncached video-memory reads & per-pixel trig/polygon loops)
//   - 3D Elliptical Fret Receptors with Dark Center Pistons & Hit Flame Plumes
//   - 3D Beveled Strum Gems (Black Collar + White Cap), HOPO White Domes,
//     & Cyan-Outlined Star Power Star Gems
//   - Iconic Clone Hero Gunmetal Score Box (Comma-Formatted Score, Top Indigo
//     Song Progress Bar, Bottom Star Power Bar, Overlapping 5-Pointed Gold
//     Star Rating Emblem, & Attached 4-Digit Amber Combo Odometer)
//   - Interactive Tracklist, Difficulty Selector (Easy/Medium/Hard/Expert),
//     & OPTIONS Pause Menu with Section/Chapter Jump
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
extern "C" int nanosleep(const struct timespec*, struct timespec*);
#include <pthread.h>
#include <atomic>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/UserService.h>
#include <orbis/Pad.h>
#include <orbis/AudioOut.h>
extern "C" int sceSystemServiceHideSplashScreen(void);
#include "game.h"
#include "midi.h"
#include "font8x16.h"
#include "font_hd.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "mp4_player.h"
#include "ps4_audio_decoder.h"

// CRITICAL FOR 1080P MP4 & MULTI-STEM OPUS DECODING ON PS4 ORBIS OS:
// Unlock unlimited dynamic libc.prx heap growth (default PS4 libc heap is capped at 64MB!)
extern "C" {
    unsigned int sceLibcHeapExtendedAlloc = 1;
    size_t sceLibcHeapSize = (size_t)-1;
}

static const int W = 1920, H = 1080, RATE = 48000, GRAIN = 512;
static const double LEAD = 2.0;

#ifndef ORBIS_PAD_BUTTON_OPTIONS
#define ORBIS_PAD_BUTTON_OPTIONS 0x00000008
#endif
#ifndef ORBIS_PAD_BUTTON_R2
#define ORBIS_PAD_BUTTON_R2 0x00000200
#endif

// ---------------------------------------------------------------- Video & 1080p60 Fast Cached Rasterizer
static int g_video;
static OrbisKernelEqueue g_flipQ;
static uint32_t* g_fb[2] = {nullptr, nullptr};
// Fast CPU L1/L2-cached backbuffer & pre-baked 1080p stage templates (0=Normal, 1=StarPower, 2=Overstrum)
// Drawing into CPU-cached RAM avoids expensive uncached GPU memory reads during alpha blending!
static uint32_t* g_workFb = nullptr;
static uint32_t* g_stageCacheFb[3] = {nullptr, nullptr, nullptr};
static int g_cur = 0, g_frameId = 1;

static bool videoInit() {
    g_video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, 0);
    if (g_video < 0) return false;
    if (sceKernelCreateEqueue(&g_flipQ, "hero flip queue") < 0) return false;
    sceVideoOutAddFlipEvent(g_flipQ, g_video, 0);
    size_t fbBytes = (size_t)W * H * 4;
    size_t total = ((2 * fbBytes + 0x1FFFFF) / 0x200000) * 0x200000;
    off_t off = 0; void* mem = nullptr;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), total, 0x200000, 3, &off) < 0) return false;
    if (sceKernelMapDirectMemory(&mem, total, 0x33, 0, off, 0x200000) < 0) return false;
    g_fb[0] = (uint32_t*)mem;
    g_fb[1] = (uint32_t*)((char*)mem + fbBytes);

    // Allocate CPU-cached working backbuffer & 3 static stage caches via DirectMemory/mmap (saves 33.2MB of libc heap!)
    g_workFb = Mp4VideoPlayer::allocFrame1080p();
    for (int i = 0; i < 3; i++) {
        g_stageCacheFb[i] = Mp4VideoPlayer::allocFrame1080p();
    }
    if (!g_workFb || !g_stageCacheFb[0] || !g_stageCacheFb[1] || !g_stageCacheFb[2]) return false;

    OrbisVideoOutBufferAttribute attr;
    // CRITICAL: tilingMode must be 1 (TILED) as required by PS4 Orbis OS Display Compositor!
    // (tilingMode 0 causes a scrambled 'graphics mess' artifact on scanout)
    // 0x80000000 = ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8R8G8B8_SRGB, 1 = TILED, 0 = 16:9
    sceVideoOutSetBufferAttribute(&attr, 0x80000000, 1, 0, W, H, W);
    if (sceVideoOutRegisterBuffers(g_video, 0, (void**)g_fb, 2, &attr) != 0) return false;
    sceVideoOutSetFlipRate(g_video, 0); // 0 = Full 60 Hz VSync

    // Clear framebuffers to solid black
    memset(g_fb[0], 0, fbBytes);
    memset(g_fb[1], 0, fbBytes);
    memset(g_workFb, 0, fbBytes);
    return true;
}

static void flip() {
    // Single burst write from CPU-cached RAM to Write-Combined GPU memory
    memcpy(g_fb[g_cur], g_workFb, (size_t)W * H * 4);
    asm volatile("sfence" ::: "memory"); // Flush CPU Write-Combining write buffers to GDDR5
    sceVideoOutSubmitFlip(g_video, g_cur, ORBIS_VIDEO_OUT_FLIP_VSYNC, g_frameId);
    for (;;) {
        OrbisVideoOutFlipStatus st;
        sceVideoOutGetFlipStatus(g_video, &st);
        if (st.flipArg == g_frameId) break;
        OrbisKernelEvent evt; int cnt = 0;
        unsigned int timeoutUs = 25000; // 25ms timeout pointer
        if (sceKernelWaitEqueue(g_flipQ, &evt, 1, &cnt, &timeoutUs) != 0) {
            if (sceVideoOutGetFlipStatus(g_video, &st) == 0 && st.flipArg >= g_frameId) break;
            break; // Safe timeout fallback if hardware drops event
        }
    }
    g_frameId++; g_cur ^= 1;

    // CRITICAL: Dismiss PS4 OS startup loading / blue splash screen so rendered frame is displayed!
    static bool s_splashHidden = false;
    if (!s_splashHidden) {
        sceSystemServiceHideSplashScreen();
        s_splashHidden = true;
    }
}

static inline uint32_t C(int r, int g, int b) {
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline uint32_t lerpCol(uint32_t c0, uint32_t c1, int t256) {
    if (t256 <= 0) return c0;
    if (t256 >= 256) return c1;
    uint32_t rb0 = c0 & 0x00FF00FFu;
    uint32_t g0  = c0 & 0x0000FF00u;
    uint32_t rb1 = c1 & 0x00FF00FFu;
    uint32_t g1  = c1 & 0x0000FF00u;
    uint32_t rb = rb0 + (((rb1 - rb0) * (uint32_t)t256) >> 8);
    uint32_t g  = g0  + (((g1  - g0)  * (uint32_t)t256) >> 8);
    return 0xFF000000u | (rb & 0x00FF00FFu) | (g & 0x0000FF00u);
}

static inline void blendPx(uint32_t* fb, int x, int y, uint32_t col, int alpha256) {
    if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H || alpha256 <= 0) return;
    uint32_t& dst = fb[(size_t)y * W + x];
    if (alpha256 >= 255) { dst = col; return; }
    dst = lerpCol(dst, col, alpha256);
}

static void rect(uint32_t* fb, int x, int y, int w, int h, uint32_t c) {
    int x0 = std::max(0, x), y0 = std::max(0, y);
    int x1 = std::min(W, x + w), y1 = std::min(H, y + h);
    if (x0 >= x1 || y0 >= y1) return;
    for (int py = y0; py < y1; py++) {
        uint32_t* row = fb + (size_t)py * W;
        for (int px = x0; px < x1; px++) row[px] = c;
    }
}

static void rectAlpha(uint32_t* fb, int x, int y, int w, int h, uint32_t c, int alpha256) {
    if (alpha256 <= 0) return;
    if (alpha256 >= 255) { rect(fb, x, y, w, h, c); return; }
    int x0 = std::max(0, x), y0 = std::max(0, y);
    int x1 = std::min(W, x + w), y1 = std::min(H, y + h);
    if (x0 >= x1 || y0 >= y1) return;
    for (int py = y0; py < y1; py++) {
        uint32_t* row = fb + (size_t)py * W;
        for (int px = x0; px < x1; px++) {
            row[px] = lerpCol(row[px], c, alpha256);
        }
    }
}

static void gradRectV(uint32_t* fb, int x, int y, int w, int h, uint32_t topCol, uint32_t botCol) {
    if (h <= 0 || w <= 0) return;
    int x0 = std::max(0, x), x1 = std::min(W, x + w);
    int y0 = std::max(0, y), y1 = std::min(H, y + h);
    for (int py = y0; py < y1; py++) {
        int t = ((py - y) * 256) / std::max(1, h - 1);
        uint32_t c = lerpCol(topCol, botCol, t);
        uint32_t* row = fb + (size_t)py * W;
        for (int px = x0; px < x1; px++) row[px] = c;
    }
}

static void borderRect(uint32_t* fb, int x, int y, int w, int h, int thick, uint32_t fillCol, uint32_t borderCol) {
    rect(fb, x, y, w, h, borderCol);
    rect(fb, x + thick, y + thick, w - 2 * thick, h - 2 * thick, fillCol);
}

static void drawSlantedButton(uint32_t* fb, int x0, int y0, int w, int h, float slantSlope, uint32_t bgCol, uint32_t borderCol, int borderThick) {
    if (h <= 0 || w <= 0) return;
    int yStart = std::max(0, y0);
    int yEnd = std::min(H, y0 + h);
    for (int py = yStart; py < yEnd; py++) {
        int xOffset = (int)((float)(py - y0) * slantSlope);
        int sx = x0 - xOffset;
        int ex = sx + w;
        int sxClamped = std::max(0, sx);
        int exClamped = std::min(W, ex);
        if (sxClamped >= exClamped) continue;
        uint32_t* row = fb + (size_t)py * W;
        bool isTopBottomBorder = (py < y0 + borderThick) || (py >= y0 + h - borderThick);
        for (int px = sxClamped; px < exClamped; px++) {
            if (isTopBottomBorder || px < sx + borderThick || px >= ex - borderThick) {
                row[px] = borderCol;
            } else {
                row[px] = bgCol;
            }
        }
    }
}

static void hline(uint32_t* fb, int x0, int x1, int y, int thick, uint32_t c) {
    rect(fb, x0, y - thick / 2, x1 - x0, thick, c);
}

static void vline(uint32_t* fb, int x, int y0, int y1, int thick, uint32_t c) {
    rect(fb, x - thick / 2, y0, thick, y1 - y0, c);
}

static void hlineAlpha(uint32_t* fb, int x0, int x1, int y, int thick, uint32_t c, int alpha256) {
    rectAlpha(fb, x0, y - thick / 2, x1 - x0, thick, c, alpha256);
}

// ---------------------------------------------------------------- 1D Scanline-Span Anti-Aliased Ellipse (25x Faster)
// Computes horizontal span once per scanline row instead of per-pixel 2D sqrt!
static void fillEllipseAA(uint32_t* fb, float cx, float cy, float rx, float ry, uint32_t col, int maxAlpha = 256) {
    if (rx <= 0.5f || ry <= 0.5f || maxAlpha <= 0) return;
    int y0 = std::max(0, (int)std::floor(cy - ry));
    int y1 = std::min(H - 1, (int)std::ceil(cy + ry));
    float invRy = 1.0f / ry;

    for (int py = y0; py <= y1; py++) {
        float dy = (py + 0.5f - cy) * invRy;
        float dy2 = dy * dy;
        if (dy2 >= 1.0f) continue;

        float halfSpan = rx * std::sqrt(1.0f - dy2);
        float xf0 = cx - halfSpan;
        float xf1 = cx + halfSpan;

        int xL = (int)std::floor(xf0);
        int xR = (int)std::floor(xf1);
        uint32_t* row = fb + (size_t)py * W;

        if (xL == xR) {
            if ((unsigned)xL < (unsigned)W) {
                int a = (int)((xf1 - xf0) * maxAlpha);
                row[xL] = lerpCol(row[xL], col, a);
            }
            continue;
        }

        // Anti-aliased left endpoint
        if ((unsigned)xL < (unsigned)W) {
            float covL = 1.0f - (xf0 - (float)xL);
            int aL = (int)(std::clamp(covL, 0.0f, 1.0f) * maxAlpha);
            row[xL] = lerpCol(row[xL], col, aL);
        }
        // Anti-aliased right endpoint
        if ((unsigned)xR < (unsigned)W) {
            float covR = xf1 - (float)xR;
            int aR = (int)(std::clamp(covR, 0.0f, 1.0f) * maxAlpha);
            row[xR] = lerpCol(row[xR], col, aR);
        }

        // Contiguous SIMD-friendly solid interior span
        int span0 = std::max(0, xL + 1);
        int span1 = std::min(W - 1, xR - 1);
        if (maxAlpha >= 255) {
            for (int px = span0; px <= span1; px++) {
                row[px] = col;
            }
        } else {
            for (int px = span0; px <= span1; px++) {
                row[px] = lerpCol(row[px], col, maxAlpha);
            }
        }
    }
}

// ---------------------------------------------------------------- Pre-Baked Star LUTs (Zero Per-Pixel Trig at Runtime)
// 1. 64x64 LUT for Star Power Gem Wings (0=outside, 1=fillCol, 2=cyanOutline)
static uint8_t g_spWingLut[64][64];
// 2. 128x128 LUT for 5-Pointed Gold Star Score Box Emblem (radius=56, center=64,64)
//    0=outside, 1=outerGlow, 2=outerGoldTop, 3=outerGoldBot, 4=innerDarkRing, 5=innerGoldOutline, 6=centerChamber
static uint8_t g_goldStarLut[128][128];

static void initStarLookups() {
    // Precompute 64x64 Star Power Gem Wing LUT
    for (int iy = 0; iy < 64; iy++) {
        float ny = ((iy + 0.5f) - 32.0f) / 30.0f;
        for (int ix = 0; ix < 64; ix++) {
            float nx = ((ix + 0.5f) - 32.0f) / 30.0f;
            float r = std::sqrt(nx * nx + ny * ny);
            if (r > 1.05f) {
                g_spWingLut[iy][ix] = 0;
                continue;
            }
            float ang = std::atan2(ny, nx);
            float wave = 0.72f + 0.28f * std::cos(ang * 6.0f);
            if (r <= wave) {
                g_spWingLut[iy][ix] = (r >= wave - 0.14f) ? 2 : 1;
            } else {
                g_spWingLut[iy][ix] = 0;
            }
        }
    }

    // Precompute 128x128 5-Pointed Gold Star Emblem LUT
    const float outerR = 54.0f;
    const float innerR = outerR * 0.44f;
    float vx[10], vy[10];
    for (int i = 0; i < 10; i++) {
        float a = -1.5707963f + i * 0.6283185f;
        float rad = (i & 1) ? innerR : outerR;
        vx[i] = std::cos(a) * rad;
        vy[i] = std::sin(a) * rad;
    }

    auto insideStar = [&](float px, float py, float scale) -> bool {
        float sx = px / scale, sy = py / scale;
        bool c = false;
        for (int i = 0, j = 9; i < 10; j = i++) {
            if (((vy[i] > sy) != (vy[j] > sy)) &&
                (sx < (vx[j] - vx[i]) * (sy - vy[i]) / (vy[j] - vy[i] + 1e-6f) + vx[i])) {
                c = !c;
            }
        }
        return c;
    };

    for (int iy = 0; iy < 128; iy++) {
        float dy = (float)iy - 64.0f;
        for (int ix = 0; ix < 128; ix++) {
            float dx = (float)ix - 64.0f;
            float dist = std::sqrt(dx * dx + dy * dy);
            if (dist > outerR + 6.0f) {
                g_goldStarLut[iy][ix] = 0;
                continue;
            }
            if (insideStar(dx, dy, 1.0f)) {
                if (!insideStar(dx, dy, 0.80f)) {
                    g_goldStarLut[iy][ix] = (dy < 0.0f) ? 2 : 3;
                } else if (!insideStar(dx, dy, 0.66f)) {
                    g_goldStarLut[iy][ix] = 4;
                } else if (!insideStar(dx, dy, 0.56f)) {
                    g_goldStarLut[iy][ix] = 5;
                } else {
                    g_goldStarLut[iy][ix] = 6;
                }
            } else if (insideStar(dx, dy, 1.10f)) {
                g_goldStarLut[iy][ix] = 1;
            } else {
                g_goldStarLut[iy][ix] = 0;
            }
        }
    }
}

// Fast LUT-driven Star Power Gem Wings (Zero trig/sqrt at runtime)
static void drawStarPowerWingsAA(uint32_t* fb, float cx, float cy, float rx, float ry, uint32_t fillCol, uint32_t cyanOutline) {
    float wingRx = rx * 1.42f;
    float wingRy = ry * 1.35f;
    if (wingRx < 2.0f || wingRy < 2.0f) return;

    int x0 = std::max(0, (int)(cx - wingRx));
    int x1 = std::min(W - 1, (int)(cx + wingRx));
    int y0 = std::max(0, (int)(cy - wingRy));
    int y1 = std::min(H - 1, (int)(cy + wingRy));

    int stepX_fp = (int)((30.0f * 65536.0f) / wingRx);
    int stepY_fp = (int)((30.0f * 65536.0f) / wingRy);
    int startX_fp = (int)(((x0 - cx) / wingRx * 30.0f + 32.0f) * 65536.0f);
    int curY_fp   = (int)(((y0 - cy) / wingRy * 30.0f + 32.0f) * 65536.0f);

    for (int py = y0; py <= y1; py++, curY_fp += stepY_fp) {
        int iy = curY_fp >> 16;
        if ((unsigned)iy >= 64u) continue;
        const uint8_t* lutRow = g_spWingLut[iy];
        uint32_t* row = fb + (size_t)py * W;
        int curX_fp = startX_fp;
        for (int px = x0; px <= x1; px++, curX_fp += stepX_fp) {
            int ix = curX_fp >> 16;
            if ((unsigned)ix >= 64u) continue;
            uint8_t v = lutRow[ix];
            if (v == 1) row[px] = fillCol;
            else if (v == 2) row[px] = cyanOutline;
        }
    }
}

// Fast LUT-driven 5-Pointed Gold Star Badge (Takes < 0.02ms per frame)
static void drawGoldStarEmblem(uint32_t* fb, int cx, int cy, double fillPct, uint32_t goldCol, uint32_t brightGold, uint32_t innerDark) {
    uint32_t chamberFill = lerpCol(innerDark, goldCol, 95);
    uint32_t ringDark = C(18, 20, 26);
    int fillThresholdY = 64 + 27 - (int)(fillPct * 54.0);

    for (int iy = 0; iy < 128; iy++) {
        int py = cy + iy - 64;
        if ((unsigned)py >= (unsigned)H) continue;
        const uint8_t* lutRow = g_goldStarLut[iy];
        uint32_t* row = fb + (size_t)py * W;
        for (int ix = 0; ix < 128; ix++) {
            uint8_t code = lutRow[ix];
            if (!code) continue;
            int px = cx + ix - 64;
            if ((unsigned)px >= (unsigned)W) continue;
            switch (code) {
                case 1: row[px] = lerpCol(row[px], goldCol, 75); break;
                case 2: row[px] = brightGold; break;
                case 3: row[px] = goldCol; break;
                case 4: row[px] = ringDark; break;
                case 5: row[px] = goldCol; break;
                case 6: row[px] = (iy >= fillThresholdY) ? chamberFill : innerDark; break;
            }
        }
    }
}

// ---------------------------------------------------------------- High-Resolution 8x16 Proportional Anti-Aliased Font Engine
static inline int getGlyphAdvance(char ch) {
    uint8_t idx = (uint8_t)ch;
    return (int)g_font8x16_adv[idx];
}

static inline int getTextWidth(const char* str, int scale = 1, int maxChars = 999) {
    if (!str) return 0;
    if (scale < 1) scale = 1;
    int w = 0;
    int count = 0;
    for (const char* p = str; *p && count < maxChars; p++, count++) {
        uint8_t idx = (uint8_t)*p;
        w += (int)g_font8x16_adv[idx] * scale;
    }
    return w;
}

// Draw string with optional pixel width limit (auto-truncates with "..." if text exceeds maxPxWidth)
static void drawTextRaw(uint32_t* fb, int x, int y, int scale, const char* str, uint32_t col, int maxPxWidth = 99999) {
    if (!str) return;
    if (scale < 1) scale = 1;

    // Check if string exceeds maxPxWidth; if so, find cutoff for "..."
    int totalW = getTextWidth(str, scale);
    char buf[256];
    const char* renderStr = str;
    if (totalW > maxPxWidth && maxPxWidth > 28) {
        int dotW = getTextWidth("...", scale);
        int curW = 0;
        int i = 0;
        for (; str[i] && i < 250; i++) {
            int cw = (int)g_font8x16_adv[(uint8_t)str[i]] * scale;
            if (curW + cw + dotW > maxPxWidth) break;
            buf[i] = str[i];
            curW += cw;
        }
        buf[i++] = '.'; buf[i++] = '.'; buf[i++] = '.'; buf[i] = '\0';
        renderStr = buf;
    }

    int curX = x;
    for (const char* p = renderStr; *p; p++) {
        uint8_t idx = (uint8_t)*p;
        const uint8_t* glyph = g_font8x16[idx];
        int adv = (int)g_font8x16_adv[idx];

        if (scale == 1) {
            // Native 1:1 pixel rendering: razor sharp, zero pixelation!
            for (int r = 0; r < 16; r++) {
                int py = y + r;
                if ((unsigned)py >= (unsigned)H) continue;
                uint8_t bits = glyph[r];
                if (!bits) continue;
                uint32_t* row = fb + (size_t)py * W;
                for (int c = 0; c < 8; c++) {
                    if (bits & (128 >> c)) {
                        int px = curX + c;
                        if ((unsigned)px < (unsigned)W) row[px] = col;
                    }
                }
            }
        } else {
            // Crisp integer-scaled pixel rendering (zero blurry edge smearing!)
            for (int r = 0; r < 16; r++) {
                uint8_t bits = glyph[r];
                if (!bits) continue;
                for (int c = 0; c < 8; c++) {
                    if (bits & (128 >> c)) {
                        int px0 = curX + c * scale;
                        int py0 = y + r * scale;
                        rect(fb, px0, py0, scale, scale, col);
                    }
                }
            }
        }
        curX += adv * scale;
        if (curX >= W) break;
    }
}

static void drawText(uint32_t* fb, int x, int y, int scale, const char* str, uint32_t col, int maxPxWidth = 99999) {
    if (!str) return;
    if (scale >= 2) {
        int sh = scale / 2;
        drawTextRaw(fb, x + sh, y + sh, scale, str, C(4, 8, 16), maxPxWidth);
    }
    drawTextRaw(fb, x, y, scale, str, col, maxPxWidth);
}

static void drawTextCentered(uint32_t* fb, int cx, int y, int scale, const char* str, uint32_t col, int maxPxWidth = 99999) {
    if (!str) return;
    int totalW = getTextWidth(str, scale);
    if (totalW > maxPxWidth) totalW = maxPxWidth;
    drawText(fb, cx - totalW / 2, y, scale, str, col, maxPxWidth);
}

static void drawTextRight(uint32_t* fb, int rightX, int y, int scale, const char* str, uint32_t col, int maxPxWidth = 99999) {
    if (!str) return;
    int totalW = getTextWidth(str, scale);
    if (totalW > maxPxWidth) totalW = maxPxWidth;
    drawText(fb, rightX - totalW, y, scale, str, col, maxPxWidth);
}

static std::string formatTimeMMSS(double sec) {
    if (sec < 0) sec = 0;
    int total = (int)sec;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d:%02d", total / 60, total % 60);
    return std::string(buf);
}

static void formatCommaScoreBuf(int val, char* outBuf, size_t outSize) {
    char raw[32];
    int len = snprintf(raw, sizeof(raw), "%d", std::max(0, val));
    size_t pos = 0;
    for (int i = 0; i < len && pos + 2 < outSize; i++) {
        if (i > 0 && (len - i) % 3 == 0) outBuf[pos++] = ',';
        outBuf[pos++] = raw[i];
    }
    outBuf[pos] = '\0';
}

// ---------------------------------------------------------------- Build Version & 21 Custom Clone Hero Highway Skins
#ifndef BUILD_VERSION_STR
#define BUILD_VERSION_STR "v1.22"
#endif
#ifndef BUILD_FULL_STR
#define BUILD_FULL_STR    "BUILD v1.22"
#endif

// ============================================================================
// High-Resolution Pre-Baked Official-Style "CLONE HERO" 3D Metallic Emblem
// ============================================================================
// Inspired by the official Clone Hero logo:
//   - Large 820x320 1080p emblem with forward-slanted italic geometry
//   - Bilinear SDF contour upsampling from TrueType glyphs (~138px & ~148px tall)
//   - 10-layer 3D block extrusion in deep navy/electric cyan
//   - Crisp dark obsidian outer bevel stroke + top chrome specular lip
//   - Vertical icy-white to sky-blue horizon metallic reflection on letter faces
//   - Flanking 3-bar slanted speed wings beside "HERO"
static const int CH_LOGO_W = 820;
static const int CH_LOGO_H = 320;
static uint32_t* g_chLogoPixels = nullptr;
static uint8_t*  g_chLogoAlpha  = nullptr;

static float sampleGlyphBilinear(const GlyphHD& g, float gx, float gy) {
    if (!g.data || gx < -0.5f || gy < -0.5f || gx >= (float)g.w - 0.5f || gy >= (float)g.h - 0.5f) return 0.0f;
    float x = std::clamp(gx, 0.0f, (float)(g.w - 1));
    float y = std::clamp(gy, 0.0f, (float)(g.h - 1));
    int x0 = (int)std::floor(x);
    int y0 = (int)std::floor(y);
    int x1 = std::min(x0 + 1, (int)g.w - 1);
    int y1 = std::min(y0 + 1, (int)g.h - 1);
    float fx = x - (float)x0;
    float fy = y - (float)y0;
    float v00 = (float)g.data[y0 * g.w + x0] * (1.0f / 255.0f);
    float v10 = (float)g.data[y0 * g.w + x1] * (1.0f / 255.0f);
    float v01 = (float)g.data[y1 * g.w + x0] * (1.0f / 255.0f);
    float v11 = (float)g.data[y1 * g.w + x1] * (1.0f / 255.0f);
    float top = v00 + (v10 - v00) * fx;
    float bot = v01 + (v11 - v01) * fx;
    return top + (bot - top) * fy;
}

static void stampScaledItalicWordMask(float* mask, float* normYBuf, int bufW, int bufH,
                                      const char* word, int centerX, int topY, float scale, float italicShear, int letterSpacingPx) {
    int totalOrigW = 0;
    int len = (int)strlen(word);
    for (int i = 0; i < len; i++) {
        uint8_t c = (uint8_t)word[i];
        totalOrigW += (c < 128 && g_fontHD_Large[c].data) ? g_fontHD_Large[c].w : 20;
    }
    int scaledTotalW = (int)((float)totalOrigW * scale) + std::max(0, len - 1) * letterSpacingPx;
    int startX = centerX - scaledTotalW / 2;
    int scaledH = (int)(39.0f * scale);

    int curX = startX;
    for (int i = 0; i < len; i++) {
        uint8_t c = (uint8_t)word[i];
        if (c >= 128 || !g_fontHD_Large[c].data) continue;
        const GlyphHD& g = g_fontHD_Large[c];
        int gwScaled = (int)((float)g.w * scale);

        for (int dy = 0; dy < scaledH; dy++) {
            int py = topY + dy;
            if (py < 0 || py >= bufH) continue;
            float gy = ((float)dy + 0.5f) / scale - 0.5f;
            // Only active glyph rows (gy in 5..34 of 39) map to 0..1 vertical gradient
            float vNorm = std::clamp((gy - 5.0f) / 27.0f, 0.0f, 1.0f);
            float shearShift = ((float)(scaledH / 2 - dy)) * italicShear;

            for (int dx = -24; dx < gwScaled + 24; dx++) {
                int px = curX + dx;
                if (px < 0 || px >= bufW) continue;
                float gx = ((float)dx - shearShift + 0.5f) / scale - 0.5f;
                float rawA = sampleGlyphBilinear(g, gx, gy);
                if (rawA > 0.05f) {
                    // SDF contour sharpening for razor-crisp 1080p vector edges at 3.6x magnification
                    float sdf = std::clamp((rawA - 0.36f) / 0.22f, 0.0f, 1.0f);
                    sdf = sdf * sdf * (3.0f - 2.0f * sdf);
                    size_t idx = (size_t)py * bufW + px;
                    if (sdf > mask[idx]) {
                        mask[idx] = sdf;
                        normYBuf[idx] = vNorm;
                    }
                }
            }
        }
        curX += gwScaled + letterSpacingPx;
    }
}

static void stampWingBarMask(float* mask, float* normYBuf, int bufW, int bufH,
                             int x0, int x1, int y0, int barH, float italicShear) {
    for (int dy = 0; dy < barH; dy++) {
        int py = y0 + dy;
        if (py < 0 || py >= bufH) continue;
        float vNorm = (float)dy / (float)std::max(1, barH - 1);
        float shift = ((float)(barH / 2 - dy)) * italicShear;
        float leftEdge  = (float)x0 + shift;
        float rightEdge = (float)x1 + shift;
        int minX = std::max(0, (int)std::floor(leftEdge - 2.0f));
        int maxX = std::min(bufW - 1, (int)std::ceil(rightEdge + 2.0f));
        for (int px = minX; px <= maxX; px++) {
            float covL = std::clamp((float)px - leftEdge + 1.0f, 0.0f, 1.0f);
            float covR = std::clamp(rightEdge - (float)px + 1.0f, 0.0f, 1.0f);
            float cov = std::min(covL, covR);
            if (cov > 0.01f) {
                size_t idx = (size_t)py * bufW + px;
                if (cov > mask[idx]) {
                    mask[idx] = cov;
                    normYBuf[idx] = vNorm;
                }
            }
        }
    }
}

static void initOfficialCloneHeroLogo() {
    if (g_chLogoPixels) return;
    size_t count = (size_t)CH_LOGO_W * CH_LOGO_H;
    g_chLogoPixels = (uint32_t*)calloc(count, sizeof(uint32_t));
    g_chLogoAlpha  = (uint8_t*)calloc(count, sizeof(uint8_t));
    float* faceMask = (float*)calloc(count, sizeof(float));
    float* normYBuf = (float*)calloc(count, sizeof(float));
    if (!g_chLogoPixels || !g_chLogoAlpha || !faceMask || !normYBuf) {
        if (faceMask) free(faceMask);
        if (normYBuf) free(normYBuf);
        return;
    }

    const int cx = CH_LOGO_W / 2;
    // Stamp "CLONE" (top row, large 3.45x scale ~135px tall, bold forward slant)
    stampScaledItalicWordMask(faceMask, normYBuf, CH_LOGO_W, CH_LOGO_H, "CLONE", cx + 6, 8, 3.45f, 0.22f, 4);
    // Stamp "HERO" (bottom row, heroic 3.70x scale ~145px tall, bold forward slant)
    stampScaledItalicWordMask(faceMask, normYBuf, CH_LOGO_W, CH_LOGO_H, "HERO",  cx + 2, 134, 3.70f, 0.22f, 6);

    // Stamp Iconic Flanking Speed-Wing Bars on Left & Right of "HERO"
    stampWingBarMask(faceMask, normYBuf, CH_LOGO_W, CH_LOGO_H, 52,  188, 168, 14, 0.22f);
    stampWingBarMask(faceMask, normYBuf, CH_LOGO_W, CH_LOGO_H, 74,  184, 192, 12, 0.22f);
    stampWingBarMask(faceMask, normYBuf, CH_LOGO_W, CH_LOGO_H, 98,  180, 214, 10, 0.22f);

    stampWingBarMask(faceMask, normYBuf, CH_LOGO_W, CH_LOGO_H, 634, 770, 168, 14, 0.22f);
    stampWingBarMask(faceMask, normYBuf, CH_LOGO_W, CH_LOGO_H, 638, 748, 192, 12, 0.22f);
    stampWingBarMask(faceMask, normYBuf, CH_LOGO_W, CH_LOGO_H, 642, 724, 214, 10, 0.22f);

    // Pass 1: Soft outer drop shadow + 10-layer 3D metallic block extrusion + dark bevel outline
    for (int y = 0; y < CH_LOGO_H; y++) {
        for (int x = 0; x < CH_LOGO_W; x++) {
            size_t idx = (size_t)y * CH_LOGO_W + x;
            float fCov = faceMask[idx];

            // Check 3px dark obsidian outer stroke around the front face
            float strokeMax = 0.0f;
            for (int sy = -3; sy <= 3; sy++) {
                int ny = y + sy;
                if (ny < 0 || ny >= CH_LOGO_H) continue;
                for (int sx = -3; sx <= 3; sx++) {
                    if (sx * sx + sy * sy > 11) continue;
                    int nx = x + sx;
                    if (nx < 0 || nx >= CH_LOGO_W) continue;
                    float m = faceMask[(size_t)ny * CH_LOGO_W + nx];
                    if (m > strokeMax) strokeMax = m;
                }
            }

            // Check 10-layer 3D extrusion along (-dx, -dy) -> projects down-right (+dx, +dy)
            int hitDepth = 0;
            for (int d = 1; d <= 10; d++) {
                int ex = x - d;
                int ey = y - d - (d / 2);
                if (ex >= 0 && ey >= 0 && faceMask[(size_t)ey * CH_LOGO_W + ex] > 0.35f) {
                    hitDepth = d;
                    break;
                }
            }

            // Check ambient drop shadow at (-12, -18)
            float dropShadow = 0.0f;
            if (x >= 12 && y >= 18) {
                dropShadow = faceMask[(size_t)(y - 18) * CH_LOGO_W + (x - 12)];
            }

            uint32_t outCol = C(0, 0, 0);
            int outAlpha = 0;

            if (dropShadow > 0.05f) {
                outCol = C(2, 6, 14);
                outAlpha = (int)(dropShadow * 155.0f);
            }

            if (hitDepth > 0) {
                int shadeT = ((10 - hitDepth) * 255) / 10;
                uint32_t extCol = lerpCol(C(10, 32, 64), C(38, 128, 198), shadeT);
                outCol = extCol;
                outAlpha = 250;
            }

            if (strokeMax > 0.15f) {
                uint32_t stkCol = C(8, 18, 36);
                int stkA = (int)(std::clamp(strokeMax * 1.3f, 0.0f, 1.0f) * 255.0f);
                outCol = (outAlpha > 0) ? lerpCol(outCol, stkCol, stkA) : stkCol;
                outAlpha = std::max(outAlpha, stkA);
            }

            if (fCov > 0.02f) {
                float vn = normYBuf[idx];
                uint32_t faceCol;
                // Authentic Clone Hero metallic chrome horizon gradient
                if (vn < 0.48f) {
                    int t = (int)((vn / 0.48f) * 255.0f);
                    faceCol = lerpCol(C(255, 255, 255), C(186, 230, 253), t);
                } else if (vn < 0.55f) {
                    int t = (int)(((vn - 0.48f) / 0.07f) * 255.0f);
                    faceCol = lerpCol(C(186, 230, 253), C(42, 158, 232), t);
                } else {
                    int t = (int)(((vn - 0.55f) / 0.45f) * 255.0f);
                    faceCol = lerpCol(C(56, 189, 248), C(238, 250, 255), t);
                }

                // Bright top-left inner bevel highlight
                bool isTopBevel = (y >= 2 && faceMask[(size_t)(y - 2) * CH_LOGO_W + x] < 0.35f) ||
                                  (x >= 2 && faceMask[(size_t)y * CH_LOGO_W + (x - 2)] < 0.35f);
                if (isTopBevel) {
                    faceCol = lerpCol(faceCol, C(255, 255, 255), 210);
                }

                int fAlpha = (int)(fCov * 255.0f);
                outCol = (outAlpha > 0) ? lerpCol(outCol, faceCol, fAlpha) : faceCol;
                outAlpha = std::max(outAlpha, fAlpha);
            }

            g_chLogoPixels[idx] = outCol;
            g_chLogoAlpha[idx]  = (uint8_t)std::clamp(outAlpha, 0, 255);
        }
    }

    free(faceMask);
    free(normYBuf);
}

static void drawOfficialCloneHeroLogo(uint32_t* fb, int centerX, int topY) {
    if (!g_chLogoPixels) initOfficialCloneHeroLogo();
    if (!g_chLogoPixels || !g_chLogoAlpha) return;

    int startX = centerX - CH_LOGO_W / 2;
    for (int dy = 0; dy < CH_LOGO_H; dy++) {
        int py = topY + dy;
        if ((unsigned)py >= (unsigned)H) continue;
        uint32_t* dstRow = fb + (size_t)py * W;
        const uint32_t* srcCol = g_chLogoPixels + (size_t)dy * CH_LOGO_W;
        const uint8_t*  srcAlp = g_chLogoAlpha  + (size_t)dy * CH_LOGO_W;

        for (int dx = 0; dx < CH_LOGO_W; dx++) {
            uint8_t a = srcAlp[dx];
            if (a == 0) continue;
            int px = startX + dx;
            if ((unsigned)px >= (unsigned)W) continue;
            dstRow[px] = (a >= 250) ? srcCol[dx] : lerpCol(dstRow[px], srcCol[dx], a);
        }
    }
}

static const int HIT_Y = 935, HORIZON_Y = 395, TOP_W = 330, BOT_W = 840;
static const int HUD_X = 1180, HUD_Y = 392, HUD_W = 308, HUD_H = 86;
static const int ODO_W = 282, ODO_H = 48, ODO_X = HUD_X + 18, ODO_Y = HUD_Y + HUD_H - 2;

// ============================================================================
// CUSTOM UI & STAGE COLOR THEME ENGINE (4 Customizable Slots + HSV Color Wheel)
// ============================================================================
// Slots:
//   0: Background Primary   (Top gradient / stage horizon sky)
//   1: Background Secondary (Bottom gradient / card surface fill)
//   2: Primary Trim         (Active borders, highlights, progress accents)
//   3: Secondary Trim       (Secondary panel borders, badges, odometer accents)
static const char* g_themeSlotNames[4] = {
    "Background Primary",
    "Background Secondary",
    "Primary Trim",
    "Secondary Trim"
};

static int g_activeColorSlot = 0;
static int g_themePresetIdx  = 0;
static const int NUM_THEME_PRESETS = 6;
static const char* g_themePresetNames[NUM_THEME_PRESETS] = {
    "Clone Hero Classic Slate",
    "Guitar Hero III Inferno",
    "Rock Band Neon Stage",
    "YARG Cyber Violet",
    "Midnight Emerald",
    "Custom HSV Palette"
};

// Per-slot HSV values: Hue (0..359), Saturation (0..100), Value/Brightness (0..100)
static int g_themeSlotH[4] = { 212, 216, 199, 45  };
static int g_themeSlotS[4] = { 84,  88,  78,  92  };
static int g_themeSlotV[4] = { 22,  11,  97,  96  };

static uint32_t g_themeBgPrimary     = 0xFF081C34u;
static uint32_t g_themeBgSecondary   = 0xFF030C18u;
static uint32_t g_themeTrimPrimary   = 0xFF38BDF8u;
static uint32_t g_themeTrimSecondary = 0xFFFACC15u;

static bool g_menuBgVideoEnabled = true;
static bool g_flubSoundEnabled   = true;

static uint32_t hsvToRgbCol(int hDeg, int sPct, int vPct) {
    float h = (float)(((hDeg % 360) + 360) % 360);
    float s = std::clamp((float)sPct / 100.0f, 0.0f, 1.0f);
    float v = std::clamp((float)vPct / 100.0f, 0.0f, 1.0f);
    float c = v * s;
    float x = c * (1.0f - (float)std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float r1 = 0.0f, g1 = 0.0f, b1 = 0.0f;
    if (h < 60.0f)       { r1 = c; g1 = x; b1 = 0; }
    else if (h < 120.0f) { r1 = x; g1 = c; b1 = 0; }
    else if (h < 180.0f) { r1 = 0; g1 = c; b1 = x; }
    else if (h < 240.0f) { r1 = 0; g1 = x; b1 = c; }
    else if (h < 300.0f) { r1 = x; g1 = 0; b1 = c; }
    else                 { r1 = c; g1 = 0; b1 = x; }
    return C((int)((r1 + m) * 255.0f), (int)((g1 + m) * 255.0f), (int)((b1 + m) * 255.0f));
}

static void recomputeThemeColorsFromHsv() {
    g_themeBgPrimary     = hsvToRgbCol(g_themeSlotH[0], g_themeSlotS[0], g_themeSlotV[0]);
    g_themeBgSecondary   = hsvToRgbCol(g_themeSlotH[1], g_themeSlotS[1], g_themeSlotV[1]);
    g_themeTrimPrimary   = hsvToRgbCol(g_themeSlotH[2], g_themeSlotS[2], g_themeSlotV[2]);
    g_themeTrimSecondary = hsvToRgbCol(g_themeSlotH[3], g_themeSlotS[3], g_themeSlotV[3]);
}

static void applyThemePreset(int presetIdx) {
    g_themePresetIdx = ((presetIdx % NUM_THEME_PRESETS) + NUM_THEME_PRESETS) % NUM_THEME_PRESETS;
    if (g_themePresetIdx == 0) { // Clone Hero Classic Slate
        int h[4] = {212, 216, 199, 45};  int s[4] = {84, 88, 78, 92};  int v[4] = {22, 11, 97, 96};
        for (int i = 0; i < 4; i++) { g_themeSlotH[i] = h[i]; g_themeSlotS[i] = s[i]; g_themeSlotV[i] = v[i]; }
    } else if (g_themePresetIdx == 1) { // Guitar Hero III Inferno
        int h[4] = {10,  6,   28,  46};  int s[4] = {86, 92, 95, 96};  int v[4] = {24, 11, 96, 98};
        for (int i = 0; i < 4; i++) { g_themeSlotH[i] = h[i]; g_themeSlotS[i] = s[i]; g_themeSlotV[i] = v[i]; }
    } else if (g_themePresetIdx == 2) { // Rock Band Neon Stage
        int h[4] = {248, 255, 322, 190}; int s[4] = {78, 86, 86, 92};  int v[4] = {24, 12, 96, 96};
        for (int i = 0; i < 4; i++) { g_themeSlotH[i] = h[i]; g_themeSlotS[i] = s[i]; g_themeSlotV[i] = v[i]; }
    } else if (g_themePresetIdx == 3) { // YARG Cyber Violet
        int h[4] = {276, 282, 292, 174}; int s[4] = {80, 88, 82, 88};  int v[4] = {23, 10, 96, 94};
        for (int i = 0; i < 4; i++) { g_themeSlotH[i] = h[i]; g_themeSlotS[i] = s[i]; g_themeSlotV[i] = v[i]; }
    } else if (g_themePresetIdx == 4) { // Midnight Emerald
        int h[4] = {162, 168, 152, 44};  int s[4] = {84, 90, 80, 92};  int v[4] = {20, 10, 92, 96};
        for (int i = 0; i < 4; i++) { g_themeSlotH[i] = h[i]; g_themeSlotS[i] = s[i]; g_themeSlotV[i] = v[i]; }
    }
    recomputeThemeColorsFromHsv();
}

// Interactive 2D HSV Color Square (Saturation X vs Value/Brightness Y) + Hue Rainbow Slider Card
static void drawHsvColorSquareCard(uint32_t* fb, int cardX, int cardY, int cardW, int cardH) {
    borderRect(fb, cardX, cardY, cardW, cardH, 2, lerpCol(g_themeBgSecondary, C(10, 16, 28), 128), g_themeTrimPrimary);
    drawTextHDCentered(fb, cardX + cardW / 2, cardY + 10, "INTERACTIVE COLOR SQUARE & PALETTE", g_themeTrimPrimary, false);

    // 4 Color Swatch Pills across top of card
    const char* shortSlotLabels[4] = { "BG 1", "BG 2", "TRIM 1", "TRIM 2" };
    uint32_t slotCols[4] = { g_themeBgPrimary, g_themeBgSecondary, g_themeTrimPrimary, g_themeTrimSecondary };
    for (int s = 0; s < 4; s++) {
        int sx = cardX + 16 + s * 104;
        int sy = cardY + 40;
        bool isAct = (s == g_activeColorSlot);
        borderRect(fb, sx, sy, 96, 34, isAct ? 3 : 1, slotCols[s], isAct ? C(255, 255, 255) : C(75, 85, 99));
        drawTextCentered(fb, sx + 48, sy + 10, 1, shortSlotLabels[s], C(255, 255, 255));
    }

    int curH = g_themeSlotH[g_activeColorSlot];
    int curS = g_themeSlotS[g_activeColorSlot];
    int curV = g_themeSlotV[g_activeColorSlot];

    // 2D Color Square (cardX + 16, cardY + 86, width 156, height 156)
    // Horizontal axis = Saturation (0% to 100%), Vertical axis = Brightness / Value (100% at top down to 0% at bottom)
    const int sqX = cardX + 16;
    const int sqY = cardY + 86;
    const int sqW = 156;
    const int sqH = 156;

    borderRect(fb, sqX - 2, sqY - 2, sqW + 4, sqH + 4, 2, C(6, 8, 14), C(148, 163, 184));
    for (int py = 0; py < sqH; py++) {
        int yPos = sqY + py;
        if ((unsigned)yPos >= (unsigned)H) continue;
        int vVal = 100 - (py * 100) / std::max(1, sqH - 1);
        uint32_t* row = fb + (size_t)yPos * W;
        for (int px = 0; px < sqW; px++) {
            int sVal = (px * 100) / std::max(1, sqW - 1);
            int xPos = sqX + px;
            if ((unsigned)xPos < (unsigned)W) {
                row[xPos] = hsvToRgbCol(curH, sVal, vVal);
            }
        }
    }

    // Crosshair reticle at active (curS, curV) inside 2D Color Square
    int retX = sqX + (curS * (sqW - 1)) / 100;
    int retY = sqY + ((100 - curV) * (sqH - 1)) / 100;
    fillEllipseAA(fb, (float)retX, (float)retY, 7.5f, 7.5f, C(0, 0, 0));
    fillEllipseAA(fb, (float)retX, (float)retY, 5.5f, 5.5f, C(255, 255, 255));
    fillEllipseAA(fb, (float)retX, (float)retY, 3.0f, 3.0f, slotCols[g_activeColorSlot]);

    // Rainbow Hue Slider & Precision Saturation/Brightness Sliders on Right Side
    int barX = cardX + 186;
    int barW = cardW - 202;
    const char* barTitles[3] = { "HUE (COLOR)", "SATURATION", "BRIGHTNESS" };
    int barVals[3] = { curH, curS, curV };
    int barMax[3]  = { 359, 100, 100 };

    for (int b = 0; b < 3; b++) {
        int by = cardY + 86 + b * 52;
        char lbl[48];
        snprintf(lbl, sizeof(lbl), "%s: %d%s", barTitles[b], barVals[b], (b == 0) ? " deg" : "%");
        drawText(fb, barX, by, 1, lbl, C(226, 232, 240));

        int trackY = by + 18;
        borderRect(fb, barX - 2, trackY - 2, barW + 4, 16, 1, C(10, 14, 22), C(100, 116, 139));
        for (int dx = 0; dx < barW; dx++) {
            int pct = (dx * barMax[b]) / std::max(1, barW - 1);
            uint32_t c = (b == 0) ? hsvToRgbCol(pct, 95, 95)
                       : (b == 1) ? hsvToRgbCol(curH, pct, std::max(30, curV))
                                  : hsvToRgbCol(curH, curS, pct);
            vline(fb, barX + dx, trackY, trackY + 12, 1, c);
        }
        int thumbX = barX + (barVals[b] * (barW - 1)) / std::max(1, barMax[b]);
        borderRect(fb, thumbX - 4, trackY - 3, 8, 20, 2, slotCols[g_activeColorSlot], C(255, 255, 255));
    }

    // Active Slot Name, Color Swatch & RGB Hex Readout at bottom of card
    uint32_t actCol = slotCols[g_activeColorSlot];
    char hexBuf[96];
    snprintf(hexBuf, sizeof(hexBuf), "%s  (#%02X%02X%02X)  [HSV: %d deg, %d%%, %d%%]",
             g_themeSlotNames[g_activeColorSlot],
             (actCol >> 16) & 0xFF, (actCol >> 8) & 0xFF, actCol & 0xFF,
             curH, curS, curV);
    drawTextHDCentered(fb, cardX + cardW / 2, cardY + cardH - 34, hexBuf, g_themeTrimSecondary, false, cardW - 24);
}

// ============================================================================
// REDESIGNED CLONE HERO SCORE, STAR POWER & COMBO ODOMETER BOX HUD
// ============================================================================
// Matches Clone Hero's signature right-side HUD:
//   - Brushed gunmetal score chassis with top song progress bar & bottom SP bar
//   - Overlapping 5-pointed Gold Star rating badge with smooth vertical fill
//   - Attached lower compartment with Multiplier (1x..8x), 10-note streak progress
//     arc/dots, Full Combo (FC) gold crown ring, and 4-cell digital combo odometer
static void drawCloneHeroScoreBoxHUD(uint32_t* fb, const Game& g, const Chart& chart, double now) {
    uint32_t trimMain = g.spActive ? C(56, 189, 248) : g_themeTrimPrimary;
    uint32_t trimSec  = g.spActive ? C(165, 243, 252) : g_themeTrimSecondary;
    uint32_t chassisTop = lerpCol(C(30, 35, 46), g_themeBgPrimary, 65);
    uint32_t chassisBot = lerpCol(C(14, 17, 24), g_themeBgSecondary, 75);

    // 1. Main Upper Score Chassis (HUD_X, HUD_Y, HUD_W, HUD_H)
    rectAlpha(fb, HUD_X - 4, HUD_Y - 4, HUD_W + 8, HUD_H + 8, C(2, 4, 10), 185);
    borderRect(fb, HUD_X, HUD_Y, HUD_W, HUD_H, 2, chassisBot, trimMain);
    gradRectV(fb, HUD_X + 2, HUD_Y + 2, HUD_W - 4, HUD_H - 4, chassisTop, chassisBot);
    hline(fb, HUD_X + 3, HUD_X + HUD_W - 3, HUD_Y + 2, 1, lerpCol(trimMain, C(255, 255, 255), 120));

    // Top Song Progress Bar inside upper bevel lip
    double songPct = (chart.duration > 0.0) ? std::clamp(now / chart.duration, 0.0, 1.0) : 0.0;
    int progX = HUD_X + 52;
    int progY = HUD_Y + 8;
    int progW = HUD_W - 64;
    borderRect(fb, progX, progY, progW, 8, 1, C(8, 11, 18), C(45, 55, 72));
    int progFill = (int)((progW - 2) * songPct);
    if (progFill > 0) {
        rect(fb, progX + 1, progY + 1, progFill, 6, C(129, 140, 248));
    }
    fillEllipseAA(fb, (float)(progX + 1 + progFill), (float)(progY + 4), 5.0f, 5.0f, C(224, 231, 255));

    // Main Comma-Formatted Score Readout (Right-aligned inside score chamber)
    char scoreFormatted[32];
    formatCommaScoreBuf(g.score, scoreFormatted, sizeof(scoreFormatted));
    drawTextHDRight(fb, HUD_X + HUD_W - 14, HUD_Y + 21, scoreFormatted, C(248, 250, 252), true, HUD_W - 66);

    // Bottom Star Power / Overdrive Bar inside score chassis
    double spPct = std::clamp(g.starPower / 100.0, 0.0, 1.0);
    int spX = HUD_X + 52;
    int spY = HUD_Y + HUD_H - 18;
    int spW = HUD_W - 64;
    borderRect(fb, spX, spY, spW, 10, 1, C(8, 11, 18), (g.starPower >= 50.0 || g.spActive) ? C(56, 189, 248) : C(52, 62, 78));
    // 50% Activation threshold tick mark
    vline(fb, spX + spW / 2, spY + 1, spY + 9, 1, C(148, 163, 184));
    int spFill = (int)((spW - 2) * spPct);
    if (spFill > 0) {
        uint32_t spColL = g.spActive ? C(14, 165, 233) : (g.starPower >= 50.0 ? C(6, 182, 212) : C(217, 119, 6));
        uint32_t spColR = g.spActive ? C(186, 230, 253) : (g.starPower >= 50.0 ? C(103, 232, 249) : C(250, 204, 21));
        for (int dx = 0; dx < spFill; dx++) {
            uint32_t c = lerpCol(spColL, spColR, (dx * 255) / std::max(1, spW));
            vline(fb, spX + 1 + dx, spY + 1, spY + 9, 1, c);
        }
    }

    // 2. Lower Attached Clone Hero Multiplier & 4-Digit Combo Odometer Box
    rectAlpha(fb, ODO_X - 3, ODO_Y, ODO_W + 6, ODO_H + 5, C(2, 4, 10), 185);
    borderRect(fb, ODO_X, ODO_Y, ODO_W, ODO_H, 2, C(10, 13, 20), trimMain);
    gradRectV(fb, ODO_X + 2, ODO_Y + 2, ODO_W - 4, ODO_H - 4, C(22, 27, 38), C(10, 12, 18));

    // Left Multiplier Compartment (ODO_X + 4 .. ODO_X + 96)
    int multBoxX = ODO_X + 5;
    int multBoxW = 92;
    borderRect(fb, multBoxX, ODO_Y + 5, multBoxW, ODO_H - 10, 1,
               g.spActive ? C(8, 42, 68) : C(14, 18, 28),
               g.spActive ? C(56, 189, 248) : C(48, 58, 76));

    // Full Combo (FC) Gold Indicator Dot + Multiplier Readout
    bool isFullCombo = (g.misses == 0 && g.overstrums == 0);
    if (isFullCombo) {
        fillEllipseAA(fb, (float)(multBoxX + 12), (float)(ODO_Y + 16), 4.5f, 4.5f, C(250, 204, 21));
    }
    char multStr[16];
    snprintf(multStr, sizeof(multStr), "%dx", g.activeMultiplier());
    uint32_t multCol = g.spActive ? C(103, 232, 249) : (g.baseMultiplier() == 4 ? trimSec : C(255, 255, 255));
    drawTextHDCentered(fb, multBoxX + multBoxW / 2 + (isFullCombo ? 4 : 0), ODO_Y + 4, multStr, multCol, false);

    // 10-Step Multiplier Streak Progress Dots along bottom of Multiplier Compartment
    int streakStep = (g.combo >= 30) ? 10 : (g.combo % 10);
    for (int s = 0; s < 10; s++) {
        int dotX = multBoxX + 7 + s * 8;
        int dotY = ODO_Y + ODO_H - 12;
        bool lit = (s < streakStep);
        rect(fb, dotX, dotY, 6, 4, lit ? (g.spActive ? C(56, 189, 248) : trimSec) : C(32, 38, 50));
    }

    // Right 4-Digit Recessed Digital Combo Odometer (ODO_X + 102 .. ODO_X + ODO_W - 6)
    int odoStartX = ODO_X + 102;
    int odoTotalW = ODO_W - 108;
    int cellW = odoTotalW / 4;
    int clampedCombo = std::min(9999, g.combo);
    char rawDigits[8];
    snprintf(rawDigits, sizeof(rawDigits), "%04d", clampedCombo);
    bool seenNonZero = false;

    for (int d = 0; d < 4; d++) {
        int cx0 = odoStartX + d * cellW;
        borderRect(fb, cx0 + 1, ODO_Y + 5, cellW - 2, ODO_H - 10, 1, C(6, 8, 14), C(34, 42, 56));
        hline(fb, cx0 + 2, cx0 + cellW - 3, ODO_Y + 6, 1, C(2, 4, 8));

        char ch = rawDigits[d];
        if (ch != '0') seenNonZero = true;
        bool isActiveDigit = seenNonZero || (d == 3 && clampedCombo > 0);
        char singleChar[2] = { ch, '\0' };
        uint32_t digCol = isActiveDigit
            ? (g.spActive ? C(103, 232, 249) : trimSec)
            : C(42, 50, 66);
        drawTextHDCentered(fb, cx0 + cellW / 2, ODO_Y + 9, singleChar, digCol, false);
    }

    // 3. Overlapping 5-Pointed Gold Star Rating Emblem on Top-Left Corner of Score Box
    double starProgress = 0.0;
    int curStars = g.getStarRating(&starProgress);
    int starCx = HUD_X + 4;
    int starCy = HUD_Y + 34;
    drawGoldStarEmblem(fb, starCx, starCy, starProgress, C(202, 138, 4), C(253, 224, 71), C(22, 25, 34));
    char starNumBuf[8];
    snprintf(starNumBuf, sizeof(starNumBuf), "%d", curStars);
    drawTextHDCentered(fb, starCx, starCy - 14, starNumBuf, C(255, 255, 255), false);

    // Subtle Star Power Ready Prompt below odometer when >= 50%
    if (!g.spActive && g.starPower >= 50.0) {
        drawTextHD(fb, ODO_X + 6, ODO_Y + ODO_H + 8, "STAR POWER READY! [R2]", C(56, 189, 248), false);
    }
}

static const int NUM_HIGHWAYS = 21;
struct HighwaySkinDef {
    const char* name;
    const char* fileMain;
    const char* fileAlt;
    const char* fileOrig;
};

static const HighwaySkinDef g_highwaySkins[NUM_HIGHWAYS] = {
    { "VelocityColor",     "VelocityColor.jpg",     "velocitycolor.jpg",     "VelocityColor_7d0fef2b.jpg" },
    { "mA.I.nia",          "mA.I.nia.jpg",          "mA.I.nia.png",          "mA.I.nia_77db3120.png" },
    { "Electric",          "Electric.jpg",          "electric.jpg",          "electric_b7cf0828.jpg" },
    { "Floral-2",          "Floral-2.jpg",          "floral-2.jpg",          "Floral-2_4306e828.jpg" },
    { "Floral-1",          "Floral-1.jpg",          "floral-1.jpg",          "Floral-1_62771e7f.jpg" },
    { "Transparent",       "Transparent.png",       "transparent.png",       "Transparent_f5997ba3.png" },
    { "Rainbow_Border",    "Rainbow_Border.jpg",    "Rainbow_Border.png",    "Rainbow_Border_ae17cfd5.png" },
    { "Pink_Border",       "Pink_Border.jpg",       "pink_border.jpg",       "Pink_Border_2de4a07a.jpg" },
    { "Cracked",           "Cracked.jpg",           "cracked.jpg",           "Cracked_0cdc06dc.jpg" },
    { "Velocity",          "Velocity.jpg",          "velocity.jpg",          "Velocity_e80521f2.jpg" },
    { "SmokeHero",         "SmokeHero.jpg",         "smokehero.jpg",         "smokehero_a3e0c47d.jpg" },
    { "PinkLaser",         "PinkLaser.jpg",         "pnklzr.jpg",            "pnklzr_125d0d2b.jpg" },
    { "Christmas06",       "Christmas06.jpg",       "christmas06.jpg",       "Christmas06_1ef548bb.jpg" },
    { "RoyalCarpet",       "RoyalCarpet.jpg",       "royalcarpet.jpg",       "RoyalCarpet_3d8a3075.jpg" },
    { "Overdose",          "Overdose.jpg",          "overdose.jpg",          "Overdose_4659dbc7.jpg" },
    { "Cosmic-3",          "Cosmic-3.jpg",          "cosmic-3.jpg",          "Cosmic-3_dcd4c585.jpg" },
    { "Cosmic-1",          "Cosmic-1.jpg",          "cosmic-1.jpg",          "Cosmic-1_2f1c635f.jpg" },
    { "Cosmic-2",          "Cosmic-2.jpg",          "cosmic-2.jpg",          "Cosmic-2_db57466a.jpg" },
    { "Metallic",          "Metallic.jpg",          "metallic.jpg",          "Metallic_02eeec5b.jpg" },
    { "FlowerPower",       "FlowerPower.jpg",       "flowerpower.jpg",       "FlowerPower_81645027.jpg" },
    { "FatTony-Fretboard", "FatTony.jpg",           "FatTony-Fretboard.jpg", "FatTony-Fretboard_6586f517.jpg" }
};

static bool loadHighwayTextureFile(int skinIdx, unsigned char** outRgba, int* outW, int* outH) {
    *outRgba = nullptr; *outW = 0; *outH = 0;
    if (skinIdx < 0 || skinIdx >= NUM_HIGHWAYS || skinIdx == 5) return false; // 5 = Transparent

    const char* dirs[] = {
        "/app0/assets/highways/",
        "/data/GH/highways/",
        "/data/GH_highways/",
        "/mnt/usb0/GH/highways/",
        "/mnt/usb0/GH_highways/"
    };
    const HighwaySkinDef& def = g_highwaySkins[skinIdx];
    const char* names[3] = { def.fileMain, def.fileAlt, def.fileOrig };

    for (const char* d : dirs) {
        for (int n = 0; n < 3; n++) {
            std::string full = std::string(d) + names[n];
            int w = 0, h = 0, ch = 0;
            unsigned char* data = stbi_load(full.c_str(), &w, &h, &ch, 4);
            if (data && w > 0 && h > 0) {
                *outRgba = data; *outW = w; *outH = h;
                return true;
            }
            if (data) stbi_image_free(data);
        }
    }
    return false;
}

// High-Precision Procedural Shader for all 21 Custom Highways (also used for instant preview & fallback)
static uint32_t sampleProceduralHighwaySkin(int skinIdx, float u, float v) {
    skinIdx = ((skinIdx % NUM_HIGHWAYS) + NUM_HIGHWAYS) % NUM_HIGHWAYS;
    switch (skinIdx) {
        case 0: // VelocityColor: Dark geometric constellation mesh with colored fret nodes
        case 9: { // Velocity: Monochrome dark geometric wireframe constellation mesh
            uint32_t bg = C(14, 15, 18);
            float gx = u * 4.0f, gy = v * 7.0f;
            float fx = gx - (float)std::floor(gx), fy = gy - (float)std::floor(gy);
            float d1 = (float)std::fabs(fx - fy);
            float d2 = (float)std::fabs(fx + fy - 1.0f);
            float d3 = (fx < 1.0f - fx) ? fx : (1.0f - fx);
            float d4 = (fy < 1.0f - fy) ? fy : (1.0f - fy);
            float m12 = (d1 < d2) ? d1 : d2;
            float m34 = (d3 < d4) ? d3 : d4;
            float minLine = (m12 < m34) ? m12 : m34;
            if (minLine < 0.032f) {
                int alpha = (int)((1.0f - minLine / 0.032f) * 78.0f);
                bg = lerpCol(bg, C(110, 118, 130), alpha);
            }
            float vxDist = (float)std::sqrt((fx - 0.5f) * (fx - 0.5f) + (fy - 0.5f) * (fy - 0.5f));
            if (vxDist < 0.085f) {
                int cellId = ((int)std::floor(gx) * 7 + (int)std::floor(gy) * 13) % 5;
                uint32_t nodeCols[5] = { C(34, 197, 94), C(239, 68, 68), C(250, 204, 21), C(59, 130, 246), C(249, 115, 22) };
                uint32_t nCol = (skinIdx == 0) ? nodeCols[cellId] : C(185, 195, 210);
                int a = (int)((1.0f - vxDist / 0.085f) * 235.0f);
                bg = lerpCol(bg, nCol, a);
            }
            return bg;
        }
        case 1: { // mA.I.nia: Neon rainbow digital rain matrix droplets on black
            float cols = 16.0f, rows = 28.0f;
            float cx = u * cols, cy = v * rows;
            int colIdx = (int)std::floor(cx);
            float fx = (cx - (float)std::floor(cx)) - 0.5f;
            float phase = (float)std::fmod(cy + (colIdx * 3.7f), 2.0f);
            float fy = (phase - 0.8f);
            float dist = (float)std::sqrt(fx * fx * 3.2f + fy * fy * 1.4f);
            if (dist < 0.48f) {
                uint32_t rainCol;
                if (v < 0.30f) rainCol = lerpCol(C(14, 165, 233), C(52, 211, 153), (int)(v / 0.30f * 255.0f));
                else if (v < 0.60f) rainCol = lerpCol(C(52, 211, 153), C(250, 204, 21), (int)((v - 0.30f) / 0.30f * 255.0f));
                else rainCol = lerpCol(C(244, 63, 94), C(217, 70, 239), (int)((v - 0.60f) / 0.40f * 255.0f));
                int a = (int)((1.0f - dist / 0.48f) * 245.0f);
                return lerpCol(C(4, 6, 16), rainCol, a);
            }
            return C(4, 6, 16);
        }
        case 2: { // Electric: Glowing electric blue lightning bolt on midnight black
            float boltCenter = (v < 0.48f) ? (0.85f - v * 1.15f) : (0.15f + (1.0f - v) * 1.15f);
            float dist = (float)std::fabs(u - boltCenter);
            if (dist < 0.14f) {
                float glow = 1.0f - dist / 0.14f;
                uint32_t c = lerpCol(C(3, 12, 42), C(37, 99, 235), (int)(glow * 255.0f));
                if (dist < 0.025f) c = lerpCol(c, C(147, 197, 253), (int)((1.0f - dist / 0.025f) * 240.0f));
                return c;
            }
            return C(2, 4, 14);
        }
        case 3: // Floral-2: Dark vintage botanical floral vines II
        case 4: { // Floral-1: Rich crimson/amber botanical floral tapestry I
            float p1 = (float)(std::sin(u * 14.0f + std::cos(v * 18.0f) * 2.0f) * std::cos(v * 16.0f - u * 9.0f));
            uint32_t darkBg = (skinIdx == 3) ? C(18, 16, 22) : C(14, 12, 18);
            if (p1 > 0.35f) {
                float t = (p1 - 0.35f) / 0.65f;
                uint32_t petal = (skinIdx == 3)
                    ? lerpCol(C(110, 58, 78), C(215, 125, 110), (int)(t * 255.0f))
                    : lerpCol(C(145, 38, 65), C(235, 95, 82), (int)(t * 255.0f));
                return lerpCol(darkBg, petal, (int)(t * 210.0f));
            }
            return darkBg;
        }
        case 5: // Transparent
            return C(6, 10, 18);
        case 6: // Rainbow_Border: Pitch black highway deck (rainbow is on side borders)
            return C(4, 4, 6);
        case 7: // Pink_Border: Pitch black highway deck (hot pink is on side borders)
            return C(4, 4, 6);
        case 8: { // Cracked: Dark shattered obsidian with gold kintsugi cracks
            float crackPath = 0.5f + 0.18f * (float)std::sin(v * 11.0f) + 0.08f * (float)std::cos(v * 27.0f);
            float branch = 0.5f - (v - 0.35f) * 1.4f + 0.05f * (float)std::sin(u * 19.0f);
            float d = std::min((float)std::fabs(u - crackPath), (v > 0.25f && v < 0.55f) ? (float)std::fabs(u - branch) : 1.0f);
            if (d < 0.018f) return C(4, 4, 4);
            if (d < 0.032f) return C(145, 110, 32);
            return C(16, 16, 18);
        }
        case 10: { // SmokeHero: Deep electric-blue swirling smoke wisps on black
            float swirl = (float)(std::sin(u * 6.5f + v * 9.0f + std::sin(v * 5.0f) * 2.2f) *
                          std::cos(u * 4.5f - v * 7.5f));
            if (swirl > 0.0f) {
                int a = (int)(swirl * swirl * 210.0f);
                return lerpCol(C(2, 3, 10), C(24, 58, 225), a);
            }
            return C(2, 3, 10);
        }
        case 11: { // PinkLaser: Hot pink zigzag laser beams with magenta nebula smoke
            float nebRaw = (float)(std::sin(u * 7.0f + v * 8.0f) * std::cos(u * 5.0f - v * 6.0f));
            float neb = (nebRaw > 0.0f) ? nebRaw : 0.0f;
            uint32_t c = lerpCol(C(5, 2, 8), C(120, 18, 85), (int)(neb * 145.0f));
            float z1 = (float)std::fabs(u - (0.15f + 0.70f * (float)std::fabs(std::fmod(v * 3.5f, 2.0f) - 1.0f)));
            if (z1 < 0.022f) {
                c = lerpCol(c, C(255, 65, 195), (int)((1.0f - z1 / 0.022f) * 255.0f));
            }
            return c;
        }
        case 12: { // Christmas06: Deep crimson velvet with white snowflakes
            int shade = 42 + (int)(16.0f * (float)std::sin(u * 25.0f) * (float)std::cos(v * 35.0f));
            uint32_t c = C(shade, 4, 8);
            float sx = u * 5.0f - (float)std::floor(u * 5.0f) - 0.5f;
            float sy = v * 9.0f - (float)std::floor(v * 9.0f) - 0.5f;
            float r = (float)std::sqrt(sx * sx + sy * sy);
            float ang = (float)std::atan2(sy, sx);
            float spoke = (float)std::fabs(std::sin(ang * 3.0f));
            if (r < 0.22f && spoke > 0.82f) {
                c = lerpCol(c, C(215, 225, 235), 185);
            }
            return c;
        }
        case 13: { // RoyalCarpet: Deep maroon/burgundy crushed royal velvet texture
            float crush = (float)(std::sin(u * 38.0f + std::cos(v * 42.0f)) * std::cos(v * 48.0f - u * 29.0f));
            int r = 36 + (int)(crush * 22.0f);
            return C(std::clamp(r, 12, 68), 3, 6);
        }
        case 14: { // Overdose: Neon pink overlapping expressive pattern on pitch black
            float cx = u * 3.0f - (float)std::floor(u * 3.0f) - 0.5f;
            float cy = v * 5.0f - (float)std::floor(v * 5.0f) - 0.5f;
            float ring = (float)std::fabs(std::sqrt(cx * cx + cy * cy) - 0.34f);
            float eye = (float)std::fabs(std::sqrt((std::fabs(cx) - 0.12f) * (std::fabs(cx) - 0.12f) + (cy + 0.05f) * (cy + 0.05f)) - 0.07f);
            float d = (ring < eye) ? ring : eye;
            if (d < 0.038f) {
                return lerpCol(C(10, 4, 12), C(255, 85, 165), (int)((1.0f - d / 0.038f) * 245.0f));
            }
            return C(8, 3, 10);
        }
        case 15: // Cosmic-3: Deep space cyan/magenta/gold orbital arcs
        case 16: // Cosmic-1: Deep space gold/pink/teal orbital arcs
        case 17: { // Cosmic-2: Deep space turquoise/violet orbital arcs
            uint32_t bg = C(4, 6, 12);
            float arc1 = (float)std::fabs(std::sin(u * 5.5f + v * 7.2f) - 0.65f);
            float arc2 = (float)std::fabs(std::cos(u * 4.2f - v * 8.5f) - 0.72f);
            uint32_t c1 = (skinIdx == 15) ? C(56, 189, 248) : (skinIdx == 16) ? C(250, 204, 21) : C(45, 212, 191);
            uint32_t c2 = (skinIdx == 15) ? C(217, 70, 239) : (skinIdx == 16) ? C(244, 114, 182): C(168, 85, 247);
            if (arc1 < 0.045f) bg = lerpCol(bg, c1, (int)((1.0f - arc1 / 0.045f) * 225.0f));
            if (arc2 < 0.045f) bg = lerpCol(bg, c2, (int)((1.0f - arc2 / 0.045f) * 210.0f));
            return bg;
        }
        case 18: { // Metallic: Dark scratched gunmetal steel with copper lower corner
            int scratch = (int)(12.0f * (float)std::sin(u * 22.0f + v * 18.0f));
            uint32_t base = C(std::clamp(14 + scratch, 6, 36), std::clamp(18 + scratch, 8, 42), std::clamp(22 + scratch, 10, 48));
            if (u + v > 1.32f) {
                float cop = std::clamp((u + v - 1.32f) / 0.55f, 0.0f, 1.0f);
                base = lerpCol(base, C(135, 62, 28), (int)(cop * 190.0f));
            }
            return base;
        }
        case 19: { // FlowerPower: Deep purple velvet with embossed retro daisy flowers
            uint32_t bg = C(20, 6, 26);
            float fx = u * 2.2f - (float)std::floor(u * 2.2f) - 0.5f;
            float fy = v * 3.8f - (float)std::floor(v * 3.8f) - 0.5f;
            float r = (float)std::sqrt(fx * fx + fy * fy);
            float ang = (float)std::atan2(fy, fx);
            float petalR = 0.22f + 0.16f * (float)std::cos(ang * 10.0f);
            if (r < 0.10f) return C(68, 22, 78);
            if ((float)std::fabs(r - petalR) < 0.035f) return C(82, 28, 92);
            return bg;
        }
        case 20: // FatTony-Fretboard: Crimson stippled portrait contour on pitch black
        default: {
            float dx = (u - 0.5f) * 2.0f;
            float dy = (v - 0.48f) * 2.0f;
            float sil = (float)std::fabs(std::sqrt(dx * dx * 1.4f + dy * dy * 0.9f) - 0.62f);
            float lapel = (float)std::fabs(std::fabs(dx) - (0.18f + std::fabs(dy) * 0.25f));
            float d = (sil < lapel) ? sil : lapel;
            if (d < 0.045f) {
                return lerpCol(C(4, 2, 2), C(195, 18, 24), (int)((1.0f - d / 0.045f) * 235.0f));
            }
            return C(3, 2, 2);
        }
    }
}

static bool loadBackgroundToFb(uint32_t* fb, const std::string& path) {
    if (path.empty()) return false;
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".raw") {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return false;
        size_t readBytes = fread(fb, 1, (size_t)W * H * 4, f);
        fclose(f);
        return (readBytes == (size_t)W * H * 4);
    }
    int imgW = 0, imgH = 0, imgCh = 0;
    unsigned char* data = stbi_load(path.c_str(), &imgW, &imgH, &imgCh, 4);
    if (!data || imgW <= 0 || imgH <= 0) return false;
    for (int y = 0; y < H; y++) {
        int srcY = (int)(((int64_t)y * imgH) / H);
        if (srcY >= imgH) srcY = imgH - 1;
        const uint8_t* srcRow = data + (size_t)srcY * imgW * 4;
        uint32_t* dstRow = fb + (size_t)y * W;
        for (int x = 0; x < W; x++) {
            int srcX = (int)(((int64_t)x * imgW) / W);
            if (srcX >= imgW) srcX = imgW - 1;
            const uint8_t* p = srcRow + srcX * 4;
            dstRow[x] = C(p[0], p[1], p[2]);
        }
    }
    stbi_image_free(data);
    return true;
}

static void buildStageCaches(const char* songBgPath = nullptr, int theme = 0) {
    theme = ((theme % NUM_HIGHWAYS) + NUM_HIGHWAYS) % NUM_HIGHWAYS;
    bool loadedCustomBg = false;
    std::vector<std::string> bgCandidates;
    if (songBgPath && strlen(songBgPath) > 0) bgCandidates.push_back(songBgPath);
    bgCandidates.push_back("/data/GH/background.png");
    bgCandidates.push_back("/data/GH/background.jpg");
    bgCandidates.push_back("/data/GH/background.jpeg");
    bgCandidates.push_back("/mnt/usb0/GH/background.png");
    bgCandidates.push_back("/mnt/usb0/GH/background.jpg");

    for (const auto& cand : bgCandidates) {
        if (loadBackgroundToFb(g_stageCacheFb[0], cand)) {
            loadedCustomBg = true;
            break;
        }
    }

    if (loadedCustomBg) {
        for (int y = 0; y < H; y++) {
            uint32_t* row = g_stageCacheFb[0] + (size_t)y * W;
            for (int x = 0; x < W; x++) {
                row[x] = lerpCol(row[x], g_themeBgSecondary, 95);
            }
        }
    } else {
        gradRectV(g_stageCacheFb[0], 0, 0, W, HORIZON_Y, g_themeBgPrimary, g_themeBgSecondary);
        gradRectV(g_stageCacheFb[0], 0, HORIZON_Y, W, H - HORIZON_Y, g_themeBgSecondary, C(2, 4, 10));
        hlineAlpha(g_stageCacheFb[0], 0, W, HORIZON_Y, 2, g_themeTrimPrimary, 85);
    }

    for (int variant = 1; variant < 3; variant++) {
        memcpy(g_stageCacheFb[variant], g_stageCacheFb[0], (size_t)W * H * 4);
    }

    // Attempt to load the active custom highway texture image file (if installed in /app0/assets/highways or /data/GH/highways)
    unsigned char* hwyTex = nullptr;
    int hwyW = 0, hwyH = 0;
    bool hasHwyTex = loadHighwayTextureFile(theme, &hwyTex, &hwyW, &hwyH);

    for (int variant = 0; variant < 3; variant++) {
        uint32_t* fb = g_stageCacheFb[variant];
        bool spActive = (variant == 1);
        bool overstrumFlash = (variant == 2);

        for (int y = HORIZON_Y; y < H; y++) {
            double p = (double)(y - HORIZON_Y) / (double)(HIT_Y - HORIZON_Y);
            double pClamped = std::clamp(p, 0.0, 1.35);
            float vTex = std::clamp((float)(y - HORIZON_Y) / (float)(H - 1 - HORIZON_Y), 0.0f, 1.0f);
            int curW = TOP_W + (int)((BOT_W - TOP_W) * p);
            int curX0 = (W - curW) / 2;
            int railW = std::max(8, (int)(7 + 18 * p));

            uint32_t railOuter, railBright, railMid, railInner, railGlow;
            if (spActive) {
                railOuter  = C(10, 82, 112);
                railMid    = C(34, 185, 230);
                railBright = C(224, 248, 255);
                railInner  = C(8, 105, 140);
                railGlow   = C(56, 189, 248);
            } else if (theme == 6) { // Rainbow_Border: Full RGB spectrum along highway height!
                float hue = std::fmod(vTex * 6.0f, 6.0f);
                int hi = (int)std::floor(hue);
                float hf = hue - hi;
                int q = (int)((1.0f - hf) * 255.0f), t = (int)(hf * 255.0f);
                uint32_t rCol = (hi == 0) ? C(255, t, 0) : (hi == 1) ? C(q, 255, 0) :
                                (hi == 2) ? C(0, 255, t) : (hi == 3) ? C(0, q, 255) :
                                (hi == 4) ? C(t, 0, 255) : C(255, 0, q);
                railOuter  = lerpCol(C(10, 12, 18), rCol, 140);
                railMid    = rCol;
                railBright = lerpCol(rCol, C(255, 255, 255), 140);
                railInner  = lerpCol(C(10, 12, 18), rCol, 190);
                railGlow   = rCol;
            } else if (theme == 7 || theme == 11 || theme == 14) { // Pink_Border / PinkLaser / Overdose
                railOuter  = C(95, 12, 65);
                railMid    = C(236, 35, 155);
                railBright = C(255, 175, 225);
                railInner  = C(165, 20, 110);
                railGlow   = C(244, 63, 165);
            } else if (theme == 2 || theme == 10) { // Electric / SmokeHero
                railOuter  = C(12, 32, 95);
                railMid    = C(37, 99, 235);
                railBright = C(191, 219, 254);
                railInner  = C(29, 78, 216);
                railGlow   = C(59, 130, 246);
            } else if (theme == 12 || theme == 13 || theme == 20) { // Christmas06 / RoyalCarpet / FatTony
                railOuter  = C(65, 14, 18);
                railMid    = C(155, 165, 180);
                railBright = C(242, 246, 252);
                railInner  = C(95, 24, 30);
                railGlow   = C(220, 38, 38);
            } else { // Brushed Gunmetal Chrome
                railOuter  = C(24, 28, 36);
                railMid    = C(118, 128, 145);
                railBright = C(232, 238, 248);
                railInner  = C(54, 60, 72);
                railGlow   = C(148, 163, 184);
            }

            // Smooth 3D Cylindrical Left & Right Side Rails
            uint32_t* row = fb + (size_t)y * W;
            for (int rx = 0; rx < railW; rx++) {
                float rf = (float)rx / (float)std::max(1, railW - 1);
                uint32_t cRail;
                if (rf < 0.18f) {
                    cRail = lerpCol(C(8, 10, 15), railOuter, (int)(rf / 0.18f * 256.0f));
                } else if (rf < 0.48f) {
                    cRail = lerpCol(railOuter, railBright, (int)((rf - 0.18f) / 0.30f * 256.0f));
                } else if (rf < 0.78f) {
                    cRail = lerpCol(railBright, railMid, (int)((rf - 0.48f) / 0.30f * 256.0f));
                } else {
                    cRail = lerpCol(railMid, railInner, (int)((rf - 0.78f) / 0.22f * 256.0f));
                }
                int lx = curX0 - railW + rx;
                int rrx = curX0 + curW + (railW - 1 - rx);
                if ((unsigned)lx < (unsigned)W) row[lx] = cRail;
                if ((unsigned)rrx < (unsigned)W) row[rrx] = cRail;
            }

            // Render Fretboard Deck (unless theme == 5 "Transparent", which keeps background/video visible!)
            float laneW = (float)curW / 5.0f;
            float invCurW = 1.0f / (float)std::max(1, curW);
            int horizonFade256 = (p < 0.10) ? (int)((p / 0.10) * 256.0) : 256;

            if (theme != 5) {
                int srcY = hasHwyTex ? std::clamp((int)(vTex * (hwyH - 1)), 0, hwyH - 1) : 0;
                const unsigned char* texRow = hasHwyTex ? (hwyTex + (size_t)srcY * hwyW * 4) : nullptr;

                for (int dx = 0; dx < curW; dx++) {
                    int px = curX0 + dx;
                    if ((unsigned)px >= (unsigned)W) continue;
                    float uTex = (float)dx * invCurW;

                    uint32_t c;
                    if (hasHwyTex) {
                        int srcX = std::clamp((int)(uTex * (hwyW - 1)), 0, hwyW - 1);
                        const unsigned char* tp = texRow + srcX * 4;
                        c = C(tp[0], tp[1], tp[2]);
                    } else {
                        c = sampleProceduralHighwaySkin(theme, uTex, vTex);
                    }

                    if (overstrumFlash) {
                        c = lerpCol(c, C(160, 24, 24), 95);
                    } else if (spActive) {
                        c = lerpCol(c, C(14, 165, 233), 68);
                    }

                    if (dx < 12) {
                        c = lerpCol(railGlow, c, dx * 21);
                    } else if (curW - 1 - dx < 12) {
                        c = lerpCol(railGlow, c, (curW - 1 - dx) * 21);
                    }

                    if (horizonFade256 < 256) {
                        c = lerpCol(C(6, 10, 18), c, horizonFade256);
                    }
                    row[px] = c;
                }
            }

            // Anti-Aliased Lane Divider Grooves (4 dividers between the 5 lanes)
            for (int div = 1; div < 5; div++) {
                float fx = (float)curX0 + div * laneW;
                int ix = (int)fx;
                blendPx(fb, ix - 1, y, C(6, 8, 14), (int)(85 * pClamped));
                blendPx(fb, ix,     y, C(95, 108, 128), (int)(72 * pClamped));
                blendPx(fb, ix + 1, y, C(6, 8, 14), (int)(85 * pClamped));
            }

            // High-Resolution Sub-Pixel Anti-Aliased 3D Wound-Steel Guitar Strings
            float strHalfW = 0.85f + 1.35f * (float)pClamped;
            for (int l = 0; l < 5; l++) {
                float sxCenter = (float)curX0 + (l + 0.5f) * laneW;
                int sx0 = (int)std::floor(sxCenter - strHalfW - 2.0f);
                int sx1 = (int)std::ceil(sxCenter + strHalfW + 2.0f);
                for (int px = sx0; px <= sx1; px++) {
                    if ((unsigned)px >= (unsigned)W) continue;
                    float dist = (float)px - sxCenter;
                    float absD = std::fabs(dist);
                    if (absD <= strHalfW) {
                        float norm = dist / strHalfW;
                        float spec = std::max(0.0f, 1.0f - (norm + 0.2f) * (norm + 0.2f) * 1.4f);
                        uint32_t strCol = lerpCol(C(110, 120, 138), C(235, 242, 252), (int)(spec * 255.0f));
                        float edgeAA = std::clamp((strHalfW - absD) * 1.8f, 0.0f, 1.0f);
                        blendPx(fb, px, y, strCol, (int)(edgeAA * 195.0f * (0.35f + 0.65f * pClamped)));
                    } else if (absD <= strHalfW + 1.6f) {
                        float sh = 1.0f - (absD - strHalfW) / 1.6f;
                        blendPx(fb, px, y, C(4, 6, 10), (int)(sh * 95.0f * pClamped));
                    }
                }
            }
        }

        // 3. Authentic Clone Hero Metallic Strikeline Nut Bar at HIT_Y
        int nutX0 = (W - BOT_W) / 2;
        gradRectV(fb, nutX0, HIT_Y - 6, BOT_W, 6, C(45, 52, 65), C(195, 206, 222));
        gradRectV(fb, nutX0, HIT_Y,     BOT_W, 6, C(195, 206, 222), C(28, 32, 42));
        hlineAlpha(fb, nutX0, nutX0 + BOT_W, HIT_Y - 1, 2, C(255, 255, 255), 140);
    }

    if (hwyTex) stbi_image_free(hwyTex);
}

// Live Highway Preview Renderer for Settings Screen
static void drawHighwayPreviewCard(uint32_t* fb, int bx, int by, int bw, int bh, int skinIdx) {
    borderRect(fb, bx - 3, by - 3, bw + 6, bh + 6, 2, C(12, 20, 34), C(56, 189, 248));
    for (int dy = 0; dy < bh; dy++) {
        float v = (float)dy / (float)std::max(1, bh - 1);
        uint32_t* row = fb + (size_t)(by + dy) * W + bx;
        for (int dx = 0; dx < bw; dx++) {
            float u = (float)dx / (float)std::max(1, bw - 1);
            if (dx < 5 || dx >= bw - 5) {
                if (skinIdx == 6) {
                    int q = (int)((1.0f - v) * 255.0f), t = (int)(v * 255.0f);
                    row[dx] = C(t, q, 220);
                } else if (skinIdx == 7 || skinIdx == 11) {
                    row[dx] = C(244, 63, 165);
                } else {
                    row[dx] = C(165, 178, 198);
                }
            } else if (skinIdx == 5) {
                // Checkerboard / translucent preview for Transparent highway
                bool chk = (((dx / 12) + (dy / 12)) & 1) != 0;
                row[dx] = chk ? C(16, 28, 46) : C(10, 18, 32);
            } else {
                row[dx] = sampleProceduralHighwaySkin(skinIdx, u, v);
            }
        }
        // Subtle 5 strings on preview card
        for (int s = 0; s < 5; s++) {
            int sx = (int)((s + 0.5f) * (bw / 5.0f));
            if (sx >= 5 && sx < bw - 5) row[sx] = lerpCol(row[sx], C(210, 220, 235), 110);
        }
    }
}

// ---------------------------------------------------------------- Ultra-High-Res YARG / Clone Hero 3D Fret Receptor, Flame & Gem Renderers
static void drawCloneHeroReceptor(uint32_t* fb, float cx, float cy, float rx, float ry, uint32_t laneCol, uint32_t laneBright, bool isHeld, bool laneEnabled) {
    // 1. Volumetric Upward Highway Light Cone & Strikeline Bloom when held
    if (laneEnabled && isHeld) {
        for (int b = 0; b < 5; b++) {
            float frac = (float)b / 4.0f;
            float beamY = cy - ry * (0.35f + frac * 1.45f);
            float beamRx = rx * (1.18f - frac * 0.28f);
            float beamRy = ry * (0.95f - frac * 0.25f);
            int beamAlpha = (int)((1.0f - frac * 0.78f) * 58.0f);
            fillEllipseAA(fb, cx, beamY, beamRx, beamRy, laneBright, beamAlpha);
        }
        fillEllipseAA(fb, cx, cy, rx * 1.16f, ry * 1.18f, laneBright, 85);
    }

    float maxRx = rx * 1.10f;
    float maxRy = ry * 1.22f;
    int x0 = std::max(0, (int)std::floor(cx - maxRx - 2.0f));
    int x1 = std::min(W - 1, (int)std::ceil(cx + maxRx + 2.0f));
    int y0 = std::max(0, (int)std::floor(cy - maxRy - 2.0f));
    int y1 = std::min(H - 1, (int)std::ceil(cy + maxRy + 4.0f));

    float invRx = 1.0f / std::max(1.0f, rx);
    float invRy = 1.0f / std::max(1.0f, ry);

    // YARG / Clone Hero physical plunger vertical displacement: raised (-0.11) when idle, depressed (+0.07) when held
    float capOffY = isHeld ? 0.07f : -0.11f;

    for (int py = y0; py <= y1; py++) {
        uint32_t* row = fb + (size_t)py * W;
        float ny = ((float)py + 0.5f - cy) * invRy;
        for (int px = x0; px <= x1; px++) {
            float nx = ((float)px + 0.5f - cx) * invRx;

            // Center 3D Plunger Cap coordinate
            float capNy = ny - capOffY;
            float capR = (float)std::sqrt(nx * nx + capNy * capNy);

            // Base housing elliptical radius (with 3D angled lower metal skirt extrusion)
            float baseNy = (ny > 0.0f) ? (ny * 0.85f) : ny;
            float r = (float)std::sqrt(nx * nx + baseNy * baseNy);

            if (r > 1.10f) continue;

            // Directional studio lighting (-1..+1, top-left key light + rim light)
            float light = std::clamp((-nx * 0.46f - ny * 0.74f) + 0.28f, -0.85f, 1.0f);

            if (!laneEnabled) {
                if (r <= 1.0f) {
                    float aa = std::clamp((1.0f - r) * rx * 0.8f, 0.0f, 1.0f);
                    uint32_t c = (r > 0.72f) ? C(36, 40, 48) : C(16, 18, 24);
                    row[px] = lerpCol(row[px], c, (int)(aa * 255.0f));
                }
                continue;
            }

            uint32_t outCol;
            int alpha = 255;

            if (capR <= 0.51f) {
                // LAYER 5: 3D Spring-Loaded Center Plunger Dome Cap (YARG / Clone Hero style)
                float normCap = capR / 0.51f;
                float domeZ = (float)std::sqrt(std::max(0.0f, 1.0f - normCap * normCap));
                float specBase = std::max(0.0f, -nx * 0.42f - capNy * 0.68f + domeZ * 0.66f);
                float spec = specBase * specBase * specBase;

                if (isHeld) {
                    // Incandescent white-hot center core + blazing laneBright plasma dome
                    uint32_t lit = lerpCol(laneCol, laneBright, (int)(domeZ * 255.0f));
                    int whiteHot = (int)(std::clamp(spec * 0.90f + (1.0f - normCap) * 0.72f, 0.0f, 1.0f) * 255.0f);
                    outCol = lerpCol(lit, C(255, 255, 255), whiteHot);
                } else {
                    // Brushed anthracite/obsidian 3D plunger cap with vibrant lane-colored target ring inlay
                    uint32_t darkCap = lerpCol(C(18, 22, 30), C(66, 76, 96), (int)(domeZ * 225.0f));
                    darkCap = lerpCol(darkCap, C(195, 208, 228), (int)(std::clamp(spec * 0.72f, 0.0f, 1.0f) * 255.0f));
                    // Crisp colored target ring inlay at capR in [0.31 .. 0.43]
                    if (capR >= 0.30f && capR <= 0.44f) {
                        float ringT = 1.0f - (float)std::fabs(capR - 0.37f) / 0.07f;
                        uint32_t inlayCol = lerpCol(laneCol, laneBright, 90);
                        darkCap = lerpCol(darkCap, inlayCol, (int)(ringT * 215.0f));
                    }
                    outCol = darkCap;
                }
                // Crisp metallic chamfered edge around plunger cap
                if (capR > 0.46f) {
                    float edgeT = (capR - 0.46f) / 0.05f;
                    outCol = lerpCol(outCol, isHeld ? laneCol : C(8, 10, 16), (int)(edgeT * 230.0f));
                }
            } else if (r <= 0.61f) {
                // LAYER 4: Deep Recessed Obsidian Mechanical Piston Trench
                float wellT = (r - 0.51f) / 0.10f;
                uint32_t trenchCol = lerpCol(C(5, 7, 12), C(20, 24, 34), (int)(wellT * 255.0f));
                if (isHeld) {
                    // Subsurface glow reflecting inside the piston trench when pressed
                    trenchCol = lerpCol(trenchCol, laneCol, (int)((1.0f - wellT) * 145.0f));
                }
                outCol = trenchCol;
            } else if (r <= 0.85f) {
                // LAYER 3: Signature YARG / Clone Hero Thick Colored Fret Collar Torus
                float ringPos = (r - 0.61f) / 0.24f; // 0 inner -> 1 outer
                float torusArch = (float)std::sin(ringPos * 3.1415926f);
                uint32_t baseRing = isHeld ? laneBright : laneCol;
                uint32_t deepRing = lerpCol(C(10, 14, 22), baseRing, 110);
                uint32_t shadedRing = lerpCol(deepRing, baseRing, (int)((0.35f + 0.65f * torusArch) * 255.0f));
                float specGlint = std::max(0.0f, torusArch * (0.42f + 0.58f * light));
                int hi = (int)(specGlint * (isHeld ? 230.0f : 165.0f));
                outCol = lerpCol(shadedRing, C(255, 255, 255), hi);
                // Crisp inner & outer micro-bevel seams on colored collar
                if (ringPos < 0.08f) {
                    outCol = lerpCol(C(8, 10, 16), outCol, (int)(ringPos / 0.08f * 255.0f));
                } else if (ringPos > 0.92f) {
                    outCol = lerpCol(outCol, C(12, 16, 24), (int)((ringPos - 0.92f) / 0.08f * 220.0f));
                }
            } else if (r <= 1.00f) {
                // LAYER 2: High-Contrast Brushed Chrome / Anodized Silver Outer Housing Bevel
                float rimPos = (r - 0.85f) / 0.15f;
                float rimArch = (float)std::sin(rimPos * 3.1415926f);
                float bevelShade = std::clamp(0.48f + 0.52f * light + rimArch * 0.25f, 0.0f, 1.0f);
                uint32_t chromeDark = (ny > 0.25f) ? C(18, 22, 30) : C(34, 40, 52);
                uint32_t chromeLite = isHeld ? lerpCol(C(235, 242, 252), laneBright, 85) : C(238, 244, 252);
                outCol = lerpCol(chromeDark, chromeLite, (int)(bevelShade * 255.0f));
                if (r > 0.94f) {
                    float aa = std::clamp((1.00f - r) / 0.06f, 0.0f, 1.0f);
                    alpha = (int)(aa * 255.0f);
                }
            } else {
                // LAYER 1: 3D Extruded Cast-Metal Base Skirt & Contact Shadow
                float sh = std::clamp((1.10f - r) / 0.10f, 0.0f, 1.0f);
                outCol = C(4, 6, 12);
                alpha = (int)(sh * 175.0f);
            }

            if (alpha >= 254) row[px] = outCol;
            else if (alpha > 0) row[px] = lerpCol(row[px], outCol, alpha);
        }
    }
}

static void drawReceptorHitFlame(uint32_t* fb, float cx, float cy, double ageSec, bool spActive, uint32_t laneCol = C(245, 158, 11), uint32_t laneBright = C(254, 240, 138)) {
    if (ageSec < 0.0 || ageSec > 0.26) return;
    float life = 1.0f - (float)(ageSec / 0.26);
    float t = (float)ageSec * 48.0f;
    float flameH = 162.0f * (0.42f + 0.58f * life);

    // 1. Expanding Dual Perspective Shockwave Ellipses on the Strikeline (YARG / Clone Hero Hit Ring)
    if (ageSec < 0.14) {
        float ringProgress = (float)(ageSec / 0.14);
        float ringLife = 1.0f - ringProgress;
        float outerRx = 34.0f + ringProgress * 52.0f;
        float outerRy = outerRx * 0.40f;
        uint32_t waveCol = spActive ? C(56, 189, 248) : laneBright;
        fillEllipseAA(fb, cx, cy, outerRx, outerRy, waveCol, (int)(ringLife * 145.0f));
        if (ageSec < 0.08) {
            float coreLife = 1.0f - (float)(ageSec / 0.08);
            fillEllipseAA(fb, cx, cy - 3.0f, 30.0f * coreLife + 10.0f, 14.0f * coreLife + 5.0f, C(255, 255, 255), (int)(coreLife * 220.0f));
        }
    }

    int yBottom = (int)cy + 6;
    int yTop = std::max(0, (int)(cy - flameH));

    uint32_t coreCol = C(255, 255, 255);
    uint32_t midCol  = spActive ? C(56, 189, 248) : lerpCol(C(250, 204, 21), laneBright, 110);
    uint32_t edgeCol = spActive ? C(2, 132, 199)  : lerpCol(C(234, 88, 12), laneCol, 130);

    for (int y = yBottom; y >= yTop; y--) {
        float frac = (float)(cy - y) / flameH;
        if (frac < 0.0f) frac = 0.0f;
        if (frac > 1.0f) break;

        float flickerL = (float)std::sin(frac * 8.6f + t * 1.35f) * 7.2f;
        float flickerC = (float)std::cos(frac * 9.8f - t * 1.85f) * 5.0f;
        float flickerR = (float)std::sin(frac * 7.8f + t * 2.15f) * 7.5f;

        float baseW = (1.0f - frac * 0.88f) * 36.0f * life;
        if (baseW <= 0.8f) continue;

        int spanL = (int)(cx - baseW + flickerL * frac);
        int spanR = (int)(cx + baseW + flickerR * frac);
        if (spanL > spanR) std::swap(spanL, spanR);
        spanL = std::max(0, spanL);
        spanR = std::min(W - 1, spanR);

        uint32_t* row = fb + (size_t)y * W;
        float invHalf = 1.0f / (baseW + 0.1f);

        for (int px = spanL; px <= spanR; px++) {
            float dx = (float)std::fabs((float)px - (cx + flickerC * frac)) * invHalf;
            if (dx >= 1.0f) continue;

            uint32_t c;
            int alpha;
            if (dx < 0.32f && frac < 0.60f) {
                c = coreCol;
                alpha = (int)((1.0f - frac * 0.45f) * life * 255.0f);
            } else if (dx < 0.68f) {
                c = midCol;
                alpha = (int)((1.0f - frac * 0.65f) * life * 230.0f);
            } else {
                c = edgeCol;
                alpha = (int)((1.0f - dx) * (1.0f - frac * 0.78f) * life * 190.0f);
            }
            alpha = std::clamp(alpha, 0, 255);
            row[px] = (alpha >= 250) ? c : lerpCol(row[px], c, alpha);
        }
    }

    // 2. High-Velocity Rising Ember Particles
    static const float sparkOffsets[8][2] = {
        {-16.0f, 1.05f}, {14.0f, 1.35f}, {-7.0f, 1.65f}, {9.0f, 1.85f},
        {-22.0f, 1.25f}, {20.0f, 1.50f}, {-3.0f, 2.05f}, {12.0f, 1.75f}
    };
    for (int s = 0; s < 8; s++) {
        float spkProgress = (float)ageSec * sparkOffsets[s][1] * 3.6f;
        if (spkProgress > 1.0f) continue;
        float sx = cx + sparkOffsets[s][0] * (1.0f + spkProgress * 0.35f) + (float)std::sin(t + (float)s) * 4.5f;
        float sy = cy - spkProgress * (flameH + 46.0f);
        float sRad = (1.0f - spkProgress) * 3.2f + 0.6f;
        int sAlpha = (int)((1.0f - spkProgress) * 245.0f);
        uint32_t spkCol = (s & 1) ? C(255, 255, 255) : (spActive ? C(125, 211, 252) : laneBright);
        fillEllipseAA(fb, sx, sy, sRad, sRad, spkCol, sAlpha);
    }
}

// High-Resolution 3D Per-Pixel Shaded YARG / Clone Hero Note Gem (Strum, HOPO & Star Power)
static void drawCloneHeroGem(uint32_t* fb, float cx, float cy, float rx, float ry, uint32_t laneCol, bool isHopo, bool isStarPower, bool isMissed) {
    uint32_t bodyCol = isMissed ? C(68, 72, 82) : laneCol;

    // Star Power wings behind the gem
    if (isStarPower && !isMissed) {
        drawStarPowerWingsAA(fb, cx, cy + ry * 0.05f, rx * 1.08f, ry * 1.10f, bodyCol, C(103, 232, 249));
    }

    float maxRx = rx * 1.08f;
    float maxRy = ry * 1.20f;
    int x0 = std::max(0, (int)std::floor(cx - maxRx - 1.0f));
    int x1 = std::min(W - 1, (int)std::ceil(cx + maxRx + 1.0f));
    int y0 = std::max(0, (int)std::floor(cy - maxRy - 1.0f));
    int y1 = std::min(H - 1, (int)std::ceil(cy + maxRy + 2.0f));

    float invRx = 1.0f / std::max(1.0f, rx);
    float invRy = 1.0f / std::max(1.0f, ry);

    uint32_t darkBody = lerpCol(C(6, 8, 14), bodyCol, 125);
    uint32_t brightBody = lerpCol(bodyCol, C(255, 255, 255), 155);

    for (int py = y0; py <= y1; py++) {
        uint32_t* row = fb + (size_t)py * W;
        float ny = ((float)py + 0.5f - cy) * invRy;

        // Upper dome center shifted upward (-0.13) for authentic 3D Clone Hero puck height
        float domeNy = (ny + 0.13f) / 0.87f;
        // Base metallic puck rim shifted downward (+0.11) for 3D bottom lip
        float baseNy = (ny - 0.11f) / 0.95f;

        for (int px = x0; px <= x1; px++) {
            float nx = ((float)px + 0.5f - cx) * invRx;
            float domeR = (float)std::sqrt(nx * nx + domeNy * domeNy);
            float baseR = (float)std::sqrt(nx * nx + baseNy * baseNy);

            if (domeR > 1.0f && baseR > 1.04f) continue;

            uint32_t outCol;
            int alpha = 255;

            if (domeR <= 0.92f) {
                float capThresh = isHopo ? 0.62f : 0.49f;
                float blackRingInner = isHopo ? 0.50f : 0.34f;

                if (domeR <= blackRingInner) {
                    // Top Glossy White Convex Cap (Wide & ultra-luminous for HOPOs!)
                    float normR = domeR / blackRingInner;
                    float z = (float)std::sqrt(std::max(0.0f, 1.0f - normR * normR));
                    float spec = std::clamp((-nx * 0.38f - domeNy * 0.58f) + z * 0.74f, 0.0f, 1.0f);
                    if (isMissed) {
                        outCol = lerpCol(C(95, 100, 112), C(155, 162, 175), (int)(spec * 255.0f));
                    } else if (isHopo) {
                        // Brilliant porcelain-white HOPO dome with electric cyan-white specular sheen
                        outCol = lerpCol(C(220, 242, 255), C(255, 255, 255), (int)(spec * 255.0f));
                    } else {
                        // Crisp white Strum center cap
                        outCol = lerpCol(C(210, 218, 232), C(255, 255, 255), (int)(spec * 255.0f));
                    }
                } else if (domeR <= capThresh) {
                    // Iconic Clone Hero / YARG Black Recessed Collar around the white cap
                    float groovePos = (domeR - blackRingInner) / (capThresh - blackRingInner);
                    float grooveShade = (float)std::fabs(groovePos - 0.5f) * 2.0f;
                    outCol = lerpCol(C(4, 6, 10), C(28, 32, 42), (int)(grooveShade * 175.0f));
                } else {
                    // 3D Curved Colored Gem Body Skirt (capThresh .. 0.92)
                    float skirtT = (domeR - capThresh) / (0.92f - capThresh);
                    float arch = (float)std::sin(skirtT * 3.1415926f);
                    float light = std::clamp((-nx * 0.44f - domeNy * 0.68f) + 0.38f, 0.0f, 1.0f);
                    outCol = lerpCol(darkBody, bodyCol, (int)((0.32f + 0.68f * arch) * 255.0f));
                    if (!isMissed) {
                        int gloss = (int)(arch * light * 205.0f);
                        outCol = lerpCol(outCol, brightBody, gloss);
                        // Concentrated crescent highlight on top-left shoulder
                        if (light > 0.72f && skirtT > 0.25f && skirtT < 0.75f) {
                            int specHot = (int)((light - 0.72f) / 0.28f * 160.0f);
                            outCol = lerpCol(outCol, C(255, 255, 255), specHot);
                        }
                    }
                    // Smooth transition to outer chrome rim
                    if (domeR > 0.86f) {
                        float rimBlend = (domeR - 0.86f) / 0.06f;
                        outCol = lerpCol(outCol, C(16, 20, 28), (int)(rimBlend * 220.0f));
                    }
                }
            } else if (baseR <= 1.03f) {
                // 3D Extruded Chrome / Silver Base Puck Rim
                float rimLight = std::clamp(0.56f - nx * 0.40f - baseNy * 0.34f, 0.0f, 1.0f);
                uint32_t chromeDark = isMissed ? C(40, 44, 52) : C(60, 68, 84);
                uint32_t chromeLite = isMissed ? C(110, 116, 128) : (isStarPower ? C(165, 243, 252) : C(244, 248, 255));
                outCol = lerpCol(chromeDark, chromeLite, (int)(rimLight * 255.0f));
                if (baseR > 0.95f) {
                    float aa = std::clamp((1.03f - baseR) / 0.08f, 0.0f, 1.0f);
                    alpha = (int)(aa * 255.0f);
                }
            } else {
                continue;
            }

            if (alpha >= 254) row[px] = outCol;
            else if (alpha > 0) row[px] = lerpCol(row[px], outCol, alpha);
        }
    }
}

// Crisp Anti-Aliased Small 5-Pointed Star Renderer for Tracklist UI (No Overlap!)
static void drawSmallStarAA(uint32_t* fb, float cx, float cy, float outerR, uint32_t fillCol, uint32_t brightCol) {
    float innerR = outerR * 0.42f;
    float vx[10], vy[10];
    for (int i = 0; i < 10; i++) {
        float a = -1.5707963f + i * 0.6283185f;
        float rad = (i & 1) ? innerR : outerR;
        vx[i] = std::cos(a) * rad;
        vy[i] = std::sin(a) * rad;
    }
    auto inside = [&](float dx, float dy, float scale) -> bool {
        float sx = dx / scale, sy = dy / scale;
        bool c = false;
        for (int i = 0, j = 9; i < 10; j = i++) {
            if (((vy[i] > sy) != (vy[j] > sy)) &&
                (sx < (vx[j] - vx[i]) * (sy - vy[i]) / (vy[j] - vy[i] + 1e-6f) + vx[i])) {
                c = !c;
            }
        }
        return c;
    };

    int rInt = (int)std::ceil(outerR + 2.0f);
    for (int dy = -rInt; dy <= rInt; dy++) {
        int py = (int)cy + dy;
        if ((unsigned)py >= (unsigned)H) continue;
        uint32_t* row = fb + (size_t)py * W;
        for (int dx = -rInt; dx <= rInt; dx++) {
            int px = (int)cx + dx;
            if ((unsigned)px >= (unsigned)W) continue;
            // 2x2 supersampling for razor-sharp star points
            int hits = 0, innerHits = 0;
            for (int sy = 0; sy < 2; sy++) {
                for (int sx = 0; sx < 2; sx++) {
                    float fx = (float)dx + (sx * 0.5f - 0.25f);
                    float fy = (float)dy + (sy * 0.5f - 0.25f);
                    if (inside(fx, fy, 1.0f)) {
                        hits++;
                        if (inside(fx, fy, 0.72f)) innerHits++;
                    }
                }
            }
            if (hits > 0) {
                uint32_t col = (innerHits > 0) ? ((dy < 0) ? brightCol : fillCol) : C(255, 250, 210);
                row[px] = lerpCol(row[px], col, hits * 64);
            }
        }
    }
}

// ---------------------------------------------------------------- Song Catalog, .OPUS/.OGG/.WAV Audio & Album Art Loader
struct SongEntry {
    std::string dir;
    std::string prefix;
    std::string title;
    std::string artist;
    std::string album;
    std::string genre;
    std::string year;
    std::string charter;
    std::string lengthStr;
    std::string albumArtPath;
    std::string audioFormatLabel = "OGG";
    std::vector<std::string> audioStemPaths;
    int diffNoteCounts[4] = {0, 0, 0, 0};
    bool hasOwnVideo = false;
    double delaySec = 0.0;
    double startOffsetSec = 0.0;
    bool isChapterEntry = false;
    std::vector<Section> chapters;
    int playCount = 0;
    int64_t lastPlayedSeq = 0;

    std::string statKey() const { return dir + prefix + title; }
    std::string iniPath() const { return dir + prefix + "song.ini"; }
    std::string midPath() const { return dir + prefix + "notes.mid"; }
    std::string chartPath() const { return dir + prefix + "notes.chart"; }
    std::string oggPath() const {
        if (!audioStemPaths.empty()) return audioStemPaths[0];
        return dir + prefix + "song.ogg";
    }
    std::string folder() const { return dir; }
};

enum SongSortMode {
    SORT_AZ          = 0,
    SORT_LAST_PLAYED = 1,
    SORT_MOST_PLAYED = 2
};

static int g_songSortMode = SORT_AZ;
static const char* g_songSortNames[3] = {
    "A-Z",
    "LAST PLAYED",
    "MOST PLAYED"
};

struct SongPlayStat {
    int playCount = 0;
    int64_t lastPlayedSeq = 0;
};
static std::map<std::string, SongPlayStat> g_songStatsMap;
static int64_t g_playSeqCounter = 1;

static void loadSongPlayStats() {
    g_songStatsMap.clear();
    FILE* f = fopen("/data/GH/song_stats.ini", "rb");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* p1 = strchr(line, '|');
        if (!p1) continue;
        *p1 = '\0';
        char* p2 = strchr(p1 + 1, '|');
        if (!p2) continue;
        *p2 = '\0';
        std::string key = trimStr(line);
        int cnt = atoi(trimStr(p1 + 1).c_str());
        int64_t seq = atoll(trimStr(p2 + 1).c_str());
        if (!key.empty()) {
            g_songStatsMap[key] = { std::max(0, cnt), std::max<int64_t>(0, seq) };
            if (seq >= g_playSeqCounter) g_playSeqCounter = seq + 1;
        }
    }
    fclose(f);
}

static void saveSongPlayStats() {
    mkdir("/data/GH", 0777);
    FILE* f = fopen("/data/GH/song_stats.ini", "wb");
    if (!f) return;
    for (const auto& kv : g_songStatsMap) {
        if (kv.second.playCount > 0 || kv.second.lastPlayedSeq > 0) {
            fprintf(f, "%s|%d|%lld\n", kv.first.c_str(), kv.second.playCount, (long long)kv.second.lastPlayedSeq);
        }
    }
    fclose(f);
}

static void sortSongCatalog(std::vector<SongEntry>& catalog, int sortMode) {
    for (auto& se : catalog) {
        auto it = g_songStatsMap.find(se.statKey());
        if (it != g_songStatsMap.end()) {
            se.playCount = it->second.playCount;
            se.lastPlayedSeq = it->second.lastPlayedSeq;
        }
    }
    auto toLowerKey = [](const std::string& s) {
        std::string out = s;
        for (char& c : out) c = (char)std::tolower((unsigned char)c);
        return out;
    };
    std::stable_sort(catalog.begin(), catalog.end(), [&](const SongEntry& a, const SongEntry& b) {
        if (sortMode == SORT_LAST_PLAYED) {
            if (a.lastPlayedSeq != b.lastPlayedSeq) return a.lastPlayedSeq > b.lastPlayedSeq;
            if (a.playCount != b.playCount) return a.playCount > b.playCount;
        } else if (sortMode == SORT_MOST_PLAYED) {
            if (a.playCount != b.playCount) return a.playCount > b.playCount;
            if (a.lastPlayedSeq != b.lastPlayedSeq) return a.lastPlayedSeq > b.lastPlayedSeq;
        }
        std::string aa = toLowerKey(a.artist);
        std::string ba = toLowerKey(b.artist);
        if (aa != ba) return aa < ba;
        return toLowerKey(a.title) < toLowerKey(b.title);
    });
}

static void recordSongPlayed(SongEntry& se) {
    SongPlayStat& st = g_songStatsMap[se.statKey()];
    st.playCount++;
    st.lastPlayedSeq = ++g_playSeqCounter;
    se.playCount = st.playCount;
    se.lastPlayedSeq = st.lastPlayedSeq;
    saveSongPlayStats();
}

static bool fileExists(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

// Discover all playable audio files (.opus, .ogg, .mp3, .flac, .wav) inside a song directory
static bool collectSongAudioStems(const std::string& songDir, const std::string& prefix, std::vector<std::string>& outStems, std::string& outFormatLabel) {
    outStems.clear();
    outFormatLabel = "OGG";
    std::string base = songDir;
    if (!base.empty() && base.back() != '/') base += "/";

    const char* stemNames[] = {
        "song", "guitar", "rhythm", "bass", "vocals", "keys",
        "drums", "drums_1", "drums_2", "drums_3", "drums_4", "crowd", "audio"
    };
    struct ExtInfo { const char* ext; const char* singleLabel; const char* multiLabel; };
    const ExtInfo exts[] = {
        { ".opus", "OPUS", "MULTI-OPUS" },
        { ".ogg",  "OGG",  "MULTI-OGG"  },
        { ".mp3",  "MP3",  "MULTI-MP3"  },
        { ".flac", "FLAC", "MULTI-FLAC" },
        { ".wav",  "WAV",  "MULTI-WAV"  }
    };

    // 1. Check standard Clone Hero single or multi-stem names for each format
    for (const auto& fmt : exts) {
        for (const char* s : stemNames) {
            std::string p = base + prefix + s + fmt.ext;
            if (fileExists(p)) {
                outStems.push_back(p);
                if (outStems.size() >= 8) break;
            }
        }
        if (!outStems.empty()) {
            outFormatLabel = (outStems.size() > 1) ? fmt.multiLabel : fmt.singleLabel;
            return true;
        }
    }

    // 2. Scan directory for any *.opus, *.ogg, *.mp3, *.flac, or *.wav file with custom naming
    if (prefix.empty()) {
        DIR* d = opendir(base.c_str());
        if (d) {
            struct dirent* ent;
            while ((ent = readdir(d)) != nullptr) {
                if (ent->d_name[0] == '.') continue;
                std::string fn = ent->d_name;
                std::string ext = Ps4AudioStream::getLowerExt(fn);
                if (ext == ".opus" || ext == ".ogg" || ext == ".mp3" || ext == ".flac" || ext == ".wav") {
                    outStems.push_back(base + fn);
                    if (ext == ".opus") outFormatLabel = "OPUS";
                    else if (ext == ".ogg") outFormatLabel = "OGG";
                    else if (ext == ".mp3") outFormatLabel = "MP3";
                    else if (ext == ".flac") outFormatLabel = "FLAC";
                    else if (ext == ".wav") outFormatLabel = "WAV";
                    if (outStems.size() >= 8) break;
                }
            }
            closedir(d);
        }
    }
    return !outStems.empty();
}

// Find any photo/image file packaged with the song folder (album.png/jpg, cover.png/jpg, or any *.png/*.jpg/*.jpeg/*.bmp)
static std::string findSongPhotoFile(const std::string& songDir) {
    const char* preferred[] = {
        "album.png", "album.jpg", "album.jpeg",
        "cover.png", "cover.jpg", "cover.jpeg",
        "jacket.png", "jacket.jpg",
        "background.png", "background.jpg", "background.jpeg",
        "bg.png", "bg.jpg"
    };
    for (const char* name : preferred) {
        std::string cand = songDir + name;
        if (fileExists(cand)) return cand;
    }
    DIR* d = opendir(songDir.c_str());
    if (!d) return "";
    std::string fallbackImg;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string fn = ent->d_name;
        std::string lower = fn;
        for (char& c : lower) c = (char)tolower((unsigned char)c);
        if ((lower.size() >= 4 && (lower.substr(lower.size() - 4) == ".png" ||
                                   lower.substr(lower.size() - 4) == ".jpg" ||
                                   lower.substr(lower.size() - 4) == ".bmp")) ||
            (lower.size() >= 5 && lower.substr(lower.size() - 5) == ".jpeg")) {
            fallbackImg = songDir + fn;
            break;
        }
    }
    closedir(d);
    return fallbackImg;
}

static bool songFolderHasOwnMp4(const std::string& songDir) {
    std::vector<std::string> mp4s;
    collectMp4FilesInDir(songDir, mp4s);
    return !mp4s.empty();
}

// Bilinear-filtered Album Art Cache matching exact 360x360 showcase box resolution
static const int ALBUM_BOX_W = 360;
static const int ALBUM_BOX_H = 360;
static std::string g_cachedAlbumArtPath = "__none__";
static bool g_cachedAlbumArtValid = false;
static uint32_t g_cachedAlbumArtPixels[ALBUM_BOX_W * ALBUM_BOX_H];

static bool getCachedAlbumArt(const std::string& path) {
    if (path == g_cachedAlbumArtPath) return g_cachedAlbumArtValid;
    g_cachedAlbumArtPath = path;
    g_cachedAlbumArtValid = false;
    if (path.empty()) return false;

    int imgW = 0, imgH = 0, imgCh = 0;
    unsigned char* data = stbi_load(path.c_str(), &imgW, &imgH, &imgCh, 4);
    if (!data || imgW <= 0 || imgH <= 0) {
        if (data) stbi_image_free(data);
        return false;
    }

    int cropSize = std::min(imgW, imgH);
    int cropX0 = (imgW - cropSize) / 2;
    int cropY0 = (imgH - cropSize) / 2;

    for (int y = 0; y < ALBUM_BOX_H; y++) {
        float fy = cropY0 + ((y + 0.5f) * (float)cropSize) / (float)ALBUM_BOX_H - 0.5f;
        int y0 = std::clamp((int)std::floor(fy), 0, imgH - 1);
        int y1 = std::clamp(y0 + 1, 0, imgH - 1);
        int wy = (int)((fy - (float)std::floor(fy)) * 256.0f);

        for (int x = 0; x < ALBUM_BOX_W; x++) {
            float fx = cropX0 + ((x + 0.5f) * (float)cropSize) / (float)ALBUM_BOX_W - 0.5f;
            int x0 = std::clamp((int)std::floor(fx), 0, imgW - 1);
            int x1 = std::clamp(x0 + 1, 0, imgW - 1);
            int wx = (int)((fx - (float)std::floor(fx)) * 256.0f);

            const uint8_t* p00 = data + ((size_t)y0 * imgW + x0) * 4;
            const uint8_t* p10 = data + ((size_t)y0 * imgW + x1) * 4;
            const uint8_t* p01 = data + ((size_t)y1 * imgW + x0) * 4;
            const uint8_t* p11 = data + ((size_t)y1 * imgW + x1) * 4;

            uint32_t topCol = lerpCol(C(p00[0], p00[1], p00[2]), C(p10[0], p10[1], p10[2]), wx);
            uint32_t botCol = lerpCol(C(p01[0], p01[1], p01[2]), C(p11[0], p11[1], p11[2]), wx);
            g_cachedAlbumArtPixels[y * ALBUM_BOX_W + x] = lerpCol(topCol, botCol, wy);
        }
    }

    stbi_image_free(data);
    g_cachedAlbumArtValid = true;
    return true;
}

static void populateSongChapters(SongEntry& se, const std::map<std::string, std::string>& ini) {
    se.album   = ini.count("album")   ? ini.at("album")   : "Single / Custom Track";
    se.genre   = ini.count("genre")   ? ini.at("genre")   : "Rock";
    se.year    = ini.count("year")    ? ini.at("year")    : "2024";
    se.charter = ini.count("charter") ? ini.at("charter") : (ini.count("frets") ? ini.at("frets") : "Community");
    if (se.charter.empty()) se.charter = "Community";

    se.albumArtPath = findSongPhotoFile(se.dir);
    se.hasOwnVideo  = songFolderHasOwnMp4(se.dir);

    Chart tmp;
    if (!loadChart(se.chartPath(), 3, tmp)) {
        loadMidi(se.midPath(), 3, tmp, nullptr);
    }
    if (se.delaySec != 0.0) {
        for (auto& n : tmp.notes) n.time += se.delaySec;
        for (auto& s : tmp.sections) s.time += se.delaySec;
    }

    int expertCount = (int)tmp.notes.size();
    se.diffNoteCounts[3] = expertCount;
    se.diffNoteCounts[2] = std::max(1, (expertCount * 78) / 100);
    se.diffNoteCounts[1] = std::max(1, (expertCount * 56) / 100);
    se.diffNoteCounts[0] = std::max(1, (expertCount * 38) / 100);

    double lastNoteTime = tmp.notes.empty() ? 0.0 : (tmp.notes.back().time + tmp.notes.back().sustain);
    if (ini.count("song_length") && atof(ini.at("song_length").c_str()) > 1000.0) {
        se.lengthStr = formatTimeMMSS(atof(ini.at("song_length").c_str()) / 1000.0);
    } else if (lastNoteTime > 1.0) {
        se.lengthStr = formatTimeMMSS(lastNoteTime);
    } else {
        se.lengthStr = "03:30";
    }

    bool isEndless = (se.title.find("Endless") != std::string::npos ||
                      se.title.find("ENDLESS") != std::string::npos ||
                      se.title.find("Setlist") != std::string::npos ||
                      lastNoteTime > 480.0);

    if (isEndless && tmp.notes.size() > 50) {
        int songNum = 1;
        double firstTime = std::max(0.0, tmp.notes.front().time - 2.0);
        std::string firstLabel = "SONG 1";
        for (auto& s : tmp.sections) {
            if (std::fabs(s.time - tmp.notes.front().time) < 8.0) { firstLabel = s.name; break; }
        }
        se.chapters.push_back({firstTime, firstLabel});

        for (size_t i = 1; i < tmp.notes.size(); i++) {
            double gap = tmp.notes[i].time - (tmp.notes[i - 1].time + tmp.notes[i - 1].sustain);
            if (gap >= 4.2 && (tmp.notes[i].time - se.chapters.back().time) >= 45.0) {
                songNum++;
                double startT = std::max(0.0, tmp.notes[i].time - 2.0);
                char fallback[64];
                snprintf(fallback, sizeof(fallback), "SETLIST TRACK %d", songNum);
                std::string label = fallback;
                for (auto& s : tmp.sections) {
                    if (s.time >= tmp.notes[i - 1].time && s.time <= tmp.notes[i].time + 5.0) {
                        label = s.name;
                        break;
                    }
                }
                se.chapters.push_back({startT, label});
            }
        }
    }

    if (se.chapters.size() <= 1 && !tmp.sections.empty()) {
        se.chapters = tmp.sections;
    }
}

static void scanFolderForSongs(const std::string& baseDir, std::vector<SongEntry>& list) {
    DIR* dir = opendir(baseDir.c_str());
    if (!dir) return;

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string sub = baseDir;
        if (sub.back() != '/') sub += "/";
        sub += ent->d_name;

        std::vector<std::string> stems;
        std::string fmtLabel;
        std::string mid = sub + "/notes.mid";
        std::string chart = sub + "/notes.chart";

        if ((fileExists(mid) || fileExists(chart)) && collectSongAudioStems(sub, "", stems, fmtLabel)) {
            SongEntry se;
            se.dir = sub + "/";
            se.prefix = "";
            se.audioStemPaths = stems;
            se.audioFormatLabel = fmtLabel;
            auto ini = loadSongIni(sub + "/song.ini");
            se.title = ini.count("name") ? ini["name"] : ent->d_name;
            se.artist = ini.count("artist") ? ini["artist"] : "Custom Artist";
            if (ini.count("delay")) se.delaySec = atof(ini["delay"].c_str()) / 1000.0;
            else if (ini.count("offset")) se.delaySec = atof(ini["offset"].c_str()) / 1000.0;
            populateSongChapters(se, ini);
            list.push_back(se);
        } else {
            DIR* subDir = opendir(sub.c_str());
            if (subDir) {
                struct dirent* subEnt;
                while ((subEnt = readdir(subDir)) != nullptr) {
                    if (subEnt->d_name[0] == '.') continue;
                    std::string nested = sub + "/" + subEnt->d_name;
                    std::vector<std::string> nStems;
                    std::string nFmt;
                    std::string nMid = nested + "/notes.mid";
                    std::string nChart = nested + "/notes.chart";
                    if ((fileExists(nMid) || fileExists(nChart)) && collectSongAudioStems(nested, "", nStems, nFmt)) {
                        SongEntry se;
                        se.dir = nested + "/";
                        se.prefix = "";
                        se.audioStemPaths = nStems;
                        se.audioFormatLabel = nFmt;
                        auto ini = loadSongIni(nested + "/song.ini");
                        se.title = ini.count("name") ? ini["name"] : subEnt->d_name;
                        se.artist = ini.count("artist") ? ini["artist"] : "Custom Artist";
                        if (ini.count("delay")) se.delaySec = atof(ini["delay"].c_str()) / 1000.0;
                        else if (ini.count("offset")) se.delaySec = atof(ini["offset"].c_str()) / 1000.0;
                        populateSongChapters(se, ini);
                        list.push_back(se);
                    }
                }
                closedir(subDir);
            }
        }
    }
    closedir(dir);
}

static std::vector<SongEntry> discoverSongs() {
    std::vector<SongEntry> list;

    scanFolderForSongs("/data/GH", list);
    scanFolderForSongs("/data/songs", list);
    scanFolderForSongs("/data/hero/song", list);

    scanFolderForSongs("/mnt/usb0/GH", list);
    scanFolderForSongs("/mnt/usb0/songs", list);
    scanFolderForSongs("/mnt/usb0/hero/song", list);
    scanFolderForSongs("/mnt/usb1/GH", list);
    scanFolderForSongs("/mnt/usb1/songs", list);

    const char* baseDirs[] = {"/mnt/usb0/hero/song/", "/data/hero/song/", "/app0/assets/song/", "/app0/songs/"};
    for (const char* bd : baseDirs) {
        std::string dir(bd);
        std::string manifestPath = dir + "manifest.txt";
        FILE* mf = fopen(manifestPath.c_str(), "rb");
        if (mf) {
            char line[256];
            while (fgets(line, sizeof(line), mf)) {
                std::string pfx = trimStr(line);
                if (pfx.empty() || pfx[0] == '#') continue;
                if (pfx.back() != '_') pfx += "_";
                SongEntry se;
                se.dir = dir;
                se.prefix = pfx;
                if (!collectSongAudioStems(dir, pfx, se.audioStemPaths, se.audioFormatLabel)) continue;
                auto ini = loadSongIni(se.iniPath());
                se.title = ini.count("name") ? ini["name"] : pfx;
                se.artist = ini.count("artist") ? ini["artist"] : "Unknown Artist";
                if (ini.count("delay")) se.delaySec = atof(ini["delay"].c_str()) / 1000.0;
                else if (ini.count("offset")) se.delaySec = atof(ini["offset"].c_str()) / 1000.0;
                populateSongChapters(se, ini);
                list.push_back(se);
            }
            fclose(mf);
        }

        std::vector<std::string> rootStems;
        std::string rootFmt;
        if (collectSongAudioStems(dir, "", rootStems, rootFmt) &&
            (fileExists(dir + "notes.chart") || fileExists(dir + "notes.mid"))) {
            SongEntry se;
            se.dir = dir;
            se.prefix = "";
            se.audioStemPaths = rootStems;
            se.audioFormatLabel = rootFmt;
            auto ini = loadSongIni(se.iniPath());
            se.title = ini.count("name") ? ini["name"] : "Queens of the Stone Age - Mexicola";
            se.artist = ini.count("artist") ? ini["artist"] : "Queens of the Stone Age";
            if (ini.count("delay")) se.delaySec = atof(ini["delay"].c_str()) / 1000.0;
            else if (ini.count("offset")) se.delaySec = atof(ini["offset"].c_str()) / 1000.0;
            populateSongChapters(se, ini);
            list.push_back(se);
        }
    }

    std::vector<SongEntry> expanded;
    for (const auto& se : list) {
        expanded.push_back(se);
        bool isEndless = (se.title.find("Endless") != std::string::npos ||
                          se.title.find("ENDLESS") != std::string::npos ||
                          se.title.find("Setlist") != std::string::npos);
        if (isEndless && se.chapters.size() > 1) {
            for (size_t c = 0; c < se.chapters.size(); c++) {
                SongEntry chEntry = se;
                chEntry.isChapterEntry = true;
                chEntry.startOffsetSec = se.chapters[c].time;
                chEntry.title = "  [" + formatTimeMMSS(se.chapters[c].time) + "] " + se.chapters[c].name;
                expanded.push_back(chEntry);
            }
        }
    }
    sortSongCatalog(expanded, g_songSortMode);
    return expanded;
}

// ---------------------------------------------------------------- Custom Button Remapper, Presets & Audio/Input Thread
enum AppState {
    STATE_MAIN_MENU = 0,
    STATE_TRACKLIST = 1,
    STATE_PLAYING   = 2,
    STATE_PAUSED    = 3,
    STATE_RESULTS   = 4,
    STATE_SETTINGS  = 5
};

static int settingsCursor = 0;
static int fretboardTheme = 0; // Index 0..20 into g_highwaySkins[21]
static bool g_randomHighwayEachSong = false;
static int g_remapListeningLane = -1; // -1 = not listening, 0..4 = Green..Orange fret capture

static int pickRandomHighwayIndex(int currentIdx) {
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned int)(time(nullptr) ^ clock() ^ 0xA5A5u));
        seeded = true;
    }
    if (NUM_HIGHWAYS <= 1) return 0;
    int next = rand() % NUM_HIGHWAYS;
    if (next == currentIdx) next = (next + 1 + (rand() % (NUM_HIGHWAYS - 1))) % NUM_HIGHWAYS;
    return next;
}

// 0: Standard DS4, 1: Bumpers + Triggers, 2: Alt Classic, 3: USB Guitar, 4: Custom Remap
static int controllerPreset = 0;
static const int NUM_CTRL_PRESETS = 5;
static const char* controllerPresetNames[5] = {
    "Standard DualShock (Cross/Circle/Tri/Sq/R1)",
    "Bumpers + Triggers (L2/L1/R1/R2/Cross)",
    "Alt Classic (Cross/Circle/Tri/Sq/L1)",
    "USB Guitar (GH / Rock Band PS3/PS4)",
    "Custom User Mapping"
};

#ifndef ORBIS_PAD_BUTTON_TOUCH_PAD
#define ORBIS_PAD_BUTTON_TOUCH_PAD 0x00100000
#endif

// Atomic per-lane button masks (0=Green, 1=Red, 2=Yellow, 3=Blue, 4=Orange, 5=StarPower)
static std::atomic<int> g_laneMasks[6] = {
    {ORBIS_PAD_BUTTON_CROSS},
    {ORBIS_PAD_BUTTON_CIRCLE},
    {ORBIS_PAD_BUTTON_TRIANGLE},
    {ORBIS_PAD_BUTTON_SQUARE},
    {ORBIS_PAD_BUTTON_R1 | ORBIS_PAD_BUTTON_L1},
    {ORBIS_PAD_BUTTON_R2}
};

static void applyControllerPreset(int preset) {
    controllerPreset = ((preset % NUM_CTRL_PRESETS) + NUM_CTRL_PRESETS) % NUM_CTRL_PRESETS;
    if (controllerPreset == 0) {
        g_laneMasks[0].store(ORBIS_PAD_BUTTON_CROSS);
        g_laneMasks[1].store(ORBIS_PAD_BUTTON_CIRCLE);
        g_laneMasks[2].store(ORBIS_PAD_BUTTON_TRIANGLE);
        g_laneMasks[3].store(ORBIS_PAD_BUTTON_SQUARE);
        g_laneMasks[4].store(ORBIS_PAD_BUTTON_R1 | ORBIS_PAD_BUTTON_L1);
        g_laneMasks[5].store(ORBIS_PAD_BUTTON_R2);
    } else if (controllerPreset == 1) {
        g_laneMasks[0].store(ORBIS_PAD_BUTTON_L2);
        g_laneMasks[1].store(ORBIS_PAD_BUTTON_L1);
        g_laneMasks[2].store(ORBIS_PAD_BUTTON_R1);
        g_laneMasks[3].store(ORBIS_PAD_BUTTON_R2);
        g_laneMasks[4].store(ORBIS_PAD_BUTTON_CROSS);
        g_laneMasks[5].store(ORBIS_PAD_BUTTON_SQUARE);
    } else if (controllerPreset == 2) {
        g_laneMasks[0].store(ORBIS_PAD_BUTTON_CROSS);
        g_laneMasks[1].store(ORBIS_PAD_BUTTON_CIRCLE);
        g_laneMasks[2].store(ORBIS_PAD_BUTTON_TRIANGLE);
        g_laneMasks[3].store(ORBIS_PAD_BUTTON_SQUARE);
        g_laneMasks[4].store(ORBIS_PAD_BUTTON_L1);
        g_laneMasks[5].store(ORBIS_PAD_BUTTON_R2);
    } else if (controllerPreset == 3) {
        // Authentic PS3/PS4 Guitar Hero & Rock Band USB Guitar layout:
        // - Green Fret: R2 trigger (or L2 on RB4 Stratocasters)
        // - Red Fret: Circle
        // - Yellow Fret: Triangle
        // - Blue Fret: Cross
        // - Orange Fret: Square / R1
        // - Star Power: Touchpad / Select / Back / Tilt
        g_laneMasks[0].store(ORBIS_PAD_BUTTON_R2 | ORBIS_PAD_BUTTON_L2);
        g_laneMasks[1].store(ORBIS_PAD_BUTTON_CIRCLE);
        g_laneMasks[2].store(ORBIS_PAD_BUTTON_TRIANGLE);
        g_laneMasks[3].store(ORBIS_PAD_BUTTON_CROSS);
        g_laneMasks[4].store(ORBIS_PAD_BUTTON_SQUARE | ORBIS_PAD_BUTTON_R1);
        g_laneMasks[5].store(ORBIS_PAD_BUTTON_TOUCH_PAD | ORBIS_PAD_BUTTON_R1 | ORBIS_PAD_BUTTON_OPTIONS);
    }
}

static const char* getButtonMaskName(int mask) {
    if (mask == (ORBIS_PAD_BUTTON_R2 | ORBIS_PAD_BUTTON_L2)) return "GUITAR GREEN (R2/L2)";
    if (mask == (ORBIS_PAD_BUTTON_SQUARE | ORBIS_PAD_BUTTON_R1)) return "GUITAR ORANGE (SQ/R1)";
    if (mask == (ORBIS_PAD_BUTTON_R1 | ORBIS_PAD_BUTTON_L1)) return "R1 / L1 BUMPERS";
    if (mask & ORBIS_PAD_BUTTON_CROSS)    return "CROSS (X) / BLUE";
    if (mask & ORBIS_PAD_BUTTON_CIRCLE)   return "CIRCLE (O) / RED";
    if (mask & ORBIS_PAD_BUTTON_TRIANGLE) return "TRIANGLE / YELLOW";
    if (mask & ORBIS_PAD_BUTTON_SQUARE)   return "SQUARE / ORANGE";
    if (mask & ORBIS_PAD_BUTTON_L1)       return "L1 BUMPER";
    if (mask & ORBIS_PAD_BUTTON_R1)       return "R1 BUMPER";
    if (mask & ORBIS_PAD_BUTTON_L2)       return "L2 TRIGGER";
    if (mask & ORBIS_PAD_BUTTON_R2)       return "R2 TRIGGER";
    if (mask & ORBIS_PAD_BUTTON_TOUCH_PAD)return "TOUCHPAD / TILT";
    if (mask & ORBIS_PAD_BUTTON_LEFT)     return "D-PAD LEFT";
    if (mask & ORBIS_PAD_BUTTON_RIGHT)    return "D-PAD RIGHT";
    if (mask & ORBIS_PAD_BUTTON_UP)       return "STRUM UP / D-PAD UP";
    if (mask & ORBIS_PAD_BUTTON_DOWN)     return "STRUM DOWN / D-PAD DOWN";
    if (mask & ORBIS_PAD_BUTTON_L3)       return "L3 STICK";
    if (mask & ORBIS_PAD_BUTTON_R3)       return "R3 STICK";
    return "CUSTOM BTN";
}

static int extractSingleRemapButton(int pressedMask) {
    const int candidates[] = {
        ORBIS_PAD_BUTTON_CROSS,
        ORBIS_PAD_BUTTON_CIRCLE,
        ORBIS_PAD_BUTTON_TRIANGLE,
        ORBIS_PAD_BUTTON_SQUARE,
        ORBIS_PAD_BUTTON_L1,
        ORBIS_PAD_BUTTON_R1,
        ORBIS_PAD_BUTTON_L2,
        ORBIS_PAD_BUTTON_R2,
        ORBIS_PAD_BUTTON_TOUCH_PAD,
        ORBIS_PAD_BUTTON_LEFT,
        ORBIS_PAD_BUTTON_RIGHT,
        ORBIS_PAD_BUTTON_L3,
        ORBIS_PAD_BUTTON_R3
    };
    for (int c : candidates) {
        if (pressedMask & c) return c;
    }
    return 0;
}

static Mp4VideoPlayer g_mp4Player;

// Graphical PlayStation Button Icon Renderer (Cross, Circle, Triangle, Square)
static void drawPSButtonIcon(uint32_t* fb, float cx, float cy, float r, char type) {
    fillEllipseAA(fb, cx, cy + 1.5f, r + 1.5f, r + 1.5f, C(8, 12, 20), 200);
    fillEllipseAA(fb, cx, cy, r, r, C(24, 32, 48));
    if (type == 'X') {
        fillEllipseAA(fb, cx, cy, r * 0.86f, r * 0.86f, C(37, 99, 235));
        fillEllipseAA(fb, cx, cy, r * 0.70f, r * 0.70f, C(15, 23, 42));
        int ir = (int)(r * 0.42f);
        for (int d = -ir; d <= ir; d++) {
            rect(fb, (int)cx + d - 1, (int)cy + d - 1, 3, 3, C(147, 197, 253));
            rect(fb, (int)cx + d - 1, (int)cy - d - 1, 3, 3, C(147, 197, 253));
        }
    } else if (type == 'O') {
        fillEllipseAA(fb, cx, cy, r * 0.86f, r * 0.86f, C(220, 38, 38));
        fillEllipseAA(fb, cx, cy, r * 0.70f, r * 0.70f, C(15, 23, 42));
        fillEllipseAA(fb, cx, cy, r * 0.48f, r * 0.48f, C(252, 165, 165));
        fillEllipseAA(fb, cx, cy, r * 0.28f, r * 0.28f, C(15, 23, 42));
    } else if (type == 'T') {
        fillEllipseAA(fb, cx, cy, r * 0.86f, r * 0.86f, C(22, 163, 74));
        fillEllipseAA(fb, cx, cy, r * 0.70f, r * 0.70f, C(15, 23, 42));
        int h = (int)(r * 0.45f);
        for (int dy = -h; dy <= h; dy++) {
            int span = ((dy + h) * h) / std::max(1, 2 * h);
            int y = (int)cy + dy;
            if (dy == h || dy == h - 1) {
                rect(fb, (int)cx - span, y, 2 * span + 1, 2, C(134, 239, 172));
            } else {
                rect(fb, (int)cx - span, y, 2, 2, C(134, 239, 172));
                rect(fb, (int)cx + span - 1, y, 2, 2, C(134, 239, 172));
            }
        }
    } else if (type == 'S') {
        fillEllipseAA(fb, cx, cy, r * 0.86f, r * 0.86f, C(219, 39, 119));
        fillEllipseAA(fb, cx, cy, r * 0.70f, r * 0.70f, C(15, 23, 42));
        int sq = (int)(r * 0.42f);
        borderRect(fb, (int)cx - sq, (int)cy - sq, sq * 2, sq * 2, 2, C(15, 23, 42), C(249, 168, 212));
    }
}

struct Ev { double t; int held; int justPressed; bool isTap; };

static const int MAX_AUDIO_STEMS = 8;
static pthread_mutex_t g_vorbisMutex = PTHREAD_MUTEX_INITIALIZER;
static Ps4AudioStream g_audioStems[MAX_AUDIO_STEMS];
static bool g_isGuitarStem[MAX_AUDIO_STEMS] = {false};
static int g_numAudioStems = 0;
static int64_t g_totalFrames = 0;
static bool g_silentClockFallback = false;

// ============================================================================
// GUITAR HERO / CLONE HERO MISS & OVERSTRUM FRETBOARD FLUB AUDIO SYNTHESIZER
// ============================================================================
// 6 distinct physical-modeling damped low guitar string thud + metallic fret
// buzz + pick scrape transients (~145ms at 48kHz). Triggered on note miss or
// overstrum when g_flubSoundEnabled is active.
static const int NUM_FLUB_VARIANTS = 6;
static const int FLUB_FRAMES = 6960; // 145ms @ 48000 Hz
static short g_flubBank[NUM_FLUB_VARIANTS][FLUB_FRAMES];
static std::atomic<int>  g_flubTriggerSeq{0};
static std::atomic<bool> g_guitarStemMuted{false};

static void initGuitarFlubSounds() {
    // 6 authentic low string fundamentals (Drop D, Low E, F, G, A, D)
    const float baseFreqs[NUM_FLUB_VARIANTS] = { 73.4f, 82.4f, 87.3f, 98.0f, 110.0f, 146.8f };
    uint32_t rng = 0x9E3779B9u;
    auto nextNoise = [&]() -> float {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return ((float)(rng & 0xFFFFu) / 32768.0f) - 1.0f;
    };

    for (int v = 0; v < NUM_FLUB_VARIANTS; v++) {
        float f0 = baseFreqs[v];
        float phase1 = 0.0f, phase2 = 0.0f, phase3 = 0.0f, phase4 = 0.0f;
        float bpf1 = 0.0f, bpf2 = 0.0f;
        for (int i = 0; i < FLUB_FRAMES; i++) {
            float t = (float)i / (float)RATE;

            // 1. Sharp Pick Attack / Plectrum Snap (first 10-12ms):
            // Bandpass-filtered high frequency click simulating plastic pick snapping wound string
            float clickEnv = std::exp(-t * 260.0f);
            float n = nextNoise();
            bpf1 += (n - bpf1) * 0.42f;
            bpf2 += (bpf1 - bpf2) * 0.42f;
            float pickClick = (bpf1 - bpf2) * 1.6f * clickEnv;

            // 2. Heavy Palm-Muted String Body:
            // Fast downward pitch detuning as palm mutes string against bridge
            float pitchDrop = 1.0f - 0.28f * (1.0f - std::exp(-t * 45.0f));
            float freq1 = f0 * pitchDrop;
            // Dissonant minor second beat frequency (audible sour clash)
            float freq2 = f0 * 1.05946f * pitchDrop;
            // Tritone disharmonic string buzz
            float freq3 = f0 * 1.4142f * pitchDrop;
            // High metallic fret wire scrape
            float freq4 = f0 * 2.82f;

            phase1 += (2.0f * 3.14159265f * freq1) / (float)RATE;
            phase2 += (2.0f * 3.14159265f * freq2) / (float)RATE;
            phase3 += (2.0f * 3.14159265f * freq3) / (float)RATE;
            phase4 += (2.0f * 3.14159265f * freq4) / (float)RATE;

            float envBody   = std::exp(-t * 18.0f);
            float envRattle = std::exp(-t * 36.0f);

            // Raw physical synthesis
            float raw = std::sin(phase1) * 0.55f * envBody
                      + std::sin(phase2) * 0.35f * envBody
                      + std::sin(phase3) * 0.25f * envRattle
                      + std::sin(phase4) * 0.18f * envRattle
                      + pickClick * 0.75f;

            // 3. Authentic High-Gain Tube Overdrive Saturation (Marshall JCM amp clipping)
            float driven = raw * 2.1f;
            float saturated = std::clamp(driven - 0.16f * (driven * driven * driven), -1.0f, 1.0f);

            // 4. Loud, punchy amplitude (±28,500 for unmistakable prominence in the mix!)
            int pcm = (int)(saturated * 28500.0f);
            g_flubBank[v][i] = (short)std::clamp(pcm, -32000, 32000);
        }
    }
}

static std::atomic<int> g_appState{STATE_MAIN_MENU};
static std::atomic<int64_t> g_submitted{0};
static std::atomic<uint64_t> g_audioTicks{0};
static std::atomic<int64_t> g_seekRequestFrame{-999999999LL};
static std::atomic<int> g_held{0}, g_calibMs{0};
static std::atomic<int> g_uiPressed{0};
static std::atomic<bool> g_tapMode{true};

static Ev g_ring[256];
static std::atomic<uint32_t> g_head{0}, g_tail{0};
static int g_audio, g_pad;

static void saveUserSettings() {
    mkdir("/data/GH", 0777);
    FILE* f = fopen("/data/GH/settings.ini", "wb");
    if (!f) return;
    fprintf(f, "highway_theme=%d\n", fretboardTheme);
    fprintf(f, "random_highway=%d\n", g_randomHighwayEachSong ? 1 : 0);
    fprintf(f, "tap_mode=%d\n", g_tapMode.load() ? 1 : 0);
    fprintf(f, "calib_ms=%d\n", g_calibMs.load());
    fprintf(f, "ctrl_preset=%d\n", controllerPreset);
    fprintf(f, "song_sort_mode=%d\n", g_songSortMode);
    fprintf(f, "theme_preset=%d\n", g_themePresetIdx);
    fprintf(f, "menu_bg_video=%d\n", g_menuBgVideoEnabled ? 1 : 0);
    fprintf(f, "flub_sound=%d\n", g_flubSoundEnabled ? 1 : 0);
    for (int s = 0; s < 4; s++) {
        fprintf(f, "slot_h_%d=%d\n", s, g_themeSlotH[s]);
        fprintf(f, "slot_s_%d=%d\n", s, g_themeSlotS[s]);
        fprintf(f, "slot_v_%d=%d\n", s, g_themeSlotV[s]);
    }
    for (int i = 0; i < 6; i++) {
        fprintf(f, "lane_mask_%d=%d\n", i, g_laneMasks[i].load());
    }
    fclose(f);
}

static void loadUserSettings() {
    FILE* f = fopen("/data/GH/settings.ini", "rb");
    if (!f) {
        recomputeThemeColorsFromHsv();
        return;
    }
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        std::string k = trimStr(line);
        int val = atoi(trimStr(eq + 1).c_str());
        if (k == "highway_theme") fretboardTheme = std::clamp(val, 0, NUM_HIGHWAYS - 1);
        else if (k == "random_highway") g_randomHighwayEachSong = (val != 0);
        else if (k == "tap_mode") g_tapMode.store(val != 0);
        else if (k == "calib_ms") g_calibMs.store(std::clamp(val, -250, 250));
        else if (k == "ctrl_preset") controllerPreset = std::clamp(val, 0, NUM_CTRL_PRESETS - 1);
        else if (k == "song_sort_mode") g_songSortMode = std::clamp(val, 0, 2);
        else if (k == "theme_preset") g_themePresetIdx = std::clamp(val, 0, NUM_THEME_PRESETS - 1);
        else if (k == "menu_bg_video") g_menuBgVideoEnabled = (val != 0);
        else if (k == "flub_sound") g_flubSoundEnabled = (val != 0);
        else if (k.rfind("slot_h_", 0) == 0) {
            int idx = atoi(k.c_str() + 7);
            if (idx >= 0 && idx < 4) g_themeSlotH[idx] = std::clamp(val, 0, 359);
        } else if (k.rfind("slot_s_", 0) == 0) {
            int idx = atoi(k.c_str() + 7);
            if (idx >= 0 && idx < 4) g_themeSlotS[idx] = std::clamp(val, 0, 100);
        } else if (k.rfind("slot_v_", 0) == 0) {
            int idx = atoi(k.c_str() + 7);
            if (idx >= 0 && idx < 4) g_themeSlotV[idx] = std::clamp(val, 0, 100);
        } else if (k.rfind("lane_mask_", 0) == 0) {
            int idx = atoi(k.c_str() + 10);
            if (idx >= 0 && idx < 6 && val > 0) g_laneMasks[idx].store(val);
        }
    }
    fclose(f);
    recomputeThemeColorsFromHsv();
}

// 55ms baseline HDMI & AudioOut DMA hardware latency compensation for TV display sync
static const double HW_AUDIO_LATENCY_SEC = 0.055;

static double songTime() {
    return (double)(g_submitted.load() - 2 * GRAIN) / RATE - HW_AUDIO_LATENCY_SEC - (double)g_calibMs.load() / 1000.0;
}

static void requestSeekSec(double targetSec) {
    int64_t frame = (int64_t)(targetSec * RATE);
    g_seekRequestFrame.store(frame);
}

static void* audioThread(void*) {
    // Elevate audio output thread priority to avoid any thread starvation/jitter
    struct sched_param param;
    param.sched_priority = 90;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);

    static short silence[GRAIN * 2];
    static short pcmBuf[GRAIN * 2];
    static short stemBuf[GRAIN * 2];
    int prev = 0;
    int prevHeld = 0;
    int lastHandledFlubSeq = 0;
    int activeFlubVariant = 0;
    int activeFlubPos = FLUB_FRAMES;

    for (;;) {
        int state = g_appState.load();
        int64_t seekTarget = g_seekRequestFrame.exchange(-999999999LL);

        if (seekTarget != -999999999LL) {
            pthread_mutex_lock(&g_vorbisMutex);
            uint64_t sampleTarget = (seekTarget > 0) ? (uint64_t)seekTarget : 0ULL;
            for (int s = 0; s < g_numAudioStems; s++) {
                if (sampleTarget == 0ULL) {
                    g_audioStems[s].seekStart();
                } else {
                    g_audioStems[s].seekSample48k(sampleTarget);
                }
            }
            g_submitted.store(seekTarget);
            pthread_mutex_unlock(&g_vorbisMutex);
        }

        // Check if gameplay or settings preview triggered a new Guitar Hero miss flub sound
        int curFlubSeq = g_flubTriggerSeq.load();
        if (curFlubSeq != lastHandledFlubSeq) {
            lastHandledFlubSeq = curFlubSeq;
            if (g_flubSoundEnabled) {
                activeFlubVariant = (curFlubSeq & 0x7FFFFFFF) % NUM_FLUB_VARIANTS;
                activeFlubPos = 0;
            }
        }

        short* outPtr = silence;
        if (state == STATE_PLAYING) {
            int64_t pos = g_submitted.load();
            bool muteGuitar = g_guitarStemMuted.load();
            if (pos >= 0 && pos < g_totalFrames) {
                pthread_mutex_lock(&g_vorbisMutex);
                if (g_numAudioStems == 1) {
                    int got = g_audioStems[0].read48kStereoFrames(pcmBuf, GRAIN);
                    if (got < GRAIN) {
                        memset(pcmBuf + got * 2, 0, (size_t)(GRAIN - got) * 2 * sizeof(short));
                    }
                    // Prominent ducking on single-track audio during an active flub transient
                    if (muteGuitar && activeFlubPos < FLUB_FRAMES) {
                        for (int i = 0; i < GRAIN * 2; i++) {
                            pcmBuf[i] = (short)(((int)pcmBuf[i] * 120) >> 8);
                        }
                    }
                    outPtr = pcmBuf;
                } else if (g_numAudioStems > 1) {
                    memset(pcmBuf, 0, sizeof(pcmBuf));
                    for (int s = 0; s < g_numAudioStems; s++) {
                        int got = g_audioStems[s].read48kStereoFrames(stemBuf, GRAIN);
                        int count = got * 2;
                        // Authentic Guitar Hero 3 / Clone Hero guitar stem muting: 0 gain on guitar stem while muted!
                        int gainNum = (muteGuitar && g_isGuitarStem[s]) ? 0 : 3;
                        for (int i = 0; i < count; i++) {
                            int sum = (int)pcmBuf[i] + ((int)stemBuf[i] * gainNum) / 4;
                            if (sum > 32767) sum = 32767;
                            else if (sum < -32768) sum = -32768;
                            pcmBuf[i] = (short)sum;
                        }
                    }
                    outPtr = pcmBuf;
                }
                pthread_mutex_unlock(&g_vorbisMutex);
            }
            g_submitted.store(pos + GRAIN);
        }

        // Mix active Guitar Hero Fretboard Flub Sound into 48kHz output buffer
        if (activeFlubPos < FLUB_FRAMES) {
            if (outPtr == silence) {
                memset(pcmBuf, 0, sizeof(pcmBuf));
                outPtr = pcmBuf;
            }
            for (int f = 0; f < GRAIN && activeFlubPos < FLUB_FRAMES; f++, activeFlubPos++) {
                int flubSample = (int)g_flubBank[activeFlubVariant][activeFlubPos];
                int L = (int)outPtr[f * 2]     + flubSample;
                int R = (int)outPtr[f * 2 + 1] + flubSample;
                outPtr[f * 2]     = (short)std::clamp(L, -32768, 32767);
                outPtr[f * 2 + 1] = (short)std::clamp(R, -32768, 32767);
            }
        }

        sceAudioOutOutput(g_audio, outPtr);
        g_audioTicks.fetch_add(GRAIN);

        OrbisPadData pd;
        if (scePadReadState(g_pad, &pd) != 0) continue;
        int b = pd.buttons;

        // Real-time 5-lane fret evaluation from customizable g_laneMasks[0..4]
        int held = 0;
        for (int l = 0; l < 5; l++) {
            if (b & g_laneMasks[l].load()) {
                held |= (1 << l);
            }
        }
        g_held.store(held);

        int pressed = b & ~prev;
        int justPressedFrets = held & ~prevHeld;
        prev = b;
        prevHeld = held;

        if (pressed != 0) {
            g_uiPressed.fetch_or(pressed);
        }

        if (state == STATE_PLAYING) {
            bool strummed = (pressed & (ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_DOWN)) != 0;
            bool tapTriggered = g_tapMode.load() && (justPressedFrets != 0);
            if (strummed || tapTriggered) {
                uint32_t h = g_head.load();
                g_ring[h % 256] = {songTime(), held, justPressedFrets, !strummed && tapTriggered};
                g_head.store(h + 1);
            }
        }
    }
    return nullptr;
}

static bool g_menuVideoModeActive = false;

static void ensureMenuBackdropVideoOpen(bool forceNextRandom = false) {
    if (!g_menuBgVideoEnabled) {
        if (g_menuVideoModeActive) {
            g_mp4Player.close();
            g_menuVideoModeActive = false;
        }
        return;
    }
    if (forceNextRandom || !g_menuVideoModeActive || !g_mp4Player.isDecodable) {
        g_mp4Player.close();
        openSongOrDefaultVideo(g_mp4Player, "");
        g_menuVideoModeActive = g_mp4Player.isDecodable;
    }
}

static void renderMenuBackdrop(uint32_t* fb) {
    double menuTimeSec = (double)g_audioTicks.load() / (double)RATE;
    if (g_menuBgVideoEnabled && g_menuVideoModeActive && g_mp4Player.isDecodable) {
        if (g_mp4Player.updateAndBlitMenuBackdrop(menuTimeSec, fb, g_themeBgPrimary, g_themeBgSecondary, 138)) {
            return;
        }
    }
    gradRectV(fb, 0, 0, W, H, g_themeBgPrimary, g_themeBgSecondary);
}

// ---------------------------------------------------------------- Load Song & Difficulty
static bool startSongSession(const SongEntry& se, int diffIdx, double customStartSec, Chart& chart, Game& game) {
    g_appState.store(STATE_TRACKLIST);

    Chart loaded;
    std::string used;
    if (!loadChart(se.chartPath(), diffIdx, loaded) &&
        !loadMidi(se.midPath(), diffIdx, loaded, &used)) {
        return false;
    }

    loaded.title = se.title;
    loaded.artist = se.artist;
    loaded.charter = se.charter;

    if (se.delaySec != 0.0) {
        for (auto& n : loaded.notes) n.time += se.delaySec;
        for (auto& s : loaded.sections) s.time += se.delaySec;
    }
    if (!se.chapters.empty() && loaded.sections.size() < se.chapters.size()) {
        loaded.sections = se.chapters;
    }

    pthread_mutex_lock(&g_vorbisMutex);
    for (int s = 0; s < MAX_AUDIO_STEMS; s++) {
        g_audioStems[s].close();
        g_isGuitarStem[s] = false;
    }
    g_numAudioStems = 0;
    g_silentClockFallback = false;
    g_guitarStemMuted.store(false);

    uint64_t maxSamples = 0;
    std::vector<std::string> stems = se.audioStemPaths;
    std::string dummyFmt;
    if (stems.empty()) {
        collectSongAudioStems(se.dir, se.prefix, stems, dummyFmt);
    }

    // Open all discovered stems natively (.opus, .ogg, .mp3, .flac, .wav)
    for (size_t i = 0; i < stems.size() && g_numAudioStems < MAX_AUDIO_STEMS; i++) {
        if (g_audioStems[g_numAudioStems].open(stems[i])) {
            uint64_t len = g_audioStems[g_numAudioStems].totalSamples48k;
            if (len > maxSamples) maxSamples = len;
            std::string lowerPath = stems[i];
            for (char& c : lowerPath) c = (char)std::tolower((unsigned char)c);
            g_isGuitarStem[g_numAudioStems] = (lowerPath.find("guitar") != std::string::npos ||
                                               lowerPath.find("rhythm") != std::string::npos);
            g_numAudioStems++;
        }
    }

    // Fallback check for sibling song.opus / song.ogg / song.mp3 / song.flac / song.wav
    if (g_numAudioStems == 0) {
        const char* fallbacks[] = {"song.opus", "song.ogg", "song.mp3", "song.flac", "song.wav", "guitar.opus", "guitar.ogg", "audio.opus", "audio.ogg"};
        for (const char* fbName : fallbacks) {
            std::string cand = se.dir + se.prefix + fbName;
            if (fileExists(cand) && g_audioStems[0].open(cand)) {
                maxSamples = g_audioStems[0].totalSamples48k;
                g_isGuitarStem[0] = false;
                g_numAudioStems = 1;
                break;
            }
        }
    }

    double chartDur = loaded.notes.empty() ? 210.0 : (loaded.notes.back().time + loaded.notes.back().sustain + 6.0);
    uint64_t chartSamples = (uint64_t)(chartDur * RATE);
    if (g_numAudioStems == 0) {
        g_silentClockFallback = true;
        maxSamples = chartSamples;
    } else if (maxSamples < chartSamples) {
        // Ensure audio playback window never cuts off before the last note ends
        maxSamples = chartSamples;
    }

    if (maxSamples == 0) maxSamples = (uint64_t)(RATE * 600);
    g_totalFrames = (int64_t)maxSamples;
    pthread_mutex_unlock(&g_vorbisMutex);

    chart = loaded;
    game.reset(&chart, diffIdx);

    // Stage background image fallback: ONLY use dedicated widescreen stage backgrounds,
    // NEVER stretch square album.png/album.jpg/cover.png/cover.jpg as the stage background!
    std::string songBg;
    const char* extList[] = {
        "background.png", "background.jpg", "background.jpeg",
        "bg.png", "bg.jpg", "bg.jpeg",
        "video.png", "video.jpg"
    };
    for (const char* ext : extList) {
        std::string cand = se.dir + ext;
        if (FILE* f = fopen(cand.c_str(), "rb")) {
            fclose(f);
            songBg = cand;
            break;
        }
    }

    // Open song's own .mp4 if present & decodable; otherwise automatically open a random default background .mp4!
    g_menuVideoModeActive = false;
    g_mp4Player.close();
    openSongOrDefaultVideo(g_mp4Player, se.dir);

    if (g_randomHighwayEachSong) {
        fretboardTheme = pickRandomHighwayIndex(fretboardTheme);
    }

    buildStageCaches(songBg.empty() ? nullptr : songBg.c_str(), fretboardTheme);

    double startSec = (customStartSec > 0.0) ? customStartSec : se.startOffsetSec;
    int64_t initialFrame = (startSec > 0.0) ? (int64_t)(startSec * RATE) : -(int64_t)(LEAD * RATE);
    g_submitted.store(initialFrame);
    if (startSec > 0.0) {
        game.seekTo(startSec);
        requestSeekSec(startSec);
    } else {
        requestSeekSec(-LEAD);
    }

    g_tail.store(g_head.load());
    g_appState.store(STATE_PLAYING);
    return true;
}

// ---------------------------------------------------------------- Main Application Loop
int main() {
    OrbisUserServiceInitializeParams up; up.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    sceUserServiceInitialize(&up);
    OrbisUserServiceUserId uid = 0; sceUserServiceGetInitialUser(&uid);

    if (!videoInit()) { for (;;) sceKernelUsleep(1000000); }
    sceSystemServiceHideSplashScreen();

    initGuitarFlubSounds();
    loadUserSettings();
    loadSongPlayStats();
    initStarLookups();
    initOfficialCloneHeroLogo();
    buildStageCaches(nullptr, fretboardTheme);
    ensureMenuBackdropVideoOpen(true);

    std::vector<SongEntry> catalog = discoverSongs();

    scePadInit();
    g_pad = scePadOpen(uid, 0, 0, NULL);
    sceAudioOutInit();
    g_audio = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, GRAIN, RATE, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    g_submitted.store(-(int64_t)(LEAD * RATE));
    pthread_t th; pthread_create(&th, NULL, audioThread, NULL);

    Chart chart;
    Game g;
    int prevGameFlubEvents = 0;

    int selectedSong = 0;
    int selectedDiff = 2; // Default to HARD (0=EASY, 1=MEDIUM, 2=HARD, 3=EXPERT)
    bool showDiffModal = false;
    int selectedChapterIdx = 0;
    int pauseCursor = 0;
    int pauseSectionCursor = 0;
    size_t cachedSectionIdx = 0;

    const char* diffNames[4] = {"EASY", "MEDIUM", "HARD", "EXPERT"};
    const char* diffDescs[4] = {
        "3 FRETS (GREEN / RED / YELLOW)  •  RELAXED SPEED",
        "4 FRETS (GREEN .. BLUE)  •  BALANCED SPEED",
        "5 FRETS (ALL 5 LANES)  •  FAST HIGHWAY",
        "5 FRETS (FULL CHART & CHORDS)  •  PRO CLONE HERO SPEED"
    };
    const double diffHighwayDur[4] = {1.50, 1.32, 1.16, 1.04};
    const uint32_t diffCols[4] = {C(34, 197, 94), C(250, 204, 21), C(249, 115, 22), C(239, 68, 68)};

    const uint32_t col[6]    = {C(22, 196, 46), C(228, 30, 42), C(245, 208, 12), C(30, 118, 242), C(238, 116, 14), C(179, 0, 255)};
    const uint32_t bright[6] = {C(74, 245, 96), C(255, 82, 92), C(255, 242, 75), C(96, 175, 255), C(255, 168, 56), C(216, 115, 255)};

    int mainMenuCursor = 0;
    const char* mainMenuLabels[3] = {
        "QUICKPLAY", "SETTINGS", "QUIT"
    };
    char scanBannerText[128] = "";
    int scanBannerTimer = 0;
    char calibToastText[96] = "";
    int calibToastTimer = 0;

    for (;;) {
        // All rendering targets CPU-cached g_workFb for zero uncached video-memory reads!
        uint32_t* fb = g_workFb;
        int pressed = g_uiPressed.exchange(0);
        int state = g_appState.load();

        // ====================================================================
        // SCREEN 0: CLONE HERO MAIN MENU TITLE SCREEN (WITH 1080P VIDEO BACKDROP)
        // ====================================================================
        if (state == STATE_MAIN_MENU) {
            ensureMenuBackdropVideoOpen(false);

            if (pressed & ORBIS_PAD_BUTTON_UP) {
                mainMenuCursor = (mainMenuCursor - 1 + 3) % 3;
            }
            if (pressed & ORBIS_PAD_BUTTON_DOWN) {
                mainMenuCursor = (mainMenuCursor + 1) % 3;
            }
            if (pressed & ORBIS_PAD_BUTTON_SQUARE) {
                ensureMenuBackdropVideoOpen(true);
            }
            if (pressed & ORBIS_PAD_BUTTON_CROSS) {
                if (mainMenuCursor == 0) {
                    showDiffModal = false;
                    g_appState.store(STATE_TRACKLIST);
                    flip();
                    continue;
                } else if (mainMenuCursor == 1) {
                    g_remapListeningLane = -1;
                    g_appState.store(STATE_SETTINGS);
                    settingsCursor = 0;
                    flip();
                    continue;
                } else if (mainMenuCursor == 2) {
                    exit(0);
                }
            }

            // Live 1080p MP4 Background Video or Custom Theme Gradient Backdrop
            renderMenuBackdrop(fb);

            // Left Slanted Parallelogram Menu Stack (3 Clean Items: Quickplay, Settings, Quit)
            for (int i = 0; i < 3; i++) {
                int by = 320 + i * 92;
                bool isSel = (i == mainMenuCursor);
                if (isSel) {
                    drawSlantedButton(fb, 90, by, 460, 68, 0.16f, C(250, 250, 252), g_themeTrimPrimary, 3);
                    drawTextHD(fb, 140, by + 20, mainMenuLabels[i], C(16, 20, 28), false, 380);
                } else {
                    drawSlantedButton(fb, 90, by, 460, 68, 0.16f,
                                      lerpCol(g_themeBgSecondary, C(12, 24, 42), 130),
                                      g_themeTrimSecondary, 2);
                    drawTextHD(fb, 140, by + 20, mainMenuLabels[i], C(226, 232, 240), false, 380);
                }
            }

            // Large High-Resolution Official-Style 3D Metallic "CLONE HERO" Logo
            drawOfficialCloneHeroLogo(fb, 1140, 238);

            // Top Right Version & Mini Now Playing Preview (Properly Sized Backbar)
            drawTextHDRight(fb, W - 45, 34, BUILD_FULL_STR, g_themeTrimPrimary, false);
            borderRect(fb, W - 470, 66, 425, 92, 2,
                       lerpCol(g_themeBgSecondary, C(12, 20, 34), 120), g_themeTrimPrimary);
            if (!catalog.empty()) {
                const SongEntry& previewSong = catalog[selectedSong];
                drawTextHD(fb, W - 452, 74, previewSong.title.c_str(), C(255, 255, 255), false, 390);
                drawTextHD(fb, W - 452, 102, previewSong.artist.c_str(), C(186, 200, 218), false, 390);
            }
            rect(fb, W - 452, 138, 390, 4, C(28, 42, 64));
            rect(fb, W - 452, 138, 165, 4, g_themeTrimSecondary);
            drawTextHDRight(fb, W - 45, 168, "SOLO QUICKPLAY READY", C(203, 213, 225), false);

            // Center-Bottom Prompt Pill
            drawSlantedButton(fb, 870, 746, 540, 54, 0.14f,
                              lerpCol(g_themeBgSecondary, C(18, 32, 54), 120), g_themeTrimPrimary, 2);
            drawTextHDCentered(fb, 1140, 759, "Press Cross (X) on Quickplay", C(255, 255, 255), false, 490);

            // Single Player Status Footer (No 48kHz audio engine text!)
            borderRect(fb, 530, 872, 860, 54, 2,
                       lerpCol(g_themeBgSecondary, C(18, 28, 48), 110), g_themeTrimPrimary);
            drawTextHDCentered(fb, 960, 885, "PLAYER 1: GUITAR READY   |   SOLO / QUICKPLAY", C(255, 255, 255), false, 820);

            drawPSButtonIcon(fb, 390.0f, 968.0f, 14.0f, 'X');
            drawTextHD(fb, 414, 955, "SELECT OPTION", C(226, 232, 240), false);
            drawPSButtonIcon(fb, 680.0f, 968.0f, 14.0f, 'S');
            drawTextHD(fb, 704, 955, "NEXT BG VIDEO", C(226, 232, 240), false);
            drawTextHD(fb, 950, 955, "[R2] STAR POWER     [OPTIONS] PAUSE", C(186, 200, 218), false);

            flip();
            continue;
        }

        // ====================================================================
        // SCREEN 5: REORGANIZED SUB-MENU SETTINGS & 2D COLOR SQUARE STUDIO
        // ====================================================================
        if (state == STATE_SETTINGS) {
            ensureMenuBackdropVideoOpen(false);

            static int g_settingsSubMenu = 0; // 0=Theme & Colors, 1=Audio & Calib, 2=Gameplay, 3=Controller
            const int subMenuCounts[4] = { 8, 4, 4, 6 };
            int curSubMenuRows = subMenuCounts[g_settingsSubMenu];

            // Modal Capture Mode: Waiting for user to press a controller button for lane g_remapListeningLane
            if (g_remapListeningLane >= 0 && g_remapListeningLane < 5) {
                if (pressed & ORBIS_PAD_BUTTON_OPTIONS) {
                    g_remapListeningLane = -1;
                } else {
                    int candidateMask = extractSingleRemapButton(pressed);
                    if (candidateMask != 0) {
                        int oldMask = g_laneMasks[g_remapListeningLane].load();
                        for (int other = 0; other < 5; other++) {
                            if (other != g_remapListeningLane && g_laneMasks[other].load() == candidateMask) {
                                g_laneMasks[other].store(oldMask);
                            }
                        }
                        g_laneMasks[g_remapListeningLane].store(candidateMask);
                        controllerPreset = 3;
                        saveUserSettings();
                        snprintf(scanBannerText, sizeof(scanBannerText), "MAPPED FRET %d TO [%s]!",
                                 g_remapListeningLane + 1, getButtonMaskName(candidateMask));
                        scanBannerTimer = 150;
                        g_remapListeningLane = -1;
                    }
                }
            } else {
                // L1 / R1 Sub-Menu Tab Switching
                if (pressed & ORBIS_PAD_BUTTON_L1) {
                    g_settingsSubMenu = (g_settingsSubMenu + 3) % 4;
                    settingsCursor = 0;
                }
                if (pressed & ORBIS_PAD_BUTTON_R1) {
                    g_settingsSubMenu = (g_settingsSubMenu + 1) % 4;
                    settingsCursor = 0;
                }

                if (pressed & ORBIS_PAD_BUTTON_UP) {
                    settingsCursor = (settingsCursor - 1 + curSubMenuRows) % curSubMenuRows;
                }
                if (pressed & ORBIS_PAD_BUTTON_DOWN) {
                    settingsCursor = (settingsCursor + 1) % curSubMenuRows;
                }

                bool triggerRandomHighway = (pressed & ORBIS_PAD_BUTTON_TRIANGLE) != 0;
                if (triggerRandomHighway) {
                    fretboardTheme = pickRandomHighwayIndex(fretboardTheme);
                    buildStageCaches(nullptr, fretboardTheme);
                    saveUserSettings();
                    scanBannerTimer = 150;
                    snprintf(scanBannerText, sizeof(scanBannerText), "RANDOM HIGHWAY: [%d/%d] %s",
                             fretboardTheme + 1, NUM_HIGHWAYS, g_highwaySkins[fretboardTheme].name);
                }

                // SUB-MENU 0: THEME & COLOR STUDIO
                if (g_settingsSubMenu == 0) {
                    if (settingsCursor == 0) { // Highway Skin
                        if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_CROSS)) {
                            int delta = (pressed & ORBIS_PAD_BUTTON_LEFT) ? (NUM_HIGHWAYS - 1) : 1;
                            fretboardTheme = (fretboardTheme + delta) % NUM_HIGHWAYS;
                            buildStageCaches(nullptr, fretboardTheme);
                            saveUserSettings();
                        }
                    } else if (settingsCursor == 1) { // Auto-Randomize Highway
                        if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_CROSS)) {
                            g_randomHighwayEachSong = !g_randomHighwayEachSong;
                            saveUserSettings();
                        }
                    } else if (settingsCursor == 2) { // UI Theme Preset
                        if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_CROSS)) {
                            int delta = (pressed & ORBIS_PAD_BUTTON_LEFT) ? (NUM_THEME_PRESETS - 1) : 1;
                            applyThemePreset((g_themePresetIdx + delta) % NUM_THEME_PRESETS);
                            buildStageCaches(nullptr, fretboardTheme);
                            saveUserSettings();
                        }
                    } else if (settingsCursor == 3) { // Color Slot Target
                        if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_CROSS)) {
                            int delta = (pressed & ORBIS_PAD_BUTTON_LEFT) ? 3 : 1;
                            g_activeColorSlot = (g_activeColorSlot + delta) % 4;
                        }
                    } else if (settingsCursor == 4) { // Hue Slider (0..359)
                        if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT)) {
                            int step = (pressed & ORBIS_PAD_BUTTON_RIGHT) ? 8 : -8;
                            g_themeSlotH[g_activeColorSlot] = (g_themeSlotH[g_activeColorSlot] + step + 360) % 360;
                            g_themePresetIdx = 5;
                            recomputeThemeColorsFromHsv();
                            buildStageCaches(nullptr, fretboardTheme);
                            saveUserSettings();
                        }
                    } else if (settingsCursor == 5) { // Saturation Slider (0..100%)
                        if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT)) {
                            int step = (pressed & ORBIS_PAD_BUTTON_RIGHT) ? 5 : -5;
                            g_themeSlotS[g_activeColorSlot] = std::clamp(g_themeSlotS[g_activeColorSlot] + step, 0, 100);
                            g_themePresetIdx = 5;
                            recomputeThemeColorsFromHsv();
                            buildStageCaches(nullptr, fretboardTheme);
                            saveUserSettings();
                        }
                    } else if (settingsCursor == 6) { // Brightness / Value Slider (0..100%)
                        if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT)) {
                            int step = (pressed & ORBIS_PAD_BUTTON_RIGHT) ? 5 : -5;
                            g_themeSlotV[g_activeColorSlot] = std::clamp(g_themeSlotV[g_activeColorSlot] + step, 4, 100);
                            g_themePresetIdx = 5;
                            recomputeThemeColorsFromHsv();
                            buildStageCaches(nullptr, fretboardTheme);
                            saveUserSettings();
                        }
                    } else if (settingsCursor == 7) { // Menu Background Video
                        if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT)) {
                            g_menuBgVideoEnabled = !g_menuBgVideoEnabled;
                            ensureMenuBackdropVideoOpen(true);
                            saveUserSettings();
                        } else if (pressed & ORBIS_PAD_BUTTON_CROSS) {
                            g_menuBgVideoEnabled = true;
                            ensureMenuBackdropVideoOpen(true);
                            saveUserSettings();
                        }
                    }
                }
                // SUB-MENU 1: AUDIO & CALIBRATION
                else if (g_settingsSubMenu == 1) {
                    if (settingsCursor == 0) { // Audio Latency / Sync Offset
                        if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT)) {
                            int delta = (pressed & ORBIS_PAD_BUTTON_RIGHT) ? 5 : -5;
                            int cur = std::clamp(g_calibMs.load() + delta, -250, 250);
                            g_calibMs.store(cur);
                            saveUserSettings();
                        }
                    } else if (settingsCursor == 1) { // Guitar Miss Flub Sound Toggle & Preview
                        if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT)) {
                            g_flubSoundEnabled = !g_flubSoundEnabled;
                            saveUserSettings();
                        } else if (pressed & ORBIS_PAD_BUTTON_CROSS) {
                            g_flubSoundEnabled = true;
                            g_flubTriggerSeq.fetch_add(1);
                            saveUserSettings();
                        }
                    } else if (settingsCursor == 2) { // Flub Sound Preview Audition
                        if (pressed & (ORBIS_PAD_BUTTON_CROSS | ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT)) {
                            g_flubSoundEnabled = true;
                            g_flubTriggerSeq.fetch_add(1);
                        }
                    } else if (settingsCursor == 3) { // Rescan Songs in /data/GH
                        if (pressed & ORBIS_PAD_BUTTON_CROSS) {
                            catalog = discoverSongs();
                            ensureMenuBackdropVideoOpen(true);
                            scanBannerTimer = 180;
                            snprintf(scanBannerText, sizeof(scanBannerText), "RESCANNED /DATA/GH: %d SONGS & BG VIDEOS!", (int)catalog.size());
                        }
                    }
                }
                // SUB-MENU 2: GAMEPLAY & MECHANICS
                else if (g_settingsSubMenu == 2) {
                    if (settingsCursor == 0) { // Tap Mode Toggle
                        if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_CROSS)) {
                            g_tapMode.store(!g_tapMode.load());
                            saveUserSettings();
                        }
                    } else if (settingsCursor == 1) { // Star Power Activation Button
                        if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT)) {
                            int curM = g_laneMasks[5].load();
                            g_laneMasks[5].store((curM == ORBIS_PAD_BUTTON_R2) ? ORBIS_PAD_BUTTON_L2 : ORBIS_PAD_BUTTON_R2);
                            saveUserSettings();
                        }
                    } else if (settingsCursor == 2) { // Hit Window Tightness Preset
                        if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_CROSS)) {
                            selectedDiff = (selectedDiff + 1) % 4;
                        }
                    } else if (settingsCursor == 3) { // Highway Scroll Speed
                        if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT)) {
                            // Cycle highway scroll factor
                        }
                    }
                }
                // SUB-MENU 3: CONTROLLER & BUTTON REMAPPING
                else if (g_settingsSubMenu == 3) {
                    if (settingsCursor == 0) { // Controller Preset Layout
                        if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_CROSS)) {
                            int delta = (pressed & ORBIS_PAD_BUTTON_LEFT) ? 3 : 1;
                            controllerPreset = (controllerPreset + delta) % 4;
                            if (controllerPreset < 3) {
                                applyControllerPreset(controllerPreset);
                            }
                            saveUserSettings();
                        }
                    } else if (settingsCursor >= 1 && settingsCursor <= 5) { // 5 Fret Lane Remappers
                        int laneIdx = settingsCursor - 1;
                        if (pressed & ORBIS_PAD_BUTTON_CROSS) {
                            g_remapListeningLane = laneIdx;
                        } else if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT)) {
                            const int cycleMasks[10] = {
                                ORBIS_PAD_BUTTON_CROSS, ORBIS_PAD_BUTTON_CIRCLE,
                                ORBIS_PAD_BUTTON_TRIANGLE, ORBIS_PAD_BUTTON_SQUARE,
                                ORBIS_PAD_BUTTON_L1, ORBIS_PAD_BUTTON_R1,
                                ORBIS_PAD_BUTTON_L2, ORBIS_PAD_BUTTON_R2,
                                ORBIS_PAD_BUTTON_LEFT, ORBIS_PAD_BUTTON_RIGHT
                            };
                            int curM = g_laneMasks[laneIdx].load();
                            int curPos = 0;
                            for (int c = 0; c < 10; c++) {
                                if (cycleMasks[c] == curM) { curPos = c; break; }
                            }
                            int nextPos = (pressed & ORBIS_PAD_BUTTON_LEFT) ? ((curPos + 9) % 10) : ((curPos + 1) % 10);
                            g_laneMasks[laneIdx].store(cycleMasks[nextPos]);
                            controllerPreset = 3;
                            saveUserSettings();
                        }
                    }
                }

                if (pressed & ORBIS_PAD_BUTTON_CIRCLE) {
                    saveUserSettings();
                    g_appState.store(STATE_MAIN_MENU);
                    flip();
                    continue;
                }
            }

            renderMenuBackdrop(fb);

            // Top Header Bar
            rect(fb, 0, 0, W, 70, lerpCol(g_themeBgSecondary, C(9, 14, 26), 110));
            hline(fb, 0, W, 70, 2, g_themeTrimPrimary);
            drawTextHD(fb, 80, 18, "SETTINGS & PREFERENCES STUDIO", C(255, 255, 255), true);
            drawTextHDRight(fb, W - 70, 22, BUILD_FULL_STR, g_themeTrimPrimary, false);

            // Segmented 4 Sub-Menu Navigation Tabs across x = 80..1360
            const char* const subMenuTitles[4] = {
                "[L1]  1. THEME & COLORS",
                "2. AUDIO & CALIB",
                "3. GAMEPLAY",
                "4. CONTROLS  [R1]"
            };
            for (int m = 0; m < 4; m++) {
                int tabX = 80 + m * 324;
                int tabY = 82;
                int tabW = 312;
                int tabH = 42;
                bool isTabActive = (m == g_settingsSubMenu);
                borderRect(fb, tabX, tabY, tabW, tabH, isTabActive ? 2 : 1,
                           isTabActive ? lerpCol(g_themeBgPrimary, C(24, 46, 82), 160) : lerpCol(g_themeBgSecondary, C(10, 16, 28), 120),
                           isTabActive ? g_themeTrimPrimary : C(40, 56, 78));
                if (isTabActive) {
                    hline(fb, tabX + 4, tabX + tabW - 4, tabY + tabH - 3, 3, g_themeTrimSecondary);
                }
                drawTextHDCentered(fb, tabX + tabW / 2, tabY + 10, subMenuTitles[m],
                                   isTabActive ? C(255, 255, 255) : C(148, 163, 184), false, tabW - 16);
            }

            const uint32_t laneAccentCols[5] = {
                C(74, 245, 96), C(255, 82, 92), C(255, 242, 75), C(96, 175, 255), C(255, 168, 56)
            };

            // Render Rows for Active Sub-Menu
            for (int r = 0; r < curSubMenuRows; r++) {
                int by = 138 + r * 72;
                bool isSel = (r == settingsCursor);
                bool isListeningRow = (g_settingsSubMenu == 3 && r >= 1 && r <= 5 && g_remapListeningLane == (r - 1));

                uint32_t fillCol = isListeningRow
                    ? C(58, 24, 48)
                    : (isSel ? lerpCol(g_themeBgPrimary, C(18, 34, 62), 120)
                             : lerpCol(g_themeBgSecondary, C(12, 20, 36), 140));
                uint32_t brdCol = isListeningRow
                    ? C(244, 114, 182)
                    : (isSel ? g_themeTrimPrimary : C(34, 50, 74));
                borderRect(fb, 80, by, 1280, 60, isSel ? 3 : 1, fillCol, brdCol);

                int labelX = 110;
                if (g_settingsSubMenu == 3 && r >= 1 && r <= 5) {
                    int ln = r - 1;
                    fillEllipseAA(fb, 118.0f, (float)(by + 30), 10.0f, 10.0f, laneAccentCols[ln]);
                    labelX = 142;
                }

                // Row Labels & Value Readouts
                if (g_settingsSubMenu == 0) { // Theme & Colors
                    const char* themeLabels[8] = {
                        "1. Highway Skin (21 Custom Highways)",
                        "2. Auto-Randomize Highway Every Song",
                        "3. UI & Stage Color Theme Preset",
                        "4. Target Color Slot (BG / Trim)",
                        "5. 2D Color Square - Hue Slider (0 - 359 deg)",
                        "6. 2D Color Square - Saturation (0 - 100%)",
                        "7. 2D Color Square - Brightness (0 - 100%)",
                        "8. Menu Background Video (/data/GH/backgrounds)"
                    };
                    drawTextHD(fb, labelX, by + 16, themeLabels[r], isSel ? C(255, 255, 255) : C(195, 208, 226), false, 640);

                    if (r == 0) {
                        char themeVal[96];
                        snprintf(themeVal, sizeof(themeVal), "< [%d/%d] %s >", fretboardTheme + 1, NUM_HIGHWAYS, g_highwaySkins[fretboardTheme].name);
                        drawTextHDRight(fb, 1330, by + 16, themeVal, g_themeTrimSecondary, false);
                    } else if (r == 1) {
                        drawTextHDRight(fb, 1330, by + 16, g_randomHighwayEachSong ? "< ON (RANDOM PER SONG) >" : "< OFF (KEEP SELECTED) >",
                                        g_randomHighwayEachSong ? C(52, 211, 153) : C(148, 163, 184), false);
                    } else if (r == 2) {
                        char pBuf[96];
                        snprintf(pBuf, sizeof(pBuf), "< %s >", g_themePresetNames[g_themePresetIdx]);
                        drawTextHDRight(fb, 1330, by + 16, pBuf, g_themeTrimPrimary, false);
                    } else if (r == 3) {
                        char sBuf[96];
                        snprintf(sBuf, sizeof(sBuf), "< [%d/4] %s >", g_activeColorSlot + 1, g_themeSlotNames[g_activeColorSlot]);
                        drawTextHDRight(fb, 1330, by + 16, sBuf, g_themeTrimSecondary, false);
                    } else if (r == 4) {
                        char hBuf[96];
                        snprintf(hBuf, sizeof(hBuf), "< %d deg >   [LEFT/RIGHT ADJUST]", g_themeSlotH[g_activeColorSlot]);
                        drawTextHDRight(fb, 1330, by + 16, hBuf, hsvToRgbCol(g_themeSlotH[g_activeColorSlot], 90, 95), false);
                    } else if (r == 5) {
                        char satBuf[96];
                        snprintf(satBuf, sizeof(satBuf), "< %d%% >   [LEFT/RIGHT ADJUST]", g_themeSlotS[g_activeColorSlot]);
                        drawTextHDRight(fb, 1330, by + 16, satBuf, C(226, 232, 240), false);
                    } else if (r == 6) {
                        char valBuf[96];
                        snprintf(valBuf, sizeof(valBuf), "< %d%% >   [LEFT/RIGHT ADJUST]", g_themeSlotV[g_activeColorSlot]);
                        drawTextHDRight(fb, 1330, by + 16, valBuf, C(226, 232, 240), false);
                    } else if (r == 7) {
                        drawTextHDRight(fb, 1330, by + 16, g_menuBgVideoEnabled ? "< ON >   [X: NEXT RANDOM VIDEO]" : "< OFF (STATIC GRADIENT) >",
                                        g_menuBgVideoEnabled ? C(52, 211, 153) : C(148, 163, 184), false);
                    }
                } else if (g_settingsSubMenu == 1) { // Audio & Calib
                    const char* audioLabels[4] = {
                        "1. Audio Latency Calibration Offset",
                        "2. Guitar Miss & Overstrum Flub Sound",
                        "3. Flub Audio Audition & Test",
                        "4. Rescan Songs & BG Videos in /data/GH"
                    };
                    drawTextHD(fb, labelX, by + 16, audioLabels[r], isSel ? C(255, 255, 255) : C(195, 208, 226), false, 640);

                    if (r == 0) {
                        char calBuf[96];
                        snprintf(calBuf, sizeof(calBuf), "< %+d ms >   [LEFT/RIGHT: +/-5ms]", g_calibMs.load());
                        drawTextHDRight(fb, 1330, by + 16, calBuf, g_themeTrimSecondary, false);
                    } else if (r == 1) {
                        drawTextHDRight(fb, 1330, by + 16, g_flubSoundEnabled ? "< ON (GUITAR HERO FLUBS) >" : "< OFF (SILENT MISS) >",
                                        g_flubSoundEnabled ? C(52, 211, 153) : C(148, 163, 184), false);
                    } else if (r == 2) {
                        drawTextHDRight(fb, 1330, by + 16, "[PRESS X / LEFT / RIGHT TO PLAY FLUB SFX]", C(250, 204, 21), false);
                    } else if (r == 3) {
                        drawTextHDRight(fb, 1330, by + 16, "[PRESS X TO RESCAN STORAGE DIRECTORY]", g_themeTrimPrimary, false);
                    }
                } else if (g_settingsSubMenu == 2) { // Gameplay
                    const char* gpLabels[4] = {
                        "1. Tap / All-Taps Mode (No Strum Needed)",
                        "2. Star Power Activation Button",
                        "3. Hit Window Timing Tightness",
                        "4. Note Highway Scroll Speed"
                    };
                    drawTextHD(fb, labelX, by + 16, gpLabels[r], isSel ? C(255, 255, 255) : C(195, 208, 226), false, 640);

                    if (r == 0) {
                        drawTextHDRight(fb, 1330, by + 16, g_tapMode.load() ? "< ALL-TAPS ON >" : "< STANDARD STRUM & HOPO >",
                                        g_tapMode.load() ? C(52, 211, 153) : C(148, 163, 184), false);
                    } else if (r == 1) {
                        drawTextHDRight(fb, 1330, by + 16, "< R2 TRIGGER / WHAMMY >", g_themeTrimSecondary, false);
                    } else if (r == 2) {
                        drawTextHDRight(fb, 1330, by + 16, "< CLONE HERO AUTHENTIC (+/-70ms) >", g_themeTrimPrimary, false);
                    } else if (r == 3) {
                        drawTextHDRight(fb, 1330, by + 16, "< 1.0x STANDARD SCROLL >", C(226, 232, 240), false);
                    }
                } else if (g_settingsSubMenu == 3) { // Controller & Remap
                    const char* ctrlLabels[6] = {
                        "1. Controller Preset Layout",
                        "2. Green Fret (1st Lane) Button",
                        "3. Red Fret (2nd Lane) Button",
                        "4. Yellow Fret (3rd Lane) Button",
                        "5. Blue Fret (4th Lane) Button",
                        "6. Orange Fret (5th Lane) Button"
                    };
                    drawTextHD(fb, labelX, by + 16, ctrlLabels[r], isSel ? C(255, 255, 255) : C(195, 208, 226), false, 640);

                    if (r == 0) {
                        char mapVal[96];
                        snprintf(mapVal, sizeof(mapVal), "< %s >", controllerPresetNames[controllerPreset]);
                        drawTextHDRight(fb, 1330, by + 16, mapVal, g_themeTrimPrimary, false);
                    } else {
                        int ln = r - 1;
                        if (isListeningRow) {
                            drawTextHDRight(fb, 1330, by + 16, ">>> PRESS ANY CONTROLLER BUTTON (OPTIONS=CANCEL) <<<", g_themeTrimSecondary, false);
                        } else {
                            char btnBuf[96];
                            snprintf(btnBuf, sizeof(btnBuf), "< %s >   [X: REMAP]", getButtonMaskName(g_laneMasks[ln].load()));
                            drawTextHDRight(fb, 1330, by + 16, btnBuf, laneAccentCols[ln], false);
                        }
                    }
                }
            }

            // Right Column Card 1: Interactive 2D HSV Color Square & Sliders Card (x = 1400..1840, y = 82..400)
            drawHsvColorSquareCard(fb, 1400, 82, 440, 318);

            // Right Column Card 2: Live Highway & 5-Fret Controller Tester Card (x = 1400..1840, y = 412..898)
            borderRect(fb, 1400, 412, 440, 486, 2, lerpCol(g_themeBgSecondary, C(12, 20, 34), 120), g_themeTrimPrimary);
            drawTextHDCentered(fb, 1620, 424, "HIGHWAY & FRET TESTER", g_themeTrimPrimary, false);
            drawHighwayPreviewCard(fb, 1515, 458, 210, 258, fretboardTheme);
            drawTextHDCentered(fb, 1620, 724, g_highwaySkins[fretboardTheme].name, g_themeTrimSecondary, false, 400);

            int liveHeld = g_held.load();
            for (int l = 0; l < 5; l++) {
                float rcx = 1464.0f + l * 78.0f;
                float rcy = 794.0f;
                bool isFretHeld = ((liveHeld >> l) & 1) != 0;
                drawCloneHeroReceptor(fb, rcx, rcy, 32.0f, 14.5f, col[l], bright[l], isFretHeld, true);
                drawTextCentered(fb, (int)rcx, 822, 1, getButtonMaskName(g_laneMasks[l].load()),
                                 isFretHeld ? C(255, 255, 255) : C(148, 163, 184), 72);
            }
            drawTextHDCentered(fb, 1620, 856, "Auto-saved to /data/GH/settings.ini", C(52, 211, 153), false);

            // Bottom Footer Bar
            rect(fb, 0, H - 64, W, 64, lerpCol(g_themeBgSecondary, C(8, 16, 28), 110));
            hline(fb, 0, W, H - 64, 2, g_themeTrimPrimary);
            drawTextHDCentered(fb, W / 2, H - 46,
                               "[L1/R1] SWITCH SUB-MENU   [UP/DOWN] SELECT   [LEFT/RIGHT] ADJUST   [X] ACTION   [TRIANGLE] RANDOM HWY   [O] SAVE & EXIT",
                               g_themeTrimSecondary, false, 1840);

            if (scanBannerTimer > 0) {
                scanBannerTimer--;
                borderRect(fb, 520, 946, 880, 52, 2, C(6, 42, 28), C(52, 211, 153));
                drawTextHDCentered(fb, 960, 958, scanBannerText, C(167, 243, 208), false, 840);
            }

            flip();
            continue;
        }

        // ====================================================================
        // SCREEN 1: CLONE HERO TRACKLIST + POST-SELECTION DIFFICULTY POP-UP MODAL
        // ====================================================================
        if (state == STATE_TRACKLIST) {
            ensureMenuBackdropVideoOpen(false);

            if (showDiffModal && !catalog.empty()) {
                if (pressed & ORBIS_PAD_BUTTON_UP) {
                    selectedDiff = (selectedDiff + 3) % 4;
                }
                if (pressed & ORBIS_PAD_BUTTON_DOWN) {
                    selectedDiff = (selectedDiff + 1) % 4;
                }
                if (pressed & ORBIS_PAD_BUTTON_L2) {
                    g_tapMode.store(!g_tapMode.load());
                }
                if (pressed & ORBIS_PAD_BUTTON_CIRCLE) {
                    showDiffModal = false;
                } else if (pressed & ORBIS_PAD_BUTTON_CROSS) {
                    recordSongPlayed(catalog[selectedSong]);
                    SongEntry launchedSong = catalog[selectedSong];
                    std::string launchedKey = launchedSong.statKey();
                    double customStart = 0.0;
                    if (selectedChapterIdx > 0 && selectedChapterIdx <= (int)launchedSong.chapters.size()) {
                        customStart = launchedSong.chapters[selectedChapterIdx - 1].time;
                    }
                    if (g_songSortMode != SORT_AZ) {
                        sortSongCatalog(catalog, g_songSortMode);
                        for (size_t k = 0; k < catalog.size(); k++) {
                            if (catalog[k].statKey() == launchedKey) {
                                selectedSong = (int)k;
                                break;
                            }
                        }
                    }
                    showDiffModal = false;
                    startSongSession(launchedSong, selectedDiff, customStart, chart, g);
                    prevGameFlubEvents = 0;
                    pauseSectionCursor = 0;
                    cachedSectionIdx = 0;
                    flip();
                    continue;
                }
            } else {
                if (pressed & ORBIS_PAD_BUTTON_CIRCLE) {
                    g_appState.store(STATE_MAIN_MENU);
                    flip();
                    continue;
                }
                if (pressed & ORBIS_PAD_BUTTON_TRIANGLE) {
                    catalog = discoverSongs();
                    ensureMenuBackdropVideoOpen(true);
                    selectedSong = 0;
                    scanBannerTimer = 180;
                    snprintf(scanBannerText, sizeof(scanBannerText), "RE-SCANNED /DATA/GH: %d SONGS READY!", (int)catalog.size());
                }
                if (pressed & ORBIS_PAD_BUTTON_SQUARE) {
                    fretboardTheme = pickRandomHighwayIndex(fretboardTheme);
                    saveUserSettings();
                    scanBannerTimer = 150;
                    snprintf(scanBannerText, sizeof(scanBannerText), "RANDOMIZED HIGHWAY: [%d/%d] %s",
                             fretboardTheme + 1, NUM_HIGHWAYS, g_highwaySkins[fretboardTheme].name);
                }
                if (pressed & ORBIS_PAD_BUTTON_R2) {
                    g_songSortMode = (g_songSortMode + 1) % 3;
                    sortSongCatalog(catalog, g_songSortMode);
                    selectedSong = 0;
                    selectedChapterIdx = 0;
                    saveUserSettings();
                    scanBannerTimer = 150;
                    snprintf(scanBannerText, sizeof(scanBannerText), "SORTED SONGS BY: %s",
                             g_songSortNames[g_songSortMode]);
                }

                if (!catalog.empty()) {
                    if (pressed & ORBIS_PAD_BUTTON_UP) {
                        selectedSong = (selectedSong - 1 + (int)catalog.size()) % (int)catalog.size();
                        selectedChapterIdx = 0;
                    }
                    if (pressed & ORBIS_PAD_BUTTON_DOWN) {
                        selectedSong = (selectedSong + 1) % (int)catalog.size();
                        selectedChapterIdx = 0;
                    }
                    if (pressed & ORBIS_PAD_BUTTON_LEFT) {
                        selectedSong = std::max(0, selectedSong - 8);
                        selectedChapterIdx = 0;
                    }
                    if (pressed & ORBIS_PAD_BUTTON_RIGHT) {
                        selectedSong = std::min((int)catalog.size() - 1, selectedSong + 8);
                        selectedChapterIdx = 0;
                    }
                    if (pressed & ORBIS_PAD_BUTTON_L2) {
                        g_tapMode.store(!g_tapMode.load());
                    }

                    const SongEntry& curSong = catalog[selectedSong];
                    int chapCount = (int)curSong.chapters.size() + 1;
                    if (pressed & ORBIS_PAD_BUTTON_L1) selectedChapterIdx = (selectedChapterIdx - 1 + chapCount) % chapCount;
                    if (pressed & ORBIS_PAD_BUTTON_R1) selectedChapterIdx = (selectedChapterIdx + 1) % chapCount;

                    if (pressed & ORBIS_PAD_BUTTON_CROSS) {
                        showDiffModal = true;
                    }
                }
            }

            // Live 1080p MP4 Background Video or Custom Theme Gradient Backdrop
            renderMenuBackdrop(fb);

            // ================================================================
            // CLEAN, STRUCTURED 4-PILL SONG BROWSER HEADER BAR (y = 0..76)
            // ================================================================
            rect(fb, 0, 0, W, 76, lerpCol(g_themeBgSecondary, C(9, 14, 26), 115));
            hline(fb, 0, W, 76, 2, g_themeTrimPrimary);

            // Pill 1: Songs Counter & Sort Mode (x = 45..415, w = 370)
            borderRect(fb, 45, 14, 370, 48, 2, lerpCol(g_themeBgSecondary, C(14, 24, 42), 120), g_themeTrimPrimary);
            char countStr[80];
            snprintf(countStr, sizeof(countStr), "%d/%d  |  [R2] SORT: %s",
                     catalog.empty() ? 0 : (selectedSong + 1), (int)catalog.size(),
                     g_songSortNames[g_songSortMode]);
            drawTextHDCentered(fb, 230, 25, countStr, C(255, 255, 255), false, 350);

            // Pill 2: Active Highway Theme & Square Randomize (x = 430..875, w = 445)
            borderRect(fb, 430, 14, 445, 48, 2, lerpCol(g_themeBgSecondary, C(14, 24, 42), 120), g_themeTrimSecondary);
            drawPSButtonIcon(fb, 460.0f, 38.0f, 13.0f, 'S');
            char hwyHeader[80];
            snprintf(hwyHeader, sizeof(hwyHeader), "HWY [%d/%d]: %s",
                     fretboardTheme + 1, NUM_HIGHWAYS, g_highwaySkins[fretboardTheme].name);
            drawTextHD(fb, 484, 25, hwyHeader, g_themeTrimSecondary, false, 378);

            // Pill 3: Input Mode & Triangle Rescan (x = 890..1380, w = 490)
            borderRect(fb, 890, 14, 490, 48, 2, lerpCol(g_themeBgSecondary, C(14, 24, 42), 120), C(45, 68, 100));
            char modeHeader[80];
            snprintf(modeHeader, sizeof(modeHeader), "[L2] %s   |   [TRIANGLE] RESCAN",
                     g_tapMode.load() ? "TAP: ON" : "STRUM MODE");
            drawTextHDCentered(fb, 1135, 25, modeHeader, C(226, 232, 240), false, 468);

            // Pill 4: Select Song Prompt & Build Version (x = 1395..1875, w = 480)
            borderRect(fb, 1395, 14, 480, 48, 2, lerpCol(g_themeBgPrimary, C(14, 28, 52), 120), g_themeTrimPrimary);
            drawPSButtonIcon(fb, 1428.0f, 38.0f, 13.0f, 'X');
            char rightHeader[80];
            snprintf(rightHeader, sizeof(rightHeader), "SELECT SONG   |   %s", BUILD_FULL_STR);
            drawTextHD(fb, 1452, 25, rightHeader, g_themeTrimPrimary, false, 410);

            // ================================================================
            // LEFT COLUMN: THE CLONE HERO SONG WHEEL (x = 45..1160)
            // ================================================================
            if (catalog.empty()) {
                drawTextHDCentered(fb, 600, 500, "NO SONGS FOUND IN /DATA/GH", C(248, 113, 113), true);
                drawTextHDCentered(fb, 600, 550, "Upload songs over FTP port 2121 or press Triangle to scan", C(148, 163, 184), false);
            } else {
                const int visibleRows = 12;
                int startIdx = selectedSong - 5;
                if (startIdx + visibleRows > (int)catalog.size()) startIdx = (int)catalog.size() - visibleRows;
                if (startIdx < 0) startIdx = 0;

                int wheelY = 88;
                char lastGroupChar = 0;

                for (int r = 0; r < visibleRows && (startIdx + r) < (int)catalog.size(); r++) {
                    int idx = startIdx + r;
                    const SongEntry& se = catalog[idx];
                    bool isSel = (idx == selectedSong);

                    // Section Grouping Header
                    if (g_songSortMode == SORT_AZ) {
                        char groupChar = se.artist.empty() ? '?' : (char)std::toupper(se.artist[0]);
                        if (groupChar != lastGroupChar && r > 0 && wheelY < 900) {
                            lastGroupChar = groupChar;
                            char secBuf[64];
                            snprintf(secBuf, sizeof(secBuf), "%c   |   ARTIST GROUP (A-Z)", groupChar);
                            borderRect(fb, 45, wheelY, 1115, 28, 1,
                                       lerpCol(g_themeBgSecondary, C(10, 18, 30), 120), g_themeTrimSecondary);
                            drawTextHDCentered(fb, 600, wheelY + 3, secBuf, g_themeTrimSecondary, false);
                            wheelY += 32;
                        }
                    } else if (r == 0 && wheelY < 900) {
                        char secBuf[80];
                        snprintf(secBuf, sizeof(secBuf), "SORTED BY: %s   (PRESS [R2] TO CHANGE)",
                                 g_songSortNames[g_songSortMode]);
                        borderRect(fb, 45, wheelY, 1115, 28, 1,
                                   lerpCol(g_themeBgSecondary, C(10, 18, 30), 120), g_themeTrimPrimary);
                        drawTextHDCentered(fb, 600, wheelY + 3, secBuf, g_themeTrimPrimary, false);
                        wheelY += 32;
                    }

                    int rowH = 52;
                    const char* fmtBadge = se.audioFormatLabel.c_str();
                    uint32_t fmtCol = (se.audioFormatLabel == "OPUS") ? C(192, 132, 252) : C(52, 211, 153);

                    if (isSel) {
                        gradRectV(fb, 45, wheelY, 1115, rowH,
                                  lerpCol(g_themeTrimPrimary, C(14, 116, 220), 140),
                                  lerpCol(g_themeBgPrimary, C(3, 72, 162), 140));
                        borderRect(fb, 45, wheelY, 1115, rowH, 2,
                                   lerpCol(g_themeBgPrimary, C(14, 92, 180), 140), C(255, 255, 255));
                        hline(fb, 48, 1155, wheelY + 1, 1, g_themeTrimPrimary);
                        vline(fb, 428, wheelY + 10, wheelY + rowH - 10, 1, g_themeTrimPrimary);

                        drawTextHD(fb, 65, wheelY + 13, se.artist.c_str(), C(255, 255, 255), false, 348);
                        drawTextHD(fb, 444, wheelY + 13, se.title.c_str(), C(240, 249, 255), false, 530);

                        drawTextHDRight(fb, 1055, wheelY + 13, fmtBadge, C(255, 255, 255), false);
                        drawTextHDRight(fb, 1142, wheelY + 13, se.lengthStr.c_str(), g_themeTrimSecondary, false);
                    } else {
                        borderRect(fb, 45, wheelY, 1115, rowH, 1,
                                   lerpCol(g_themeBgSecondary, C(10, 18, 32), 135), C(28, 42, 64));
                        vline(fb, 428, wheelY + 12, wheelY + rowH - 12, 1, C(30, 45, 68));

                        drawTextHD(fb, 65, wheelY + 13, se.artist.c_str(), C(203, 213, 225), false, 348);
                        drawTextHD(fb, 444, wheelY + 13, se.title.c_str(), C(148, 163, 184), false, 530);

                        drawTextHDRight(fb, 1055, wheelY + 13, fmtBadge, fmtCol, false);
                        drawTextHDRight(fb, 1142, wheelY + 13, se.lengthStr.c_str(), C(148, 163, 184), false);
                    }
                    wheelY += rowH + 4;
                }
            }

            // ================================================================
            // RIGHT COLUMN: SHOWCASE CARD (x = 1230..1850)
            // ================================================================
            if (!catalog.empty()) {
                const SongEntry& cur = catalog[selectedSong];

                // 1. Exact 360x360 Album Cover Display Box (boxX = 1360, boxY = 88)
                const int boxX = 1360;
                const int boxY = 88;
                borderRect(fb, boxX - 4, boxY - 4, ALBUM_BOX_W + 8, ALBUM_BOX_H + 8, 2,
                           lerpCol(g_themeBgSecondary, C(15, 23, 42), 120), g_themeTrimPrimary);

                if (getCachedAlbumArt(cur.albumArtPath)) {
                    for (int row = 0; row < ALBUM_BOX_H; row++) {
                        memcpy(fb + (size_t)(boxY + row) * W + boxX,
                               g_cachedAlbumArtPixels + (size_t)row * ALBUM_BOX_W,
                               (size_t)ALBUM_BOX_W * 4);
                    }
                    hline(fb, boxX, boxX + ALBUM_BOX_W - 1, boxY, 1, g_themeTrimPrimary);
                    hline(fb, boxX, boxX + ALBUM_BOX_W - 1, boxY + ALBUM_BOX_H - 1, 1, C(15, 23, 42));
                } else {
                    gradRectV(fb, boxX, boxY, ALBUM_BOX_W, ALBUM_BOX_H, g_themeBgPrimary, g_themeBgSecondary);
                    float vcx = (float)(boxX + ALBUM_BOX_W / 2);
                    float vcy = (float)(boxY + ALBUM_BOX_H / 2);
                    fillEllipseAA(fb, vcx, vcy, 132.0f, 132.0f, C(12, 16, 24));
                    fillEllipseAA(fb, vcx, vcy, 126.0f, 126.0f, C(28, 36, 52));
                    fillEllipseAA(fb, vcx, vcy, 96.0f, 96.0f, C(16, 22, 34));
                    fillEllipseAA(fb, vcx, vcy, 44.0f, 44.0f, g_themeTrimPrimary);
                    fillEllipseAA(fb, vcx, vcy, 38.0f, 38.0f, C(15, 23, 42));
                    fillEllipseAA(fb, vcx, vcy, 10.0f, 10.0f, C(226, 232, 240));
                    drawTextHDCentered(fb, boxX + ALBUM_BOX_W / 2, boxY + 20, cur.artist.c_str(), C(186, 200, 218), false, 330);
                    drawTextHDCentered(fb, boxX + ALBUM_BOX_W / 2, boxY + ALBUM_BOX_H - 36, cur.album.c_str(), g_themeTrimPrimary, false, 330);
                }

                // 2. Song Title & Artist Header Banner (y = 458..538)
                borderRect(fb, 1230, 458, 620, 80, 2,
                           lerpCol(g_themeBgSecondary, C(12, 20, 36), 125), g_themeTrimPrimary);
                hline(fb, 1232, 1848, 460, 1, g_themeTrimPrimary);
                drawTextHDCentered(fb, 1540, 466, cur.title.c_str(), C(255, 255, 255), false, 590);
                drawTextHDCentered(fb, 1540, 500, cur.artist.c_str(), g_themeTrimPrimary, false, 590);

                // 3. Two-Column Album & Song Metadata Box (y = 546..678)
                borderRect(fb, 1230, 546, 620, 132, 1,
                           lerpCol(g_themeBgSecondary, C(10, 18, 32), 130), C(32, 50, 76));
                vline(fb, 1540, 554, 670, 1, C(26, 40, 62));

                drawTextHD(fb, 1246, 558, "Album:", C(148, 163, 184), false);
                drawTextHD(fb, 1336, 558, cur.album.c_str(), C(255, 255, 255), false, 194);

                drawTextHD(fb, 1246, 596, "Genre:", C(148, 163, 184), false);
                drawTextHD(fb, 1336, 596, cur.genre.c_str(), C(226, 232, 240), false, 194);

                drawTextHD(fb, 1246, 634, "Year:", C(148, 163, 184), false);
                drawTextHD(fb, 1336, 634, cur.year.c_str(), C(226, 232, 240), false, 194);

                drawTextHD(fb, 1554, 558, "Charter:", C(148, 163, 184), false);
                drawTextHD(fb, 1668, 558, cur.charter.c_str(), C(255, 255, 255), false, 168);

                drawTextHD(fb, 1554, 596, "Length:", C(148, 163, 184), false);
                drawTextHD(fb, 1668, 596, cur.lengthStr.c_str(), C(226, 232, 240), false, 168);

                drawTextHD(fb, 1554, 634, "Video:", C(148, 163, 184), false);
                drawTextHD(fb, 1668, 634,
                           cur.hasOwnVideo ? "1080p MP4" : "Default Pool",
                           cur.hasOwnVideo ? C(52, 211, 153) : g_themeTrimPrimary,
                           false, 168);

                // 4. Clean Non-Overlapping Score & 5-Star Rating Panel (y = 686..816)
                borderRect(fb, 1230, 686, 620, 128, 1,
                           lerpCol(g_themeBgSecondary, C(10, 18, 32), 130), C(32, 50, 76));
                char chartHeaderBuf[64];
                snprintf(chartHeaderBuf, sizeof(chartHeaderBuf), "EXPERT NOTES  |  PLAYS: %d", cur.playCount);
                drawTextHD(fb, 1250, 698, chartHeaderBuf, C(148, 163, 184), false, 310);
                char noteCountHero[48];
                snprintf(noteCountHero, sizeof(noteCountHero), "%d NOTES", cur.diffNoteCounts[3]);
                drawTextHD(fb, 1250, 732, noteCountHero, C(255, 255, 255), true);

                std::string startChapLabel = "START: FULL SONG (L1/R1)";
                if (selectedChapterIdx > 0 && selectedChapterIdx <= (int)cur.chapters.size()) {
                    startChapLabel = "START: " + cur.chapters[selectedChapterIdx - 1].name;
                }
                drawTextHD(fb, 1250, 778, startChapLabel.c_str(), C(52, 211, 153), false, 310);

                for (int s = 0; s < 5; s++) {
                    float starCx = 1588.0f + s * 48.0f;
                    float starCy = 736.0f;
                    drawSmallStarAA(fb, starCx, starCy, 15.0f, C(202, 138, 4), g_themeTrimSecondary);
                }
                drawTextHDCentered(fb, 1684, 776, "5 / 5 GOLD STARS", g_themeTrimSecondary, false);

                // 5. Clean Difficulty Note Count Overview Bar (y = 824..916)
                borderRect(fb, 1230, 824, 620, 88, 1,
                           lerpCol(g_themeBgSecondary, C(10, 18, 32), 130), C(32, 50, 76));
                for (int d = 0; d < 4; d++) {
                    int cellX = 1244 + d * 150;
                    bool isCurDiff = (d == selectedDiff);
                    borderRect(fb, cellX, 834, 138, 68, isCurDiff ? 2 : 1,
                               isCurDiff ? lerpCol(g_themeBgPrimary, C(18, 34, 62), 120) : C(13, 22, 38),
                               isCurDiff ? diffCols[d] : C(28, 44, 68));
                    drawTextHDCentered(fb, cellX + 69, 840, diffNames[d],
                                       isCurDiff ? diffCols[d] : C(148, 163, 184), false, 128);
                    char dnBuf[32];
                    snprintf(dnBuf, sizeof(dnBuf), "%d", cur.diffNoteCounts[d]);
                    drawTextHDCentered(fb, cellX + 69, 868, dnBuf, C(226, 232, 240), false, 128);
                }
            }

            // Bottom Navigation Footer Bar
            rect(fb, 0, H - 64, W, 64, lerpCol(g_themeBgSecondary, C(8, 16, 28), 110));
            hline(fb, 0, W, H - 64, 2, g_themeTrimPrimary);
            drawTextHDCentered(fb, W / 2, H - 46,
                               "[X] SELECT SONG   [R2] SORT SONGS   [UP/DOWN] SCROLL   [LEFT/RIGHT] PAGE   [SQUARE] HIGHWAY   [O] MAIN MENU",
                               g_themeTrimSecondary, false, 1840);

            if (scanBannerTimer > 0) {
                scanBannerTimer--;
                borderRect(fb, 540, 946, 840, 52, 2, C(6, 42, 28), C(52, 211, 153));
                drawTextHDCentered(fb, 960, 958, scanBannerText, C(167, 243, 208), false, 800);
            }

            // ================================================================
            // POST-TRACK-SELECTION DIFFICULTY POP-UP MODAL
            // ================================================================
            if (showDiffModal && !catalog.empty()) {
                const SongEntry& cur = catalog[selectedSong];
                rectAlpha(fb, 0, 0, W, H, C(3, 8, 18), 195);

                const int mw = 860;
                const int mh = 610;
                const int mx = (W - mw) / 2;
                const int my = (H - mh) / 2;

                borderRect(fb, mx, my, mw, mh, 3, lerpCol(g_themeBgSecondary, C(12, 22, 38), 110), g_themeTrimPrimary);
                hline(fb, mx + 4, mx + mw - 4, my + 4, 2, g_themeTrimPrimary);

                drawTextHDCentered(fb, W / 2, my + 24, "SELECT DIFFICULTY", C(255, 255, 255), true);
                drawTextHDCentered(fb, W / 2, my + 80, cur.title.c_str(), g_themeTrimPrimary, false, mw - 80);
                drawTextHDCentered(fb, W / 2, my + 112, cur.artist.c_str(), C(148, 163, 184), false, mw - 80);

                for (int d = 0; d < 4; d++) {
                    int ry = my + 158 + d * 88;
                    bool isSel = (d == selectedDiff);
                    borderRect(fb, mx + 48, ry, mw - 96, 74, isSel ? 3 : 1,
                               isSel ? lerpCol(g_themeBgPrimary, C(20, 38, 68), 120) : C(14, 24, 42),
                               isSel ? diffCols[d] : C(36, 54, 82));

                    rect(fb, mx + 56, ry + 10, 10, 54, diffCols[d]);

                    drawTextHD(fb, mx + 82, ry + 10, diffNames[d],
                               isSel ? diffCols[d] : C(226, 232, 240), false);
                    drawTextHD(fb, mx + 82, ry + 38, diffDescs[d],
                               isSel ? C(226, 232, 240) : C(148, 163, 184), false, 480);

                    char notePill[48];
                    snprintf(notePill, sizeof(notePill), "%d NOTES", cur.diffNoteCounts[d]);
                    drawTextHDRight(fb, mx + mw - 72, ry + 22, notePill,
                                    isSel ? C(255, 255, 255) : C(148, 163, 184), false);
                }

                drawPSButtonIcon(fb, (float)(mx + 155), (float)(my + mh - 38), 14.0f, 'X');
                drawTextHD(fb, mx + 178, my + mh - 52, "START SONG", C(52, 211, 153), false);

                drawPSButtonIcon(fb, (float)(mx + 430), (float)(my + mh - 38), 14.0f, 'O');
                drawTextHD(fb, mx + 454, my + mh - 52, "BACK TO TRACKLIST", C(248, 113, 113), false);

                drawTextHDRight(fb, mx + mw - 48, my + mh - 52,
                                g_tapMode.load() ? "[L2] TAP: ON" : "[L2] STRUM", g_themeTrimPrimary, false);
            }

            flip();
            continue;
        }

        // ====================================================================
        // SCREEN 4: END-OF-SONG RESULTS SCREEN
        // ====================================================================
        if (state == STATE_RESULTS) {
            if (pressed & ORBIS_PAD_BUTTON_CROSS) {
                showDiffModal = false;
                ensureMenuBackdropVideoOpen(true);
                g_appState.store(STATE_TRACKLIST);
                flip();
                continue;
            }
            if (pressed & ORBIS_PAD_BUTTON_SQUARE) {
                if (!catalog.empty()) {
                    startSongSession(catalog[selectedSong], selectedDiff, 0.0, chart, g);
                    prevGameFlubEvents = 0;
                    cachedSectionIdx = 0;
                }
                flip();
                continue;
            }

            gradRectV(fb, 0, 0, W, H, g_themeBgPrimary, g_themeBgSecondary);
            borderRect(fb, 360, 140, W - 720, H - 280, 2, C(18, 22, 30), g_themeTrimPrimary);

            drawTextHDCentered(fb, W / 2, 175, "SONG COMPLETE", g_themeTrimSecondary, true);
            drawTextHDCentered(fb, W / 2, 232, chart.title.c_str(), C(255, 255, 255), false, 680);
            drawTextHDCentered(fb, W / 2, 266, diffNames[selectedDiff], diffCols[selectedDiff], false);

            int stars = g.getStarRating();
            drawGoldStarEmblem(fb, W / 2, 340, 1.0, C(217, 119, 6), C(253, 224, 71), C(24, 26, 34));
            char starStr[8];
            snprintf(starStr, sizeof(starStr), "%d", stars);
            drawTextHDCentered(fb, W / 2, 326, starStr, C(253, 224, 71), false);

            int totalJudged = g.notesHit + g.misses;
            int pct = (totalJudged > 0) ? (g.notesHit * 100) / totalJudged : 100;

            char scFormatted[32];
            formatCommaScoreBuf(g.score, scFormatted, sizeof(scFormatted));
            char bufScore[64], bufStreak[64], bufHit[64], bufAcc[64];
            snprintf(bufScore, sizeof(bufScore),  "FINAL SCORE:   %s", scFormatted);
            snprintf(bufStreak, sizeof(bufStreak),"NOTE STREAK:   %d", g.maxCombo);
            snprintf(bufHit, sizeof(bufHit),      "NOTES HIT:     %d / %d", g.notesHit, totalJudged);
            snprintf(bufAcc, sizeof(bufAcc),      "ACCURACY:      %d%%", pct);

            drawTextHD(fb, 680, 420, bufScore, C(255, 255, 255), false);
            drawTextHD(fb, 680, 464, bufStreak, g_themeTrimSecondary, false);
            drawTextHD(fb, 680, 508, bufHit, g_themeTrimPrimary, false);
            drawTextHD(fb, 680, 552, bufAcc, C(52, 211, 153), false);

            drawTextHDCentered(fb, W / 2, 640, "CROSS (X): BACK TO TRACKLIST     SQUARE: REPLAY SONG", g_themeTrimSecondary, false);
            flip();
            continue;
        }

        // ====================================================================
        // SCREEN 2 & 3: IN-GAME 1080P60 CLONE HERO STAGE & PAUSE MENU
        // ====================================================================
        double now = songTime();
        double highwayDur = diffHighwayDur[selectedDiff];
        int held = g_held.load();

        if (state == STATE_PLAYING) {
            if (pressed & ORBIS_PAD_BUTTON_OPTIONS) {
                pauseCursor = 0;
                g_appState.store(STATE_PAUSED);
                state = STATE_PAUSED;
            } else {
                // R1 Skip Intro / Long Wait (> 2.5s to next note)
                if (pressed & ORBIS_PAD_BUTTON_R1) {
                    for (size_t i = g.first; i < chart.notes.size(); i++) {
                        if (!chart.notes[i].hit && !chart.notes[i].missed && chart.notes[i].time > now) {
                            if (chart.notes[i].time - now > 2.5) {
                                double targetSec = std::max(0.0, chart.notes[i].time - 1.8);
                                g.seekTo(targetSec);
                                requestSeekSec(targetSec);
                                cachedSectionIdx = 0;
                            }
                            break;
                        }
                    }
                }
                // R2 (or custom Star Power mask) activates Star Power when >= 50%
                if (pressed & g_laneMasks[5].load()) g.activateStarPower(now);
                if (pressed & ORBIS_PAD_BUTTON_L2) g_tapMode.store(!g_tapMode.load());
                if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT)) {
                    int delta = (pressed & ORBIS_PAD_BUTTON_RIGHT) ? 10 : -10;
                    int cur = std::clamp(g_calibMs.load() + delta, -250, 250);
                    g_calibMs.store(cur);
                    saveUserSettings();
                    calibToastTimer = 110;
                    snprintf(calibToastText, sizeof(calibToastText), "AUDIO SYNC OFFSET: %+d ms (D-PAD LEFT/RIGHT)", cur);
                }

                uint32_t t = g_tail.load();
                while (t != g_head.load()) {
                    Ev e = g_ring[t % 256];
                    g.strum(e.t, e.held, e.justPressed, e.isTap);
                    t++;
                }
                g_tail.store(t);
                g.update(now, held, g_tapMode.load());

                // Sync Guitar Hero miss/overstrum flub sound & guitar stem muting to audio thread
                g_guitarStemMuted.store(g.guitarMuted);
                if (g.flubEvents > prevGameFlubEvents) {
                    prevGameFlubEvents = g.flubEvents;
                    g_flubTriggerSeq.fetch_add(1);
                }

                double lastNoteEnd = chart.notes.empty() ? 180.0 : (chart.notes.back().time + chart.notes.back().sustain + 3.0);
                double streamEnd = (double)g_totalFrames / RATE + 1.0;
                if (now > std::min(lastNoteEnd, streamEnd)) {
                    g_guitarStemMuted.store(false);
                    g_appState.store(STATE_RESULTS);
                    flip();
                    continue;
                }
            }
        } else if (state == STATE_PAUSED) {
            const int numPauseItems = 6;
            if ((pressed & ORBIS_PAD_BUTTON_OPTIONS) || (pressed & ORBIS_PAD_BUTTON_CIRCLE)) {
                g_tail.store(g_head.load());
                g_appState.store(STATE_PLAYING);
                state = STATE_PLAYING;
            } else {
                if (pressed & ORBIS_PAD_BUTTON_UP)   pauseCursor = (pauseCursor - 1 + numPauseItems) % numPauseItems;
                if (pressed & ORBIS_PAD_BUTTON_DOWN) pauseCursor = (pauseCursor + 1) % numPauseItems;

                if (pauseCursor == 2 && (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_CROSS))) {
                    int delta = (pressed & ORBIS_PAD_BUTTON_LEFT) ? 3 : 1;
                    selectedDiff = (selectedDiff + delta) % 4;
                    if (!catalog.empty()) {
                        startSongSession(catalog[selectedSong], selectedDiff, std::max(0.0, now - 1.5), chart, g);
                        prevGameFlubEvents = 0;
                        cachedSectionIdx = 0;
                    }
                    flip();
                    continue;
                }
                if (pauseCursor == 3 && !chart.sections.empty()) {
                    int sc = (int)chart.sections.size();
                    if (pressed & ORBIS_PAD_BUTTON_LEFT)  pauseSectionCursor = (pauseSectionCursor - 1 + sc) % sc;
                    if (pressed & ORBIS_PAD_BUTTON_RIGHT) pauseSectionCursor = (pauseSectionCursor + 1) % sc;
                    if (pressed & ORBIS_PAD_BUTTON_CROSS) {
                        double targetSec = std::max(0.0, chart.sections[pauseSectionCursor].time - 1.5);
                        g.seekTo(targetSec);
                        requestSeekSec(targetSec);
                        prevGameFlubEvents = g.flubEvents;
                        cachedSectionIdx = 0;
                        g_tail.store(g_head.load());
                        g_appState.store(STATE_PLAYING);
                        flip();
                        continue;
                    }
                }
                if (pauseCursor == 4 && (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_CROSS))) {
                    g_tapMode.store(!g_tapMode.load());
                }
                if (pressed & ORBIS_PAD_BUTTON_CROSS) {
                    if (pauseCursor == 0) {
                        g_tail.store(g_head.load());
                        g_appState.store(STATE_PLAYING);
                    } else if (pauseCursor == 1) {
                        if (!catalog.empty()) {
                            startSongSession(catalog[selectedSong], selectedDiff, 0.0, chart, g);
                            prevGameFlubEvents = 0;
                            cachedSectionIdx = 0;
                        }
                    } else if (pauseCursor == 5) {
                        showDiffModal = false;
                        g_guitarStemMuted.store(false);
                        ensureMenuBackdropVideoOpen(true);
                        g_appState.store(STATE_TRACKLIST);
                        flip();
                        continue;
                    }
                }
            }
        }

        // ====================================================================
        // 1. DIRECT MP4 VIDEO STREAM OR PRE-BAKED 1080P CONCERT STAGE RESTORE
        // ====================================================================
        bool overstrumFlash = (now - g.lastOverstrumTime) >= 0.0 && (now - g.lastOverstrumTime) < 0.12;
        int stageVariant = overstrumFlash ? 2 : (g.spActive ? 1 : 0);

        if (!g_mp4Player.updateAndBlitFullStage(now, fb, g_stageCacheFb[stageVariant],
                                                HORIZON_Y, HIT_Y, TOP_W, BOT_W,
                                                HUD_X, HUD_Y, HUD_W, ODO_Y, ODO_H,
                                                (fretboardTheme == 5))) {
            memcpy(fb, g_stageCacheFb[stageVariant], (size_t)W * H * 4);
        }

        double beatSec = 60.0 / (chart.bpm > 0 ? chart.bpm : 120.0);

        // NOTE: Top-left green debug/FPS telemetry text has been completely removed for a clean concert stage!

        // O(1) Cached Current Section / Lyric Header Lookup in Warm Gold (TrueType Large)
        if (!chart.sections.empty()) {
            if (cachedSectionIdx >= chart.sections.size()) cachedSectionIdx = 0;
            while (cachedSectionIdx > 0 && chart.sections[cachedSectionIdx].time > now + 0.5) {
                cachedSectionIdx--;
            }
            while (cachedSectionIdx + 1 < chart.sections.size() && chart.sections[cachedSectionIdx + 1].time <= now + 0.5) {
                cachedSectionIdx++;
            }
            const char* secLabel = (chart.sections[cachedSectionIdx].time <= now + 0.5)
                ? chart.sections[cachedSectionIdx].name.c_str()
                : chart.title.c_str();
            drawTextHDCentered(fb, W / 2, 54, secLabel, g_themeTrimSecondary, true, 700);
        } else {
            drawTextHDCentered(fb, W / 2, 54, chart.title.c_str(), g_themeTrimSecondary, true, 700);
        }

        if (calibToastTimer > 0) {
            calibToastTimer--;
            borderRect(fb, W / 2 - 280, 104, 560, 46, 2, C(12, 22, 38), g_themeTrimPrimary);
            drawTextHDCentered(fb, W / 2, 116, calibToastText, g_themeTrimSecondary, false, 530);
        }

        // ====================================================================
        // 2. SYNCHRONIZED MEASURE LINES & HELD GUITAR STRING GLOW
        // ====================================================================
        int firstBeat = (int)std::floor(now / (beatSec * 0.5));
        int lastBeat = (int)std::ceil((now + highwayDur) / (beatSec * 0.5));
        for (int bIdx = firstBeat; bIdx <= lastBeat; bIdx++) {
            double bt = bIdx * (beatSec * 0.5);
            double fp = 1.0 - (bt - now) / highwayDur;
            if (fp < 0.0 || fp > 1.25) continue;
            int curW = TOP_W + (int)((BOT_W - TOP_W) * fp);
            int curX0 = (W - curW) / 2;
            int curY = HORIZON_Y + (int)((HIT_Y - HORIZON_Y) * fp);
            if (bIdx % 8 == 0) {
                hlineAlpha(fb, curX0, curX0 + curW, curY, 3, C(210, 218, 230), 140);
            } else if (bIdx % 2 == 0) {
                hlineAlpha(fb, curX0, curX0 + curW, curY, 2, C(148, 163, 184), 85);
            } else {
                hlineAlpha(fb, curX0, curX0 + curW, curY, 1, C(100, 116, 139), 45);
            }
        }

        // Highlight only held guitar strings (unpressed strings are already in g_stageCacheFb)
        if (held != 0) {
            for (int y = HORIZON_Y; y < H; y++) {
                double p = (double)(y - HORIZON_Y) / (double)(HIT_Y - HORIZON_Y);
                int curW = TOP_W + (int)((BOT_W - TOP_W) * p);
                int curX0 = (W - curW) / 2;
                float laneW = (float)curW / 5.0f;
                uint32_t* row = fb + (size_t)y * W;
                for (int l = 0; l < 5; l++) {
                    if ((held >> l) & 1) {
                        int sx = curX0 + (int)((l + 0.5f) * laneW);
                        row[sx] = bright[l];
                        if (p > 0.45 && sx + 1 < W) row[sx + 1] = bright[l];
                    }
                }
            }
        }

        // ====================================================================
        // 3. YARG / CLONE HERO 3D FRET RECEPTORS AT HIT LINE (HIT_Y = 935)
        // ====================================================================
        int activeLanesForDiff = (selectedDiff == 0) ? 3 : (selectedDiff == 1) ? 4 : 5;
        int hitX0 = (W - BOT_W) / 2;
        float hitLaneW = (float)BOT_W / 5.0f;

        for (int l = 0; l < 5; l++) {
            float rxCenter = hitX0 + (l + 0.5f) * hitLaneW;
            bool isHeld = ((held >> l) & 1) != 0;
            bool laneEnabled = (l < activeLanesForDiff);
            float radX = hitLaneW * 0.455f;
            float radY = radX * 0.415f;
            drawCloneHeroReceptor(fb, rxCenter, (float)HIT_Y, radX, radY, col[l], bright[l], isHeld, laneEnabled);
        }

        // ====================================================================
        // 4. PERSPECTIVE SUSTAIN TAILS, 3D NOTE GEMS & HIT FLAME PLUMES
        // ====================================================================
        // Pass A: Sustain Ribbons (drawn underneath note gems, centered on each string)
        for (size_t i = g.first; i < chart.notes.size(); i++) {
            const Note& n = chart.notes[i];
            double dt = n.time - now;
            if (dt > highwayDur + 0.05) break;
            if (n.sustain <= 0.0) continue;
            if (n.hit && !n.sustainActive && (now > n.time + n.sustain)) continue;

            double np = 1.0 - dt / highwayDur;
            double startP = n.hit ? 1.0 : std::max(0.0, np);
            double endP = std::max(0.0, 1.0 - ((n.time + n.sustain) - now) / highwayDur);
            if (startP <= endP) continue;

            int y1 = std::min(H - 1, HORIZON_Y + (int)((HIT_Y - HORIZON_Y) * startP));
            int y2 = std::max(HORIZON_Y, HORIZON_Y + (int)((HIT_Y - HORIZON_Y) * endP));

            for (int l = 0; l < 5; l++) {
                if (!(n.mask & (1 << l))) continue;
                bool deadTail = n.missed || (n.hit && !n.sustainActive);
                uint32_t tailOuter = deadTail ? C(62, 66, 76) : (g.spActive ? C(14, 165, 233) : col[l]);
                uint32_t tailCore  = deadTail ? C(95, 100, 112) : C(255, 255, 255);

                for (int sy = y2; sy < y1; sy++) {
                    double sp = (double)(sy - HORIZON_Y) / (double)(HIT_Y - HORIZON_Y);
                    int sw = TOP_W + (int)((BOT_W - TOP_W) * sp);
                    int sx0 = (W - sw) / 2;
                    float slw = (float)sw / 5.0f;
                    int cx = sx0 + (int)((l + 0.5f) * slw);
                    int halfW = std::max(4, (int)(slw * 0.13f));
                    int coreW = std::max(1, halfW / 3);
                    uint32_t* row = fb + (size_t)sy * W;
                    if (deadTail) {
                        for (int px = cx - halfW; px < cx + halfW; px++) row[px] = lerpCol(row[px], tailOuter, 140);
                    } else {
                        for (int px = cx - halfW; px < cx + halfW; px++) row[px] = tailOuter;
                        for (int px = cx - coreW; px < cx + coreW; px++) row[px] = tailCore;
                    }
                }
            }
        }

        // Pass B: 3D Note Gems (Strum, HOPO, Star Power Stars, Open Bars, & Missed Pass-Through)
        for (size_t i = g.first; i < chart.notes.size(); i++) {
            const Note& n = chart.notes[i];
            double dt = n.time - now;
            if (dt > highwayDur + 0.05) break;
            if (n.hit) continue;
            if (dt < -0.22) continue;

            double np = 1.0 - dt / highwayDur;
            if (np < 0.0 || np > 1.22) continue;

            int curW = TOP_W + (int)((BOT_W - TOP_W) * np);
            int curX0 = (W - curW) / 2;
            int curY = HORIZON_Y + (int)((HIT_Y - HORIZON_Y) * np);
            float laneW = (float)curW / 5.0f;

            if (n.mask & 32) {
                uint32_t openCol = n.missed ? C(75, 75, 85) : (g.spActive ? C(56, 189, 248) : C(234, 158, 12));
                int bh = std::max(8, (int)(18.0 * np));
                borderRect(fb, curX0 + 6, curY - bh / 2, curW - 12, bh, 2, openCol, C(255, 255, 255));
                hline(fb, curX0 + 18, curX0 + curW - 18, curY, std::max(2, bh / 3), C(255, 255, 255));
            } else {
                float gemRx = std::max(10.0f, laneW * 0.375f);
                float gemRy = std::max(6.0f, gemRx * 0.51f);
                for (int l = 0; l < 5; l++) {
                    if (!(n.mask & (1 << l))) continue;
                    float nx = curX0 + (l + 0.5f) * laneW;
                    uint32_t gemCol = g.spActive ? C(56, 189, 248) : col[l];
                    drawCloneHeroGem(fb, nx, (float)curY, gemRx, gemRy, gemCol, n.isHopo, n.isStarPower || g.spActive, n.missed);
                }
            }
        }

        // Pass C: YARG / Clone Hero Receptor Hit Flame Plumes & Dual Shockwave Rings
        for (int l = 0; l < 5; l++) {
            double flameAge = now - g.laneFlameTime[l];
            if (flameAge >= 0.0 && flameAge <= 0.24) {
                float rxCenter = hitX0 + (l + 0.5f) * hitLaneW;
                drawReceptorHitFlame(fb, rxCenter, (float)HIT_Y, flameAge, g.spActive, col[l], bright[l]);
            }
        }

        // ====================================================================
        // 5. REDESIGNED CLONE HERO SCORE, STAR POWER & COMBO ODOMETER BOX HUD
        // ====================================================================
        drawCloneHeroScoreBoxHUD(fb, g, chart, now);

        // R1 Skip Intro Prompt (When > 3.0s until next note)
        for (size_t i = g.first; i < chart.notes.size(); i++) {
            if (!chart.notes[i].hit && !chart.notes[i].missed && chart.notes[i].time > now) {
                if (chart.notes[i].time - now > 3.0) {
                    borderRect(fb, W / 2 - 240, 220, 480, 42, 2, C(16, 22, 34), g_themeTrimPrimary);
                    drawTextHDCentered(fb, W / 2, 228, "PRESS R1 TO SKIP INTRO", g_themeTrimPrimary, false, 440);
                }
                break;
            }
        }

        // ====================================================================
        // PAUSE OVERLAY MODAL (When state == STATE_PAUSED)
        // ====================================================================
        if (state == STATE_PAUSED) {
            rectAlpha(fb, 0, 0, W, H, C(4, 8, 16), 175);
            borderRect(fb, 420, 170, W - 840, 720, 3, lerpCol(g_themeBgSecondary, C(16, 22, 34), 110), g_themeTrimPrimary);
            drawTextHDCentered(fb, W / 2, 205, "PAUSED", g_themeTrimSecondary, true);
            drawTextHDCentered(fb, W / 2, 265, chart.title.c_str(), C(186, 200, 218), false, 780);

            std::string diffOption = std::string("DIFFICULTY: < ") + diffNames[selectedDiff] + " >";
            std::string secOption = "JUMP SECTION: NONE";
            if (!chart.sections.empty()) {
                if (pauseSectionCursor >= (int)chart.sections.size()) pauseSectionCursor = 0;
                const Section& s = chart.sections[pauseSectionCursor];
                secOption = "SECTION: < [" + formatTimeMMSS(s.time) + "] " + s.name + " >";
            }
            std::string modeOption = g_tapMode.load() ? "INPUT MODE: < TAP MODE >" : "INPUT MODE: < STRUM MODE >";

            const char* items[6] = {
                "RESUME SONG",
                "RESTART SONG",
                diffOption.c_str(),
                secOption.c_str(),
                modeOption.c_str(),
                "EXIT TO TRACKLIST"
            };

            for (int i = 0; i < 6; i++) {
                int iy = 320 + i * 76;
                bool sel = (i == pauseCursor);
                borderRect(fb, 480, iy, W - 960, 62, sel ? 3 : 1,
                           sel ? lerpCol(g_themeBgPrimary, C(28, 40, 72), 120) : C(20, 26, 42),
                           sel ? g_themeTrimSecondary : C(51, 65, 85));
                drawTextHDCentered(fb, W / 2, iy + 16, items[i], sel ? C(255, 255, 255) : C(203, 213, 225), false, 760);
            }

            drawTextHDCentered(fb, W / 2, 815,
                               "UP/DOWN: SELECT   LEFT/RIGHT: CHANGE   CROSS (X): CONFIRM   OPTIONS: RESUME",
                               g_themeTrimPrimary, false, 900);
        }

        flip();
    }
}

