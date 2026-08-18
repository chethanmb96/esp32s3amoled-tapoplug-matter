// =============================================================================
// display_ui.cpp — High-Contrast Dark UI Renderer for RM67162 AMOLED
// =============================================================================

#include "display_ui.h"
#include "rm67162.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "display_ui";

// Framebuffer in PSRAM & UI Render Mutex
static uint16_t *s_fb = NULL;
static SemaphoreHandle_t s_ui_mutex = NULL;

// ── Color Definitions (Byte-swapped for ESP32 SPI big-endian transfer) ────────
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (uint16_t)((c >> 8) | (c << 8)); // swap bytes for SPI
}

#define C_BLACK        0x0000
#define C_WHITE        0xFFFF
#define C_MUTED_GRAY   rgb565(140, 150, 170) // #8C96AA / 0x8CD2
#define C_NEON_CYAN    rgb565(0, 230, 255)   // #00E6FF / 0x077F
#define C_GLOW_EMERALD rgb565(0, 70, 35)     // 0x03E0 glow
#define C_EMERALD      rgb565(0, 230, 118)   // #00E676 / 0x07E0
#define C_GLOW_RED     rgb565(80, 10, 20)    // 0x4000 glow
#define C_RED          rgb565(248, 0, 0)     // #F800
#define C_AMBER_GOLD   rgb565(255, 185, 40)  // #FFB928 / 0xFD85 (Voltage)
#define C_SKY_BLUE     rgb565(60, 165, 255)  // #78c0ffff / 0x3D3F (Current)
#define C_TEXT_MUTED   C_MUTED_GRAY
#define C_CYAN         C_NEON_CYAN
#define C_AMBER        C_AMBER_GOLD
#define C_SKY          C_SKY_BLUE
#define C_CARD_BG      rgb565(18, 20, 26)
#define C_CARD_BORDER  rgb565(40, 45, 58)

// ── Primitive Drawing Routines ────────────────────────────────────────────────
static void draw_pixel(int x, int y, uint16_t color)
{
    if (x >= 0 && x < LCD_WIDTH && y >= 0 && y < LCD_HEIGHT) {
        s_fb[y * LCD_WIDTH + x] = color;
    }
}

static void fill_circle(int cx, int cy, int r, uint16_t color)
{
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y <= r * r) {
                draw_pixel(cx + x, cy + y, color);
            }
        }
    }
}

static inline void blend_pixel(int x, int y, uint16_t color, uint8_t alpha)
{
    if (x < 0 || x >= LCD_WIDTH || y < 0 || y >= LCD_HEIGHT || alpha == 0) return;
    if (alpha >= 250) {
        s_fb[y * LCD_WIDTH + x] = color;
        return;
    }

    uint16_t bg = s_fb[y * LCD_WIDTH + x];
    uint16_t bg_n = (bg >> 8) | (bg << 8);
    uint16_t fg_n = (color >> 8) | (color << 8);

    uint32_t bg_r = (bg_n >> 11) & 0x1F;
    uint32_t bg_g = (bg_n >> 5)  & 0x3F;
    uint32_t bg_b =  bg_n        & 0x1F;

    uint32_t fg_r = (fg_n >> 11) & 0x1F;
    uint32_t fg_g = (fg_n >> 5)  & 0x3F;
    uint32_t fg_b =  fg_n        & 0x1F;

    uint32_t r = (fg_r * alpha + bg_r * (255 - alpha)) / 255;
    uint32_t g = (fg_g * alpha + bg_g * (255 - alpha)) / 255;
    uint32_t b = (fg_b * alpha + bg_b * (255 - alpha)) / 255;

    uint16_t res_n = (r << 11) | (g << 5) | b;
    s_fb[y * LCD_WIDTH + x] = (res_n >> 8) | (res_n << 8);
}

static inline void blend_pixel_glow(int x, int y, uint16_t color, uint8_t alpha)
{
    if (x < 0 || x >= LCD_WIDTH || y < 0 || y >= LCD_HEIGHT || alpha == 0) return;

    uint16_t bg = s_fb[y * LCD_WIDTH + x];
    uint16_t bg_n = (bg >> 8) | (bg << 8);
    uint16_t fg_n = (color >> 8) | (color << 8);

    uint32_t bg_r = (bg_n >> 11) & 0x1F;
    uint32_t bg_g = (bg_n >> 5)  & 0x3F;
    uint32_t bg_b =  bg_n        & 0x1F;

    uint32_t fg_r = (fg_n >> 11) & 0x1F;
    uint32_t fg_g = (fg_n >> 5)  & 0x3F;
    uint32_t fg_b =  fg_n        & 0x1F;

    uint32_t r = bg_r + (fg_r * alpha) / 255; if (r > 31) r = 31;
    uint32_t g = bg_g + (fg_g * alpha) / 255; if (g > 63) g = 63;
    uint32_t b = bg_b + (fg_b * alpha) / 255; if (b > 31) b = 31;

    uint16_t res_n = (r << 11) | (g << 5) | b;
    s_fb[y * LCD_WIDTH + x] = (res_n >> 8) | (res_n << 8);
}

