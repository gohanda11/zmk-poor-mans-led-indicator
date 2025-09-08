#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>

#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/battery.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/layer_state_changed.h>

#include <zephyr/logging/log.h>

#define LENGTH(x)  (sizeof(x) / sizeof((x)[0]))
#define SET_BLINK_SEQUENCE(seq) \
do { \
    blink.sequence = seq; \
    blink.sequence_len = LENGTH(seq); \
} while(0)

#define SET_BLINK_COLOR(clr) \
do { \
    blink.color = clr; \
} while(0)

#define BLINK_STRUCT(seq, num_repeats) \
    (struct blink_item) { \
        .sequence = seq, \
        .sequence_len = LENGTH(seq), \
        .n_repeats = num_repeats, \
        .color = COLOR_WHITE \
    }

#define BLINK_STRUCT_COLOR(seq, num_repeats, clr) \
    (struct blink_item) { \
        .sequence = seq, \
        .sequence_len = LENGTH(seq), \
        .n_repeats = num_repeats, \
        .color = clr \
    }

static const uint16_t CONFIG_INDICATOR_LED_LAYER_PATTERN[] = {80, 120};
static const uint16_t CONFIG_INDICATOR_LED_BATTERY_CRITICAL_PATTERN[] = {40, 40};
static const uint16_t CONFIG_INDICATOR_LED_BATTERY_HIGH_PATTERN[] = {500, 500};
static const uint16_t CONFIG_INDICATOR_LED_BATTERY_LOW_PATTERN[] = {100, 100};
// When connected, more on than off
static const uint16_t CONFIG_INDICATOR_LED_BLE_PROFILE_CONNECTED_PATTERN[] = {1000, 100};
// When open/unpaired, tiny blips.
static const uint16_t CONFIG_INDICATOR_LED_BLE_PROFILE_OPEN_PATTERN[] = {80, 80};
// When unconnected and searching, more off than on
static const uint16_t CONFIG_INDICATOR_LED_PROFILE_UNCONNECTED_PATTERN[] = {200, 800};
static const uint16_t STAY_ON[] = {10};


LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// LED Strip device for WS2812/SK6812
#if DT_NODE_EXISTS(DT_CHOSEN(zmk_underglow))
#define LED_STRIP_NODE DT_CHOSEN(zmk_underglow)
static const struct device *led_dev = DEVICE_DT_GET(LED_STRIP_NODE);
static const bool use_led_strip = true;
#else
// Fallback to GPIO LED
#define LED_GPIO_NODE_ID DT_COMPAT_GET_ANY_STATUS_OKAY(gpio_leds)
static const struct device *led_dev = DEVICE_DT_GET(LED_GPIO_NODE_ID);
BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(indicator_led)),
             "An alias for indicator-led is not found for INDICATOR_LED");
static const uint8_t led_idx = DT_NODE_CHILD_IDX(DT_ALIAS(indicator_led));
static const bool use_led_strip = false;
#endif

// RGB color structure for WS2812/SK6812 (moved up for struct blink_item)
struct rgb_color {
    uint8_t r;
    uint8_t g; 
    uint8_t b;
};

// flag to indicate whether the initial boot up sequence is complete
static bool initialized = false;

// a blink work item as specified by the blink rate
struct blink_item {
    const uint16_t *sequence;
    size_t sequence_len;
    uint8_t n_repeats;
    struct rgb_color color; // Add color information
};


// define message queue of blink work items, that will be processed by a separate thread
// Max 6 sequences; more in queue will be dropped.
K_MSGQ_DEFINE(led_msgq, sizeof(struct blink_item), 6, 1);

// Color definitions
static const struct rgb_color COLOR_OFF = {0, 0, 0};
static const struct rgb_color COLOR_RED = {255, 0, 0};
static const struct rgb_color COLOR_GREEN = {0, 255, 0};
static const struct rgb_color COLOR_BLUE = {0, 0, 255};
static const struct rgb_color COLOR_YELLOW = {255, 255, 0};
static const struct rgb_color COLOR_WHITE = {255, 255, 255};

// Set LED color - support both LED strip and GPIO LED
static void set_led_color(struct rgb_color color) {
    if (use_led_strip) {
        // Use LED strip for WS2812/SK6812
        struct led_rgb led_color = {.r = color.r, .g = color.g, .b = color.b};
        led_strip_update_rgb(led_dev, &led_color, 1);
    } else {
        // Use GPIO LED (simple on/off)
        if (color.r > 0 || color.g > 0 || color.b > 0) {
            led_on(led_dev, led_idx);
        } else {
            led_off(led_dev, led_idx);
        }
    }
}

// Turn LED off
static void led_off_func(void) {
    if (use_led_strip) {
        set_led_color(COLOR_OFF);
    } else {
        led_off(led_dev, led_idx);
    }
}

static void led_do_blink(struct blink_item blink) {
    led_off_func();
    k_sleep(K_MSEC(200));
    for (int n = 0; n < blink.n_repeats; n++) {
        for (int i = 0; i < blink.sequence_len; i++) {
            // on for evens (0 == start, off for odds. If the sequence contains an odd number, will stay on.
            if (i%2 == 0){
                set_led_color(blink.color); // Use color from blink item
            } else {
                led_off_func();
            }
            uint16_t blink_time = blink.sequence[i];
            k_sleep(K_MSEC(blink_time));
        }
    }
}

