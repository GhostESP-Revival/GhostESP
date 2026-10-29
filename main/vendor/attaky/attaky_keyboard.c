/* Control: Keyboard (Bottom) 1.0 - two AW9523B 5x5 matrices.
 *
 * Key tables below are transcribed verbatim from the Attaky Keyboard (Bottom)
 * datasheet, which the public hardware catalog agrees with cell for cell:
 *
 *   left half  @ 0x5A                 right half @ 0x5B
 *   +-----+-----+-----+-----+-----+   +-----+-----+-----+-----+-----+
 *   |  1  |  2  |  3  |  4  |  5  |   |  6  |  7  |  8  |  9  |  0  |
 *   |  Q  |  W  |  E  |  R  |  T  |   |  Y  |  U  |  I  |  O  |  P  |
 *   |  A  |  S  |  D  |  F  |  G  |   |  H  |  J  |  K  |  L  | DEL |
 *   |SHIFT|  Z  |  X  |  C  |  V  |   |  B  |  N  |  M  |   - | ENT |
 *   | LANG| TAB | ALT |  ,  | SPC |   |  -  |  .  | MIC |  #  | FN  |
 *   +-----+-----+-----+-----+-----+   +-----+-----+-----+-----+-----+
 *
 * The two '-' cells on the right half (ROW3 x COL3, ROW4 x COL0) are routed
 * but have no physical key; they are marked in the table and ignored.
 *
 * There is no Caps Lock key on this module, so shift is a held modifier only.
 */

#include "vendor/attaky/attaky_keyboard.h"

#if defined(CONFIG_ATTAKY_TARGET) && defined(CONFIG_ATTAKY_HAS_KEYBOARD)

#include <string.h>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_shared.h"
#include "managers/display_manager.h"
#include "vendor/attaky/attaky_board.h"
#include "vendor/m5/m5_keyboard_def.h"

static const char *TAG = "ATTAKY_KB";

#define KB_I2C_HZ         400000
#define KB_I2C_TIMEOUT_MS 100
#define KB_ADDR_LEFT      0x5A
#define KB_ADDR_RIGHT     0x5B

/* AW9523B registers used by the scanner. */
#define AW9523_REG_INPUT_P0    0x00
#define AW9523_REG_OUTPUT_P1   0x03
#define AW9523_REG_CONFIG_P0   0x04
#define AW9523_REG_CONFIG_P1   0x05
#define AW9523_REG_LEDMODE_P0  0x12
#define AW9523_REG_LEDMODE_P1  0x13
#define AW9523_REG_SOFT_RESET  0x7F
/* Constant-current dimming (AW9523B "Table 13"): the DIM registers are not in
 * pin order - pins 12-15 map to 0x2C-0x2F. P17/P16/P15 are pins 15/14/13. */
#define AW9523_REG_DIM_RED     0x2F   /* pin 15 = P17 */
#define AW9523_REG_DIM_GREEN   0x2E   /* pin 14 = P16 */
#define AW9523_REG_DIM_BLUE    0x2D   /* pin 13 = P15 */
#define AW9523_ID_VALUE        0x23
#define AW9523_REG_ID          0x10

/* P1 = P10 rows (bits 0-4, driven low to select) + P15/P16/P17 RGB (bits
 * 5-7, active low). Columns P00-P04 are inputs with the module's own
 * pull-ups; there is no internal pull-up on the AW9523.
 *
 * The three LED bits are held LOW and left in the expander's constant-current
 * mode: a sinking pin lights its channel and the brightness comes from the DIM
 * registers, so "off" is DIM = 0, not a high output bit. Holding them low is
 * also what lets attaky_rgb light this LED at all - every scan pass rewrites
 * this port for the rows, so a high LED bit here would switch the zone LED back
 * off several times a millisecond. */
#define KB_ROWS_IDLE      0x1F   /* rows high, LED sinks enabled           */
#define KB_ROWS_ALL_LOW   0x00   /* presence test: every row low           */
#define KB_LEDMODE_P1     0x1F   /* bits 0-4 rows stay GPIO, 5-7 sink      */
#define KB_COLUMN_MASK    0x1F   /* COL0..COL4 on P0 */
#define KB_ROW_COUNT      5
#define KB_COL_COUNT      5

/* A cell with no physical key. */
#define KB_NONE 0xFE

/* Physical-modifier markers (resolved at scan time, never emitted as text). */
#define KB_SHIFT 0xFD
#define KB_ALT   0xFC
#define KB_LANG  0xFB
#define KB_FN    0xFA
#define KB_MIC   0xF9

