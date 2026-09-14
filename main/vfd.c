// =====================================================================
//  Race the Synth  --  optional external VFD status display (see vfd.h)
// ---------------------------------------------------------------------
//  NE-HCS12SS59T-R1 register map (8-bit registers):
//    0       system control (bit 0 = enable, bit 1 = test, bit 2 = LED)
//    1       display offset into the text buffer
//    2       scroll length (maximum offset while auto-scrolling)
//    3       scroll mode (bits 3-0: 0 off / 1 left / 2 right; bit 4 loop)
//    4-5     scroll speed, ms per step (little-endian 16 bit)
//    6       filament current (default 110)
//    10-255  ASCII text buffer
// =====================================================================

#include "vfd.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bsp/catt.h"
#include "bsp/i2c.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static char const TAG[] = "vfd";

#define VFD_ADDRESS     0x13    // 7-bit address (default 0x10 + jumpers 1 and 2)
#define VFD_SPEED_HZ    100000
#define VFD_CHARS       12      // visible characters
#define VFD_TIMEOUT_MS  50      // per-transfer timeout
#define VFD_PROBE_MS    20      // per-bus probe timeout

#define VFD_REG_CONTROL      0
#define VFD_REG_OFFSET       1
#define VFD_REG_SCROLL_LEN   2
#define VFD_REG_SCROLL_MODE  3
#define VFD_REG_SCROLL_SPEED 4
#define VFD_REG_BRIGHTNESS   6
#define VFD_REG_TEXT         10

#define VFD_CTRL_ENABLE      (1 << 0)
#define VFD_SCROLL_LEFT      1
#define VFD_SCROLL_LOOP      (1 << 4)

// Filament current. DON'T GO MUCH HIGHER: it drastically shortens the
// tube's life (same value as the vfdclock app and the device default).
#define VFD_BRIGHTNESS       110

#define VFD_IDLE_TEXT        "RACE THE SYNTH BY CAVAC"
#define VFD_SCROLL_STEP_MS   200
#define VFD_PAUSE_BLINK_MS   500   // on/off half period of the PAUSED blink
#define VFD_POLL_MS          50    // task wake-up interval

#define VFD_TASK_STACK_BYTES (6 * 1024)  // ESP-IDF stack sizes are in bytes
#define VFD_TASK_PRIORITY    (tskIDLE_PRIORITY + 1)
#define VFD_TASK_CORE        1           // off the render loop's core 0

#define VFD_WANT(mode, stage) (((uint32_t)(mode) << 24) | ((uint32_t)(stage) & 0xFFFFFFu))

typedef struct {
    char const*             name;
    i2c_master_bus_handle_t handle;
    bool                    shared;   // internal bus: shared with the coprocessor, needs claim/release
} vfd_bus_t;

static vfd_bus_t s_buses[2] = {
    {.name = "CATT I2C", .shared = false},
    {.name = "Internal I2C", .shared = true},
};

static vfd_bus_t*              s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

// Game -> task hand-off. Single-word stores, so no lock is needed.
static volatile uint32_t s_want         = VFD_WANT(VFD_MODE_IDLE, 0);
static volatile bool     s_exit_request = false;
static volatile bool     s_task_done    = true;   // true until the task runs, and once it has finished

static void bus_lock(vfd_bus_t const* bus) {
    if (bus->shared) bsp_i2c_primary_bus_claim();
}

static void bus_unlock(vfd_bus_t const* bus) {
    if (bus->shared) bsp_i2c_primary_bus_release();
}

static esp_err_t vfd_write(uint8_t const* data, size_t len) {
    bus_lock(s_bus);
    esp_err_t const res = i2c_master_transmit(s_dev, data, len, VFD_TIMEOUT_MS);
    bus_unlock(s_bus);
    return res;
}

static esp_err_t vfd_write_reg(uint8_t reg, uint8_t value) {
    uint8_t const data[2] = {reg, value};
    return vfd_write(data, sizeof(data));
}

