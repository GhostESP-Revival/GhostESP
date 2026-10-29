/**
 * @file st7789.c
 *
 * Mostly taken from lbthomsen/esp-idf-littlevgl github.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"

#include "st7789.h"

#include "disp_spi.h"
#include "driver/gpio.h"
#include "esp_idf_version.h"
#if ESP_IDF_VERSION <= ESP_IDF_VERSION_VAL(5,0,0)
#include "rom/gpio.h"
#endif

/*********************
 *      DEFINES
 *********************/
#define TAG "st7789"
/**********************
 *      TYPEDEFS
 **********************/

/*The LCD needs a bunch of command/argument values to be initialized. They are stored in this struct. */
typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t databytes; //No of data in data; bit 7 = delay after set; 0xFF = end of cmds.
} lcd_init_cmd_t;

/* MADCTL the panel is actually driven with. The orientation table below is the
 * only thing that decides it on the Attaky - there is no on-device tuner and no
 * stored override. */
static uint8_t s_madctl_applied = 0;

/* Attaky Cyber Deck Plus: the glass is mounted 180 degrees from the byte that
 * renders this UI correctly, so every byte gets MX+MY toggled on its way to the
 * controller. Kept as a plain bring-up constant rather than a Kconfig symbol so
 * turning it off is a one-line edit here. Display-side only: the touch mapping
 * in ft6x36.c needs no matching change - a both-axis mirror there was measured
 * wrong on the glass and was removed. */
#define ATTAKY_PANEL_FLIP_180 1

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void st7789_set_orientation(uint8_t orientation);

static void st7789_send_color(void *data, size_t length);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void st7789_init(void)
{
    static const lcd_init_cmd_t st7789_clean_init_cmds[] = {
        {ST7789_SLPOUT, {0}, 0x80},
        {ST7789_NORON, {0}, 0},
        {ST7789_MADCTL, {0x00}, 1},
        {ST7789_COLMOD, {0x55}, 1},
        {ST7789_RAMCTRL, {0x00, 0xF0}, 2},
        {ST7789_PORCTRL, {0x0c, 0x0c, 0x00, 0x33, 0x33}, 5},
        {ST7789_GCTRL, {0x75}, 1},
        {ST7789_VCOMS, {0x1a}, 1},
        {ST7789_LCMCTRL, {0x2c}, 1},
        {ST7789_VDVVRHEN, {0x01}, 1},
        {ST7789_VRHS, {0x13}, 1},
        {ST7789_VDVSET, {0x20}, 1},
        {ST7789_FRCTR2, {0x0f}, 1},
        {ST7789_PWCTRL1, {0xa4, 0xa1}, 2},
        {ST7789_PVGAMCTRL, {0xd0, 0x0D, 0x14, 0x0D, 0x0D, 0x09, 0x38, 0x44, 0x4E, 0x3A, 0x17, 0x18, 0x2F, 0x30}, 14},
        {ST7789_NVGAMCTRL, {0xd0, 0x09, 0x0F, 0x08, 0x07, 0x14, 0x37, 0x44, 0x4D, 0x38, 0x15, 0x16, 0x2C, 0x3E}, 14},
        {ST7789_INVON, {0}, 0},
        {ST7789_CASET, {0x00, 0x00, 0x00, 0xEF}, 4},
        {ST7789_RASET, {0x00, 0x00, 0x01, 0x3F}, 4},
        {ST7789_RAMWR, {0}, 0},
        {ST7789_DISPON, {0}, 0x80},
        {0, {0}, 0xff},
    };

    static const lcd_init_cmd_t st7789_generic_init_cmds[] = {
        {0xCF, {0x00, 0x83, 0X30}, 3},
        {0xED, {0x64, 0x03, 0X12, 0X81}, 4},
        {ST7789_PWCTRL2, {0x85, 0x01, 0x79}, 3},
        {0xCB, {0x39, 0x2C, 0x00, 0x34, 0x02}, 5},
        {0xF7, {0x20}, 1},
        {0xEA, {0x00, 0x00}, 2},
        {ST7789_LCMCTRL, {0x26}, 1},
        {ST7789_IDSET, {0x11}, 1},
        {ST7789_VCMOFSET, {0x35, 0x3E}, 2},
        {ST7789_CABCCTRL, {0xBE}, 1},
        {ST7789_COLMOD, {0x55}, 1},
#if ST7789_INVERT_COLORS == 1
		{ST7789_INVON, {0}, 0}, // set inverted mode
#else
 		{ST7789_INVOFF, {0}, 0}, // set non-inverted mode
#endif
        {ST7789_RGBCTRL, {0x00, 0x1B}, 2},
        {0xF2, {0x08}, 1},
        {ST7789_GAMSET, {0x01}, 1},
        {ST7789_PVGAMCTRL, {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32, 0x44, 0x42, 0x06, 0x0E, 0x12, 0x14, 0x17}, 14},
        {ST7789_NVGAMCTRL, {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31, 0x54, 0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E}, 14},
        {ST7789_CASET, {0x00, 0x00, 0x00, 0xEF}, 4},
        {ST7789_RASET, {0x00, 0x00, 0x01, 0x3f}, 4},
        {ST7789_RAMWR, {0}, 0},
        {ST7789_GCTRL, {0x07}, 1},
        {0xB6, {0x0A, 0x82, 0x27, 0x00}, 4},
        {ST7789_SLPOUT, {0}, 0x80},
        {0, {0}, 0xff},
    };

    bool use_clean_st7789_init = false;
    bool nm_cyd_c5_panel = false;
#ifdef CONFIG_USE_TDECK
    use_clean_st7789_init = true;
#elif defined(CONFIG_CROWPANEL_ADVANCE_SMALL_SPI_LCD)
    /* Elecrow's factory driver uses the normal ST7789 command set, inverted
     * colors and no hardware reset line on both the 2.4 and 2.8-inch boards. */
    use_clean_st7789_init = true;
#elif defined(CONFIG_BUILD_CONFIG_TEMPLATE)
    nm_cyd_c5_panel = strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "NM-CYD-C5") == 0;
    use_clean_st7789_init = nm_cyd_c5_panel;