static void fill_rect(int x1, int y1, int w, int h, uint16_t color)
{
    int x2 = x1 + w;
    int y2 = y1 + h;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > LCD_WIDTH) x2 = LCD_WIDTH;
    if (y2 > LCD_HEIGHT) y2 = LCD_HEIGHT;

    for (int y = y1; y < y2; y++) {
        for (int x = x1; x < x2; x++) {
            s_fb[y * LCD_WIDTH + x] = color;
        }
    }
}

static void draw_rounded_rect(int x, int y, int w, int h, int r, uint16_t bg, uint16_t border)
{
    fill_rect(x + r, y, w - 2 * r, h, bg);
    fill_rect(x, y + r, r, h - 2 * r, bg);
    fill_rect(x + w - r, y + r, r, h - 2 * r, bg);

    for (int cx = 0; cx < r; cx++) {
        for (int cy = 0; cy < r; cy++) {
            if ((cx - r + 1) * (cx - r + 1) + (cy - r + 1) * (cy - r + 1) <= r * r) {
                draw_pixel(x + r - 1 - cx, y + r - 1 - cy, bg);
                draw_pixel(x + w - r + cx, y + r - 1 - cy, bg);
                draw_pixel(x + r - 1 - cx, y + h - r + cy, bg);
                draw_pixel(x + w - r + cx, y + h - r + cy, bg);
            }
        }
    }

    if (border != bg) {
        for (int i = x + r; i < x + w - r; i++) {
            draw_pixel(i, y, border);
            draw_pixel(i, y + h - 1, border);
        }
        for (int i = y + r; i < y + h - r; i++) {
            draw_pixel(x, i, border);
            draw_pixel(x + w - 1, i, border);
        }
    }
}

// ── Smooth Anti-Aliased Vector Typography Engine with Edge Bloom Halo ─────────
static void draw_smooth_segment(float x1, float y1, float x2, float y2, float r, uint16_t color)
{
    float dx = x2 - x1, dy = y2 - y1;
    float len2 = dx * dx + dy * dy;
    float max_reach = r + 5.5f; // Multi-layer soft luminous bloom halo
    int min_x = (int)floorf(fminf(x1, x2) - max_reach);
    int max_x = (int)ceilf(fmaxf(x1, x2) + max_reach);
    int min_y = (int)floorf(fminf(y1, y2) - max_reach);
    int max_y = (int)ceilf(fmaxf(y1, y2) + max_reach);

    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= LCD_WIDTH) max_x = LCD_WIDTH - 1;
    if (max_y >= LCD_HEIGHT) max_y = LCD_HEIGHT - 1;

    for (int y = min_y; y <= max_y; y++) {
        for (int x = min_x; x <= max_x; x++) {
            float px = (float)x - x1;
            float py = (float)y - y1;
            float t = (len2 > 0.0001f) ? (px * dx + py * dy) / len2 : 0.0f;
            if (t < 0.0f) t = 0.0f;
            else if (t > 1.0f) t = 1.0f;

            float qx = x1 + t * dx;
            float qy = y1 + t * dy;
            float dist = sqrtf(((float)x - qx) * ((float)x - qx) + ((float)y - qy) * ((float)y - qy));

            if (dist <= r - 0.5f) {
                draw_pixel(x, y, color);
            } else if (dist <= r + 0.6f) {
                // Core anti-aliasing
                uint8_t a = (uint8_t)((r + 0.6f - dist) * 255.0f);
                blend_pixel(x, y, color, a);
            } else if (dist <= r + 2.4f) {
                // Inner bright luminous glow
                float f = (r + 2.4f - dist) / 1.8f;
                uint8_t glow_a = (uint8_t)(f * f * 115.0f);
                if (glow_a > 0) blend_pixel_glow(x, y, color, glow_a);
            } else if (dist <= r + 5.5f) {
                // Outer diffused bloom halo
                float f = (r + 5.5f - dist) / 3.1f;
                uint8_t glow_a = (uint8_t)(f * f * 48.0f);
                if (glow_a > 0) blend_pixel_glow(x, y, color, glow_a);
            }
        }
    }
}