static const uint8_t kLeftKeys[KB_ROW_COUNT][KB_COL_COUNT] = {
    {'1', '2', '3', '4', '5'},
    {'q', 'w', 'e', 'r', 't'},
    {'a', 's', 'd', 'f', 'g'},
    {KB_SHIFT, 'z', 'x', 'c', 'v'},
    {KB_LANG, KEY_TAB, KB_ALT, ',', ' '},
};

static const uint8_t kRightKeys[KB_ROW_COUNT][KB_COL_COUNT] = {
    {'6', '7', '8', '9', '0'},
    {'y', 'u', 'i', 'o', 'p'},
    {'h', 'j', 'k', 'l', KEY_BACKSPACE},
    {'b', 'n', 'm', KB_NONE, KEY_ENTER},
    {KB_NONE, '.', KB_MIC, '#', KB_FN},
};

/* Standard US QWERTY shifted digits. The module prints these on the keycaps;
 * other punctuation keys emit their base legend. */
static const char kShiftedDigits[26] = ")!@#$%^&*(";   /* index 1..10 -> '!'..'(' */

typedef struct {
    uint8_t addr;
    const uint8_t (*keys)[KB_COL_COUNT];
    uint8_t last_raw[KB_ROW_COUNT];                  /* previous column byte per row */
    uint8_t held[KB_ROW_COUNT][KB_COL_COUNT];        /* value emitted on press */
    uint8_t held_count;                             /* cells reported as down */
    bool shift_down;
} kb_half_t;

static kb_half_t s_halves[2] = {
    {.addr = KB_ADDR_LEFT, .keys = kLeftKeys},
    {.addr = KB_ADDR_RIGHT, .keys = kRightKeys},
};

static i2c_master_bus_handle_t s_bus = NULL;
static TaskHandle_t s_task = NULL;
static bool s_active = false;

static esp_err_t kb_write(uint8_t addr, uint8_t reg, uint8_t val)
{
    uint8_t payload[2] = {reg, val};
    return i2c_shared_transmit_to_addr(s_bus, addr, KB_I2C_HZ, payload,
                                       sizeof(payload), KB_I2C_TIMEOUT_MS);
}

static esp_err_t kb_read(uint8_t addr, uint8_t reg, uint8_t *val)
{
    return i2c_shared_transmit_receive_from_addr(s_bus, addr, KB_I2C_HZ, &reg, 1,
                                                 val, 1, KB_I2C_TIMEOUT_MS);
}

static void kb_push_key(uint8_t value, bool pressed)
{
    InputEvent ev = {0};
    ev.type = INPUT_TYPE_KEYBOARD;
    ev.data.key_value = value;
    ev.is_touch_move = !pressed;   /* upstream convention: set = release */
    if (input_queue == NULL) {
        return;
    }
    if (xQueueSend((QueueHandle_t)input_queue, &ev, 0) != pdTRUE) {
        ESP_LOGD(TAG, "input queue full, dropped key 0x%02X", value);
    }
}

/* Resolve a matrix cell to the value to emit, applying the held shift state. */
static uint8_t kb_resolve(const kb_half_t *half, uint8_t cell, bool shift)
{
    if (cell == KB_NONE) {
        return 0;
    }
    if (cell >= 'a' && cell <= 'z') {
        return shift ? (uint8_t)(cell - ('a' - 'A')) : cell;
    }
    if (shift && cell >= '1' && cell <= '9') {
        return (uint8_t)kShiftedDigits[cell - '0'];
    }
    if (shift && cell == '0') {
        return (uint8_t)kShiftedDigits[10];
    }
    switch (cell) {
        case KB_SHIFT: return KEY_LEFT_SHIFT;
        case KB_ALT:   return KEY_LEFT_ALT;
        default:       return cell;   /* letters, digits and KEY_* codes as-is */
    }
}

/* Drive all rows low and look for any pulled-down column: one write + one
 * read, used to skip full scans while nobody is typing. */
static bool kb_presence(kb_half_t *half)
{
    if (kb_write(half->addr, AW9523_REG_OUTPUT_P1, KB_ROWS_ALL_LOW) != ESP_OK) {
        return false;
    }
    esp_rom_delay_us(60);
    uint8_t cols = 0;
    if (kb_read(half->addr, AW9523_REG_INPUT_P0, &cols) != ESP_OK) {
        return false;
    }
    return (cols & KB_COLUMN_MASK) != KB_COLUMN_MASK;
}

