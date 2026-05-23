/*
 * Temporary Corne BLE/profile observability probe.
 *
 * This is intentionally built only by the corne-ble-debug snippet. It emits
 * INFO logs so we can diagnose profile storage, BT_CLR/BT_SEL input, and
 * advertising preconditions from the keyboard side without carrying logging in
 * normal firmware.
 */

#include <zephyr/bluetooth/addr.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
#include <zmk/events/usb_conn_state_changed.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define CORNE_BLE_DEBUG_FIRST_LOG_MS 3000
#define CORNE_BLE_DEBUG_PERIOD_MS 10000

static struct k_work_delayable corne_ble_debug_work;

static bool is_interesting_position(uint32_t position) {
    switch (position) {
    case 15: /* SETTINGS: BT_SEL 0 */
    case 16: /* SETTINGS: BT_SEL 1 */
    case 17: /* SETTINGS: BT_SEL 2 */
    case 18: /* SETTINGS: BT_SEL 3 */
    case 19: /* SETTINGS: BT_SEL 4 */
    case 29: /* SETTINGS: BT_CLR */
        return true;
    default:
        return false;
    }
}

static bool is_interesting_layer(uint8_t layer) {
    switch (layer) {
    case 1: /* NUMBER */
    case 2: /* SYMBOL */
    case 3: /* SETTINGS */
        return true;
    default:
        return false;
    }
}

static void log_profile_state(const char *reason) {
    int active = zmk_ble_active_profile_index();

    LOG_INF("corne_ble_debug: %s active=%d open=%d connected=%d name=%s profiles=%d", reason,
            active, zmk_ble_active_profile_is_open(), zmk_ble_active_profile_is_connected(),
            zmk_ble_active_profile_name(), ZMK_BLE_PROFILE_COUNT);

    for (int i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
        char addr[BT_ADDR_LE_STR_LEN];

        bt_addr_le_to_str(zmk_ble_profile_address(i), addr, sizeof(addr));
        LOG_INF("corne_ble_debug: profile[%d] open=%d connected=%d addr=%s", i,
                zmk_ble_profile_is_open(i), zmk_ble_profile_is_connected(i), addr);
    }
}

static void corne_ble_debug_work_handler(struct k_work *work) {
    log_profile_state("periodic");
    k_work_reschedule(&corne_ble_debug_work, K_MSEC(CORNE_BLE_DEBUG_PERIOD_MS));
}

static int corne_ble_debug_listener(const zmk_event_t *eh) {
    const struct zmk_ble_active_profile_changed *profile_ev = as_zmk_ble_active_profile_changed(eh);

    if (profile_ev != NULL) {
        char addr[BT_ADDR_LE_STR_LEN];

        bt_addr_le_to_str(&profile_ev->profile->peer, addr, sizeof(addr));
        LOG_INF("corne_ble_debug: profile_changed index=%d name=%s addr=%s open=%d connected=%d",
                profile_ev->index, profile_ev->profile->name, addr,
                zmk_ble_profile_is_open(profile_ev->index),
                zmk_ble_profile_is_connected(profile_ev->index));
        log_profile_state("profile_changed");
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_layer_state_changed *layer_ev = as_zmk_layer_state_changed(eh);

    if (layer_ev != NULL && is_interesting_layer(layer_ev->layer)) {
        LOG_INF("corne_ble_debug: layer=%d state=%d", layer_ev->layer, layer_ev->state);
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_position_state_changed *pos_ev = as_zmk_position_state_changed(eh);

    if (pos_ev != NULL && is_interesting_position(pos_ev->position)) {
        LOG_INF("corne_ble_debug: position=%u state=%d source=%u", pos_ev->position, pos_ev->state,
                pos_ev->source);
        return ZMK_EV_EVENT_BUBBLE;
    }

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    const struct zmk_usb_conn_state_changed *usb_ev = as_zmk_usb_conn_state_changed(eh);

    if (usb_ev != NULL) {
        LOG_INF("corne_ble_debug: usb_conn_state=%d", usb_ev->conn_state);
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

    return ZMK_EV_EVENT_BUBBLE;
}

static int corne_ble_debug_init(void) {
    LOG_INF("corne_ble_debug: init");
    k_work_init_delayable(&corne_ble_debug_work, corne_ble_debug_work_handler);
    k_work_schedule(&corne_ble_debug_work, K_MSEC(CORNE_BLE_DEBUG_FIRST_LOG_MS));

    return 0;
}

ZMK_LISTENER(corne_ble_debug, corne_ble_debug_listener);
ZMK_SUBSCRIPTION(corne_ble_debug, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(corne_ble_debug, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(corne_ble_debug, zmk_position_state_changed);

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(corne_ble_debug, zmk_usb_conn_state_changed);
#endif

SYS_INIT(corne_ble_debug_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
