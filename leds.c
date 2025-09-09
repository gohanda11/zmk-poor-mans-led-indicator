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

#define BLINK_STRUCT(seq, num_repeats, led_color) \
    (struct blink_item) { \
        .sequence = seq, \
        .sequence_len = LENGTH(seq), \
        .n_repeats = num_repeats, \
        .color = led_color \
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

#define LED_STRIP_NODE_ID DT_ALIAS(led_strip)

// WS2812/SK6812 LED strip device
static const struct device *led_strip = DEVICE_DT_GET(LED_STRIP_NODE_ID);

BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(led_strip)),
             "An alias for led-strip is not found for SK6812 LED");

// RGB color definitions for WS2812 - corrected based on actual behavior
// Observed: COLOR_BLUE {0,0,255} shows as green -> B channel goes to Green LED
// color-mapping = <LED_COLOR_ID_GREEN LED_COLOR_ID_RED LED_COLOR_ID_BLUE>
// Actual mapping: led_rgb.r->Red, led_rgb.g->Blue, led_rgb.b->Green
static const struct led_rgb COLOR_RED = {255, 0, 0};      // {R=255, B=0, G=0} -> Red
static const struct led_rgb COLOR_GREEN = {0, 0, 255};    // {R=0, B=0, G=255} -> Green
static const struct led_rgb COLOR_BLUE = {0, 255, 0};     // {R=0, B=255, G=0} -> Blue
static const struct led_rgb COLOR_YELLOW = {255, 0, 255}; // {R=255, B=0, G=255} -> Yellow (Red+Green)
static const struct led_rgb COLOR_MAGENTA = {255, 255, 0}; // {R=255, B=255, G=0} -> Magenta (Red+Blue)
static const struct led_rgb COLOR_CYAN = {0, 255, 255};   // {R=0, B=255, G=255} -> Cyan (Green+Blue)
static const struct led_rgb COLOR_WHITE = {255, 255, 255}; // {R=255, B=255, G=255} -> White
static const struct led_rgb COLOR_OFF = {0, 0, 0};        // {R=0, B=0, G=0} -> Off

// Layer color mapping (like zmk-rgbled-widget)
static const struct led_rgb LAYER_COLORS[] = {
    COLOR_OFF,      // Layer 0 (base): Off/Black
    COLOR_RED,      // Layer 1: Red
    COLOR_GREEN,    // Layer 2: Green
    COLOR_YELLOW,   // Layer 3: Yellow
    COLOR_BLUE,     // Layer 4: Blue
    COLOR_MAGENTA,  // Layer 5: Magenta
    COLOR_CYAN,     // Layer 6: Cyan
    COLOR_WHITE,    // Layer 7: White
};

#define NUM_LAYER_COLORS (sizeof(LAYER_COLORS) / sizeof(LAYER_COLORS[0]))

// flag to indicate whether the initial boot up sequence is complete
static bool initialized = false;

// a blink work item as specified by the blink rate
struct blink_item {
    const uint16_t *sequence;
    size_t sequence_len;
    uint8_t n_repeats;
    struct led_rgb color;
};


// define message queue of blink work items, that will be processed by a separate thread
// Max 6 sequences; more in queue will be dropped.
K_MSGQ_DEFINE(led_msgq, sizeof(struct blink_item), 6, 1);

static void led_do_blink(struct blink_item blink) {
    struct led_rgb pixels[1];
    
    // 初期消灯 (Initial turn off)
    pixels[0] = COLOR_OFF;
    led_strip_update_rgb(led_strip, pixels, 1);
    k_sleep(K_MSEC(100));
    
    // Skip blink sequence if no repeats or no sequence
    if (blink.n_repeats == 0 || blink.sequence_len == 0) {
        return;
    }
    
    for (int n = 0; n < blink.n_repeats; n++) {
        for (int i = 0; i < blink.sequence_len; i++) {
            // On for evens (0 == start), off for odds
            if (i % 2 == 0) {
                pixels[0] = blink.color;  // 指定色で点灯
            } else {
                pixels[0] = COLOR_OFF;    // 消灯
            }
            led_strip_update_rgb(led_strip, pixels, 1);
            
            uint16_t blink_time = blink.sequence[i];
            k_sleep(K_MSEC(blink_time));
        }
        
        // Brief pause between repetitions
        if (n < blink.n_repeats - 1) {
            pixels[0] = COLOR_OFF;
            led_strip_update_rgb(led_strip, pixels, 1);
            k_sleep(K_MSEC(150));
        }
    }
    
    // Final turn off unless it's a "stay on" pattern
    if (blink.sequence != STAY_ON) {
        pixels[0] = COLOR_OFF;
        led_strip_update_rgb(led_strip, pixels, 1);
    }
}

