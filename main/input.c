#include "input.h"

#include "bsp/input.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static char const TAG[] = "input";

static QueueHandle_t s_event_queue = NULL;
static input_mode_t  s_mode        = INPUT_MODE_TITLE;
static bool          s_pickup_edge = false;
static int           s_speed_delta = 0;
static int           s_sun_delta   = 0;

void input_init(void) {
    esp_err_t res = bsp_input_get_queue(&s_event_queue);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "bsp_input_get_queue failed: %d", res);
        s_event_queue = NULL;
    }
}

void input_set_mode(input_mode_t mode) {
    s_mode = mode;
}

bool input_drain_events(void) {
    bool exit_requested = false;
    if (s_event_queue == NULL) return false;

    bsp_input_event_t event;
    while (xQueueReceive(s_event_queue, &event, 0) == pdTRUE) {
        switch (event.type) {
            case INPUT_EVENT_TYPE_NAVIGATION:
                if (event.args_navigation.state) {
                    if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_F1) {
                        exit_requested = true;
                    } else if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_GAMEPAD_A) {
                        s_pickup_edge = true;
                    } else if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_UP) {
                        s_speed_delta += 1;
                    } else if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
                        s_speed_delta -= 1;
                    }
                }
                break;
            case INPUT_EVENT_TYPE_SCANCODE:
                // Space (use-pickup) and the Q/A debug sun nudge are
                // the queued events we care about mid-game. Steering
                // keys come in through the polled API.
                if (event.args_scancode.scancode == BSP_INPUT_SCANCODE_SPACE) {
                    s_pickup_edge = true;
                } else if (event.args_scancode.scancode == BSP_INPUT_SCANCODE_Q) {
                    s_sun_delta += 1;     // push sun toward sunset
                } else if (event.args_scancode.scancode == BSP_INPUT_SCANCODE_A) {
                    s_sun_delta -= 1;     // push sun back toward zenith
                }
                break;
            default:
                break;
        }
    }

    return exit_requested;
}

static bool poll_nav(bsp_input_navigation_key_t key) {
    bool      held = false;
    esp_err_t res  = bsp_input_read_navigation_key(key, &held);
    return (res == ESP_OK) && held;
}

static bool poll_scancode(bsp_input_scancode_t code) {
    bool      held = false;
    esp_err_t res  = bsp_input_read_scancode(code, &held);
    return (res == ESP_OK) && held;
}

int input_steering(void) {
    bool left  = poll_nav(BSP_INPUT_NAVIGATION_KEY_LEFT);
    bool right = poll_nav(BSP_INPUT_NAVIGATION_KEY_RIGHT);

    // Modal: ESC and Backspace only steer during PLAYING.
    if (s_mode == INPUT_MODE_PLAYING) {
        left  = left  || poll_scancode(BSP_INPUT_SCANCODE_ESC);
        right = right || poll_scancode(BSP_INPUT_SCANCODE_BACKSPACE);
    }

    return (right ? 1 : 0) - (left ? 1 : 0);
}

bool input_consume_pickup(void) {
    bool e        = s_pickup_edge;
    s_pickup_edge = false;
    return e;
}

int input_consume_speed_delta(void) {
    int d         = s_speed_delta;
    s_speed_delta = 0;
    return d;
}

int input_consume_sun_delta(void) {
    int d       = s_sun_delta;
    s_sun_delta = 0;
    return d;
}
