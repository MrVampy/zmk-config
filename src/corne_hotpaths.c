/*
 * Typing hot-paths for the user's Corne layout.
 *
 * Stock ZMK combos are intentionally unordered and their prior-idle guard is
 * keycode-timestamp based. This listener adds narrow physical-event rules for
 * cases that need more specific behavior.
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>

#include <dt-bindings/zmk/hid_usage_pages.h>
#include <dt-bindings/zmk/keys.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define HOTPATH_LAYER_BASE 0
#define HOTPATH_E_POSITION 3
#define HOTPATH_R_POSITION 4
#define HOTPATH_S_POSITION 16
#define HOTPATH_I_POSITION 10
#define HOTPATH_O_POSITION 11
#define HOTPATH_INVALID_POSITION UINT32_MAX
#define HOTPATH_LSHIFT_MODIFIER 1

enum guarded_combo {
    GUARDED_COMBO_NONE,
    GUARDED_COMBO_ASTERISK,
    GUARDED_COMBO_EQUAL,
};

static struct zmk_position_state_changed_event pending_left_shift_i_event;
static struct zmk_position_state_changed_event pending_left_shift_i_i_event;
static struct zmk_position_state_changed_event pending_guarded_event;
static struct k_work_delayable pending_guarded_timeout;

static bool physical_position_down[ZMK_KEYMAP_LEN];
static bool suppress_position_release[ZMK_KEYMAP_LEN];
static uint8_t physical_down_count;
static int64_t last_physical_activity = INT32_MIN;

static bool pending_left_shift_i;
static bool pending_left_shift_i_i;
static bool pending_guarded;
static uint32_t pending_left_shift_i_position;
static uint32_t pending_guarded_position;
static bool pending_guarded_allows_asterisk;
static bool pending_guarded_allows_equal;
static uint32_t suppress_left_shift_i_release_position = HOTPATH_INVALID_POSITION;

static bool base_layer_active(void) {
    return zmk_keymap_highest_layer_active() == HOTPATH_LAYER_BASE;
}

static bool position_in_keymap(uint32_t position) { return position < ZMK_KEYMAP_LEN; }

static bool is_left_shift_i_hotpath_position(uint32_t position) {
    return position == HOTPATH_S_POSITION;
}

static bool physical_idle_before_event(const struct zmk_position_state_changed *ev) {
    return physical_down_count == 0 &&
           (ev->timestamp - last_physical_activity) >= CONFIG_ZMK_CORNE_HOTPATH_COMBO_IDLE_MS;
}

static void record_physical_activity(const struct zmk_position_state_changed *ev) {
    if (position_in_keymap(ev->position)) {
        bool was_down = physical_position_down[ev->position];

        if (ev->state && !was_down) {
            physical_position_down[ev->position] = true;
            physical_down_count++;
        } else if (!ev->state && was_down) {
            physical_position_down[ev->position] = false;
            physical_down_count--;
        }
    }

    last_physical_activity = ev->timestamp;
}

static int release_pending_left_shift_i(void) {
    if (!pending_left_shift_i) {
        return 0;
    }

    struct zmk_position_state_changed_event ev = pending_left_shift_i_event;

    pending_left_shift_i = false;

    return ZMK_EVENT_RELEASE(ev);
}

static int release_pending_left_shift_i_roll(void) {
    int ret = release_pending_left_shift_i();

    if (!pending_left_shift_i_i) {
        return ret;
    }

    struct zmk_position_state_changed_event ev = pending_left_shift_i_i_event;

    pending_left_shift_i_i = false;

    int i_ret = ZMK_EVENT_RELEASE(ev);
    return ret < 0 ? ret : i_ret;
}

static int keep_first_error(int first_error, int ret) {
    return first_error < 0 ? first_error : (ret < 0 ? ret : first_error);
}

static int send_keyboard_report(void) {
    int ret = zmk_endpoints_send_report(HID_USAGE_KEY);

    if (ret < 0) {
        LOG_ERR("Failed to send keyboard report (%d)", ret);
    }

    return ret;
}

static int tap_lshift_key(uint32_t key_usage) {
    zmk_key_t key = ZMK_HID_USAGE_ID(key_usage);
    int first_error = 0;
    bool mod_registered = false;
    bool key_pressed = false;

    // Avoid ZMK's implicit-modifier path for these synthetic hotpath taps.
    // The implicit path stores one global modifier bit and clears it only on
    // the paired key-release event; if that release is ever captured/reordered,
    // Shift can appear stuck. Explicit modifiers are refcounted, so the cleanup
    // below is local to this synthetic tap even if physical Shift is also held.
    int ret = zmk_hid_register_mod(HOTPATH_LSHIFT_MODIFIER);
    if (ret < 0) {
        return ret;
    }
    mod_registered = true;
    first_error = keep_first_error(first_error, send_keyboard_report());

    ret = zmk_hid_keyboard_press(key);
    if (ret < 0) {
        first_error = keep_first_error(first_error, ret);
    } else {
        key_pressed = true;
        first_error = keep_first_error(first_error, send_keyboard_report());
    }

    if (key_pressed) {
        first_error = keep_first_error(first_error, zmk_hid_keyboard_release(key));
        first_error = keep_first_error(first_error, send_keyboard_report());
    }

    if (mod_registered) {
        first_error =
            keep_first_error(first_error, zmk_hid_unregister_mod(HOTPATH_LSHIFT_MODIFIER));
        first_error = keep_first_error(first_error, send_keyboard_report());
    }

    return first_error;
}

static int tap_guarded_combo(enum guarded_combo combo, int64_t timestamp) {
    ARG_UNUSED(timestamp);

    switch (combo) {
    case GUARDED_COMBO_ASTERISK:
        // Danish-layout asterisk is DA_ASTRK in the keymap, equivalent to Shift+BSLH.
        return tap_lshift_key(BSLH);
    case GUARDED_COMBO_EQUAL:
        // Danish-layout equals is DA_EQUAL in the keymap, equivalent to Shift+0.
        return tap_lshift_key(N0);
    default:
        return -EINVAL;
    }
}

static int release_pending_guarded_combo(void) {
    if (!pending_guarded) {
        return 0;
    }

    struct zmk_position_state_changed_event ev = pending_guarded_event;

    pending_guarded = false;
    k_work_cancel_delayable(&pending_guarded_timeout);

    return ZMK_EVENT_RELEASE(ev);
}

static void pending_guarded_timeout_handler(struct k_work *work) {
    ARG_UNUSED(work);

    release_pending_guarded_combo();
}

static bool is_guarded_combo_position(uint32_t position) {
    return position == HOTPATH_E_POSITION || position == HOTPATH_R_POSITION ||
           position == HOTPATH_I_POSITION || position == HOTPATH_O_POSITION;
}

static void start_guarded_combo(const struct zmk_position_state_changed *ev) {
    pending_guarded_event = copy_raised_zmk_position_state_changed(ev);
    pending_guarded = true;
    pending_guarded_position = ev->position;
    pending_guarded_allows_asterisk =
        ev->position == HOTPATH_E_POSITION || ev->position == HOTPATH_R_POSITION;
    pending_guarded_allows_equal =
        ev->position == HOTPATH_I_POSITION || ev->position == HOTPATH_O_POSITION;

    k_work_reschedule(&pending_guarded_timeout, K_MSEC(CONFIG_ZMK_CORNE_HOTPATH_COMBO_TIMEOUT_MS));
}

static enum guarded_combo completed_guarded_combo(uint32_t second_position) {
    if (pending_guarded_allows_asterisk && ((pending_guarded_position == HOTPATH_E_POSITION &&
                                             second_position == HOTPATH_R_POSITION) ||
                                            (pending_guarded_position == HOTPATH_R_POSITION &&
                                             second_position == HOTPATH_E_POSITION))) {
        return GUARDED_COMBO_ASTERISK;
    }

    if (pending_guarded_allows_equal && ((pending_guarded_position == HOTPATH_I_POSITION &&
                                          second_position == HOTPATH_O_POSITION) ||
                                         (pending_guarded_position == HOTPATH_O_POSITION &&
                                          second_position == HOTPATH_I_POSITION))) {
        return GUARDED_COMBO_EQUAL;
    }

    return GUARDED_COMBO_NONE;
}

// Keep the S->I window synchronous. A delayed work timeout can race with
// the I-release success path and forward the HRM press while its release is
// still suppressed, which looks like a temporarily stuck Shift.
static bool pending_left_shift_i_expired(const struct zmk_position_state_changed *ev) {
    return (ev->timestamp - pending_left_shift_i_event.data.timestamp) >
           CONFIG_ZMK_CORNE_HOTPATH_SI_TIMEOUT_MS;
}

static int corne_hotpaths_listener(const zmk_event_t *eh) {
    struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    bool may_start_guarded_combo =
        ev->state && base_layer_active() && physical_idle_before_event(ev);

    record_physical_activity(ev);

    if (!ev->state && position_in_keymap(ev->position) && suppress_position_release[ev->position]) {
        suppress_position_release[ev->position] = false;
        return ZMK_EV_EVENT_HANDLED;
    }

    if (!ev->state && ev->position == suppress_left_shift_i_release_position) {
        suppress_left_shift_i_release_position = HOTPATH_INVALID_POSITION;
        return ZMK_EV_EVENT_HANDLED;
    }

    if (pending_left_shift_i) {
        if (!pending_left_shift_i_i && pending_left_shift_i_expired(ev)) {
            release_pending_left_shift_i();
            return ZMK_EV_EVENT_BUBBLE;
        }

        if (pending_left_shift_i_i) {
            if (!ev->state && ev->position == HOTPATH_I_POSITION) {
                pending_left_shift_i = false;
                pending_left_shift_i_i = false;

                suppress_left_shift_i_release_position = pending_left_shift_i_position;

                int ret = tap_lshift_key(I);
                return ret < 0 ? ret : ZMK_EV_EVENT_HANDLED;
            }

            if (!ev->state && ev->position == pending_left_shift_i_position) {
                release_pending_left_shift_i_roll();
                return ZMK_EV_EVENT_BUBBLE;
            }

            if (ev->state) {
                release_pending_left_shift_i_roll();
                return ZMK_EV_EVENT_BUBBLE;
            }
        }

        if (!ev->state && ev->position == pending_left_shift_i_position) {
            release_pending_left_shift_i();
            return ZMK_EV_EVENT_BUBBLE;
        }

        if (ev->state && ev->position == HOTPATH_I_POSITION && base_layer_active()) {
            pending_left_shift_i_i_event = copy_raised_zmk_position_state_changed(ev);
            pending_left_shift_i_i = true;
            return ZMK_EV_EVENT_CAPTURED;
        }

        if (ev->state) {
            release_pending_left_shift_i();
            return ZMK_EV_EVENT_BUBBLE;
        }
    }

    if (pending_guarded) {
        if (!ev->state && ev->position == pending_guarded_position) {
            release_pending_guarded_combo();
            return ZMK_EV_EVENT_BUBBLE;
        }

        if (ev->state && base_layer_active()) {
            enum guarded_combo combo = completed_guarded_combo(ev->position);

            if (combo != GUARDED_COMBO_NONE) {
                pending_guarded = false;
                k_work_cancel_delayable(&pending_guarded_timeout);

                suppress_position_release[pending_guarded_position] = true;
                if (position_in_keymap(ev->position)) {
                    suppress_position_release[ev->position] = true;
                }

                int ret = tap_guarded_combo(combo, ev->timestamp);
                return ret < 0 ? ret : ZMK_EV_EVENT_CAPTURED;
            }
        }

        if (ev->state) {
            release_pending_guarded_combo();
            return ZMK_EV_EVENT_BUBBLE;
        }
    }

    if (may_start_guarded_combo && is_guarded_combo_position(ev->position)) {
        start_guarded_combo(ev);
        return ZMK_EV_EVENT_CAPTURED;
    }

    if (ev->state && is_left_shift_i_hotpath_position(ev->position) && base_layer_active()) {
        pending_left_shift_i_event = copy_raised_zmk_position_state_changed(ev);
        pending_left_shift_i = true;
        pending_left_shift_i_position = ev->position;

        return ZMK_EV_EVENT_CAPTURED;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(corne_hotpaths, corne_hotpaths_listener);
ZMK_SUBSCRIPTION(corne_hotpaths, zmk_position_state_changed);

static int corne_hotpaths_init(void) {
    k_work_init_delayable(&pending_guarded_timeout, pending_guarded_timeout_handler);
    return 0;
}

SYS_INIT(corne_hotpaths_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
