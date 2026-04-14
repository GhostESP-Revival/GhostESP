/*----------------------------------------------------------------------------/
  Lovyan GFX - Graphics library for embedded devices.

Original Source:
 https://github.com/lovyan03/LovyanGFX/

Licence:
 [FreeBSD](https://github.com/lovyan03/LovyanGFX/blob/master/license.txt)

Author:
 [lovyan03](https://twitter.com/lovyan03)

Contributors:
 [ciniml](https://github.com/ciniml)
 [mongonta0716](https://github.com/mongonta0716)
 [tobozo](https://github.com/tobozo)
/----------------------------------------------------------------------------*/

#include "Bus_EPD.h"
#if SOC_LCD_I80_SUPPORTED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_lcd_panel_ops.h>

#include "../common.hpp"


namespace lgfx
{
 inline namespace v1
 {

//----------------------------------------------------------------------------
bool IRAM_ATTR Bus_EPD::notify_line_done(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
  auto me = (Bus_EPD*)user_ctx;
  me->_bus_busy = false;
  return false;
}

void Bus_EPD::wait(void) { while (_bus_busy) taskYIELD(); }

void Bus_EPD::beginTransaction(void)
{
  auto ckv = _config.pin_ckv;
  auto spv = _config.pin_spv;
  while (_bus_busy) taskYIELD();

  lgfx::gpio_hi(ckv);  lgfx::delayMicroseconds(1);
  lgfx::gpio_lo(spv);  lgfx::delayMicroseconds(1); //100ns
  lgfx::gpio_lo(ckv);  lgfx::delayMicroseconds(1); //0.5us
  lgfx::gpio_hi(ckv);  lgfx::delayMicroseconds(1); //100ns
  lgfx::gpio_hi(spv);  lgfx::delayMicroseconds(1); //0.5us
  lgfx::gpio_lo(ckv);  lgfx::delayMicroseconds(1); //0.5us
  lgfx::gpio_hi(ckv);  lgfx::delayMicroseconds(1); //0.5us
  lgfx::gpio_lo(ckv);  lgfx::delayMicroseconds(1); //0.5us
  lgfx::gpio_hi(ckv);  lgfx::delayMicroseconds(1); //0.5us
  lgfx::gpio_lo(ckv);  lgfx::delayMicroseconds(1); //0.5us
  lgfx::gpio_hi(ckv);  lgfx::delayMicroseconds(1); //0.5us
  lgfx::gpio_lo(ckv);
}

void Bus_EPD::endTransaction(void)
{
  while (_bus_busy) taskYIELD();
}

void Bus_EPD::scanlineDone(void)
{
  auto ckv = _config.pin_ckv;
  auto le = _config.pin_le;
  while (_bus_busy) taskYIELD();

  lgfx::gpio_lo(ckv); lgfx::gpio_lo(ckv);
  lgfx::gpio_hi(le);  lgfx::gpio_hi(le);
  lgfx::gpio_lo(le);
}

bool Bus_EPD::powerControl(bool flg_on)
{
  if (_pwr_on != flg_on) {
    _pwr_on = flg_on;
    wait();
    if (flg_on) {
      lgfx::gpio_hi(_config.pin_oe);
      lgfx::delayMicroseconds(100);
      lgfx::gpio_hi(_config.pin_pwr);
      lgfx::delayMicroseconds(100);
      lgfx::gpio_hi(_config.pin_spv);
      lgfx::gpio_hi(_config.pin_sph);
      lgfx::delay(1);
    } else {
      lgfx::delay(1);
      lgfx::gpio_lo(_config.pin_pwr);
      lgfx::delayMicroseconds(10);
      lgfx::gpio_lo(_config.pin_oe);
      lgfx::delayMicroseconds(100);
      lgfx::gpio_lo(_config.pin_spv);
    }
  }
  return true;
}

void Bus_EPD::writeScanLine(const uint8_t *data, uint32_t length)
{
  wait();
  _bus_busy = true;
  lgfx::gpio_hi(_config.pin_ckv);

  // background transfer start
  esp_lcd_panel_io_tx_color(_io_handle, -1, data, length);
}

bool Bus_EPD::init(void)
{
  _bus_busy = false;
  _pwr_on = false;

  lgfx::pinMode(_config.pin_spv, lgfx::pin_mode_t::output);
  lgfx::pinMode(_config.pin_ckv, lgfx::pin_mode_t::output);
  lgfx::pinMode(_config.pin_sph, lgfx::pin_mode_t::output);
  lgfx::pinMode(_config.pin_oe, lgfx::pin_mode_t::output);
  lgfx::pinMode(_config.pin_le, lgfx::pin_mode_t::output);
  lgfx::pinMode(_config.pin_cl, lgfx::pin_mode_t::output);

  esp_lcd_i80_bus_config_t bus_config;
  memset(&bus_config, 0, sizeof(esp_lcd_i80_bus_config_t));

  bus_config.max_transfer_bytes = 1024;
  bus_config.clk_src = LCD_CLK_SRC_PLL160M;
  bus_config.dc_gpio_num = (gpio_num_t)_config.pin_pwr; //<= dummy setting.
  bus_config.wr_gpio_num = (gpio_num_t)_config.pin_cl;
  bus_config.bus_width = _config.bus_width;
  for (int i = 0; i < _config.bus_width; ++i) {
    bus_config.data_gpio_nums[i] = (gpio_num_t)_config.pin_data[i];
  }   
  if (ESP_OK != esp_lcd_new_i80_bus(&bus_config, &_i80_bus_handle))
  { return false; }

  lgfx::pinMode(_config.pin_pwr, lgfx::pin_mode_t::output);

  esp_lcd_panel_io_i80_config_t io_config;
  memset(&io_config, 0, sizeof(esp_lcd_panel_io_i80_config_t));
  io_config.trans_queue_depth = 4;
  io_config.on_color_trans_done = notify_line_done;
  io_config.user_ctx = this;
  io_config.lcd_cmd_bits = 8;
  io_config.lcd_param_bits = 8;
  io_config.pclk_hz = _config.bus_speed;
  io_config.cs_gpio_num = (gpio_num_t)_config.pin_sph;

  if (ESP_OK != esp_lcd_new_panel_io_i80(_i80_bus_handle, &io_config, &_io_handle))
  { return false; }

  return true;
}

//----------------------------------------------------------------------------
 }
}

#endif