#if IS_ENABLED(CONFIG_ZMK_BLE) && IS_ENABLED(CONFIG_INDICATOR_LED_SHOW_BLE)
static void indicate_ble(void) {
    struct blink_item blink = {};

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
    uint8_t profile_index = zmk_ble_active_profile_index() + 1;
    if (zmk_ble_active_profile_is_connected()) {
        LOG_INF("Profile %d connected, blinking for connected", profile_index);
        SET_BLINK_SEQUENCE(CONFIG_INDICATOR_LED_BLE_PROFILE_CONNECTED_PATTERN);
        SET_BLINK_COLOR(COLOR_BLUE);  // Blue for connected
        blink.n_repeats = profile_index;
    } else if (zmk_ble_active_profile_is_open()) {
        LOG_INF("Profile %d open, blinking for open", profile_index);
        SET_BLINK_SEQUENCE(CONFIG_INDICATOR_LED_BLE_PROFILE_OPEN_PATTERN);
        SET_BLINK_COLOR(COLOR_YELLOW);  // Yellow for advertising/open
        blink.n_repeats = profile_index;
    } else {
        LOG_INF("Profile %d not connected, blinking for unconnected", profile_index);
        SET_BLINK_SEQUENCE(CONFIG_INDICATOR_LED_PROFILE_UNCONNECTED_PATTERN);
        SET_BLINK_COLOR(COLOR_RED);  // Red for disconnected
        blink.n_repeats = profile_index;
    }
    k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
#endif
#if IS_ENABLED(CONFIG_INDICATOR_LED_SHOW_PERIPHERAL_BLE) && \
    IS_ENABLED(CONFIG_ZMK_SPLIT) && \
    !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (zmk_split_bt_peripheral_is_connected()) {
        LOG_INF("Peripheral connected, blinking once");
        SET_BLINK_SEQUENCE(CONFIG_INDICATOR_LED_BLE_PROFILE_CONNECTED_PATTERN);
        SET_BLINK_COLOR(COLOR_BLUE);  // Blue for connected
        blink.n_repeats = 1;
    } else {
        LOG_INF("Peripheral not connected, blinking for unconnected");
        SET_BLINK_SEQUENCE(CONFIG_INDICATOR_LED_PROFILE_UNCONNECTED_PATTERN);
        SET_BLINK_COLOR(COLOR_RED);  // Red for disconnected
        blink.n_repeats = 10;
    }
    k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
#endif

}

static int led_output_listener_cb(const zmk_event_t *eh) {
#if IS_ENABLED(CONFIG_ZMK_BLE) && IS_ENABLED(CONFIG_INDICATOR_LED_SHOW_BLE)
    if (initialized) {
        indicate_ble();
    }
#endif
    return 0;
}

ZMK_LISTENER(led_output_listener, led_output_listener_cb);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
// run led_output_listener_cb on BLE profile change (on central)
ZMK_SUBSCRIPTION(led_output_listener, zmk_ble_active_profile_changed);
#else
// // run led_output_listener_cb on peripheral status change event
ZMK_SUBSCRIPTION(led_output_listener, zmk_split_peripheral_status_changed);
#endif

#endif // IS_ENABLED(CONFIG_ZMK_BLE)


#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)

#if IS_ENABLED(CONFIG_INDICATOR_LED_SHOW_CRITICAL_BATTERY_CHANGES)
static int led_battery_listener_cb(const zmk_event_t *eh) {
    if (!initialized) {
        return 0;
    }

    // check if we are in critical battery levels at state change, blink if we are
    uint8_t battery_level = as_zmk_battery_state_changed(eh)->state_of_charge;

    if (battery_level > 0 && battery_level <= CONFIG_INDICATOR_LED_BATTERY_LEVEL_CRITICAL) {
        LOG_INF("Battery level %d, blinking for critical", battery_level);

        static const struct blink_item blink = BLINK_STRUCT_COLOR(
            CONFIG_INDICATOR_LED_BATTERY_CRITICAL_PATTERN, 1, COLOR_RED
        );
        k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
    }
    return 0;
}
// run led_battery_listener_cb on battery state change event
ZMK_LISTENER(led_battery_listener, led_battery_listener_cb);
ZMK_SUBSCRIPTION(led_battery_listener, zmk_battery_state_changed);
#endif

