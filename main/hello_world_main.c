/*
 * ============================================================================
 *  ST7789 1.54" 240x240 + SHT3x 温湿度传感器
 *  ESP32-S3-N16R8 + ESP-IDF v5.5.5
 * ----------------------------------------------------------------------------
 *  接线：
 *    LCD : SCLK=12  SDA(MOSI)=11  CS=10  DC=13  RES=14  BLK=3.3V
 *    SHT3x: VCC=3.3V  GND=GND  SDA=GPIO8  SCL=GPIO9
 *           （地址默认 0x44；模块 ADDR 脚接高则 0x45，代码会自动扫描）
 *
 *  特点：
 *    - SHT3x 驱动自己实现（单次测量 + CRC8 校验），不依赖第三方库
 *    - 传感器没接 / 读失败 时显示 "--.-" 并在底部提示 NO SENSOR
 *    - 4 点滑动平均，读数不会乱跳
 *    - 如果想先不接传感器看效果，把 USE_FAKE_SENSOR 改回 1
 * ============================================================================
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

static const char *TAG = "TEMP_HUMI";

/* ==========================================================================
 * 1. 接线
 * ========================================================================== */
/* LCD */
#define PIN_SCLK   12
#define PIN_MOSI   11
#define PIN_CS     10
#define PIN_DC     13
#define PIN_RST    14

/* SHT3x (I2C) */
#define PIN_I2C_SDA   8
#define PIN_I2C_SCL   9
#define SHT3X_ADDR_A  0x44      /* ADDR 接 GND（默认） */
#define SHT3X_ADDR_B  0x45      /* ADDR 接 VDD */

/* ==========================================================================
 * 2. 屏幕参数（已验证，别乱改）
 * ========================================================================== */
#define LCD_PCLK_HZ           (8 * 1000 * 1000)
#define LCD_GAP_Y             0
#define LCD_INVERT            1
#define LCD_BGR               0
#define LCD_LITTLE_ENDIAN     1

/* ==========================================================================
 * 3. 数据源
 * ========================================================================== */
#define USE_FAKE_SENSOR       0     /* 0 = 用 SHT3x；1 = 用模拟数据 */

#define AVG_N                 4     /* 滑动平均点数，1 = 不平滑 */

/* ========================================================================== */

#define LCD_H_RES   240
#define LCD_V_RES   240
#define BUF_ROWS    64
#define FILL_ROWS   60

#define C_BLACK   0x0000
#define C_WHITE   0xFFFF
#define C_CYAN    0x07FF
#define C_GOLD    0xFD20
#define C_GRAY    0x8410
#define C_NAVY    0x0010
#define C_GREEN   0x07E0
#define C_RED     0xF800

static esp_lcd_panel_handle_t s_panel      = NULL;
static esp_lcd_panel_io_handle_t s_io      = NULL;
static uint16_t              *s_buf        = NULL;
static SemaphoreHandle_t      s_flush_done = NULL;

/* ==========================================================================
 * 4. 5x7 点阵字库（列优先，bit0 = 最上面一行）
 * ========================================================================== */
typedef struct {
    char    c;
    uint8_t col[5];
} glyph_t;

