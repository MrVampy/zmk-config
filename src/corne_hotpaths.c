/*
 * Typing hot-paths for the user's Corne layout.
 *
 * Stock ZMK combos are intentionally unordered and their prior-idle guard is
 * keycode-timestamp based. This listener adds narrow physical-event rules for
 * cases that need more specific behavior.
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>

#include <dt-bindings/zmk/keys.h>
#include <dt-bindings/zmk/modifiers.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define HOTPATH_LAYER_BASE 0
#define HOTPATH_E_POSITION 3
#define HOTPATH_R_POSITION 4
#define HOTPATH_T_POSITION 5
#define HOTPATH_S_POSITION 16
#define HOTPATH_I_POSITION 10
#define HOTPATH_O_POSITION 11
#define KEY_PRESS DEVICE_DT_NAME(DT_INST(0, zmk_behavior_key_press))
#define CAPS_WORD DEVICE_DT_NAME(DT_NODELABEL(caps_word))

enum guarded_combo {
    GUARDED_COMBO_NONE,
    GUARDED_COMBO_ASTERISK,
    GUARDED_COMBO_CAPS_WORD,
    GUARDED_COMBO_EQUAL,
};

static struct zmk_position_state_changed_event pending_s_event;
static struct zmk_position_state_changed_event pending_si_i_event;
static struct zmk_position_state_changed_event pending_guarded_event;
static struct k_work_delayable pending_s_timeout;
static struct k_work_delayable pending_guarded_timeout;

static bool physical_position_down[ZMK_KEYMAP_LEN];
static bool suppress_position_release[ZMK_KEYMAP_LEN];
static uint8_t physical_down_count;
static int64_t last_physical_activity = INT32_MIN;

static bool pending_s;
static bool pending_si_i;
static bool pending_guarded;
static uint32_t pending_guarded_position;
static bool pending_guarded_allows_asterisk;
static bool pending_guarded_allows_caps_word;
static bool pending_guarded_allows_equal;
static bool suppress_s_release;

static const struct zmk_behavior_binding shifted_i = {
    .behavior_dev = KEY_PRESS,
    .param1 = LS(I),
};

// Danish-layout asterisk is DA_ASTRK in the keymap, equivalent to Shift+BSLH.
static const struct zmk_behavior_binding asterisk = {
    .behavior_dev = KEY_PRESS,
    .param1 = LS(BSLH),
};

static const struct zmk_behavior_binding caps_word = {
    .behavior_dev = CAPS_WORD,
};

// Danish-layout equals is DA_EQUAL in the keymap, equivalent to Shift+0.
static const struct zmk_behavior_binding equal = {
    .behavior_dev = KEY_PRESS,
    .param1 = LS(N0),
};

static bool base_layer_active(void) {
    return zmk_keymap_highest_layer_active() == HOTPATH_LAYER_BASE;
}

static bool position_in_keymap(uint32_t position) { return position < ZMK_KEYMAP_LEN; }

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

static int release_pending_s(void) {
    if (!pending_s) {
        return 0;
    }

    struct zmk_position_state_changed_event ev = pending_s_event;

    pending_s = false;
    k_work_cancel_delayable(&pending_s_timeout);

    return ZMK_EVENT_RELEASE(ev);
}

static int release_pending_si_roll(void) {
    int ret = release_pending_s();

    if (!pending_si_i) {
        return ret;
    }

    struct zmk_position_state_changed_event ev = pending_si_i_event;

    pending_si_i = false;

    int i_ret = ZMK_EVENT_RELEASE(ev);
    return ret < 0 ? ret : i_ret;
}

static int tap_shifted_i(int64_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = HOTPATH_I_POSITION,
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    int ret = zmk_behavior_invoke_binding(&shifted_i, event, true);
    if (ret < 0) {
        return ret;
    }

    return zmk_behavior_invoke_binding(&shifted_i, event, false);
}

static int tap_binding(const struct zmk_behavior_binding *binding, uint32_t position,
                       int64_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = position,
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    int ret = zmk_behavior_invoke_binding(binding, event, true);
    if (ret < 0) {
        return ret;
    }

    return zmk_behavior_invoke_binding(binding, event, false);
}

static int tap_guarded_combo(enum guarded_combo combo, int64_t timestamp) {
    switch (combo) {
    case GUARDED_COMBO_ASTERISK:
        return tap_binding(&asterisk, pending_guarded_position, timestamp);
    case GUARDED_COMBO_CAPS_WORD:
        return tap_binding(&caps_word, pending_guarded_position, timestamp);
    case GUARDED_COMBO_EQUAL:
        return tap_binding(&equal, pending_guarded_position, timestamp);
    default:
        return -EINVAL;
    }
}

static void pending_s_timeout_handler(struct k_work *work) {
    ARG_UNUSED(work);

    release_pending_s();
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
           position == HOTPATH_T_POSITION || position == HOTPATH_I_POSITION ||
           position == HOTPATH_O_POSITION;
}

static void start_guarded_combo(const struct zmk_position_state_changed *ev) {
    pending_guarded_event = copy_raised_zmk_position_state_changed(ev);
    pending_guarded = true;
    pending_guarded_position = ev->position;
    pending_guarded_allows_asterisk =
        ev->position == HOTPATH_E_POSITION || ev->position == HOTPATH_R_POSITION;
    pending_guarded_allows_caps_word =
        ev->position == HOTPATH_R_POSITION || ev->position == HOTPATH_T_POSITION;
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

    if (pending_guarded_allows_caps_word && ((pending_guarded_position == HOTPATH_R_POSITION &&
                                              second_position == HOTPATH_T_POSITION) ||
                                             (pending_guarded_position == HOTPATH_T_POSITION &&
                                              second_position == HOTPATH_R_POSITION))) {
        return GUARDED_COMBO_CAPS_WORD;
    }

    if (pending_guarded_allows_equal && ((pending_guarded_position == HOTPATH_I_POSITION &&
                                          second_position == HOTPATH_O_POSITION) ||
                                         (pending_guarded_position == HOTPATH_O_POSITION &&
                                          second_position == HOTPATH_I_POSITION))) {
        return GUARDED_COMBO_EQUAL;
    }

    return GUARDED_COMBO_NONE;
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

    if (!ev->state && suppress_s_release && ev->position == HOTPATH_S_POSITION) {
        suppress_s_release = false;
        return ZMK_EV_EVENT_HANDLED;
    }

    if (pending_s) {
        if (pending_si_i) {
            if (!ev->state && ev->position == HOTPATH_I_POSITION) {
                pending_s = false;
                pending_si_i = false;

                suppress_s_release = true;

                int ret = tap_shifted_i(ev->timestamp);
                return ret < 0 ? ret : ZMK_EV_EVENT_HANDLED;
            }

            if (!ev->state && ev->position == HOTPATH_S_POSITION) {
                release_pending_si_roll();
                return ZMK_EV_EVENT_BUBBLE;
            }

            if (ev->state) {
                release_pending_si_roll();
                return ZMK_EV_EVENT_BUBBLE;
            }
        }

        if (!ev->state && ev->position == HOTPATH_S_POSITION) {
            release_pending_s();
            return ZMK_EV_EVENT_BUBBLE;
        }

        if (ev->state && ev->position == HOTPATH_I_POSITION && base_layer_active()) {
            pending_si_i_event = copy_raised_zmk_position_state_changed(ev);
            pending_si_i = true;
            k_work_cancel_delayable(&pending_s_timeout);
            return ZMK_EV_EVENT_CAPTURED;
        }

        if (ev->state) {
            release_pending_s();
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

    if (ev->state && ev->position == HOTPATH_S_POSITION && base_layer_active()) {
        pending_s_event = copy_raised_zmk_position_state_changed(ev);
        pending_s = true;

        k_work_reschedule(&pending_s_timeout, K_MSEC(CONFIG_ZMK_CORNE_HOTPATH_SI_TIMEOUT_MS));

        return ZMK_EV_EVENT_CAPTURED;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(corne_hotpaths, corne_hotpaths_listener);
ZMK_SUBSCRIPTION(corne_hotpaths, zmk_position_state_changed);

static int corne_hotpaths_init(void) {
    k_work_init_delayable(&pending_s_timeout, pending_s_timeout_handler);
    k_work_init_delayable(&pending_guarded_timeout, pending_guarded_timeout_handler);
    return 0;
}

SYS_INIT(corne_hotpaths_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
