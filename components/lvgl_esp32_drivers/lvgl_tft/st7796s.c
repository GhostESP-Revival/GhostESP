/**
 * @file st7796s.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "st7796s.h"
#if defined(CONFIG_BANSHEE_LITE_C5) && defined(CONFIG_USE_C5_PARLIO_DISPLAY)
#include "banshee_c5_parlio.h"
#endif
#include "disp_spi.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_idf_version.h"
#if ESP_IDF_VERSION <= ESP_IDF_VERSION_VAL(5,0,0)
#include "rom/gpio.h"
#endif

/*********************
 *      DEFINES
 *********************/
#if defined(CONFIG_BANSHEE_LITE_C5)
#define TAG "ST7796U"
#else
#define TAG "ST7796S"
#endif

/**********************
 *      TYPEDEFS
 **********************/

/*The LCD needs a bunch of command/argument values to be initialized. They are stored in this struct. */
typedef struct
{
	uint8_t cmd;
	uint8_t data[16];
	uint8_t databytes; //No of data in data; bit 7 = delay after set; 0xFF = end of cmds.
} lcd_init_cmd_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void st7796s_set_orientation(uint8_t orientation);

static void st7796s_send_cmd(uint8_t cmd);
static void st7796s_send_data(void *data, uint16_t length);
#if defined(CONFIG_BANSHEE_LITE_C5) && defined(CONFIG_USE_C5_PARLIO_DISPLAY)
static esp_err_t st7796s_send_color(void *data, uint16_t length,
                                    lv_disp_drv_t *drv);
#else
static void st7796s_send_color(void *data, uint16_t length);
#endif

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void st7796s_init(void)
{
#if defined(CONFIG_BANSHEE_LITE_C5)
	/* The Banshee Lite panel is an ST7796U.  Its command unlock and power
	 * setup differ from the legacy ILI9341-compatible table below. */
	lcd_init_cmd_t init_cmds[] = {
		{0x01, {0}, 0x80},	/* Software reset */
		{0x11, {0}, 0x80},	/* Exit sleep */
		{0xF0, {0xC3}, 1},	/* Enable extension command set, part I */
		{0xF0, {0x96}, 1},	/* Enable extension command set, part II */
		{0x36, {0x48}, 1},	/* Memory access: portrait, RGB/BGR panel order */
		{0x3A, {0x55}, 1},	/* RGB565 */
		{0xB4, {0x01}, 1},	/* One-dot inversion */
		{0xB6, {0x80, 0x02, 0x3B}, 3},	/* Display function */
		{0xE8, {0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33}, 8},
		{0xC1, {0x06}, 1},	/* Power control 2 */
		{0xC2, {0xA7}, 1},	/* Power control 3 */
		{0xC5, {0x18}, 1},	/* VCOM control */
		{0xE0, {0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F, 0x54,
				0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B}, 14},
		{0xE1, {0xE0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2B, 0x43,
				0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B}, 14},
		{0xF0, {0x3C}, 1},	/* Disable extension command set, part I */
		{0xF0, {0x69}, 1},	/* Disable extension command set, part II */
		{0x29, {0}, 0x80},	/* Display on */
		{0, {0}, 0xff},
	};
#else
	lcd_init_cmd_t init_cmds[] = {
		{0xCF, {0x00, 0x83, 0X30}, 3},
		{0xED, {0x64, 0x03, 0X12, 0X81}, 4},
		{0xE8, {0x85, 0x01, 0x79}, 3},
		{0xCB, {0x39, 0x2C, 0x00, 0x34, 0x02}, 5},
		{0xF7, {0x20}, 1},
		{0xEA, {0x00, 0x00}, 2},
		{0xC0, {0x26}, 1},		 /*Power control*/
		{0xC1, {0x11}, 1},		 /*Power control */
		{0xC5, {0x35, 0x3E}, 2}, /*VCOM control*/
		{0xC7, {0xBE}, 1},		 /*VCOM control*/
		{0x36, {0x28}, 1},		 /*Memory Access Control*/
		{0x3A, {0x55}, 1},		 /*Pixel Format Set*/
		{0xB1, {0x00, 0x1B}, 2},
		{0xF2, {0x08}, 1},
		{0x26, {0x01}, 1},
		{0xE0, {0x1F, 0x1A, 0x18, 0x0A, 0x0F, 0x06, 0x45, 0X87, 0x32, 0x0A, 0x07, 0x02, 0x07, 0x05, 0x00}, 15},
		{0XE1, {0x00, 0x25, 0x27, 0x05, 0x10, 0x09, 0x3A, 0x78, 0x4D, 0x05, 0x18, 0x0D, 0x38, 0x3A, 0x1F}, 15},
#if defined(CONFIG_BANSHEE_LITE_C5)
		{0x2A, {0x00, 0x00, 0x01, 0x3F}, 4},
		{0x2B, {0x00, 0x00, 0x01, 0xDF}, 4},
#else
		{0x2A, {0x00, 0x00, 0x00, 0xEF}, 4},
		{0x2B, {0x00, 0x00, 0x01, 0x3f}, 4},
#endif
		{0x2C, {0}, 0},
		{0xB7, {0x07}, 1},
		{0xB6, {0x0A, 0x82, 0x27, 0x00}, 4},
		{0x11, {0}, 0x80},
		{0x29, {0}, 0x80},
		{0, {0}, 0xff},
	};
#endif

	//Initialize non-SPI GPIOs
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5,0,0)
    gpio_pad_select_gpio(ST7796S_DC);
#else
    esp_rom_gpio_pad_select_gpio(ST7796S_DC);
#endif
	gpio_set_direction(ST7796S_DC, GPIO_MODE_OUTPUT);

