#include "input.h"

#include "bsp/input.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "hw_settings.h"

// Volume keys step in 5% increments — matches the launcher and the
// volume_howto.md guidance.
#define INPUT_VOLUME_STEP_PERCENT 5

static char const TAG[] = "input";

static QueueHandle_t s_event_queue   = NULL;
static input_mode_t  s_mode          = INPUT_MODE_TITLE;
static bool          s_pickup_edge   = false;
static int           s_speed_delta   = 0;
static int           s_sun_delta     = 0;
static int           s_menu_nav      = 0;     // latest -1/0/+1 menu direction
static bool          s_menu_cancel   = false; // latest ESC press
static bool          s_backspace     = false; // latest BACKSPACE press
static int           s_digit         = -1;    // 0..9 if a digit was typed, else -1
static bool          s_pause_toggle  = false; // latest F4 press edge
static bool          s_force_area    = false; // latest TAB press edge (debug)

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
                    } else if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_F4) {
                        s_pause_toggle = true;
                    } else if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_GAMEPAD_A) {
                        s_pickup_edge = true;
                    } else if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_UP) {
                        s_speed_delta += 1;
                        s_menu_nav     = +1;
                    } else if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
                        s_speed_delta -= 1;
                        s_menu_nav     = -1;
                    } else if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_VOLUME_UP) {
                        // Master volume — read/write goes straight to
                        // the launcher-shared NVS via hw_settings.
                        // No latch + main-loop poll because volume is
                        // system housekeeping, not gameplay input.
                        hw_settings_step_volume(+INPUT_VOLUME_STEP_PERCENT);
                    } else if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN) {
                        hw_settings_step_volume(-INPUT_VOLUME_STEP_PERCENT);
                    }
                }
                break;
            case INPUT_EVENT_TYPE_ACTION:
                if (event.args_action.type == BSP_INPUT_ACTION_TYPE_AUDIO_JACK) {
                    // event.args_action.state == true → jack inserted
                    // → switch the codec to the headphone volume and
                    // mute the speaker amp.
                    hw_settings_on_jack_event(event.args_action.state);
                }
                break;
            case INPUT_EVENT_TYPE_SCANCODE:
                // Space (use-pickup / menu-confirm) and the Q/A debug
                // sun nudge are the queued events we care about
                // mid-game; ENTER also confirms menus; ESC and
                // BACKSPACE are menu-cancel / digit-edit when we're
                // not steering. Steering keys come in through the
                // polled API.
                if (event.args_scancode.scancode == BSP_INPUT_SCANCODE_SPACE) {
                    s_pickup_edge = true;
                } else if (event.args_scancode.scancode == BSP_INPUT_SCANCODE_ENTER) {
                    s_pickup_edge = true;
                } else if (event.args_scancode.scancode == BSP_INPUT_SCANCODE_ESC) {
                    if (s_mode != INPUT_MODE_PLAYING) {
                        s_menu_cancel = true;
                    }
                } else if (event.args_scancode.scancode == BSP_INPUT_SCANCODE_BACKSPACE) {
                    if (s_mode != INPUT_MODE_PLAYING) {
                        s_backspace = true;
                    }
                } else if (event.args_scancode.scancode == BSP_INPUT_SCANCODE_Q) {
                    s_sun_delta += 1;     // push sun toward sunset
                } else if (event.args_scancode.scancode == BSP_INPUT_SCANCODE_A) {
                    s_sun_delta -= 1;     // push sun back toward zenith
                } else if (event.args_scancode.scancode == BSP_INPUT_SCANCODE_TAB) {
                    s_force_area = true;  // debug: force next area type
                }
                break;
            case INPUT_EVENT_TYPE_KEYBOARD:
                if (s_mode == INPUT_MODE_MENU_SEED) {
                    char c = event.args_keyboard.ascii;
                    if (c >= '0' && c <= '9') {
                        s_digit = c - '0';
                    }
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

int input_consume_menu_nav(void) {
    int n      = s_menu_nav;
    s_menu_nav = 0;
    return n;
}

bool input_consume_menu_confirm(void) {
    bool e        = s_pickup_edge;
    s_pickup_edge = false;
    return e;
}

bool input_consume_menu_cancel(void) {
    bool e        = s_menu_cancel;
    s_menu_cancel = false;
    return e;
}

bool input_consume_backspace(void) {
    bool e      = s_backspace;
    s_backspace = false;
    return e;
}

bool input_consume_digit(int* out_digit) {
    if (s_digit < 0) return false;
    if (out_digit) *out_digit = s_digit;
    s_digit = -1;
    return true;
}

bool input_consume_pause_toggle(void) {
    bool e          = s_pause_toggle;
    s_pause_toggle = false;
    return e;
}

bool input_consume_force_next_area(void) {
    bool e        = s_force_area;
    s_force_area = false;
    return e;
}