static const glyph_t FONT[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {'%', {0x62, 0x64, 0x08, 0x13, 0x23}},
    {'+', {0x08, 0x08, 0x3E, 0x08, 0x08}},
    {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
    {'/', {0x20, 0x10, 0x08, 0x04, 0x02}},
    {':', {0x00, 0x36, 0x36, 0x00, 0x00}},
    {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}},
    {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
    {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}},
    {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}},
    {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
    {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
    {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}},
    {'B', {0x7F, 0x49, 0x49, 0x49, 0x36}},
    {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}},
    {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
    {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
    {'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}},
    {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
    {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}},
    {'J', {0x20, 0x40, 0x41, 0x3F, 0x01}},
    {'K', {0x7F, 0x08, 0x14, 0x22, 0x41}},
    {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}},
    {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
    {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
    {'Q', {0x3E, 0x41, 0x51, 0x21, 0x5E}},
    {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x32}},
    {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
    {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}},
    {'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
    {'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}},
    {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
    {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
};

static const uint8_t *glyph_for(char c)
{
    if (c >= 'a' && c <= 'z') {
        c -= 32;
    }
    for (size_t i = 0; i < sizeof(FONT) / sizeof(FONT[0]); i++) {
        if (FONT[i].c == c) {
            return FONT[i].col;
        }
    }
    return NULL;
}

/* ==========================================================================
 * 5. 传输同步 + 绘图
 * ========================================================================== */
static bool on_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t need_yield = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &need_yield);
    return need_yield == pdTRUE;
}

static esp_err_t lcd_blit(int x0, int y0, int x1, int y1)
{
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, s_buf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "draw_bitmap 失败: %s", esp_err_to_name(err));
        return err;
    }
    if (xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "等待 DMA 完成超时");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void lcd_fill(int x0, int y0, int x1, int y1, uint16_t color)
{
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    int w = x1 - x0;
    for (int y = y0; y < y1; y += FILL_ROWS) {
        int rows = (y1 - y) < FILL_ROWS ? (y1 - y) : FILL_ROWS;
        for (int i = 0; i < w * rows; i++) {
            s_buf[i] = color;
        }
        lcd_blit(x0, y, x1, y + rows);
    }
}

static void draw_text(int x, int y, int box_w, int scale,
                      const char *s, uint16_t fg, uint16_t bg)
{
    int h = 8 * scale;
    if (x < 0 || y < 0 || scale < 1) {
        return;
    }
    if (x + box_w > LCD_H_RES) {
        box_w = LCD_H_RES - x;
    }
    if (y + h > LCD_V_RES) {
        h = LCD_V_RES - y;
    }
    if (box_w <= 0 || h <= 0 || box_w > LCD_H_RES || h > BUF_ROWS) {
        return;
    }

    for (int i = 0; i < box_w * h; i++) {
        s_buf[i] = bg;
    }

    int cx = 0;
    for (const char *p = s; *p && cx < box_w; p++) {
        const uint8_t *g = glyph_for(*p);
        if (g) {
            for (int col = 0; col < 5; col++) {
                for (int row = 0; row < 7; row++) {
                    if (g[col] & (1 << row)) {
                        int px = cx + col * scale;
                        int py = row * scale;
                        for (int dy = 0; dy < scale; dy++) {
                            int Y = py + dy;
                            if (Y >= h) {
                                continue;
                            }
                            uint16_t *line = &s_buf[Y * box_w];
                            for (int dx = 0; dx < scale; dx++) {
                                int X = px + dx;
                                if (X < box_w) {
                                    line[X] = fg;
                                }
                            }
                        }
                    }
                }
            }
        }
        cx += 6 * scale;
    }

    lcd_blit(x, y, x + box_w, y + h);
}

static void fmt_1dec(char *out, size_t n, float v)
{
    int t10 = (int)(v * 10.0f + (v >= 0 ? 0.5f : -0.5f));
    if (t10 < 0) {
        snprintf(out, n, "-%d.%d", (-t10) / 10, (-t10) % 10);
    } else {
        snprintf(out, n, "%d.%d", t10 / 10, t10 % 10);
    }
}

/* ==========================================================================
 * 6. SHT3x 驱动（自己实现，不依赖第三方库）
 * ========================================================================== */
static i2c_master_bus_handle_t  s_i2c_bus  = NULL;
static i2c_master_dev_handle_t  s_sht3x    = NULL;
static uint8_t                  s_sht3x_addr = 0;

/* CRC8: 多项式 0x31，初值 0xFF，不反射 */
static uint8_t sht3x_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static esp_err_t sht3x_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = PIN_I2C_SDA,
        .scl_io_num        = PIN_I2C_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C 总线初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 扫描 0x44 / 0x45 */
    s_sht3x_addr = 0;
    const uint8_t cand[2] = {SHT3X_ADDR_A, SHT3X_ADDR_B};
    for (int i = 0; i < 2; i++) {
        if (i2c_master_probe(s_i2c_bus, cand[i], 100) == ESP_OK) {
            s_sht3x_addr = cand[i];
            break;
        }
    }
    if (s_sht3x_addr == 0) {
        ESP_LOGE(TAG, "没有找到 SHT3x（扫描了 0x44 / 0x45）");
        ESP_LOGE(TAG, "检查: VCC=3.3V  GND  SDA=GPIO%d  SCL=GPIO%d  有没有接反",
                 PIN_I2C_SDA, PIN_I2C_SCL);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "找到 SHT3x，地址 0x%02X", s_sht3x_addr);

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = s_sht3x_addr,
        .scl_speed_hz    = 100000,
    };
    err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_sht3x);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "挂载设备失败: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

static esp_err_t sht3x_read_raw(float *temp, float *humi)
{
    if (!s_sht3x) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 单次测量：高重复性，关闭时钟拉伸 -> 0x2400 */
    const uint8_t cmd[2] = {0x24, 0x00};
    esp_err_t err = i2c_master_transmit(s_sht3x, cmd, 2, 100);
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(20));      /* 高重复性最大 15ms */

    uint8_t buf[6];
    err = i2c_master_receive(s_sht3x, buf, 6, 100);
    if (err != ESP_OK) {
        return err;
    }

    if (sht3x_crc8(&buf[0], 2) != buf[2] || sht3x_crc8(&buf[3], 2) != buf[5]) {
        return ESP_ERR_INVALID_CRC;
    }

    uint16_t raw_t = ((uint16_t)buf[0] << 8) | buf[1];
    uint16_t raw_h = ((uint16_t)buf[3] << 8) | buf[4];

    *temp = -45.0f + 175.0f * (float)raw_t / 65535.0f;
    *humi = 100.0f * (float)raw_h / 65535.0f;
    return ESP_OK;
}

