/*
 * Copyright (c) 2026 MrVampy
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_reactive_rgb

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <drivers/ext_power.h>

#include <dt-bindings/zmk/rgb.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/workqueue.h>

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if !DT_HAS_CHOSEN(zmk_underglow)
#error "Reactive RGB requires a zmk,underglow chosen node"
#endif

#define STRIP_CHOSEN DT_CHOSEN(zmk_underglow)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_CHOSEN, chain_length)

static const struct device *led_strip;
static const struct device *ext_power;

static struct led_rgb pixels[STRIP_NUM_PIXELS];
static bool active_pixels[STRIP_NUM_PIXELS];
static bool reactive_rgb_enabled;

static K_MUTEX_DEFINE(reactive_rgb_lock);

/*
 * Keebart Corne Choc Pro WS2812 order from the vendor QMK/Vial hardware
 * definition, translated to ZMK physical positions. Both halves use the same
 * local LED indices, mirrored through their global ZMK position numbers.
 */
static const int position_to_pixel_map[] = {
    18, 17, 12, 11, 4,  3,  21, 21, 3,  4,  11, 12, 17, 18, 19, 16,
    13, 10, 5,  2,  22, 22, 2,  5,  10, 13, 16, 19, 20, 15, 14, 9,
    6,  1,  1,  6,  9,  14, 15, 20, 8,  7,  0,  0,  7,  8,
};

static int position_to_pixel(uint32_t position) {
    if (position >= ARRAY_SIZE(position_to_pixel_map)) {
        return -EINVAL;
    }

    return position_to_pixel_map[position];
}

static void clear_active_pixels(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        active_pixels[i] = false;
    }
}

static void reactive_rgb_update(struct k_work *work) {
    bool enabled;
    uint8_t brightness = CONFIG_CORNE_REACTIVE_RGB_BRT_BATTERY;

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    if (zmk_usb_is_powered()) {
        brightness = CONFIG_CORNE_REACTIVE_RGB_BRT_USB;
    }
#endif

    const uint8_t red = (255 * brightness) / 100;

    k_mutex_lock(&reactive_rgb_lock, K_FOREVER);
    enabled = reactive_rgb_enabled;
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = (enabled && active_pixels[i]) ? (struct led_rgb){.r = red, .g = 0, .b = 0}
                                                  : (struct led_rgb){.r = 0, .g = 0, .b = 0};
    }
    k_mutex_unlock(&reactive_rgb_lock);

    int err = led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
    if (err < 0) {
        LOG_ERR("Failed to update reactive RGB strip (%d)", err);
    }

    if (!enabled && ext_power != NULL) {
        err = ext_power_disable(ext_power);
        if (err < 0) {
            LOG_WRN("Failed to disable RGB external power (%d)", err);
        }
    }
}

K_WORK_DEFINE(reactive_rgb_update_work, reactive_rgb_update);

static void schedule_reactive_rgb_update(void) {
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &reactive_rgb_update_work);
}

static int set_reactive_rgb_enabled(bool enabled) {
    if (enabled && ext_power != NULL) {
        int err = ext_power_enable(ext_power);
        if (err < 0) {
            LOG_WRN("Failed to enable RGB external power (%d)", err);
        }
    }

    k_mutex_lock(&reactive_rgb_lock, K_FOREVER);
    reactive_rgb_enabled = enabled;
    clear_active_pixels();
    k_mutex_unlock(&reactive_rgb_lock);

    schedule_reactive_rgb_update();
    return 0;
}

static int reactive_rgb_convert_params(struct zmk_behavior_binding *binding,
                                       struct zmk_behavior_binding_event event) {
    if (binding->param1 == RGB_TOG_CMD) {
        binding->param1 = reactive_rgb_enabled ? RGB_OFF_CMD : RGB_ON_CMD;
    }

    return 0;
}

static int reactive_rgb_binding_pressed(struct zmk_behavior_binding *binding,
                                        struct zmk_behavior_binding_event event) {
    switch (binding->param1) {
    case RGB_ON_CMD:
        return set_reactive_rgb_enabled(true);
    case RGB_OFF_CMD:
        return set_reactive_rgb_enabled(false);
    case RGB_TOG_CMD:
        return set_reactive_rgb_enabled(!reactive_rgb_enabled);
    default:
        LOG_WRN("Unsupported reactive RGB command %d", binding->param1);
        return -ENOTSUP;
    }
}

static int reactive_rgb_binding_released(struct zmk_behavior_binding *binding,
                                         struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api reactive_rgb_driver_api = {
    .binding_convert_central_state_dependent_params = reactive_rgb_convert_params,
    .binding_pressed = reactive_rgb_binding_pressed,
    .binding_released = reactive_rgb_binding_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &reactive_rgb_driver_api);

static int reactive_rgb_position_listener(const zmk_event_t *eh) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    if (as_zmk_usb_conn_state_changed(eh) != NULL) {
        if (reactive_rgb_enabled) {
            schedule_reactive_rgb_update();
        }
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL || ev->source != ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (!reactive_rgb_enabled) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    int pixel = position_to_pixel(ev->position);
    if (pixel < 0 || pixel >= STRIP_NUM_PIXELS) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    k_mutex_lock(&reactive_rgb_lock, K_FOREVER);
    active_pixels[pixel] = ev->state;
    k_mutex_unlock(&reactive_rgb_lock);

    schedule_reactive_rgb_update();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(reactive_rgb, reactive_rgb_position_listener);
ZMK_SUBSCRIPTION(reactive_rgb, zmk_position_state_changed);

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(reactive_rgb, zmk_usb_conn_state_changed);
#endif

static int reactive_rgb_init(void) {
    led_strip = DEVICE_DT_GET(STRIP_CHOSEN);
    if (!device_is_ready(led_strip)) {
        LOG_ERR("Reactive RGB strip device is not ready");
        return -ENODEV;
    }

    ext_power = device_get_binding("EXT_POWER");
    if (ext_power == NULL) {
        LOG_WRN("Reactive RGB could not find EXT_POWER");
    }

    reactive_rgb_enabled = false;
    clear_active_pixels();
    schedule_reactive_rgb_update();

    return 0;
}

SYS_INIT(reactive_rgb_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