#endif

    const lcd_init_cmd_t *st7789_init_cmds = use_clean_st7789_init
        ? st7789_clean_init_cmds
        : st7789_generic_init_cmds;

    //Initialize non-SPI GPIOs
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5,0,0)
    gpio_pad_select_gpio(ST7789_DC);
#else
    esp_rom_gpio_pad_select_gpio(ST7789_DC);
#endif
    gpio_set_direction(ST7789_DC, GPIO_MODE_OUTPUT);

#if !defined(ST7789_SOFT_RST)
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5,0,0)
    gpio_pad_select_gpio(ST7789_RST);
#else
    esp_rom_gpio_pad_select_gpio(ST7789_RST);
#endif
    gpio_set_direction(ST7789_RST, GPIO_MODE_OUTPUT);
#endif

    //Reset the display
#if !defined(ST7789_SOFT_RST)
    gpio_set_level(ST7789_RST, 0);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    gpio_set_level(ST7789_RST, 1);
    vTaskDelay(100 / portTICK_PERIOD_MS);
#else
    st7789_send_cmd(ST7789_SWRESET);
    vTaskDelay(120 / portTICK_PERIOD_MS);
#endif

    printf("ST7789 initialization.\n");

    //Send all the commands
    uint16_t cmd = 0;
    while (st7789_init_cmds[cmd].databytes!=0xff) {
        st7789_send_cmd(st7789_init_cmds[cmd].cmd);
        st7789_send_data(st7789_init_cmds[cmd].data, st7789_init_cmds[cmd].databytes&0x1F);
        if (st7789_init_cmds[cmd].databytes & 0x80) {
                vTaskDelay(100 / portTICK_PERIOD_MS);
        }
        cmd++;
    }

    st7789_set_orientation(CONFIG_LV_DISPLAY_ORIENTATION);
    if (nm_cyd_c5_panel) {
        st7789_send_cmd(ST7789_INVOFF);
        st7789_send_cmd(ST7789_DISPON);
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
    if (!use_clean_st7789_init) {
        st7789_send_cmd(ST7789_DISPON);
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
}

/* The ST7789 display controller can drive up to 320*240 displays, when using a 240*240 or 240*135
 * displays there's a gap of 80px or 40/52/53px respectively. 52px or 53x offset depends on display orientation.
 * We need to edit the coordinates to take into account those gaps, this is not necessary in all orientations. */
void st7789_flush(lv_disp_drv_t * drv, const lv_area_t * area, lv_color_t * color_map)
{
    uint8_t data[4] = {0};

    uint16_t offsetx1 = area->x1;
    uint16_t offsetx2 = area->x2;
    uint16_t offsety1 = area->y1;
    uint16_t offsety2 = area->y2;

#if (CONFIG_LV_TFT_DISPLAY_OFFSETS)
    offsetx1 += CONFIG_LV_TFT_DISPLAY_X_OFFSET;
    offsetx2 += CONFIG_LV_TFT_DISPLAY_X_OFFSET;
    offsety1 += CONFIG_LV_TFT_DISPLAY_Y_OFFSET;
    offsety2 += CONFIG_LV_TFT_DISPLAY_Y_OFFSET;

#elif (LV_HOR_RES_MAX == 240) && (LV_VER_RES_MAX == 240)
    #if (CONFIG_LV_DISPLAY_ORIENTATION_PORTRAIT)
        offsetx1 += 80;
        offsetx2 += 80;
    #elif (CONFIG_LV_DISPLAY_ORIENTATION_LANDSCAPE_INVERTED)
        offsety1 += 80;
        offsety2 += 80;
    #endif
#elif (LV_HOR_RES_MAX == 240) && (LV_VER_RES_MAX == 135)
    #if (CONFIG_LV_DISPLAY_ORIENTATION_PORTRAIT) || \
        (CONFIG_LV_DISPLAY_ORIENTATION_PORTRAIT_INVERTED)
        offsetx1 += 40;
        offsetx2 += 40;
        offsety1 += 53;
        offsety2 += 53;
    #endif
#elif (LV_HOR_RES_MAX == 135) && (LV_VER_RES_MAX == 240)
    #if (CONFIG_LV_DISPLAY_ORIENTATION_LANDSCAPE) || \
        (CONFIG_LV_DISPLAY_ORIENTATION_LANDSCAPE_INVERTED)
        offsetx1 += 52;
        offsetx2 += 52;
        offsety1 += 40;
        offsety2 += 40;
    #endif
#elif (LV_HOR_RES_MAX == 320) && (LV_VER_RES_MAX == 170) // 1.9 inch 170×320 LCD, physically landscape
    #if (CONFIG_LV_DISPLAY_ORIENTATION_PORTRAIT) || (CONFIG_LV_DISPLAY_ORIENTATION_PORTRAIT_INVERTED)
        offsety1 += 35;
        offsety2 += 35;
    #endif
#elif (LV_HOR_RES_MAX == 170) && (LV_VER_RES_MAX == 320) // 1.9 inch 170×320 LCD, physically vertical
    #if (CONFIG_LV_DISPLAY_ORIENTATION_LANDSCAPE) || (CONFIG_LV_DISPLAY_ORIENTATION_LANDSCAPE_INVERTED)
        offsetx1 += 35;
        offsetx2 += 35;
    #endif
#endif
    /*Column addresses*/
    st7789_send_cmd(ST7789_CASET);
    data[0] = (offsetx1 >> 8) & 0xFF;
    data[1] = offsetx1 & 0xFF;
    data[2] = (offsetx2 >> 8) & 0xFF;
    data[3] = offsetx2 & 0xFF;
    st7789_send_data(data, 4);

    /*Page addresses*/
    st7789_send_cmd(ST7789_RASET);
    data[0] = (offsety1 >> 8) & 0xFF;
    data[1] = offsety1 & 0xFF;
    data[2] = (offsety2 >> 8) & 0xFF;
    data[3] = offsety2 & 0xFF;
    st7789_send_data(data, 4);

    /*Memory write*/
    st7789_send_cmd(ST7789_RAMWR);

    size_t size = (size_t)lv_area_get_width(area) * (size_t)lv_area_get_height(area);

    st7789_send_color((void*)color_map, size * 2);

}

/**********************
 *   STATIC FUNCTIONS
 **********************/
void st7789_send_cmd(uint8_t cmd)
{
    disp_wait_for_pending_transactions();
    gpio_set_level(ST7789_DC, 0);
    disp_spi_send_data(&cmd, 1);
}

void st7789_send_data(const void * data, uint16_t length)
{
    disp_wait_for_pending_transactions();
    gpio_set_level(ST7789_DC, 1);
    disp_spi_send_data((uint8_t *)data, length);
}

static void st7789_send_color(void * data, size_t length)
{
    disp_wait_for_pending_transactions();
    gpio_set_level(ST7789_DC, 1);
    disp_spi_send_colors(data, length);
}

static void st7789_set_orientation(uint8_t orientation)
{
    // ESP_ASSERT(orientation < 4);

    const char *orientation_str[] = {
        "PORTRAIT", "PORTRAIT_INVERTED", "LANDSCAPE", "LANDSCAPE_INVERTED"
    };

    ESP_LOGI(TAG, "Display orientation: %s", orientation_str[orientation]);

    uint8_t data[] =
    {
#if defined(CONFIG_ATTAKY_TARGET)
    /* Attaky Core's 2.0-inch ST7789 (240x320 glass, 320x240 UI frame). Two
     * readings of the same panel existed and only one of them survives the
     * hardware:
     *
     *   - The vendor's own bring-up drives it as swap_xy + mirror_y, i.e.
     *     MADCTL 0xA0 (the esp_lcd equivalent of the Arduino TFT_eSPI
     *     setRotation(3) their notes quote for this 180-degree mount).
     *   - On this unit, driven by THIS driver with a 320x240 LVGL frame,
     *     0xA0 is what put the whole UI 90 degrees out and unreadable: MV
     *     addresses the GRAM along the 240px axis while LVGL pushes
     *     320-wide rows. That is the failure ili9341.c documents for the
     *     CoreS3's native-landscape ILI9342C.
     *
     * So the MV-clear family is what this panel is driven with: 0x00 for the
     * profile's landscape frame, corrected to 0xC0 (MX|MY) on the wire by the
     * 180-degree mount flip in st7789_set_madctl(). That pair was confirmed
     * upright on the glass, so it is simply the value the panel gets - earlier
     * builds carried an on-device orientation tuner to find it, and that is
     * gone now that the answer is known.
     *
     * Colour order is a separate bit (D3, RGB/BGR) and the glass has answered
     * it: driven as 0xC8 (D3 set, "BGR") the panel drew red as blue and blue
     * as red - unmistakable in the terminal, where errors and the prompt sit
     * side by side. This panel is RGB-order, so D3 stays clear.
     *
     * MV-clear family (this table, RGB order): 0x00 = none (index 2, the
     * profile's LANDSCAPE), 0x40 = MX, 0xC0 = MX|MY, 0x80 = MY.
     * MV-set family (portrait-addressed panel): 0xA0 = MY|MV, 0x60 = MX|MV.
     * Portrait entries keep the stock values; this profile is landscape. */
    0xC0, 0x00, 0x00, 0xC0
#elif defined(CONFIG_CROWPANEL_ADVANCE_SMALL_SPI_LCD)
    /* Match Elecrow's ESP-IDF LVGL port: swap_xy=true, mirror_x=false,
     * mirror_y=true, RGB element order.  The previous 0xB8 also enabled
     * BGR and ML, which produced incorrect colors and panel orientation. */
    0x00, 0xC0, 0xA0, 0x60
#elif CONFIG_LV_PREDEFINED_DISPLAY_TTGO
	0x60, 0xA0, 0x00, 0xC0
#else
	0xC0, 0x00, 0x60, 0xA0
#endif
    };

    uint8_t madctl = data[orientation];

    st7789_set_madctl(madctl);
}

void st7789_set_madctl(uint8_t madctl)
{
    /* The orientation table is the only caller, which makes this the one place
     * the Attaky's extra 180-degree mount correction belongs. Toggling MX and
     * MY rotates the frame by 180 degrees whatever the rest of the byte says
     * (MV and the RGB/BGR bit are untouched), so the table stays readable as
     * "the byte for this orientation" and the panel still gets the mount fix. */
#if defined(CONFIG_ATTAKY_TARGET) && ATTAKY_PANEL_FLIP_180
    madctl ^= 0xC0;
#endif

    /* Send the byte straight through: the controller applies it to the next
     * frame, and st7789_flush() re-asserts CASET/RASET on every flush, so a
     * live change while the UI keeps drawing is safe. */
    st7789_send_cmd(ST7789_MADCTL);
    st7789_send_data(&madctl, 1);
    s_madctl_applied = madctl;
    ESP_LOGI(TAG, "MADCTL (0x36) = 0x%02X", madctl);
}

uint8_t st7789_get_madctl(void)
{
    return s_madctl_applied;
}