static void draw_smooth_char(float x, float y, float w, float h, float r, char c, uint16_t color)
{
    float x1 = x + r, x2 = x + w - r;
    float y1 = y + r, y2 = y + h - r;
    float xm = x + w * 0.5f;
    float ym = y + h * 0.5f;
    float rc_w = w * 0.22f;
    float rc_h = h * 0.16f;

    switch (c) {
    case '0':
        // Modern rounded stadium / capsule
        draw_smooth_segment(x1 + rc_w, y1, x2 - rc_w, y1, r, color);
        draw_smooth_segment(x2, y1 + rc_h, x2, y2 - rc_h, r, color);
        draw_smooth_segment(x2 - rc_w, y2, x1 + rc_w, y2, r, color);
        draw_smooth_segment(x1, y2 - rc_h, x1, y1 + rc_h, r, color);
        draw_smooth_segment(x1 + rc_w, y1, x1, y1 + rc_h, r, color);
        draw_smooth_segment(x2 - rc_w, y1, x2, y1 + rc_h, r, color);
        draw_smooth_segment(x2, y2 - rc_h, x2 - rc_w, y2, r, color);
        draw_smooth_segment(x1, y2 - rc_h, x1 + rc_w, y2, r, color);
        break;
    case '1':
        draw_smooth_segment(xm, y1, xm, y2, r, color);
        draw_smooth_segment(x + w * 0.22f, y + h * 0.26f, xm, y1, r, color);
        break;
    case '2':
        // Smooth rounded top, sweeping diagonal, flat baseline
        draw_smooth_segment(x1, y1 + rc_h, x1 + rc_w, y1, r, color);
        draw_smooth_segment(x1 + rc_w, y1, x2 - rc_w, y1, r, color);
        draw_smooth_segment(x2 - rc_w, y1, x2, y1 + rc_h, r, color);
        draw_smooth_segment(x2, y1 + rc_h, x2, y1 + h * 0.32f, r, color);
        draw_smooth_segment(x2, y1 + h * 0.32f, x1, y2, r, color);
        draw_smooth_segment(x1, y2, x2, y2, r, color);
        break;
    case '3':
        // Modern rounded 3 with center inward junction
        draw_smooth_segment(x1, y1, x2 - rc_w, y1, r, color);
        draw_smooth_segment(x2 - rc_w, y1, x2, y1 + rc_h, r, color);
        draw_smooth_segment(x2, y1 + rc_h, x2, ym - h * 0.06f, r, color);
        draw_smooth_segment(x2, ym - h * 0.06f, xm + w * 0.05f, ym, r, color);
        draw_smooth_segment(xm + w * 0.05f, ym, x2, ym + h * 0.06f, r, color);
        draw_smooth_segment(x2, ym + h * 0.06f, x2, y2 - rc_h, r, color);
        draw_smooth_segment(x2, y2 - rc_h, x2 - rc_w, y2, r, color);
        draw_smooth_segment(x2 - rc_w, y2, x1 + rc_w, y2, r, color);
        draw_smooth_segment(x1 + rc_w, y2, x1, y2 - rc_h, r, color);
        break;
    case '4':
        // Classic modern geometric 4: vertical right stem + diagonal left arm + crossbar
        draw_smooth_segment(x + w * 0.74f, y1, x + w * 0.74f, y2, r, color);
        draw_smooth_segment(x + w * 0.74f, y1, x1, ym + h * 0.12f, r, color);
        draw_smooth_segment(x1, ym + h * 0.12f, x2, ym + h * 0.12f, r, color);
        break;
    case '5':
        draw_smooth_segment(x2, y1, x1, y1, r, color);
        draw_smooth_segment(x1, y1, x1, ym - h * 0.02f, r, color);
        draw_smooth_segment(x1, ym - h * 0.02f, x2 - rc_w, ym - h * 0.02f, r, color);
        draw_smooth_segment(x2 - rc_w, ym - h * 0.02f, x2, ym + rc_h, r, color);
        draw_smooth_segment(x2, ym + rc_h, x2, y2 - rc_h, r, color);
        draw_smooth_segment(x2, y2 - rc_h, x2 - rc_w, y2, r, color);
        draw_smooth_segment(x2 - rc_w, y2, x1, y2, r, color);
        break;
    case '6':
        draw_smooth_segment(x2 - rc_w, y1, x1 + rc_w, y1, r, color);
        draw_smooth_segment(x1 + rc_w, y1, x1, y1 + rc_h, r, color);
        draw_smooth_segment(x1, y1 + rc_h, x1, y2 - rc_h, r, color);
        draw_smooth_segment(x1, y2 - rc_h, x1 + rc_w, y2, r, color);
        draw_smooth_segment(x1 + rc_w, y2, x2 - rc_w, y2, r, color);
        draw_smooth_segment(x2 - rc_w, y2, x2, y2 - rc_h, r, color);
        draw_smooth_segment(x2, y2 - rc_h, x2, ym, r, color);
        draw_smooth_segment(x2, ym, x1, ym, r, color);
        break;
    case '7':
        draw_smooth_segment(x1, y1, x2, y1, r, color);
        draw_smooth_segment(x2, y1, x1 + w * 0.15f, y2, r, color);
        break;
    case '8':
        draw_smooth_segment(x1 + rc_w, y1, x2 - rc_w, y1, r, color);
        draw_smooth_segment(x1 + rc_w, ym, x2 - rc_w, ym, r, color);
        draw_smooth_segment(x1 + rc_w, y2, x2 - rc_w, y2, r, color);
        draw_smooth_segment(x1, y1 + rc_h, x1, ym - rc_h, r, color);
        draw_smooth_segment(x2, y1 + rc_h, x2, ym - rc_h, r, color);
        draw_smooth_segment(x1, ym + rc_h, x1, y2 - rc_h, r, color);
        draw_smooth_segment(x2, ym + rc_h, x2, y2 - rc_h, r, color);
        draw_smooth_segment(x1 + rc_w, y1, x1, y1 + rc_h, r, color);
        draw_smooth_segment(x2 - rc_w, y1, x2, y1 + rc_h, r, color);
        draw_smooth_segment(x1, ym - rc_h, x1 + rc_w, ym, r, color);
        draw_smooth_segment(x2, ym - rc_h, x2 - rc_w, ym, r, color);
        draw_smooth_segment(x1 + rc_w, ym, x1, ym + rc_h, r, color);
        draw_smooth_segment(x2 - rc_w, ym, x2, ym + rc_h, r, color);
        draw_smooth_segment(x1, y2 - rc_h, x1 + rc_w, y2, r, color);
        draw_smooth_segment(x2, y2 - rc_h, x2 - rc_w, y2, r, color);
        break;
    case '9':
        draw_smooth_segment(x1 + rc_w, ym, x2, ym, r, color);
        draw_smooth_segment(x1, ym - rc_h, x1, y1 + rc_h, r, color);
        draw_smooth_segment(x1, y1 + rc_h, x1 + rc_w, y1, r, color);
        draw_smooth_segment(x1 + rc_w, y1, x2 - rc_w, y1, r, color);
        draw_smooth_segment(x2 - rc_w, y1, x2, y1 + rc_h, r, color);
        draw_smooth_segment(x2, y1 + rc_h, x2, y2 - rc_h, r, color);
        draw_smooth_segment(x2, y2 - rc_h, x2 - rc_w, y2, r, color);
        draw_smooth_segment(x2 - rc_w, y2, x1 + rc_w, y2, r, color);
        break;
    case 'A':
        draw_smooth_segment(x1, y2, xm, y1, r, color);
        draw_smooth_segment(xm, y1, x2, y2, r, color);
        draw_smooth_segment(x + w * 0.22f, ym + h * 0.12f, x + w * 0.78f, ym + h * 0.12f, r, color);
        break;
    case 'B':
        draw_smooth_segment(x1, y1, x1, y2, r, color);
        draw_smooth_segment(x1, y1, x2 - rc_w, y1, r, color);
        draw_smooth_segment(x2 - rc_w, y1, x2, y1 + rc_h, r, color);
        draw_smooth_segment(x2, y1 + rc_h, x1, ym, r, color);
        draw_smooth_segment(x1, ym, x2, ym + rc_h, r, color);
        draw_smooth_segment(x2, ym + rc_h, x2 - rc_w, y2, r, color);
        draw_smooth_segment(x2 - rc_w, y2, x1, y2, r, color);
        break;
    case 'C':
        draw_smooth_segment(x2, y1, x1 + rc_w, y1, r, color);
        draw_smooth_segment(x1 + rc_w, y1, x1, y1 + rc_h, r, color);
        draw_smooth_segment(x1, y1 + rc_h, x1, y2 - rc_h, r, color);
        draw_smooth_segment(x1, y2 - rc_h, x1 + rc_w, y2, r, color);
        draw_smooth_segment(x1 + rc_w, y2, x2, y2, r, color);
        break;
    case 'D':
        draw_smooth_segment(x1, y1, x1, y2, r, color);
        draw_smooth_segment(x1, y1, x2 - rc_w, y1, r, color);
        draw_smooth_segment(x2 - rc_w, y1, x2, ym, r, color);
        draw_smooth_segment(x2, ym, x2 - rc_w, y2, r, color);
        draw_smooth_segment(x2 - rc_w, y2, x1, y2, r, color);
        break;
    case 'E':
        draw_smooth_segment(x1, y1, x1, y2, r, color);
        draw_smooth_segment(x1, y1, x2, y1, r, color);
        draw_smooth_segment(x1, ym, x2 - w * 0.18f, ym, r, color);
        draw_smooth_segment(x1, y2, x2, y2, r, color);
        break;
    case 'F':
        draw_smooth_segment(x1, y1, x1, y2, r, color);
        draw_smooth_segment(x1, y1, x2, y1, r, color);
        draw_smooth_segment(x1, ym, x2 - w * 0.18f, ym, r, color);
        break;
    case 'G':
        draw_smooth_segment(x2, y1, x1 + rc_w, y1, r, color);
        draw_smooth_segment(x1 + rc_w, y1, x1, y1 + rc_h, r, color);
        draw_smooth_segment(x1, y1 + rc_h, x1, y2 - rc_h, r, color);
        draw_smooth_segment(x1, y2 - rc_h, x1 + rc_w, y2, r, color);
        draw_smooth_segment(x1 + rc_w, y2, x2, y2, r, color);
        draw_smooth_segment(x2, y2, x2, ym, r, color);
        draw_smooth_segment(x2, ym, xm, ym, r, color);
        break;
    case 'H':
        draw_smooth_segment(x1, y1, x1, y2, r, color);
        draw_smooth_segment(x2, y1, x2, y2, r, color);
        draw_smooth_segment(x1, ym, x2, ym, r, color);
        break;
    case 'I':
        draw_smooth_segment(xm, y1, xm, y2, r, color);
        break;
    case 'J':
        draw_smooth_segment(x2, y1, x2, y2 - rc_h, r, color);
        draw_smooth_segment(x2, y2 - rc_h, x2 - rc_w, y2, r, color);
        draw_smooth_segment(x2 - rc_w, y2, x1, y2 - rc_h, r, color);
        break;
    case 'K':
        draw_smooth_segment(x1, y1, x1, y2, r, color);
        draw_smooth_segment(x2, y1, x1, ym, r, color);
        draw_smooth_segment(x1 + w * 0.15f, ym, x2, y2, r, color);
        break;
    case 'L':
        draw_smooth_segment(x1, y1, x1, y2, r, color);
        draw_smooth_segment(x1, y2, x2, y2, r, color);
        break;
    case 'M':
        draw_smooth_segment(x1, y2, x1, y1, r, color);
        draw_smooth_segment(x1, y1, xm, ym + h * 0.15f, r, color);
        draw_smooth_segment(xm, ym + h * 0.15f, x2, y1, r, color);
        draw_smooth_segment(x2, y1, x2, y2, r, color);
        break;
    case 'N':
        draw_smooth_segment(x1, y2, x1, y1, r, color);
        draw_smooth_segment(x1, y1, x2, y2, r, color);
        draw_smooth_segment(x2, y2, x2, y1, r, color);
        break;
    case 'O':
        draw_smooth_segment(x1 + rc_w, y1, x2 - rc_w, y1, r, color);
        draw_smooth_segment(x2, y1 + rc_h, x2, y2 - rc_h, r, color);
        draw_smooth_segment(x2 - rc_w, y2, x1 + rc_w, y2, r, color);
        draw_smooth_segment(x1, y2 - rc_h, x1, y1 + rc_h, r, color);
        draw_smooth_segment(x1 + rc_w, y1, x1, y1 + rc_h, r, color);
        draw_smooth_segment(x2 - rc_w, y1, x2, y1 + rc_h, r, color);
        draw_smooth_segment(x2, y2 - rc_h, x2 - rc_w, y2, r, color);
        draw_smooth_segment(x1, y2 - rc_h, x1 + rc_w, y2, r, color);
        break;
    case 'P':
        draw_smooth_segment(x1, y1, x1, y2, r, color);
        draw_smooth_segment(x1, y1, x2 - rc_w, y1, r, color);
        draw_smooth_segment(x2 - rc_w, y1, x2, y1 + rc_h, r, color);
        draw_smooth_segment(x2, y1 + rc_h, x2, ym - rc_h, r, color);
        draw_smooth_segment(x2, ym - rc_h, x2 - rc_w, ym, r, color);
        draw_smooth_segment(x2 - rc_w, ym, x1, ym, r, color);
        break;
    case 'Q':
        draw_smooth_segment(x1 + rc_w, y1, x2 - rc_w, y1, r, color);
        draw_smooth_segment(x2, y1 + rc_h, x2, y2 - rc_h, r, color);
        draw_smooth_segment(x2 - rc_w, y2, x1 + rc_w, y2, r, color);
        draw_smooth_segment(x1, y2 - rc_h, x1, y1 + rc_h, r, color);
        draw_smooth_segment(xm, ym + h * 0.15f, x2 + w * 0.1f, y2 + h * 0.1f, r, color);
        break;
    case 'R':
        draw_smooth_segment(x1, y1, x1, y2, r, color);
        draw_smooth_segment(x1, y1, x2 - rc_w, y1, r, color);
        draw_smooth_segment(x2 - rc_w, y1, x2, y1 + rc_h, r, color);
        draw_smooth_segment(x2, y1 + rc_h, x2, ym - rc_h, r, color);
        draw_smooth_segment(x2, ym - rc_h, x2 - rc_w, ym, r, color);
        draw_smooth_segment(x2 - rc_w, ym, x1, ym, r, color);
        draw_smooth_segment(x1 + w * 0.25f, ym, x2, y2, r, color);
        break;
    case 'S':
        draw_smooth_segment(x2, y1, x1 + rc_w, y1, r, color);
        draw_smooth_segment(x1 + rc_w, y1, x1, y1 + rc_h, r, color);
        draw_smooth_segment(x1, y1 + rc_h, x2, ym + rc_h, r, color);
        draw_smooth_segment(x2, ym + rc_h, x2, y2 - rc_h, r, color);
        draw_smooth_segment(x2, y2 - rc_h, x2 - rc_w, y2, r, color);
        draw_smooth_segment(x2 - rc_w, y2, x1, y2, r, color);
        break;
    case 'T':
        draw_smooth_segment(x1, y1, x2, y1, r, color);
        draw_smooth_segment(xm, y1, xm, y2, r, color);
        break;
    case 'U':
        draw_smooth_segment(x1, y1, x1, y2 - rc_h, r, color);
        draw_smooth_segment(x1, y2 - rc_h, x1 + rc_w, y2, r, color);
        draw_smooth_segment(x1 + rc_w, y2, x2 - rc_w, y2, r, color);
        draw_smooth_segment(x2 - rc_w, y2, x2, y2 - rc_h, r, color);
        draw_smooth_segment(x2, y2 - rc_h, x2, y1, r, color);
        break;
    case 'V':
        draw_smooth_segment(x1, y1, xm, y2, r, color);
        draw_smooth_segment(xm, y2, x2, y1, r, color);
        break;
    case 'W':
    case 'w':
        // Modern geometric lowercase w (or W)
        draw_smooth_segment(x1, y1, x + w * 0.28f, y2, r, color);
        draw_smooth_segment(x + w * 0.28f, y2, xm, y1 + h * 0.35f, r, color);
        draw_smooth_segment(xm, y1 + h * 0.35f, x + w * 0.72f, y2, r, color);
        draw_smooth_segment(x + w * 0.72f, y2, x2, y1, r, color);
        break;
    case 'X':
        draw_smooth_segment(x1, y1, x2, y2, r, color);
        draw_smooth_segment(x2, y1, x1, y2, r, color);
        break;
    case 'Y':
        draw_smooth_segment(x1, y1, xm, ym, r, color);
        draw_smooth_segment(x2, y1, xm, ym, r, color);
        draw_smooth_segment(xm, ym, xm, y2, r, color);
        break;
    case 'Z':
        draw_smooth_segment(x1, y1, x2, y1, r, color);
        draw_smooth_segment(x2, y1, x1, y2, r, color);
        draw_smooth_segment(x1, y2, x2, y2, r, color);
        break;
    case 'k':
        draw_smooth_segment(x1, y1, x1, y2, r, color);
        draw_smooth_segment(x2, y + h * 0.32f, x1, ym + h * 0.08f, r, color);
        draw_smooth_segment(x1, ym + h * 0.08f, x2, y2, r, color);
        break;
    case 'h':
        draw_smooth_segment(x1, y1, x1, y2, r, color);
        draw_smooth_segment(x1, ym, x2 - rc_w, ym, r, color);
        draw_smooth_segment(x2 - rc_w, ym, x2, ym + rc_h, r, color);
        draw_smooth_segment(x2, ym + rc_h, x2, y2, r, color);
        break;
    case '.':
        draw_smooth_segment(xm, y2 - r * 0.6f, xm, y2 - r * 0.6f, r * 1.3f, color);
        break;
    case ':':
        draw_smooth_segment(xm, ym - h * 0.25f, xm, ym - h * 0.25f, r * 1.2f, color);
        draw_smooth_segment(xm, ym + h * 0.25f, xm, ym + h * 0.25f, r * 1.2f, color);
        break;
    case '-':
        draw_smooth_segment(x1, ym, x2, ym, r, color);
        break;
    default:
        break;
    }
}