#if IS_ENABLED(CONFIG_ZMK_BLE) && IS_ENABLED(CONFIG_INDICATOR_LED_SHOW_BLE)
static void indicate_ble(void) {
    struct blink_item blink = {};

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
    uint8_t profile_index = zmk_ble_active_profile_index() + 1;
    if (zmk_ble_active_profile_is_connected()) {
        LOG_INF("Profile %d connected, blinking blue", profile_index);
        SET_BLINK_SEQUENCE(CONFIG_INDICATOR_LED_BLE_PROFILE_CONNECTED_PATTERN);
        blink.n_repeats = profile_index;
        blink.color = COLOR_BLUE;      // 接続: 青
    } else if (zmk_ble_active_profile_is_open()) {
        LOG_INF("Profile %d open, blinking cyan", profile_index);
        SET_BLINK_SEQUENCE(CONFIG_INDICATOR_LED_BLE_PROFILE_OPEN_PATTERN);
        blink.n_repeats = profile_index;
        blink.color = COLOR_CYAN;      // 広告中: シアン
    } else {
        LOG_INF("Profile %d not connected, blinking magenta", profile_index);
        SET_BLINK_SEQUENCE(CONFIG_INDICATOR_LED_PROFILE_UNCONNECTED_PATTERN);
        blink.n_repeats = profile_index;
        blink.color = COLOR_MAGENTA;   // 未接続: マゼンタ
    }
    k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
#endif
#if IS_ENABLED(CONFIG_INDICATOR_LED_SHOW_PERIPHERAL_BLE) && \
    IS_ENABLED(CONFIG_ZMK_SPLIT) && \
    !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (zmk_split_bt_peripheral_is_connected()) {
        LOG_INF("Peripheral connected, blinking blue");
        SET_BLINK_SEQUENCE(CONFIG_INDICATOR_LED_BLE_PROFILE_CONNECTED_PATTERN);
        blink.n_repeats = 1;
        blink.color = COLOR_BLUE;      // 接続: 青
    } else {
        LOG_INF("Peripheral not connected, blinking magenta");
        SET_BLINK_SEQUENCE(CONFIG_INDICATOR_LED_PROFILE_UNCONNECTED_PATTERN);
        blink.n_repeats = 10;
        blink.color = COLOR_MAGENTA;   // 未接続: マゼンタ
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

        struct blink_item blink = BLINK_STRUCT(
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
        blink.color = COLOR_OFF;
    } else if (battery_level >= CONFIG_INDICATOR_LED_BATTERY_LEVEL_HIGH) {
        LOG_INF("Startup Battery level %d, blinking green", battery_level);
        SET_BLINK_SEQUENCE(CONFIG_INDICATOR_LED_BATTERY_HIGH_PATTERN);
        blink.n_repeats = CONFIG_INDICATOR_LED_BATTERY_HIGH_BLINK_REPEAT;
        blink.color = COLOR_GREEN;     // 高: 緑
    } else if (battery_level <= CONFIG_INDICATOR_LED_BATTERY_LEVEL_CRITICAL){
        LOG_INF("Startup Battery level %d, blinking red", battery_level);
        SET_BLINK_SEQUENCE(CONFIG_INDICATOR_LED_BATTERY_CRITICAL_PATTERN);
        blink.n_repeats = CONFIG_INDICATOR_LED_BATTERY_CRITICAL_BLINK_REPEAT;
        blink.color = COLOR_RED;       // 危険: 赤
    } else if (battery_level <= CONFIG_INDICATOR_LED_BATTERY_LEVEL_LOW) {
        LOG_INF("Startup Battery level %d, blinking yellow", battery_level);
        SET_BLINK_SEQUENCE(CONFIG_INDICATOR_LED_BATTERY_LOW_PATTERN);
        blink.n_repeats = CONFIG_INDICATOR_LED_BATTERY_LOW_BLINK_REPEAT;
        blink.color = COLOR_YELLOW;    // 低: 黄
    } else {
        blink.n_repeats = 0;
        blink.color = COLOR_OFF;
    }

    k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
}
#endif

#endif // IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)


#if IS_ENABLED(CONFIG_INDICATOR_LED_SHOW_LAYER_CHANGE)
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
static void set_layer_color(uint8_t layer) {
    struct led_rgb pixels[1];
    
    // Get color for the layer (use white if layer exceeds defined colors)
    if (layer < NUM_LAYER_COLORS) {
        pixels[0] = LAYER_COLORS[layer];
    } else {
        pixels[0] = COLOR_WHITE;
    }
    
    // Set LED to the layer color
    led_strip_update_rgb(led_strip, pixels, 1);
    LOG_INF("Set layer %d color", layer);
}

static int led_layer_listener_cb(const zmk_event_t *eh) {
    if (!initialized) {
        return 0;
    }

    uint8_t layer = zmk_keymap_highest_layer_active();
    LOG_INF("Changed to layer %d", layer);
    
    // Set constant color based on layer (like zmk-rgbled-widget)
    set_layer_color(layer);
    
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

    // Wait for system to stabilize
    k_sleep(K_MSEC(500));

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) && \
    IS_ENABLED(CONFIG_INDICATOR_LED_SHOW_BATTERY_ON_BOOT)
    LOG_INF("Indicating initial battery status");
    indicate_startup_battery();
    // Wait between sequences
    k_sleep(K_MSEC(CONFIG_INDICATOR_LED_INTERVAL_MS * 2));
#endif // IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)

#if IS_ENABLED(CONFIG_ZMK_BLE) && IS_ENABLED(CONFIG_INDICATOR_LED_SHOW_BLE)
    // check and indicate current profile or peripheral connectivity status
    LOG_INF("Indicating initial connectivity status");
    indicate_ble();
    // Wait between sequences
    k_sleep(K_MSEC(CONFIG_INDICATOR_LED_INTERVAL_MS * 2));
#endif // IS_ENABLED(CONFIG_ZMK_BLE)

    initialized = true;
    LOG_INF("Finished initializing LED widget");

#if IS_ENABLED(CONFIG_INDICATOR_LED_SHOW_LAYER_CHANGE)
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
    // Set initial layer color (like zmk-rgbled-widget)
    LOG_INF("Setting initial layer color");
    set_layer_color(zmk_keymap_highest_layer_active());
#endif
#endif
}

// run init thread on boot for initial battery+output checks  
// Increased delay to ensure system is fully initialized
K_THREAD_DEFINE(led_init_tid, 1024, led_init_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO,
                0, 1000);