/* ==========================================================================
 * 7. 数据源（带滑动平均）
 * ========================================================================== */
static float s_tbuf[AVG_N];
static float s_hbuf[AVG_N];
static int   s_ai   = 0;
static int   s_acnt = 0;

static void avg_push(float t, float h)
{
    s_tbuf[s_ai] = t;
    s_hbuf[s_ai] = h;
    s_ai = (s_ai + 1) % AVG_N;
    if (s_acnt < AVG_N) {
        s_acnt++;
    }
}

static void avg_get(float *t, float *h)
{
    float st = 0, sh = 0;
    for (int i = 0; i < s_acnt; i++) {
        st += s_tbuf[i];
        sh += s_hbuf[i];
    }
    *t = st / s_acnt;
    *h = sh / s_acnt;
}

/* 返回 true 表示读到有效数据 */
static bool sensor_read(float *temp_c, float *humi_pct)
{
#if USE_FAKE_SENSOR
    static int n = 0;
    n++;
    *temp_c   = 24.0f + (float)(n % 60) * 0.1f;
    *humi_pct = 55.0f + (float)((n * 7) % 30);
    return true;
#else
    float t, h;
    if (sht3x_read_raw(&t, &h) != ESP_OK) {
        return false;
    }
    avg_push(t, h);
    avg_get(temp_c, humi_pct);
    return true;
#endif
}

/* ==========================================================================
 * 8. 屏幕初始化
 * ========================================================================== */
