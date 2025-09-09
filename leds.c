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
        .color = led_color, \
        .is_persistent = false \
    }

#define BLINK_STRUCT_PERSISTENT(seq, num_repeats, led_color) \
    (struct blink_item) { \
        .sequence = seq, \
        .sequence_len = LENGTH(seq), \
        .n_repeats = num_repeats, \
        .color = led_color, \
        .is_persistent = true \
    }

static const uint16_t CONFIG_INDICATOR_LED_LAYER_PATTERN[] = {80, 120};
static const uint16_t CONFIG_INDICATOR_LED_BATTERY_CRITICAL_PATTERN[] = {40, 40};
static const uint16_t CONFIG_INDICATOR_LED_BATTERY_HIGH_PATTERN[] = {800, 200};
static const uint16_t CONFIG_INDICATOR_LED_BATTERY_LOW_PATTERN[] = {400, 200};
// When connected, solid blink
static const uint16_t CONFIG_INDICATOR_LED_BLE_PROFILE_CONNECTED_PATTERN[] = {800, 200};
// When open/unpaired, shorter blips
static const uint16_t CONFIG_INDICATOR_LED_BLE_PROFILE_OPEN_PATTERN[] = {400, 200};
// When unconnected, quick blinks
static const uint16_t CONFIG_INDICATOR_LED_PROFILE_UNCONNECTED_PATTERN[] = {300, 200};
static const uint16_t STAY_ON[] = {10};


LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define LED_STRIP_NODE_ID DT_ALIAS(led_strip)

// WS2812/SK6812 LED strip device
static const struct device *led_strip = DEVICE_DT_GET(LED_STRIP_NODE_ID);

BUILD_ASSERT(DT_NODE_EXISTS(DT_ALIAS(led_strip)),
             "An alias for led-strip is not found for SK6812 LED");

// RGB color definitions
static const struct led_rgb COLOR_RED = {255, 0, 0};
static const struct led_rgb COLOR_GREEN = {0, 255, 0};
static const struct led_rgb COLOR_BLUE = {0, 0, 255};
static const struct led_rgb COLOR_YELLOW = {255, 255, 0};
static const struct led_rgb COLOR_MAGENTA = {255, 0, 255};
static const struct led_rgb COLOR_CYAN = {0, 255, 255};
static const struct led_rgb COLOR_WHITE = {255, 255, 255};
static const struct led_rgb COLOR_OFF = {0, 0, 0};

// flag to indicate whether the initial boot up sequence is complete
static bool initialized = false;

// track current persistent layer color
static struct led_rgb led_current_persistent_color = {0, 0, 0};

// Layer color mapping for different layers
static const struct led_rgb LAYER_COLORS[] = {
    {0, 0, 0},       // Layer 0: OFF (default)
    {255, 0, 0},     // Layer 1: Red
    {0, 255, 0},     // Layer 2: Green  
    {0, 0, 255},     // Layer 3: Blue
    {255, 255, 0},   // Layer 4: Yellow
    {255, 0, 255},   // Layer 5: Magenta
    {0, 255, 255},   // Layer 6: Cyan
    {255, 255, 255}, // Layer 7: White
};

// a blink work item as specified by the blink rate
struct blink_item {
    const uint16_t *sequence;
    size_t sequence_len;
    uint8_t n_repeats;
    struct led_rgb color;
    bool is_persistent;  // if true, this color persists after blinking
};


// define message queue of blink work items, that will be processed by a separate thread
// Max 6 sequences; more in queue will be dropped.
K_MSGQ_DEFINE(led_msgq, sizeof(struct blink_item), 6, 1);

// Semaphore to signal completion of LED blink operations
K_SEM_DEFINE(led_blink_complete_sem, 0, 1);

// Helper function to wait for blink completion with timeout
static bool wait_for_blink_completion(int timeout_ms) {
    return k_sem_take(&led_blink_complete_sem, K_MSEC(timeout_ms)) == 0;
}