static void draw_smooth_string(float x, float y, float char_w, float char_h, float stroke_r, float gap, const char *str, uint16_t color)
{
    float cur_x = x;
    while (*str) {
        float w = char_w;
        if (*str == '.' || *str == ':' || *str == ' ') {
            w = char_w * 0.35f;
        } else if (*str == '1' || *str == 'I') {
            w = char_w * 0.6f;
        } else if (*str == 'W' || *str == 'M') {
            w = char_w * 1.2f;
        }
        if (*str != ' ') {
            draw_smooth_char(cur_x, y, w, char_h, stroke_r, *str, color);
        }
        cur_x += w + gap;
        str++;
    }
}

static float smooth_string_width(float char_w, float gap, const char *str)
{
    float total = 0;
    while (*str) {
        float w = char_w;
        if (*str == '.' || *str == ':' || *str == ' ') {
            w = char_w * 0.35f;
        } else if (*str == '1' || *str == 'I') {
            w = char_w * 0.6f;
        } else if (*str == 'W' || *str == 'M') {
            w = char_w * 1.2f;
        }
        total += w + gap;
        str++;
    }
    return (total > 0) ? total - gap : 0;
}

// ── Built-in 8x16 Basic Font Table ───────────────────────────────────────────
#include "font_8x16.h"