/* Full 5x5 scan: one row driven low at a time, columns read back. */
static void kb_scan_half(kb_half_t *half, bool *any_pressed)
{
    for (int row = 0; row < KB_ROW_COUNT; row++) {
        const uint8_t drive = (uint8_t)(KB_ROWS_IDLE & ~(1u << row));
        if (kb_write(half->addr, AW9523_REG_OUTPUT_P1, drive) != ESP_OK) {
            continue;
        }
        /* 10k pull-up on ~25pF trace settles in well under this. */
        esp_rom_delay_us(60);

        uint8_t cols = KB_COLUMN_MASK;
        if (kb_read(half->addr, AW9523_REG_INPUT_P0, &cols) != ESP_OK) {
            continue;
        }
        const uint8_t raw = (uint8_t)(cols & KB_COLUMN_MASK);
        const uint8_t changed = (uint8_t)(raw ^ half->last_raw[row]);
        half->last_raw[row] = raw;

        if (changed == 0) {
            continue;
        }
        if ((raw & KB_COLUMN_MASK) != KB_COLUMN_MASK) {
            *any_pressed = true;
        }

        for (int col = 0; col < KB_COL_COUNT; col++) {
            const uint8_t bit = (uint8_t)(1u << col);
            if ((changed & bit) == 0) {
                continue;
            }
            const bool pressed = (raw & bit) == 0;   /* active low */
            const uint8_t cell = half->keys[row][col];
            if (cell == KB_NONE) {
                continue;
            }
            /* The value is resolved once, on press, and cached until the
             * release: if shift changes while a key is held, the release must
             * still carry the value the press announced or views would keep
             * that key stuck. */
            if (pressed) {
                uint8_t value;
                if (cell == KB_SHIFT) {
                    half->shift_down = true;
                    value = KEY_LEFT_SHIFT;
                } else if (cell == KB_ALT) {
                    value = KEY_LEFT_ALT;
                } else if (cell == KB_LANG || cell == KB_FN || cell == KB_MIC) {
                    /* No key code for these in the shared input contract yet;
                     * they are logged so a later feature can claim them. */
                    ESP_LOGD(TAG, "modifier key 0x%02X pressed on 0x%02X",
                             cell, half->addr);
                    continue;
                } else {
                    value = kb_resolve(half, cell, half->shift_down);
                }
                if (value == 0) {
                    continue;
                }
                half->held[row][col] = value;
                if (half->held_count < 0xFF) {
                    half->held_count++;
                }
                kb_push_key(value, true);
            } else {
                const uint8_t value = half->held[row][col];
                half->held[row][col] = 0;
                if (cell == KB_SHIFT) {
                    half->shift_down = false;
                }
                if (value == 0) {
                    continue;
                }
                if (half->held_count > 0) {
                    half->held_count--;
                }
                kb_push_key(value, false);
            }
        }
    }
    /* Back to idle: all rows high, LEDs off. */
    kb_write(half->addr, AW9523_REG_OUTPUT_P1, KB_ROWS_IDLE);
}

static esp_err_t kb_configure_half(kb_half_t *half)
{
    esp_err_t ret = kb_write(half->addr, AW9523_REG_SOFT_RESET, 0x00);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
    /* Park the port high before touching anything else: a soft reset leaves the
     * output register cleared, and P15-P17 low in GPIO mode would light all
     * three channels of the zone LED at full current. */
    if (kb_write(half->addr, AW9523_REG_OUTPUT_P1, 0xFF) != ESP_OK) {
        ESP_LOGW(TAG, "0x%02X: idle write failed", half->addr);
    }
    /* Zero the dimming registers *before* the mode switch: they become the
     * brightness the moment the pins turn into current sinks, and their
     * power-on value is not zero. */
    if (kb_write(half->addr, AW9523_REG_DIM_RED, 0) != ESP_OK ||
        kb_write(half->addr, AW9523_REG_DIM_GREEN, 0) != ESP_OK ||
        kb_write(half->addr, AW9523_REG_DIM_BLUE, 0) != ESP_OK) {
        ESP_LOGW(TAG, "0x%02X: LED dimming init failed", half->addr);
    }
    /* P0 columns stay plain GPIO inputs; on P1 the five rows stay GPIO and the
     * three LED pins join the constant-current bank. */
    if (kb_write(half->addr, AW9523_REG_LEDMODE_P0, 0xFF) != ESP_OK ||
        kb_write(half->addr, AW9523_REG_LEDMODE_P1, KB_LEDMODE_P1) != ESP_OK) {
        ESP_LOGW(TAG, "0x%02X: LEDMODE write failed", half->addr);
    }
    /* P0 = columns (inputs), P1 = rows + LED (outputs). */
    if (kb_write(half->addr, AW9523_REG_CONFIG_P0, 0xFF) != ESP_OK ||
        kb_write(half->addr, AW9523_REG_CONFIG_P1, 0x00) != ESP_OK) {
        ESP_LOGE(TAG, "0x%02X: direction write failed", half->addr);
        return ESP_FAIL;
    }
    if (kb_write(half->addr, AW9523_REG_OUTPUT_P1, KB_ROWS_IDLE) != ESP_OK) {
        ESP_LOGE(TAG, "0x%02X: idle write failed", half->addr);
        return ESP_FAIL;
    }

    uint8_t id = 0;
    if (kb_read(half->addr, AW9523_REG_ID, &id) == ESP_OK && id != AW9523_ID_VALUE) {
        ESP_LOGW(TAG, "0x%02X: unexpected expander ID 0x%02X", half->addr, id);
    }

    /* Seed the previous-state cache so a key already held at boot does not
     * look like a fresh press on the first scan. */
    memset(half->last_raw, 0xFF, sizeof(half->last_raw));
    memset(half->held, 0, sizeof(half->held));
    half->held_count = 0;
    uint8_t cols = KB_COLUMN_MASK;
    if (kb_write(half->addr, AW9523_REG_OUTPUT_P1, KB_ROWS_ALL_LOW) == ESP_OK) {
        esp_rom_delay_us(60);
        if (kb_read(half->addr, AW9523_REG_INPUT_P0, &cols) == ESP_OK) {
            memset(half->last_raw, cols & KB_COLUMN_MASK, sizeof(half->last_raw));
        }
    }
    kb_write(half->addr, AW9523_REG_OUTPUT_P1, KB_ROWS_IDLE);
    return ESP_OK;
}

