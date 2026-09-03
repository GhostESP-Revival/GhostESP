/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Wrapper interfaces for SPI to communicated with slave using SDIO */

#ifndef __PORT_ESP_HOSTED_HOST_SPI_H_
#define __PORT_ESP_HOSTED_HOST_SPI_H_

#define MAX_TRANSPORT_BUFFER_SIZE        MAX_SPI_BUFFER_SIZE
/* Hosted SPI init function
 * returns a pointer to the spi context */
void * hosted_spi_init(void);

/* Hosted SPI deinit function */
int hosted_spi_deinit(void *handle);

/* Hosted SPI transfer function */
int hosted_do_spi_transfer(void *trans);

#endif