static void draw_char_scaled(int x, int y, char c, uint16_t color, int scale)
{
    if (c < 32 || c > 126) c = '?';
    const uint8_t *glyph = &font_8x16[(c - 32) * 16];

    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                if (scale == 1) {
                    draw_pixel(x + col, y + row, color);
                } else {
                    fill_rect(x + col * scale, y + row * scale, scale, scale, color);
                }
            }
        }
    }
}

static void draw_string(int x, int y, const char *str, uint16_t color, int scale)
{
    int cur_x = x;
    while (*str) {
        draw_char_scaled(cur_x, y, *str, color, scale);
        cur_x += 8 * scale;
        str++;
    }
}

static int string_width(const char *str, int scale)
{
    return strlen(str) * 8 * scale;
}

// ── Public API ────────────────────────────────────────────────────────────────
void display_ui_init(void)
{
    if (s_ui_mutex == NULL) {
        s_ui_mutex = xSemaphoreCreateMutex();
    }
    if (s_fb == NULL) {
        s_fb = (uint16_t *)heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_fb == NULL) {
            ESP_LOGE(TAG, "Failed to allocate %d bytes in PSRAM for framebuffer",
                     LCD_WIDTH * LCD_HEIGHT * 2);
            return;
        }
        memset(s_fb, 0, LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t));
    }
    rm67162_init();
    rm67162_set_brightness(255);
}