#if ST7796S_USE_RST
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5,0,0)
    gpio_pad_select_gpio(ST7796S_RST);
#else
    esp_rom_gpio_pad_select_gpio(ST7796S_RST);
#endif
	gpio_set_direction(ST7796S_RST, GPIO_MODE_OUTPUT);

	//Reset the display
	gpio_set_level(ST7796S_RST, 0);
	vTaskDelay(100 / portTICK_PERIOD_MS);
	gpio_set_level(ST7796S_RST, 1);
	vTaskDelay(100 / portTICK_PERIOD_MS);
#endif

	ESP_LOGI(TAG, "Initialization.");

	//Send all the commands
	uint16_t cmd = 0;
	while (init_cmds[cmd].databytes != 0xff)
	{
		st7796s_send_cmd(init_cmds[cmd].cmd);
		st7796s_send_data(init_cmds[cmd].data, init_cmds[cmd].databytes & 0x1F);
		if (init_cmds[cmd].databytes & 0x80)
		{
			vTaskDelay(100 / portTICK_PERIOD_MS);
		}
		cmd++;
	}

	st7796s_set_orientation(CONFIG_LV_DISPLAY_ORIENTATION);

#if defined(CONFIG_BANSHEE_LITE_C5) || ST7796S_INVERT_COLORS == 1
	st7796s_send_cmd(0x21);
#else
	st7796s_send_cmd(0x20);
#endif
}

void st7796s_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
	uint8_t data[4];

	/*Column addresses*/
	st7796s_send_cmd(0x2A);
	data[0] = (area->x1 >> 8) & 0xFF;
	data[1] = area->x1 & 0xFF;
	data[2] = (area->x2 >> 8) & 0xFF;
	data[3] = area->x2 & 0xFF;
	st7796s_send_data(data, 4);

	/*Page addresses*/
	st7796s_send_cmd(0x2B);
	data[0] = (area->y1 >> 8) & 0xFF;
	data[1] = area->y1 & 0xFF;
	data[2] = (area->y2 >> 8) & 0xFF;
	data[3] = area->y2 & 0xFF;
	st7796s_send_data(data, 4);

	/*Memory write*/
	st7796s_send_cmd(0x2C);

	uint32_t size = lv_area_get_width(area) * lv_area_get_height(area);

#if defined(CONFIG_BANSHEE_LITE_C5) && defined(CONFIG_USE_C5_PARLIO_DISPLAY)
	(void)st7796s_send_color((void *)color_map, size * 2, drv);
#else
	st7796s_send_color((void *)color_map, size * 2);
#endif
}

void st7796s_sleep_in()
{
	uint8_t data[] = {0x08};
	st7796s_send_cmd(0x10);
	st7796s_send_data(&data, 1);
}

void st7796s_sleep_out()
{
	uint8_t data[] = {0x08};
	st7796s_send_cmd(0x11);
	st7796s_send_data(&data, 1);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void st7796s_send_cmd(uint8_t cmd)
{
#if defined(CONFIG_BANSHEE_LITE_C5) && defined(CONFIG_USE_C5_PARLIO_DISPLAY)
	(void)banshee_c5_parlio_send_cmd(cmd);
#else
	disp_wait_for_pending_transactions();
	gpio_set_level(ST7796S_DC, 0); /*Command mode*/
	disp_spi_send_data(&cmd, 1);
#endif
}

static void st7796s_send_data(void *data, uint16_t length)
{
#if defined(CONFIG_BANSHEE_LITE_C5) && defined(CONFIG_USE_C5_PARLIO_DISPLAY)
	(void)banshee_c5_parlio_send_data(data, length);
#else
	disp_wait_for_pending_transactions();
	gpio_set_level(ST7796S_DC, 1); /*Data mode*/
	disp_spi_send_data(data, length);
#endif
}

#if defined(CONFIG_BANSHEE_LITE_C5) && defined(CONFIG_USE_C5_PARLIO_DISPLAY)
static esp_err_t st7796s_send_color(void *data, uint16_t length,
                                    lv_disp_drv_t *drv)
{
	return banshee_c5_parlio_send_color(data, length, drv);

}
#else
static void st7796s_send_color(void *data, uint16_t length)
{
	disp_wait_for_pending_transactions();
	gpio_set_level(ST7796S_DC, 1); /*Data mode*/
	disp_spi_send_colors(data, length);
}
#endif

static void st7796s_set_orientation(uint8_t orientation)
{
	// ESP_ASSERT(orientation < 4);

	const char *orientation_str[] = {
		"PORTRAIT", "PORTRAIT_INVERTED", "LANDSCAPE", "LANDSCAPE_INVERTED"};

	ESP_LOGI(TAG, "Display orientation: %s", orientation_str[orientation]);

#if defined CONFIG_LV_PREDEFINED_DISPLAY_M5STACK
	uint8_t data[] = {0x68, 0x68, 0x08, 0x08};
#elif defined(CONFIG_LV_PREDEFINED_DISPLAY_WROVER4)
	uint8_t data[] = {0x4C, 0x88, 0x28, 0xE8};
#elif defined(CONFIG_LV_PREDEFINED_DISPLAY_WT32_SC01)
	uint8_t data[] = {0x48, 0x88, 0x28, 0xE8};
#elif defined(CONFIG_LV_PREDEFINED_DISPLAY_NONE)
	uint8_t data[] = {0x48, 0x88, 0x28, 0xE8};
#endif

	ESP_LOGI(TAG, "0x36 command value: 0x%02X", data[orientation]);

	st7796s_send_cmd(0x36);
	st7796s_send_data((void *)&data[orientation], 1);
}
