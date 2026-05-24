#include "input.h"

#include <math.h>

#include "bsp/input.h"
#include "controls_settings.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "graceloader_imu.h"
#include "se_bindings.h"     // se_bindings_get (remappable keybinds)

static char const TAG[] = "input";

static input_mode_t  s_mode          = INPUT_MODE_TITLE;
static bool          s_pickup_edge   = false;
static int           s_speed_delta   = 0;
static int           s_sun_delta     = 0;
static int           s_menu_nav      = 0;     // latest -1/0/+1 menu direction
static int           s_menu_horiz    = 0;     // latest -1/0/+1 menu LEFT/RIGHT
static bool          s_menu_cancel   = false; // latest ESC press
static bool          s_backspace     = false; // latest BACKSPACE press
static int           s_digit         = -1;    // 0..9 if a digit was typed, else -1
static bool          s_pause_toggle  = false; // latest pause-key press edge
static bool          s_force_area    = false; // latest TAB press edge (debug)
static bool          s_godmode_edge  = false; // latest G press edge (debug)

// (Key-rebind capture is the engine's now — se_ui_capture_key blocks and
// drains the queue itself, so input.c no longer has a capture mode.)

void input_init(void) {
    // The engine (se_run) owns the BSP input queue and pumps it each
    // frame, forwarding the events it doesn't consume to our
    // input_handle_event() below. We only bring up the accelerometer
    // here for optional tilt ("gyroscope") steering.
    //
    // Bring up the accelerometer for optional tilt ("gyroscope")
    // steering. The BMI270 is already initialized by
    // bsp_device_initialize() (called in main()); we only enable the
    // accel sensor here. Leaving it on for the whole app lifetime is
    // fine — power draw is negligible. Failure is non-fatal: tilt
    // steering just won't produce any input.
    esp_err_t imu = bsp_orientation_enable_accelerometer();
    if (imu != ESP_OK) {
        ESP_LOGW(TAG, "accelerometer enable failed: %d (tilt steering off)", imu);
    }
}

void input_set_mode(input_mode_t mode) {
    s_mode = mode;
}

void input_handle_event(bsp_input_event_t const* ev) {
    if (ev == NULL) return;
    bsp_input_event_t const event = *ev;

    switch (event.type) {
        case INPUT_EVENT_TYPE_NAVIGATION:
            if (event.args_navigation.state) {
                if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_GAMEPAD_A) {
                    s_pickup_edge = true;
                } else if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_UP) {
                    s_speed_delta += 1;
                    s_menu_nav     = +1;
                } else if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
                    s_speed_delta -= 1;
                    s_menu_nav     = -1;
                } else if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_LEFT) {
                    s_menu_horiz   = -1;   // menu slider down (consumed only in menus)
                } else if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_RIGHT) {
                    s_menu_horiz   = +1;   // menu slider up (consumed only in menus)
                }
                // The D-pad LEFT/RIGHT edges above feed only the menu
                // sliders; in-game steering reads LEFT/RIGHT through the
                // polled path (input_steer_held), not this latch.
                // F1-exit and the volume keys are consumed by the engine
                // (se_run's input pump) and never arrive here.
            }
            break;
        case INPUT_EVENT_TYPE_SCANCODE: {
            uint16_t const sc = event.args_scancode.scancode;
            // Space (use-pickup / menu-confirm) and the Q/A debug sun
            // nudge are the queued events we care about mid-game; ENTER
            // also confirms menus; ESC and BACKSPACE are menu-cancel /
            // digit-edit when we're not steering. Steering keys come in
            // through the polled API.
            if (sc == BSP_INPUT_SCANCODE_SPACE) {
                s_pickup_edge = true;
            } else if (sc == BSP_INPUT_SCANCODE_ENTER) {
                s_pickup_edge = true;
            } else if (sc == BSP_INPUT_SCANCODE_ESC) {
                if (s_mode != INPUT_MODE_PLAYING) {
                    s_menu_cancel = true;
                }
            } else if (sc == BSP_INPUT_SCANCODE_BACKSPACE) {
                if (s_mode != INPUT_MODE_PLAYING) {
                    s_backspace = true;
                }
            } else if (sc == BSP_INPUT_SCANCODE_Q) {
                s_sun_delta += 1;     // push sun toward sunset
            } else if (sc == BSP_INPUT_SCANCODE_A) {
                s_sun_delta -= 1;     // push sun back toward zenith
            } else if (sc == BSP_INPUT_SCANCODE_TAB) {
                s_force_area = true;  // debug: force next area type
            } else if (sc == BSP_INPUT_SCANCODE_G) {
                s_godmode_edge = true;  // debug: toggle godmode
            }
            // Pause is a remappable bind (default F4) — checked
            // independently of the chain above so a player who binds
            // pause onto an already-meaningful key still gets both
            // behaviours.
            if (sc == se_bindings_get(CONTROL_KEY_PAUSE)) {
                s_pause_toggle = true;
            }
            break;
        }
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