static void attaky_keyboard_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(CONFIG_ATTAKY_KEYBOARD_SCAN_MS);
    /* Adaptive polling (per the AW9523 driver notes): once typing stops, only
     * run the cheap presence test until a key shows up again. */
    bool presence_mode = false;
    uint16_t idle_cycles = 0;
    const uint16_t idle_threshold = (uint16_t)(3000 / CONFIG_ATTAKY_KEYBOARD_SCAN_MS);

    while (1) {
        bool any_pressed = false;
        bool need_full_scan = true;

        /* The presence test only sees keys that are *down* - a release looks
         * exactly like an idle bus. So as long as a cell is still reported as
         * held, the full scan has to keep running; skipping it would leave the
         * cell pressed forever, and display_manager's held-key repeat would go
         * on re-injecting that key with nothing left to cancel it (one press of
         * 'h' typing a whole row of them, backspace wiping a field). */
        bool any_held = false;
        for (size_t i = 0; i < sizeof(s_halves) / sizeof(s_halves[0]); i++) {
            if (s_halves[i].held_count > 0) {
                any_held = true;
                break;
            }
        }

        if (presence_mode && !any_held) {
            need_full_scan = false;
            for (size_t i = 0; i < sizeof(s_halves) / sizeof(s_halves[0]); i++) {
                if (kb_presence(&s_halves[i])) {
                    need_full_scan = true;
                    break;
                }
            }
        }

        if (need_full_scan) {
            for (size_t i = 0; i < sizeof(s_halves) / sizeof(s_halves[0]); i++) {
                kb_scan_half(&s_halves[i], &any_pressed);
            }
            if (any_pressed) {
                idle_cycles = 0;
            } else if (++idle_cycles >= idle_threshold) {
                presence_mode = true;
                idle_cycles = 0;
            }
        }
        vTaskDelay(period);
    }
}

esp_err_t attaky_keyboard_init(void)
{
    if (s_active) {
        return ESP_OK;
    }
    s_bus = (i2c_master_bus_handle_t)attaky_board_i2c_bus();
    if (s_bus == NULL) {
        ESP_LOGE(TAG, "shared I2C bus not available");
        return ESP_ERR_INVALID_STATE;
    }
    if (!attaky_board_module_present(ATTAKY_MODULE_KEYBOARD)) {
        ESP_LOGI(TAG, "no Keyboard module attached; skipping keyboard driver");
        return ESP_ERR_NOT_FOUND;
    }

    int configured = 0;
    for (size_t i = 0; i < sizeof(s_halves) / sizeof(s_halves[0]); i++) {
        s_halves[i].shift_down = false;
        if (kb_configure_half(&s_halves[i]) == ESP_OK) {
            configured++;
        } else {
            ESP_LOGE(TAG, "half @ 0x%02X failed to configure", s_halves[i].addr);
        }
    }
    if (configured == 0) {
        return ESP_FAIL;
    }

    if (xTaskCreate(attaky_keyboard_task, "attaky_kb", 4096, NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to start keyboard scan task");
        return ESP_ERR_NO_MEM;
    }
    s_active = true;
    ESP_LOGI(TAG, "Keyboard module ready: 2x 5x5 matrices @ 0x5A/0x5B, %d ms scan",
             CONFIG_ATTAKY_KEYBOARD_SCAN_MS);
    return ESP_OK;
}

bool attaky_keyboard_active(void)
{
    return s_active;
}

#endif /* CONFIG_ATTAKY_TARGET && CONFIG_ATTAKY_HAS_KEYBOARD */