void display_ui_show_splash(const char *status_text, const char *subtext)
{
    if (s_fb == NULL) return;
    if (s_ui_mutex && xSemaphoreTake(s_ui_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    memset(s_fb, 0, LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t));

    // Logo / Title
    const char *title = "TAPO P116M";
    float tw = smooth_string_width(20.0f, 4.0f, title);
    draw_smooth_string((LCD_WIDTH - tw) / 2.0f, 45.0f, 20.0f, 32.0f, 2.4f, 4.0f, title, C_CYAN);

    // Accent line
    fill_rect(80, 95, LCD_WIDTH - 160, 2, C_CARD_BORDER);

    // Status box
    if (status_text) {
        float sw = smooth_string_width(14.0f, 3.0f, status_text);
        draw_smooth_string((LCD_WIDTH - sw) / 2.0f, 120.0f, 14.0f, 22.0f, 1.8f, 3.0f, status_text, C_EMERALD);
    }

    if (subtext) {
        float subw = smooth_string_width(10.0f, 2.0f, subtext);
        draw_smooth_string((LCD_WIDTH - subw) / 2.0f, 165.0f, 10.0f, 16.0f, 1.3f, 2.0f, subtext, C_TEXT_MUTED);
    }

    rm67162_push_frame(s_fb);
    if (s_ui_mutex) xSemaphoreGive(s_ui_mutex);
}

void display_ui_update(const ui_state_t *state)
{
    if (s_fb == NULL || state == NULL) return;
    if (s_ui_mutex && xSemaphoreTake(s_ui_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    memset(s_fb, 0, LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t));

    // 1. TOP HEADER: Status Dot with Glow + "TAPO P116M" + Glowing Pill ON/OFF Badge
    int dotX = 24, dotY = 22;
    if (state->is_on) {
        fill_circle(dotX, dotY, 7, C_GLOW_EMERALD);
        fill_circle(dotX, dotY, 4, C_EMERALD);
    } else {
        fill_circle(dotX, dotY, 7, C_GLOW_RED);
        fill_circle(dotX, dotY, 4, C_RED);
    }
    
    // Top Title: TAPO P116M (Smooth Font in Muted Silver-Gray)
    draw_smooth_string(38.0f, 13.0f, 11.0f, 18.0f, 1.5f, 2.5f, "TAPO P116M", C_MUTED_GRAY);

    // Top Right: Compact Glowing ON / OFF Rounded Pill Badge
    int pW = 56, pH = 26, pR = 12;
    int pX = LCD_WIDTH - pW - 24, pY = 9;
    if (state->is_on) {
        draw_rounded_rect(pX - 1, pY - 1, pW + 2, pH + 2, pR + 1, C_GLOW_EMERALD, C_GLOW_EMERALD);
        draw_rounded_rect(pX, pY, pW, pH, pR, C_EMERALD, C_EMERALD);
        float on_w = smooth_string_width(12.0f, 3.0f, "ON");
        draw_smooth_string(pX + (pW - on_w) / 2.0f, pY + 4.0f, 12.0f, 18.0f, 1.8f, 3.0f, "ON", C_BLACK);
    } else {
        draw_rounded_rect(pX - 1, pY - 1, pW + 2, pH + 2, pR + 1, C_GLOW_RED, C_GLOW_RED);
        draw_rounded_rect(pX, pY, pW, pH, pR, C_RED, C_RED);
        float off_w = smooth_string_width(12.0f, 3.0f, "OFF");
        draw_smooth_string(pX + (pW - off_w) / 2.0f, pY + 4.0f, 12.0f, 18.0f, 1.8f, 3.0f, "OFF", C_WHITE);
    }

    // 2. HERO METRIC: Active Power (Smooth Anti-Aliased Vector Readout)
    char power_buf[32];
    char unit_buf[8] = "W";
    float p_val = state->power_w;

    if (p_val >= 1000.0f) {
        snprintf(power_buf, sizeof(power_buf), "%.1f", p_val / 1000.0f);
        strcpy(unit_buf, "kW");
    } else {
        snprintf(power_buf, sizeof(power_buf), "%d", (int)roundf(p_val));
    }

    // Smooth typography parameters for Power Readout
    float char_w = 54.0f;
    float char_h = 98.0f;
    float stroke_r = 5.0f;
    float gap = 10.0f;

    float unit_w = 28.0f;
    float unit_h = 42.0f;
    float unit_stroke_r = 3.0f;
    float unit_gap = 6.0f;

    float num_total_w = smooth_string_width(char_w, gap, power_buf);
    float unit_total_w = smooth_string_width(unit_w, unit_gap, unit_buf);
    float total_hero_w = num_total_w + 14.0f + unit_total_w;

    float hero_x = (LCD_WIDTH - total_hero_w) / 2.0f;
    float hero_y = 48.0f;

    // Render smooth anti-aliased power digits in Pure White
    uint16_t power_color = state->is_on ? C_WHITE : rgb565(120, 120, 130);
    draw_smooth_string(hero_x, hero_y, char_w, char_h, stroke_r, gap, power_buf, power_color);

    // Render smooth unit in Neon Cyan aligned directly to the baseline
    float unit_y = hero_y + char_h - unit_h;
    draw_smooth_string(hero_x + num_total_w + 14.0f, unit_y, unit_w, unit_h, unit_stroke_r, unit_gap, unit_buf, C_NEON_CYAN);

    // 3. BOTTOM METRICS: 2 Columns (Voltage on Left | Current on Right)
    // Rendered with large, smooth anti-aliased vector typography
    float bottom_lbl_y = 154.0f;
    float bottom_val_y = 178.0f;
    float lbl_w = 11.0f, lbl_h = 17.0f, lbl_r = 1.5f, lbl_gap = 3.0f;
    float val_w = 21.0f, val_h = 36.0f, val_r = 2.8f, val_gap = 4.5f;

    // Metric 1: Voltage (Left aligned at x=24)
    draw_smooth_string(24.0f, bottom_lbl_y, lbl_w, lbl_h, lbl_r, lbl_gap, "VOLTAGE", C_MUTED_GRAY);

    char volt_buf[32];
    snprintf(volt_buf, sizeof(volt_buf), "%dV", (int)roundf(state->voltage_v));
    draw_smooth_string(24.0f, bottom_val_y, val_w, val_h, val_r, val_gap, volt_buf, C_AMBER_GOLD);

    // Metric 2: Current (Right aligned at x=LCD_WIDTH - width - 24)
    char curr_buf[32];
    snprintf(curr_buf, sizeof(curr_buf), "%.2fA", state->current_a);
    float c_val_w = smooth_string_width(val_w, val_gap, curr_buf);
    float c_lbl_w = smooth_string_width(lbl_w, lbl_gap, "CURRENT");
    float c_val_x = (float)LCD_WIDTH - c_val_w - 24.0f;
    float c_lbl_x = (float)LCD_WIDTH - c_lbl_w - 24.0f;

    draw_smooth_string(c_lbl_x, bottom_lbl_y, lbl_w, lbl_h, lbl_r, lbl_gap, "CURRENT", C_MUTED_GRAY);
    draw_smooth_string(c_val_x, bottom_val_y, val_w, val_h, val_r, val_gap, curr_buf, C_SKY_BLUE);

    // Push full frame to AMOLED
    rm67162_push_frame(s_fb);
    if (s_ui_mutex) xSemaphoreGive(s_ui_mutex);
}