// Write `text` at the start of the text buffer, space-padded to at least
// `min_len` characters.
static esp_err_t vfd_write_text(char const* text, size_t min_len) {
    uint8_t buf[1 + 64];
    size_t  len = strlen(text);
    if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
    if (min_len > sizeof(buf) - 1) min_len = sizeof(buf) - 1;
    size_t const total = (len > min_len) ? len : min_len;

    buf[0] = VFD_REG_TEXT;
    memcpy(&buf[1], text, len);
    memset(&buf[1 + len], ' ', total - len);
    return vfd_write(buf, 1 + total);
}

// Static text, centred on the 12 visible characters.
static esp_err_t vfd_show_static(char const* text) {
    char   line[VFD_CHARS + 1];
    size_t len = strlen(text);
    if (len > VFD_CHARS) len = VFD_CHARS;
    size_t const lead = (VFD_CHARS - len) / 2;
    memset(line, ' ', lead);
    memcpy(&line[lead], text, len);
    line[lead + len] = '\0';

    esp_err_t res = vfd_write_reg(VFD_REG_SCROLL_MODE, 0);  // stop scrolling first so the offset holds
    if (res == ESP_OK) res = vfd_write_reg(VFD_REG_OFFSET, 0);
    if (res == ESP_OK) res = vfd_write_text(line, VFD_CHARS);
    return res;
}

// The idle title, scrolled in from the right by the display itself. The
// text is framed by a blank screen width on both sides; the loop wraps
// after the last character leaves on the left, landing on the leading
// blank screen, so the marquee repeats seamlessly.
static esp_err_t vfd_show_scroller(void) {
    char text[2 * VFD_CHARS + sizeof(VFD_IDLE_TEXT)];
    snprintf(text, sizeof(text), "%*s%s", VFD_CHARS, "", VFD_IDLE_TEXT);
    size_t const  len        = strlen(text);
    uint8_t const scroll_len = (uint8_t)(len - 1);  // last offset shows the final character alone

    uint8_t const speed[3] = {VFD_REG_SCROLL_SPEED, VFD_SCROLL_STEP_MS & 0xFF, (VFD_SCROLL_STEP_MS >> 8) & 0xFF};

    esp_err_t res = vfd_write_reg(VFD_REG_SCROLL_MODE, 0);
    if (res == ESP_OK) res = vfd_write_reg(VFD_REG_OFFSET, 0);
    if (res == ESP_OK) res = vfd_write_text(text, len + VFD_CHARS);  // trailing blank screen width
    if (res == ESP_OK) res = vfd_write_reg(VFD_REG_SCROLL_LEN, scroll_len);
    if (res == ESP_OK) res = vfd_write(speed, sizeof(speed));
    if (res == ESP_OK) res = vfd_write_reg(VFD_REG_SCROLL_MODE, VFD_SCROLL_LEFT | VFD_SCROLL_LOOP);
    return res;
}

static esp_err_t vfd_show_stage(int stage) {
    char text[24];  // vfd_show_static clips to the visible width
    snprintf(text, sizeof(text), "STAGE %d", stage);
    return vfd_show_static(text);
}

// Look for the VFD on the CATT bus, then the internal bus, and attach to
// the first bus it answers on.
static bool vfd_probe(void) {
    // CATT bus: bsp_device_initialize() leaves it down if an add-on held the lines, so retry once
    if (bsp_catt_i2c_bus_get_handle(&s_buses[0].handle) != ESP_OK) {
        s_buses[0].handle = NULL;
        if (bsp_catt_set_i2c_enabled(true) != ESP_OK ||
            bsp_catt_i2c_bus_get_handle(&s_buses[0].handle) != ESP_OK) {
            s_buses[0].handle = NULL;
        }
    }
    if (bsp_i2c_primary_bus_get_handle(&s_buses[1].handle) != ESP_OK) {
        s_buses[1].handle = NULL;
    }

    for (size_t i = 0; i < sizeof(s_buses) / sizeof(s_buses[0]); i++) {
        vfd_bus_t* const bus = &s_buses[i];
        if (bus->handle == NULL) continue;
        bus_lock(bus);
        esp_err_t const res = i2c_master_probe(bus->handle, VFD_ADDRESS, VFD_PROBE_MS);
        bus_unlock(bus);
        if (res == ESP_OK) {
            s_bus = bus;
            break;
        }
    }
    if (s_bus == NULL) {
        ESP_LOGI(TAG, "no VFD at 0x%02X", VFD_ADDRESS);
        return false;
    }

    i2c_device_config_t const dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = VFD_ADDRESS,
        .scl_speed_hz    = VFD_SPEED_HZ,
    };
    esp_err_t const res = i2c_master_bus_add_device(s_bus->handle, &dev_cfg, &s_dev);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "failed to add VFD device: %s", esp_err_to_name(res));
        s_dev = NULL;
        return false;
    }
    ESP_LOGI(TAG, "VFD at 0x%02X on %s", VFD_ADDRESS, s_bus->name);
    return true;
}

