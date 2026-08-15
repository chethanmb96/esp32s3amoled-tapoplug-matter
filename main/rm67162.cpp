// =============================================================================
// rm67162.cpp — QSPI AMOLED Driver for LILYGO T-Display-S3 AMOLED
// =============================================================================

#include "rm67162.h"
#include <string.h>
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"

static const char *TAG = "rm67162";

static spi_device_handle_t s_spi_dev = NULL;
static uint16_t *s_dma_buf = NULL;
static const int LINES_PER_STRIP = 20;

// ── Low Level QSPI Send ───────────────────────────────────────────────────────
static void rm67162_write_cmd(uint8_t cmd, const uint8_t *params, size_t param_len)
{
    spi_transaction_ext_t t = {};
    t.base.flags = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR;
    t.base.cmd = 0x02;                          // 1-wire command prefix
    t.command_bits = 8;
    t.base.addr = ((uint32_t)cmd) << 8;         // 24-bit address containing register
    t.address_bits = 24;

    if (params && param_len > 0) {
        t.base.tx_buffer = params;
        t.base.length = param_len * 8;
    } else {
        t.base.length = 0;
    }

    spi_device_polling_transmit(s_spi_dev, &t.base);
}

static void rm67162_write_cmd_u8(uint8_t cmd, uint8_t val)
{
    rm67162_write_cmd(cmd, &val, 1);
}

static void rm67162_set_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    // CASET (Column Address Set)
    uint8_t caset[4] = {
        (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF),
        (uint8_t)(x2 >> 8), (uint8_t)(x2 & 0xFF)
    };
    rm67162_write_cmd(0x2A, caset, sizeof(caset));

    // RASET (Row Address Set)
    uint8_t raset[4] = {
        (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF),
        (uint8_t)(y2 >> 8), (uint8_t)(y2 & 0xFF)
    };
    rm67162_write_cmd(0x2B, raset, sizeof(raset));
}

// ── Initialization Sequence ───────────────────────────────────────────────────
typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t datalen;
} init_cmd_t;

static const init_cmd_t s_init_cmds[] = {
    {0x11, {0}, 0 | 0x80},                       // Sleep Out (with delay)
    {0x3A, {0x55}, 1},                           // Interface Pixel Format: 16-bit RGB565
    {0x51, {0xFF}, 1},                           // Write Display Brightness: 255 (Max)
    {0x36, {0x60}, 1},                           // MADCTL: Landscape orientation (0x60 or 0x70)
    {0x29, {0}, 0 | 0x80},                       // Display ON (with delay)
};

esp_err_t rm67162_init(void)
{
    ESP_LOGI(TAG, "Initializing RM67162 AMOLED QSPI display...");

    // 1. Configure Power Enable and Reset GPIOs
    gpio_config_t pwr_cfg = {
        .pin_bit_mask = (1ULL << LCD_PIN_EN) | (1ULL << LCD_PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwr_cfg);

    // Power ON the display LDO
    gpio_set_level((gpio_num_t)LCD_PIN_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    // Hardware Reset
    gpio_set_level((gpio_num_t)LCD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level((gpio_num_t)LCD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    // 2. Configure QSPI Bus
    spi_bus_config_t buscfg = {
        .data0_io_num = LCD_PIN_D0,
        .data1_io_num = LCD_PIN_D1,
        .sclk_io_num = LCD_PIN_SCK,
        .data2_io_num = LCD_PIN_D2,
        .data3_io_num = LCD_PIN_D3,
        .max_transfer_sz = 32768,
        .flags = SPICOMMON_BUSFLAG_QUAD | SPICOMMON_BUSFLAG_MASTER,
    };

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_device_interface_config_t devcfg = {
        .command_bits = 8,
        .address_bits = 24,
        .mode = 0,
        .clock_speed_hz = 40 * 1000 * 1000,     // 40 MHz QSPI
        .spics_io_num = LCD_PIN_CS,
        .flags = SPI_DEVICE_HALFDUPLEX,
        .queue_size = 10,
    };

    ret = spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SPI device: %s", esp_err_to_name(ret));
        return ret;
    }

    // 3. Send Initialization Commands
    for (size_t i = 0; i < sizeof(s_init_cmds) / sizeof(init_cmd_t); i++) {
        uint8_t cmd = s_init_cmds[i].cmd;
        uint8_t len = s_init_cmds[i].datalen & 0x7F;
        bool has_delay = (s_init_cmds[i].datalen & 0x80) != 0;

        rm67162_write_cmd(cmd, len > 0 ? s_init_cmds[i].data : NULL, len);
        if (has_delay) {
            vTaskDelay(pdMS_TO_TICKS(120));
        }
    }

    s_dma_buf = (uint16_t *)heap_caps_malloc(LCD_WIDTH * LINES_PER_STRIP * sizeof(uint16_t),
                                             MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_dma_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate internal DMA buffer");
    }

    ESP_LOGI(TAG, "RM67162 AMOLED Initialized (536x240 RGB565)");
    return ESP_OK;
}

void rm67162_set_brightness(uint8_t brightness)
{
    rm67162_write_cmd_u8(0x51, brightness);
}

void rm67162_push_rect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, const uint16_t *data)
{
    if (s_spi_dev == NULL || data == NULL) return;

    rm67162_set_window(x1, y1, x2, y2);

    size_t num_pixels = (x2 - x1 + 1) * (y2 - y1 + 1);
    size_t byte_len = num_pixels * 2;

    spi_transaction_ext_t t = {};
    t.base.flags = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_MODE_QIO;
    t.base.cmd = 0x32;                          // Quad write command
    t.command_bits = 8;
    t.base.addr = 0x002C00;                     // RAMWR register address
    t.address_bits = 24;
    t.base.tx_buffer = data;
    t.base.length = byte_len * 8;

    spi_device_polling_transmit(s_spi_dev, &t.base);
}

void rm67162_push_frame(const uint16_t *buffer)
{
    if (buffer == NULL || s_dma_buf == NULL) return;
    for (int y = 0; y < LCD_HEIGHT; y += LINES_PER_STRIP) {
        int h = LINES_PER_STRIP;
        if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
        memcpy(s_dma_buf, &buffer[y * LCD_WIDTH], LCD_WIDTH * h * sizeof(uint16_t));
        rm67162_push_rect(0, y, LCD_WIDTH - 1, y + h - 1, s_dma_buf);
    }
}