// --- Tilt ("gyroscope") steering -----------------------------------
// The Controls menu's "Gyroscope" toggle enables motion steering.
// Despite the menu label the signal comes from the *accelerometer*,
// not the rate gyroscope: both control styles the player expects —
// holding the device upright and rolling it like a steering wheel,
// or holding it flat and tipping it like a marble game — are
// absolute-tilt gestures. Gravity gives a drift-free absolute tilt
// reference; a rate gyro would need integration and would drift.
// Conveniently the device's +Y axis (right edge → left edge)
// captures the lateral gravity component in *both* poses, so one
// signal — accel_y — drives both styles:
//   * flat,    tipped right-edge-down → left edge rises → accel_y > 0
//   * upright, rolled wheel-right     → left edge rises → accel_y > 0
// so accel_y > 0 always means "steer right".
#define GYRO_DEADZONE_ACCEL   0.7f    // m/s² (~4° tilt) — ignore hand jitter
#define GYRO_FULL_TILT_ACCEL  4.5f    // m/s² (~27° tilt) — full lock
#define GYRO_FILTER_ALPHA     0.35f   // per-frame low-pass on accel_y

static float s_accel_y_filt = 0.0f;
static bool  s_accel_seen   = false;

// Returns tilt steering in [-1, +1] (+ = right), or 0 when the
// gyro toggle is off or no accelerometer sample is available.
static float gyro_steering(void) {
    if (!controls_settings_gyro_on()) return 0.0f;

    bool      a_ready = false;
    float     ay      = 0.0f;
    esp_err_t res     = bsp_orientation_get(NULL, &a_ready, NULL, NULL, NULL,
                                            NULL, &ay, NULL);
    if (res == ESP_OK && a_ready) {
        if (!s_accel_seen) {
            s_accel_y_filt = ay;
            s_accel_seen   = true;
        } else {
            s_accel_y_filt += (ay - s_accel_y_filt) * GYRO_FILTER_ALPHA;
        }
    }

    float const a   = s_accel_y_filt;
    float const mag = fabsf(a);
    if (mag <= GYRO_DEADZONE_ACCEL) return 0.0f;

    float v = (mag - GYRO_DEADZONE_ACCEL)
            / (GYRO_FULL_TILT_ACCEL - GYRO_DEADZONE_ACCEL);
    if (v > 1.0f) v = 1.0f;
    return (a < 0.0f) ? -v : v;
}

void input_steer_held(bool* out_left, bool* out_right) {
    bool left  = poll_nav(BSP_INPUT_NAVIGATION_KEY_LEFT);
    bool right = poll_nav(BSP_INPUT_NAVIGATION_KEY_RIGHT);

    // Modal: the configurable steering keys (default ESC / Backspace)
    // only steer during PLAYING — elsewhere those scancodes keep
    // their conventional menu-cancel / edit roles. The D-pad
    // LEFT/RIGHT above always steers and is not remappable.
    if (s_mode == INPUT_MODE_PLAYING) {
        left  = left  || poll_scancode(
                    (bsp_input_scancode_t)se_bindings_get(CONTROL_KEY_LEFT));
        right = right || poll_scancode(
                    (bsp_input_scancode_t)se_bindings_get(CONTROL_KEY_RIGHT));
    }
    *out_left  = left;
    *out_right = right;
}

float input_steering(void) {
    bool left, right;
    input_steer_held(&left, &right);

    int const key_steer = (right ? 1 : 0) - (left ? 1 : 0);

    // A held key locks to full deflection and overrides tilt input —
    // digital steering always wins over the gyro. With no key held
    // the gyro provides proportional steering (0 when its toggle is
    // off, so key-only players are unaffected).
    if (key_steer != 0) return (float)key_steer;
    return gyro_steering();
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

int input_consume_menu_horiz(void) {
    int h        = s_menu_horiz;
    s_menu_horiz = 0;
    return h;
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

bool input_consume_godmode_toggle(void) {
    bool e          = s_godmode_edge;
    s_godmode_edge = false;
    return e;
}