#if IS_ENABLED(CONFIG_INDICATOR_LED_SHOW_BATTERY_ON_BOOT)
static void indicate_startup_battery(void) {
    // check and indicate battery level on thread start
    LOG_INF("Indicating initial battery status");

    struct blink_item blink = {};
    uint8_t battery_level = zmk_battery_state_of_charge();
    int retry = 0;
    while (battery_level == 0 && retry++ < 10) {
        k_sleep(K_MSEC(100));
        battery_level = zmk_battery_state_of_charge();
    };

    if (battery_level == 0) {
        LOG_INF("Startup Battery level undetermined (zero), blinking off");
        blink.sequence_len = 0;
        blink.n_repeats = 0;
    } else if (battery_level >= CONFIG_INDICATOR_LED_BATTERY_LEVEL_HIGH) {
        LOG_INF("Startup Battery level %d, blinking for high", battery_level);
        SET_BLINK_SEQUENCE(CONFIG_INDICATOR_LED_BATTERY_HIGH_PATTERN);
        SET_BLINK_COLOR(COLOR_GREEN);  // Green for high battery
        blink.n_repeats = CONFIG_INDICATOR_LED_BATTERY_HIGH_BLINK_REPEAT;
    } else if (battery_level <= CONFIG_INDICATOR_LED_BATTERY_LEVEL_CRITICAL){
        LOG_INF("Startup Battery level %d, blinking for critical", battery_level);
        SET_BLINK_SEQUENCE(CONFIG_INDICATOR_LED_BATTERY_CRITICAL_PATTERN);
        SET_BLINK_COLOR(COLOR_RED);  // Red for critical battery
        blink.n_repeats = CONFIG_INDICATOR_LED_BATTERY_CRITICAL_BLINK_REPEAT;
    } else if (battery_level <= CONFIG_INDICATOR_LED_BATTERY_LEVEL_LOW) {
        LOG_INF("Startup Battery level %d, blinking for low", battery_level);
        SET_BLINK_SEQUENCE(CONFIG_INDICATOR_LED_BATTERY_LOW_PATTERN);
        SET_BLINK_COLOR(COLOR_YELLOW);  // Yellow for low battery
        blink.n_repeats = CONFIG_INDICATOR_LED_BATTERY_LOW_BLINK_REPEAT;
    } else {
        blink.n_repeats = 0;
    }

    k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
}
#endif

#endif // IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)


#if IS_ENABLED(CONFIG_INDICATOR_LED_SHOW_LAYER_CHANGE)
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
static int led_layer_listener_cb(const zmk_event_t *eh) {
    if (!initialized) {
        return 0;
    }

    // // ignore layer off events
    // if (!as_zmk_layer_state_changed(eh)->state) {
    //     return 0;
    // }

    uint8_t index = zmk_keymap_highest_layer_active()+1;
    LOG_INF("Changed to layer %d", index);
    
    // Get color based on layer
    struct rgb_color layer_color;
    switch (zmk_keymap_highest_layer_active()) {
        case 0: layer_color = COLOR_WHITE; break;   // Layer 0 - White
        case 1: layer_color = COLOR_RED; break;     // Layer 1 - Red  
        case 2: layer_color = COLOR_GREEN; break;   // Layer 2 - Green
        case 3: layer_color = COLOR_BLUE; break;    // Layer 3 - Blue
        case 4: layer_color = COLOR_YELLOW; break;  // Layer 4 - Yellow
        default: layer_color = COLOR_WHITE; break;  // Default - White
    }
    
    struct blink_item blink = BLINK_STRUCT_COLOR(
        CONFIG_INDICATOR_LED_LAYER_PATTERN, index, layer_color
    );
    k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
    if (zmk_keymap_highest_layer_active() >=
        CONFIG_INDICATOR_LED_LAYER_PERSISTENCE_THRESHOLD) {
        blink = BLINK_STRUCT_COLOR(
            STAY_ON, 1, layer_color
        );
        k_msgq_put(&led_msgq, &blink, K_NO_WAIT);

    }
    return 0;
}

ZMK_LISTENER(led_layer_listener, led_layer_listener_cb);
ZMK_SUBSCRIPTION(led_layer_listener, zmk_layer_state_changed);
#endif
#endif // IS_ENABLED(CONFIG_INDICATOR_LED_SHOW_LAYER_CHANGE)


extern void led_process_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);
    while (true) {
        // wait until a blink item is received and process it
        struct blink_item blink;
        k_msgq_get(&led_msgq, &blink, K_FOREVER);
        LOG_DBG("Got a blink item from msgq");

        led_do_blink(blink);

        // wait interval before processing another blink sequence
        k_sleep(K_MSEC(CONFIG_INDICATOR_LED_INTERVAL_MS));
    }
}

// define led_process_thread with stack size 1024, start running it 100 ms after boot
K_THREAD_DEFINE(led_process_tid, 1024, led_process_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO,
                0, 100);

extern void led_init_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) && \
    IS_ENABLED(CONFIG_INDICATOR_LED_SHOW_BATTERY_ON_BOOT)
    indicate_startup_battery();
#endif // IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)

#if IS_ENABLED(CONFIG_ZMK_BLE) && IS_ENABLED(CONFIG_INDICATOR_LED_SHOW_BLE)
    // check and indicate current profile or peripheral connectivity status
    LOG_INF("Indicating initial connectivity status");
    indicate_ble();
#endif // IS_ENABLED(CONFIG_ZMK_BLE)

    initialized = true;
    LOG_INF("Finished initializing LED widget");
}

// run init thread on boot for initial battery+output checks
K_THREAD_DEFINE(led_init_tid, 1024, led_init_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO,
                0, 200);
