/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*prevent recursive inclusion */
#ifndef __SERIAL_DRV_H
#define __SERIAL_DRV_H

#ifdef __cplusplus
extern "C" {
#endif

/** includes **/
#include "serial_ll_if.h"

/** Exported Functions **/
/*
 * rpc_platform_init function initializes the rpc
 * path data structures
 * Input parameter
 *      None
 * Returns
 *      SUCCESS(0) or FAILURE(-1) of above operation
 */
int rpc_platform_init(void);

/*
 * rpc_platform_deinit function cleans up the rpc
 * path library data structure
 * Input parameter
 *      None
 * Returns
 *      SUCCESS(0) or FAILURE(-1) of above operation
 */
int rpc_platform_deinit(void);

/*
 * serial_drv_open function opens driver interface.
 *
 * Input parameter
 *      transport                   :   Pointer to transport driver
 * Returns
 *      serial_drv_handle           :   Driver Handle
 */
struct serial_drv_handle_t* serial_drv_open (const char* transport);

/*
 * serial_drv_write function writes in_count bytes
 * from buffer to driver interface
 *
 * Input parameter
 *      serial_drv_handle           :   Driver Handler
 *      buf                         :   Data Buffer (Data written from buf to
 *                                      driver interface)
 *      in_count                    :   Number of Bytes to be written
 * Output parameter
 *      out_count                   :   Number of Bytes written
 *
 * Returns
 *      SUCCESS(0) or FAILURE(-1) of above operation
 */
int serial_drv_write (struct serial_drv_handle_t* serial_drv_handle,
     uint8_t* buf, int in_count, int* out_count);

/*
 * serial_drv_read function gets buffer from serial driver
 * after TLV parsing. output buffer is protobuf encoded
 *
 * Input parameter
 *      serial_drv_handle           :   Driver Handle
 * Output parameter
 *      out_nbyte                   :   Size of TLV parsed buffer
 * Returns
 *      buf                         :   Protocol encoded data Buffer
 *                                      caller will decode the protobuf
 */

uint8_t * serial_drv_read(struct serial_drv_handle_t *serial_drv_handle,
		uint32_t *out_nbyte);

/*
 * serial_drv_close function closes driver interface.
 *
 * Input parameter
 *      serial_drv_handle           :   Driver Handle
 * Returns
 *      SUCCESS(0) or FAILURE(-1) of above operation
 */

int serial_drv_close (struct serial_drv_handle_t** serial_drv_handle);


#ifdef __cplusplus
}
#endif

#endif