static void lcd_init(void)
{
    s_flush_done = xSemaphoreCreateBinary();
    if (!s_flush_done) {
        ESP_LOGE(TAG, "信号量创建失败");
        abort();
    }

    spi_bus_config_t buscfg = {
        .sclk_io_num     = PIN_SCLK,
        .mosi_io_num     = PIN_MOSI,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * BUF_ROWS * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num         = PIN_DC,
        .cs_gpio_num         = PIN_CS,
        .pclk_hz             = LCD_PCLK_HZ,
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
        .spi_mode            = 0,
        .trans_queue_depth   = 4,
        .on_color_trans_done = on_color_trans_done,
        .user_ctx            = NULL,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                             &io_config, &s_io));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order  = LCD_BGR ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian    = LCD_LITTLE_ENDIAN ? LCD_RGB_DATA_ENDIAN_LITTLE
                                            : LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io, &panel_config, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 0, LCD_GAP_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, LCD_INVERT));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    s_buf = heap_caps_malloc(LCD_H_RES * BUF_ROWS * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!s_buf) {
        ESP_LOGE(TAG, "缓冲区分配失败");
        abort();
    }
}

/* ==========================================================================
 * 9. 界面
 * ========================================================================== */
static char s_status[24] = "";

static void draw_status(const char *msg, uint16_t color)
{
    if (strcmp(msg, s_status) == 0) {
        return;                       /* 内容没变就不重画 */
    }
    strncpy(s_status, msg, sizeof(s_status) - 1);
    s_status[sizeof(s_status) - 1] = '\0';
    draw_text(8, 220, 224, 1, msg, color, C_BLACK);
}

static void draw_static_ui(void)
{
    lcd_fill(0, 0, LCD_H_RES, LCD_V_RES, C_BLACK);

    lcd_fill(0, 0, LCD_H_RES, 2, C_NAVY);
    lcd_fill(0, LCD_V_RES - 2, LCD_H_RES, LCD_V_RES, C_NAVY);
    lcd_fill(0, 0, 2, LCD_V_RES, C_NAVY);
    lcd_fill(LCD_H_RES - 2, 0, LCD_H_RES, LCD_V_RES, C_NAVY);

    draw_text(6, 6, 130, 2, "ESP32 TFT", C_CYAN, C_BLACK);
    lcd_fill(6, 26, 234, 27, C_NAVY);

    draw_text(8, 34, 100, 2, "TEMP C", C_GOLD, C_BLACK);
    draw_text(128, 34, 104, 2, "HUMI %", C_GOLD, C_BLACK);

    lcd_fill(118, 30, 120, 88, C_NAVY);
    lcd_fill(6, 92, 234, 93, C_NAVY);
}

void app_main(void)
{
    lcd_init();
    draw_static_ui();

#if !USE_FAKE_SENSOR
    bool sensor_ok = (sht3x_init() == ESP_OK);
    if (sensor_ok) {
        draw_status("SHT3X READY", C_GREEN);
    } else {
        draw_status("NO SENSOR", C_RED);
    }
#else
    draw_status("FAKE DATA", C_GRAY);
#endif

    int64_t t0 = esp_timer_get_time();
    int     n  = 0;

    while (1) {
        float t = 0, h = 0;
        char  buf[16];
        bool  ok = sensor_read(&t, &h);

        if (ok) {
            fmt_1dec(buf, sizeof(buf), t);
            draw_text(8, 52, 100, 4, buf, C_WHITE, C_BLACK);

            fmt_1dec(buf, sizeof(buf), h);
            draw_text(128, 52, 100, 4, buf, C_WHITE, C_BLACK);
        } else {
            draw_text(8, 52, 100, 4, "--.-", C_GRAY, C_BLACK);
            draw_text(128, 52, 100, 4, "--.-", C_GRAY, C_BLACK);
        }

        int sec = (int)((esp_timer_get_time() - t0) / 1000000);
        snprintf(buf, sizeof(buf), "UP %dS", sec);
        draw_text(130, 6, 100, 2, buf, C_GRAY, C_BLACK);

        if ((n++ % 5) == 0) {
            if (ok) {
                ESP_LOGI(TAG, "T=%.2f C  H=%.2f %%", t, h);
            } else {
                ESP_LOGW(TAG, "读数失败（传感器未接 / 接线有问题）");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
