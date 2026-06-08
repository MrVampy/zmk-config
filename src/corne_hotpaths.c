/*
 * Ordered typing hot-paths for the user's Corne layout.
 *
 * Stock ZMK combos are intentionally unordered. This listener keeps the
 * high-frequency Shift-HRM path for S->I order-aware, so I->S still types
 * normally as "is".
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>

#include <dt-bindings/zmk/keys.h>
#include <dt-bindings/zmk/modifiers.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define HOTPATH_LAYER_BASE 0
#define HOTPATH_S_POSITION 16
#define HOTPATH_I_POSITION 10
#define KEY_PRESS DEVICE_DT_NAME(DT_INST(0, zmk_behavior_key_press))

static struct zmk_position_state_changed_event pending_s_event;
static struct k_work_delayable pending_s_timeout;

static bool pending_s;
static bool suppress_s_release;
static bool suppress_i_release;

static const struct zmk_behavior_binding shifted_i = {
    .behavior_dev = KEY_PRESS,
    .param1 = LS(I),
};

static bool base_layer_active(void) {
    return zmk_keymap_highest_layer_active() == HOTPATH_LAYER_BASE;
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

static void pending_s_timeout_handler(struct k_work *work) {
    ARG_UNUSED(work);

    release_pending_s();
}

static int corne_hotpaths_listener(const zmk_event_t *eh) {
    struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (!ev->state && suppress_s_release && ev->position == HOTPATH_S_POSITION) {
        suppress_s_release = false;
        return ZMK_EV_EVENT_HANDLED;
    }

    if (!ev->state && suppress_i_release && ev->position == HOTPATH_I_POSITION) {
        suppress_i_release = false;
        return ZMK_EV_EVENT_HANDLED;
    }

    if (pending_s) {
        if (!ev->state && ev->position == HOTPATH_S_POSITION) {
            release_pending_s();
            return ZMK_EV_EVENT_BUBBLE;
        }

        if (ev->state && ev->position == HOTPATH_I_POSITION && base_layer_active()) {
            pending_s = false;
            k_work_cancel_delayable(&pending_s_timeout);

            suppress_s_release = true;
            suppress_i_release = true;

            int ret = tap_shifted_i(ev->timestamp);
            return ret < 0 ? ret : ZMK_EV_EVENT_CAPTURED;
        }

        if (ev->state) {
            release_pending_s();
            return ZMK_EV_EVENT_BUBBLE;
        }
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
    return 0;
}

SYS_INIT(corne_hotpaths_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
