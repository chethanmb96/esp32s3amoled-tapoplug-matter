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

// ── Real Arial TrueType Font Rendering Engine with Precomputed Edge Glow ─────
#include "arial_font.h"

static const arial_glyph_t *find_arial_glyph(const arial_glyph_t *font, char c)
{
    for (int i = 0; font[i].c != 0; i++) {
        if (font[i].c == c) return &font[i];
    }
    return NULL;
}

static void draw_arial_glyph(int x, int y, const arial_glyph_t *g, uint16_t color, uint16_t glow_color)
{
    if (!g || g->w == 0 || g->h == 0) return;
    int gx = x + g->off_x;
    int gy = y + g->off_y;

    for (int dy = 0; dy < g->h; dy++) {
        int py = gy + dy;
        if (py < 0 || py >= LCD_HEIGHT) continue;
        for (int dx = 0; dx < g->w; dx++) {
            int px = gx + dx;
            if (px < 0 || px >= LCD_WIDTH) continue;
            int idx = dy * g->w + dx;

            uint8_t glow_a = g->glow_data ? g->glow_data[idx] : 0;
            uint8_t core_a = g->core_data ? g->core_data[idx] : 0;

            if (glow_a > 0 && glow_color != 0) {
                blend_pixel_glow(px, py, glow_color, glow_a);
            }
            if (core_a > 0) {
                blend_pixel(px, py, color, core_a);
            }
        }
    }
}

static int draw_arial_string(int x, int y, const arial_glyph_t *font, const char *str, uint16_t color, uint16_t glow_color)
{
    int cur_x = x;
    while (*str) {
        const arial_glyph_t *g = find_arial_glyph(font, *str);
        if (g) {
            draw_arial_glyph(cur_x, y, g, color, glow_color);
            cur_x += g->adv_x;
        } else {
            cur_x += 10;
        }
        str++;
    }
    return cur_x - x;
}

static int arial_string_width(const arial_glyph_t *font, const char *str)
{
    int w = 0;
    while (*str) {
        const arial_glyph_t *g = find_arial_glyph(font, *str);
        if (g) {
            w += g->adv_x;
        } else {
            w += 10;
        }
        str++;
    }
    return w;
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

    // Logo / Title in Arial
    const char *title = "TAPO P116M";
    int tw = arial_string_width(font_arial_metric, title);
    draw_arial_string((LCD_WIDTH - tw) / 2, 70, font_arial_metric, title, C_CYAN, C_CYAN);

    // Accent line
    fill_rect(80, 100, LCD_WIDTH - 160, 2, C_CARD_BORDER);

    // Status text in Arial
    if (status_text) {
        int sw = arial_string_width(font_arial_label, status_text);
        draw_arial_string((LCD_WIDTH - sw) / 2, 140, font_arial_label, status_text, C_EMERALD, C_EMERALD);
    }

    if (subtext) {
        int subw = arial_string_width(font_arial_label, subtext);
        draw_arial_string((LCD_WIDTH - subw) / 2, 180, font_arial_label, subtext, C_TEXT_MUTED, 0);
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
    
    // Top Title: TAPO P116M (Real Arial Font in Muted Silver-Gray)
    draw_arial_string(38, 27, font_arial_label, "TAPO P116M", C_MUTED_GRAY, 0);

    // Top Right: Compact Glowing ON / OFF Rounded Pill Badge
    int pW = 56, pH = 26, pR = 12;
    int pX = LCD_WIDTH - pW - 24, pY = 9;
    if (state->is_on) {
        draw_rounded_rect(pX - 1, pY - 1, pW + 2, pH + 2, pR + 1, C_GLOW_EMERALD, C_GLOW_EMERALD);
        draw_rounded_rect(pX, pY, pW, pH, pR, C_EMERALD, C_EMERALD);
        int on_w = arial_string_width(font_arial_label, "ON");
        draw_arial_string(pX + (pW - on_w) / 2, pY + 19, font_arial_label, "ON", C_BLACK, 0);
    } else {
        draw_rounded_rect(pX - 1, pY - 1, pW + 2, pH + 2, pR + 1, C_GLOW_RED, C_GLOW_RED);
        draw_rounded_rect(pX, pY, pW, pH, pR, C_RED, C_RED);
        int off_w = arial_string_width(font_arial_label, "OFF");
        draw_arial_string(pX + (pW - off_w) / 2, pY + 19, font_arial_label, "OFF", C_WHITE, 0);
    }

    // 2. HERO METRIC: Active Power (Real Arial Bold Digits + Real Arial Cyan Unit)
    char power_buf[32];
    char unit_buf[8] = "w";
    float p_val = state->power_w;

    if (p_val >= 1000.0f) {
        snprintf(power_buf, sizeof(power_buf), "%.1f", p_val / 1000.0f);
        strcpy(unit_buf, "kw");
    } else {
        snprintf(power_buf, sizeof(power_buf), "%d", (int)roundf(p_val));
    }

    int num_total_w = arial_string_width(font_arial_hero, power_buf);
    int unit_total_w = arial_string_width(font_arial_unit, unit_buf);
    int gap = 8;
    int total_hero_w = num_total_w + gap + unit_total_w;

    int hero_x = (LCD_WIDTH - total_hero_w) / 2;
    int hero_y = 136; // Baseline alignment for Arial 118pt

    // Render Real Arial Bold power digits with luminous white halo glow
    uint16_t power_color = state->is_on ? C_WHITE : rgb565(120, 120, 130);
    uint16_t power_glow  = state->is_on ? C_WHITE : rgb565(40, 40, 50);
    draw_arial_string(hero_x, hero_y, font_arial_hero, power_buf, power_color, power_glow);

    // Render Real Arial unit in Neon Cyan with cyan halo glow aligned to baseline
    draw_arial_string(hero_x + num_total_w + gap, hero_y, font_arial_unit, unit_buf, C_NEON_CYAN, C_NEON_CYAN);

    // 3. BOTTOM METRICS: 2 Columns (Voltage on Left | Current on Right)
    int bottom_lbl_y = 172;
    int bottom_val_y = 218;

    // Metric 1: Voltage (Left aligned at x=24)
    draw_arial_string(24, bottom_lbl_y, font_arial_label, "VOLTAGE", C_MUTED_GRAY, 0);

    char volt_buf[32];
    snprintf(volt_buf, sizeof(volt_buf), "%dV", (int)roundf(state->voltage_v));
    draw_arial_string(24, bottom_val_y, font_arial_metric, volt_buf, C_AMBER_GOLD, C_AMBER_GOLD);

    // Metric 2: Current (Right aligned at x=LCD_WIDTH - width - 24)
    char curr_buf[32];
    snprintf(curr_buf, sizeof(curr_buf), "%.2fA", state->current_a);
    int c_lbl_w = arial_string_width(font_arial_label, "CURRENT");
    int c_val_w = arial_string_width(font_arial_metric, curr_buf);
    int c_lbl_x = LCD_WIDTH - c_lbl_w - 24;
    int c_val_x = LCD_WIDTH - c_val_w - 24;

    draw_arial_string(c_lbl_x, bottom_lbl_y, font_arial_label, "CURRENT", C_MUTED_GRAY, 0);
    draw_arial_string(c_val_x, bottom_val_y, font_arial_metric, curr_buf, C_SKY_BLUE, C_SKY_BLUE);

    // Push full frame to AMOLED
    rm67162_push_frame(s_fb);
    if (s_ui_mutex) xSemaphoreGive(s_ui_mutex);
}


