// =============================================================================
// display_ui.h — High-Contrast Dark UI for 536x240 AMOLED Display
// =============================================================================

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float power_w;         // Active power in Watts
    float voltage_v;       // RMS voltage in Volts
    float current_a;       // RMS current in Amperes
    bool is_on;            // Relay state (true = ON, false = OFF)
    bool is_connected;     // Matter connection state
    const char *status_msg;// Status text (e.g. "CONNECTED", "CONNECTING...")
} ui_state_t;

/**
 * @brief Initialize the UI system and allocate PSRAM framebuffer
 */
void display_ui_init(void);

/**
 * @brief Render boot / pairing splash screen
 */
void display_ui_show_splash(const char *status_text, const char *subtext);

/**
 * @brief Update and render the live telemetry UI
 */
void display_ui_update(const ui_state_t *state);

#ifdef __cplusplus
}
#endif
