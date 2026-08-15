// =============================================================================
// rm67162.h — QSPI AMOLED Driver for LILYGO T-Display-S3 AMOLED
// Resolution: 536 x 240, Color: RGB565 (16-bit)
// =============================================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LCD_WIDTH   536
#define LCD_HEIGHT  240

// Hardware Pin Definitions (LILYGO T-Display-S3 AMOLED)
#define LCD_PIN_CS      6
#define LCD_PIN_SCK     47
#define LCD_PIN_D0      18
#define LCD_PIN_D1      7
#define LCD_PIN_D2      48
#define LCD_PIN_D3      5
#define LCD_PIN_RST     17
#define LCD_PIN_EN      38

/**
 * @brief Initialize the RM67162 QSPI AMOLED display controller
 * @return ESP_OK on success
 */
esp_err_t rm67162_init(void);

/**
 * @brief Set display brightness (0 - 255)
 */
void rm67162_set_brightness(uint8_t brightness);

/**
 * @brief Push a full framebuffer (536x240 RGB565) to the display
 * @param buffer Pointer to 536*240*2 bytes in memory (e.g. PSRAM)
 */
void rm67162_push_frame(const uint16_t *buffer);

/**
 * @brief Push a rectangular area of pixels to the display
 */
void rm67162_push_rect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, const uint16_t *data);

#ifdef __cplusplus
}
#endif