static void led_do_blink(struct blink_item blink) {
    struct led_rgb pixels[1];
    
    // 持続表示の場合（点滅なし）
    if (blink.is_persistent) {
        led_current_persistent_color = blink.color;
        pixels[0] = blink.color;
        led_strip_update_rgb(led_strip, pixels, 1);
        return;
    }
    
    // 点滅表示の場合
    // 初期消灯
    pixels[0] = COLOR_OFF;
    led_strip_update_rgb(led_strip, pixels, 1);
    k_sleep(K_MSEC(100));  // 短い待機
    
    for (int n = 0; n < blink.n_repeats; n++) {
        for (int i = 0; i < blink.sequence_len; i++) {
            // on for evens (0 == start), off for odds
            if (i % 2 == 0) {
                pixels[0] = blink.color;  // 指定色で点灯
            } else {
                pixels[0] = COLOR_OFF;    // 消灯
            }
            led_strip_update_rgb(led_strip, pixels, 1);
            
            uint16_t blink_time = blink.sequence[i];
            k_sleep(K_MSEC(blink_time));
        }
        // 各繰り返し間に短い間隔
        if (n < blink.n_repeats - 1) {
            pixels[0] = COLOR_OFF;
            led_strip_update_rgb(led_strip, pixels, 1);
            k_sleep(K_MSEC(200));
        }
    }
    
    // 点滅後は持続色に戻す
    pixels[0] = led_current_persistent_color;
    led_strip_update_rgb(led_strip, pixels, 1);
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
        blink.is_persistent = false;
    } else if (zmk_ble_active_profile_is_open()) {
        LOG_INF("Profile %d open, blinking yellow", profile_index);
        SET_BLINK_SEQUENCE(CONFIG_INDICATOR_LED_BLE_PROFILE_OPEN_PATTERN);
        blink.n_repeats = profile_index;
        blink.color = COLOR_YELLOW;    // 広告中: 黄色
        blink.is_persistent = false;
    } else {
        LOG_INF("Profile %d not connected, blinking red", profile_index);
        SET_BLINK_SEQUENCE(CONFIG_INDICATOR_LED_PROFILE_UNCONNECTED_PATTERN);
        blink.n_repeats = profile_index;
        blink.color = COLOR_RED;       // 未接続: 赤
        blink.is_persistent = false;
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
        blink.is_persistent = false;
    } else {
        LOG_INF("Peripheral not connected, blinking red");
        SET_BLINK_SEQUENCE(CONFIG_INDICATOR_LED_PROFILE_UNCONNECTED_PATTERN);
        blink.n_repeats = 10;
        blink.color = COLOR_RED;       // 未接続: 赤
        blink.is_persistent = false;
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

// Helper function to send BLE indication and wait for completion
static void indicate_ble_and_wait(void) {
    indicate_ble();
    // Wait for blink completion with timeout (max 5 seconds)
    if (!wait_for_blink_completion(5000)) {
        LOG_WRN("BLE indication timeout");
    }
}

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

        static const struct blink_item blink = BLINK_STRUCT(
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
    LOG_INF("Starting battery status check");

    struct blink_item blink = {};
    uint8_t battery_level = zmk_battery_state_of_charge();
    LOG_INF("Initial battery level reading: %d", battery_level);
    
    int retry = 0;
    while (battery_level == 0 && retry++ < 10) {
        LOG_DBG("Battery level is 0, retrying %d/10", retry);
        k_sleep(K_MSEC(100));
        battery_level = zmk_battery_state_of_charge();
        LOG_DBG("Retry %d battery level: %d", retry, battery_level);
    };

    if (battery_level == 0) {
        LOG_WRN("Startup Battery level undetermined (zero after %d retries), using default green blink", retry);
        SET_BLINK_SEQUENCE(CONFIG_INDICATOR_LED_BATTERY_HIGH_PATTERN);
        blink.n_repeats = 1;
        blink.color = COLOR_GREEN;     // デフォルト: 緑
        blink.is_persistent = false;
    } else if (battery_level >= CONFIG_INDICATOR_LED_BATTERY_LEVEL_HIGH) {
        LOG_INF("Startup Battery level %d >= %d, blinking green", battery_level, CONFIG_INDICATOR_LED_BATTERY_LEVEL_HIGH);
        SET_BLINK_SEQUENCE(CONFIG_INDICATOR_LED_BATTERY_HIGH_PATTERN);
        blink.n_repeats = CONFIG_INDICATOR_LED_BATTERY_HIGH_BLINK_REPEAT;
        blink.color = COLOR_GREEN;     // 高: 緑
        blink.is_persistent = false;
    } else if (battery_level <= CONFIG_INDICATOR_LED_BATTERY_LEVEL_CRITICAL){
        LOG_INF("Startup Battery level %d <= %d, blinking red", battery_level, CONFIG_INDICATOR_LED_BATTERY_LEVEL_CRITICAL);
        SET_BLINK_SEQUENCE(CONFIG_INDICATOR_LED_BATTERY_CRITICAL_PATTERN);
        blink.n_repeats = CONFIG_INDICATOR_LED_BATTERY_CRITICAL_BLINK_REPEAT;
        blink.color = COLOR_RED;       // 危険: 赤
        blink.is_persistent = false;
    } else if (battery_level <= CONFIG_INDICATOR_LED_BATTERY_LEVEL_LOW) {
        LOG_INF("Startup Battery level %d <= %d, blinking yellow", battery_level, CONFIG_INDICATOR_LED_BATTERY_LEVEL_LOW);
        SET_BLINK_SEQUENCE(CONFIG_INDICATOR_LED_BATTERY_LOW_PATTERN);
        blink.n_repeats = CONFIG_INDICATOR_LED_BATTERY_LOW_BLINK_REPEAT;
        blink.color = COLOR_YELLOW;    // 低: 黄
        blink.is_persistent = false;
    } else {
        LOG_INF("Startup Battery level %d is in middle range, no blink", battery_level);
        blink.n_repeats = 0;
        blink.color = COLOR_OFF;
    }

    LOG_INF("Sending battery blink command: repeats=%d, color=0x%x", blink.n_repeats, blink.color.r << 16 | blink.color.g << 8 | blink.color.b);
    int result = k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
    if (result != 0) {
        LOG_ERR("Failed to send battery blink command: %d", result);
    }
}

// Helper function to send blink command and wait for completion
static void indicate_startup_battery_and_wait(void) {
    indicate_startup_battery();
    // Wait for blink completion with timeout (max 5 seconds)
    if (!wait_for_blink_completion(5000)) {
        LOG_WRN("Battery indication timeout");
    }
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

    // レイヤー色を直接持続表示に設定（点滅なし）
    uint8_t layer_idx = zmk_keymap_highest_layer_active();
    if (layer_idx < LENGTH(LAYER_COLORS)) {
        LOG_INF("Changed to layer %d, setting color", layer_idx);
        struct blink_item blink = BLINK_STRUCT_PERSISTENT(
            STAY_ON, 1, LAYER_COLORS[layer_idx]
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
    
    LOG_INF("LED process thread started");
    
    while (true) {
        // wait until a blink item is received and process it
        struct blink_item blink;
        LOG_DBG("Waiting for blink item from msgq");
        k_msgq_get(&led_msgq, &blink, K_FOREVER);
        LOG_INF("Got blink item: repeats=%d, color=0x%x, persistent=%s", 
                blink.n_repeats, 
                blink.color.r << 16 | blink.color.g << 8 | blink.color.b,
                blink.is_persistent ? "yes" : "no");

        led_do_blink(blink);
        LOG_INF("Completed blink operation");

        // Signal completion of blink operation
        k_sem_give(&led_blink_complete_sem);
        LOG_DBG("Signaled blink completion");

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

    LOG_INF("LED init thread started");

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING) && \
    IS_ENABLED(CONFIG_INDICATOR_LED_SHOW_BATTERY_ON_BOOT)
    // 1回目: バッテリー残量表示
    LOG_INF("Starting battery indication sequence");
    indicate_startup_battery_and_wait();
    LOG_INF("Battery indication sequence completed");
#else
    LOG_INF("Battery indication is disabled");
#endif // IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)

#if IS_ENABLED(CONFIG_ZMK_BLE) && IS_ENABLED(CONFIG_INDICATOR_LED_SHOW_BLE)
    // 2回目: Bluetooth接続状態表示
    LOG_INF("Starting BLE indication sequence");
    indicate_ble_and_wait();
    LOG_INF("BLE indication sequence completed");
#else
    LOG_INF("BLE indication is disabled");
#endif // IS_ENABLED(CONFIG_ZMK_BLE)

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
    // 3回目: 現在のレイヤー色を持続表示に設定 (central側のみ)
    uint8_t layer_idx = zmk_keymap_highest_layer_active();
    if (layer_idx < LENGTH(LAYER_COLORS)) {
        LOG_INF("Setting initial layer color for layer %d", layer_idx);
        struct blink_item layer_blink = BLINK_STRUCT_PERSISTENT(
            STAY_ON, 1, LAYER_COLORS[layer_idx]
        );
        k_msgq_put(&led_msgq, &layer_blink, K_NO_WAIT);
    }
#else
    // peripheral側では消灯状態を設定
    struct blink_item layer_blink = BLINK_STRUCT_PERSISTENT(
        STAY_ON, 1, COLOR_OFF
    );
    k_msgq_put(&led_msgq, &layer_blink, K_NO_WAIT);
#endif

    initialized = true;
    LOG_INF("Finished initializing LED widget");
}

// run init thread on boot for initial battery+output checks
K_THREAD_DEFINE(led_init_tid, 1024, led_init_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO,
                0, 200);
