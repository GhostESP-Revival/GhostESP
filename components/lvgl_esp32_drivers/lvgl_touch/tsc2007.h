/**
 * @file tsc2007.h
 */

#ifndef TSC2007_H
#define TSC2007_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

void tsc2007_init(void);
bool tsc2007_read(lv_indev_drv_t *drv, lv_indev_data_t *data);

#ifdef __cplusplus
}
#endif

#endif /* TSC2007_H */