static void vfd_task(void* arg) {
    (void)arg;

    if (vfd_probe()) {
        // A previous app may have left the display off, or scrolling.
        vfd_write_reg(VFD_REG_SCROLL_MODE, 0);
        vfd_write_reg(VFD_REG_BRIGHTNESS, VFD_BRIGHTNESS);
        vfd_write_reg(VFD_REG_CONTROL, VFD_CTRL_ENABLE);

        uint32_t   shown      = UINT32_MAX;  // forces the first update
        TickType_t blink_from = 0;
        bool       blink_on   = false;
        bool       write_ok   = true;

        while (!s_exit_request) {
            uint32_t const   want = s_want;
            vfd_mode_t const mode = (vfd_mode_t)(want >> 24);
            esp_err_t        res  = ESP_OK;
            bool             wrote = false;

            if (want != shown) {
                switch (mode) {
                    case VFD_MODE_STAGE:  res = vfd_show_stage((int)(want & 0xFFFFFFu)); break;
                    case VFD_MODE_PAUSED: res = vfd_show_static("PAUSED");               break;
                    case VFD_MODE_IDLE:
                    default:              res = vfd_show_scroller();                     break;
                }
                // Retry on the next poll if the write didn't make it.
                shown      = (res == ESP_OK) ? want : UINT32_MAX;
                blink_from = xTaskGetTickCount();
                blink_on   = true;
                wrote      = true;
            } else if (mode == VFD_MODE_PAUSED) {
                TickType_t const elapsed = xTaskGetTickCount() - blink_from;
                bool const       on      = ((elapsed / pdMS_TO_TICKS(VFD_PAUSE_BLINK_MS)) % 2) == 0;
                if (on != blink_on) {
                    res      = vfd_write_text(on ? "   PAUSED   " : "", VFD_CHARS);
                    blink_on = on;
                    wrote    = true;
                }
            }

            if (wrote && (res == ESP_OK) != write_ok) {
                write_ok = (res == ESP_OK);
                if (write_ok) ESP_LOGI(TAG, "VFD writes recovered");
                else          ESP_LOGW(TAG, "VFD write failed: %s", esp_err_to_name(res));
            }
            vTaskDelay(pdMS_TO_TICKS(VFD_POLL_MS));
        }

        // Leave the tube dark: blank the text, stop scrolling, switch off.
        vfd_write_reg(VFD_REG_SCROLL_MODE, 0);
        vfd_write_reg(VFD_REG_OFFSET, 0);
        vfd_write_text("", VFD_CHARS);
        vfd_write_reg(VFD_REG_CONTROL, 0);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }

    s_task_done = true;
    vTaskDelete(NULL);
}

void vfd_init(void) {
    s_exit_request = false;
    s_task_done    = false;
    BaseType_t const ok = xTaskCreatePinnedToCore(vfd_task, "vfd", VFD_TASK_STACK_BYTES, NULL,
                                                  VFD_TASK_PRIORITY, NULL, VFD_TASK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create VFD task");
        s_task_done = true;
    }
}

void vfd_set_mode(vfd_mode_t mode, int stage) {
    if (mode != VFD_MODE_STAGE) stage = 0;
    s_want = VFD_WANT(mode, stage < 0 ? 0 : stage);
}

void vfd_shutdown(void) {
    s_exit_request = true;
    // The task finishes its current write (at most a few bus timeouts),
    // then blanks and powers the display down. Bound the wait so a hung
    // bus can never keep the app from exiting.
    for (int i = 0; i < 100 && !s_task_done; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
