/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** Includes **/
#include "string.h"
#include "serial_ll_if.h"
#include "transport_drv.h"
#include "esp_hosted_transport.h"
#include "esp_hosted_header.h"
#include "port_esp_hosted_host_log.h"
#include "esp_hosted_log.h"

DEFINE_LOG_TAG(serial_ll);

/** Macros / Constants **/
#define MAX_SERIAL_INTF                   2
#define TO_SERIAL_INFT_QUEUE_SIZE         10

typedef enum {
	INIT,
	ACTIVE,
	DESTROY
} serial_ll_state_e;

static struct rx_data {
	int len;
	uint8_t *data;
} r;

/* data structures needed for serial driver */
static queue_handle_t to_serial_ll_intf_queue[MAX_SERIAL_INTF];
static serial_ll_handle_t * interface_handle_g[MAX_SERIAL_INTF] = {NULL};
static uint8_t conn_num = 0;

/** Function Declarations **/
static int       serial_ll_open    (serial_ll_handle_t *serial_ll_hdl);
static uint8_t * serial_ll_read    (const serial_ll_handle_t * serial_ll_hdl,
		uint16_t * rlen);
static int       serial_ll_write   (const serial_ll_handle_t * serial_ll_hdl,
		uint8_t * wbuffer, const uint16_t wlen);
static int       serial_ll_close   (serial_ll_handle_t * serial_ll_hdl);


/* define serial interface */
static struct serial_ll_operations serial_ll_fops = {
	.open    = serial_ll_open,
	.read    = serial_ll_read,
	.write   = serial_ll_write,
	.close   = serial_ll_close,
};

/** function definition **/

/** Local Functions **/

/**
  * @brief Open new Serial interface
  * @param  serial_ll_hdl - handle of serial interface
  * @retval 0 if success, -1 on failure
  */
static int serial_ll_open(serial_ll_handle_t *serial_ll_hdl)
{
	if (! serial_ll_hdl) {
		ESP_LOGE(TAG, "serial invalid hdr");
		return -1;
	}

	if (serial_ll_hdl->queue) {
		/* clean up earlier queue */
		ESP_LOGW(TAG, "Flush existing serial queue");
		g_h.funcs->_h_destroy_queue(serial_ll_hdl->queue);
	}

	/* Queue - serial rx */
	serial_ll_hdl->queue = g_h.funcs->_h_create_queue(TO_SERIAL_INFT_QUEUE_SIZE,
		sizeof(interface_buffer_handle_t));

	if (! serial_ll_hdl->queue) {
		serial_ll_close(serial_ll_hdl);
		return -1;
	}

	serial_ll_hdl->state  = ACTIVE;
	return 0;
}

/**
  * @brief Get serial handle for iface_num
  * @param  iface_num - serial connection number
  * @retval serial_ll_hdl - output handle of serial interface
  */
static serial_ll_handle_t * get_serial_ll_handle(const uint8_t iface_num)
{
	if ((iface_num < MAX_SERIAL_INTF) &&
		(interface_handle_g[iface_num]) &&
		(interface_handle_g[iface_num]->state == ACTIVE)) {

		return interface_handle_g[iface_num];
	}
	ESP_LOGD(TAG, "%s Failed to get interface handle", __func__);
	return NULL;
}

/**
  * @brief Close serial interface
  * @param  serial_ll_hdl - handle
  * @retval rbuffer - ready buffer read on serial interface
  */
static int serial_ll_close(serial_ll_handle_t * serial_ll_hdl)
{
	serial_ll_hdl->state = DESTROY;

	if (serial_ll_hdl->queue) {
		ESP_LOGI(TAG, "Clean-up serial queue");

		interface_buffer_handle_t buf_handle;
		while (g_h.funcs->_h_dequeue_item(serial_ll_hdl->queue, &buf_handle, 0) == 0) {
			/* Free buffer using the provided free function */
			if (buf_handle.priv_buffer_handle && buf_handle.free_buf_handle) {
				buf_handle.free_buf_handle(buf_handle.priv_buffer_handle);
			}
		}

		g_h.funcs->_h_destroy_queue(serial_ll_hdl->queue);
		serial_ll_hdl->queue = NULL;
	}

	/* reset connection */
	if (conn_num > 0) {
		interface_handle_g[--conn_num] = NULL;
	}

	if (serial_ll_hdl) {
		g_h.funcs->_h_free(serial_ll_hdl);
		serial_ll_hdl = NULL;
	}
	return 0;
}


/**
  * @brief  Serial interface read non blocking
  * @param  serial_ll_hdl - handle
  *         rlen - output param, number of bytes read
  * @retval rbuffer - ready buffer read on serial interface
  */
static uint8_t * serial_ll_read(const serial_ll_handle_t * serial_ll_hdl,
							 uint16_t * rlen)
{
	/* This is a non-blocking call */
	interface_buffer_handle_t buf_handle = {0};

	/* Initial value */
	*rlen = 0 ;

	/* check if serial interface valid */
	if ((! serial_ll_hdl) || (serial_ll_hdl->state != ACTIVE)) {
		ESP_LOGE(TAG, "serial invalid interface");
		return NULL;
	}

	/* This is **blocking** receive.
	 *
	 * Although not needed in normal circumstances,
	 * User can convert it to non blocking using below steps:
	 *
	 * To make it non blocking:
	 *   As an another design option, serial_rx_callback can also be
	 *   thought of incoming data indication, i.e. asynchronous rx
	 *   indication, which can be used by higher layer in separate
	 *   dedicated rx task to receive and process rx data.
	 *
	 * In our example, first approach of blocking read is used.
	 */
	ESP_LOGV(TAG, "before deQ for ll_read");
	if (g_h.funcs->_h_dequeue_item(serial_ll_hdl->queue, &buf_handle, HOSTED_BLOCK_MAX)) {
		ESP_LOGE(TAG, "serial queue recv failed ");
		return NULL;
	}
	ESP_LOGV(TAG, "after deQ for ll_read");

	/* proceed only if payload and length are sane */
	if (!buf_handle.payload || !buf_handle.payload_len) {
		ESP_LOGE(TAG, "%s: Dequeue result in empty buffer",__func__);
		return NULL;
	}

	*rlen = buf_handle.payload_len;

	ESP_HEXLOGV("ll_read", buf_handle.payload, buf_handle.payload_len, 32);

	return buf_handle.payload;
}

/**
  * @brief Serial interface write
  * @param  serial_ll_hdl - handle
  *         wlen - number of bytes to write
  *         wbuffer - buffer to send
  * @retval 0 on success, -1 on failure
  */
static int serial_ll_write(const serial_ll_handle_t * serial_ll_hdl,
	uint8_t * wbuffer, const uint16_t wlen)
{

	if ((! serial_ll_hdl) || (serial_ll_hdl->state != ACTIVE)) {
		ESP_LOGE(TAG, "serial invalid interface for write");
		return -1;
	}

	if (!wbuffer || !wlen) {
		return -1;
	}

	if (wlen > MAX_FRAGMENTABLE_PAYLOAD_SIZE) {
		ESP_LOGE(TAG, "Payload too large: %u bytes (max allowed: %u)", wlen, MAX_FRAGMENTABLE_PAYLOAD_SIZE);
		return -1;
	}

	uint16_t offset = 0;
	uint16_t remaining_len = wlen;
	void (*free_func)(void *) = NULL;
	uint8_t *buf_to_free = NULL;

	while (remaining_len > 0) {
		uint16_t frag_len = (remaining_len > MAX_PAYLOAD_SIZE) ? MAX_PAYLOAD_SIZE : remaining_len;
		uint8_t *frag_ptr = wbuffer + offset;

		uint8_t flags = 0;
		if (remaining_len > MAX_PAYLOAD_SIZE) {
			flags |= MORE_FRAGMENT;
		}
		else {
			// FRAGMENTATION COMPLETED
			buf_to_free = wbuffer;
			free_func = H_DEFLT_FREE_FUNC;
		}

		int ret = esp_hosted_tx(serial_ll_hdl->if_type,
					serial_ll_hdl->if_num,
					frag_ptr,
					frag_len,
					H_BUFF_NO_ZEROCOPY,
					buf_to_free, free_func,
					flags);
		if (ret != ESP_OK) {
			if (flags & MORE_FRAGMENT) {
				H_FREE_PTR_WITH_FUNC(H_DEFLT_FREE_FUNC, wbuffer);
			}
			ESP_LOGE(TAG, "esp_hosted_tx failed at offset=%u len=%u", offset, frag_len);
			return ret;
		}

		offset += frag_len;
		remaining_len -= frag_len;
	}

	return 0;
}

/**
  * @brief Serial rx handler is called by spi driver when there
  *        is incoming data with interface type is Serial.
  * @param  if_num - interface instance
  *         rxbuff - buffer from spi driver
  *         rx_len - size of rxbuff
  *         seq_num - serial sequence number
  *         flag_more_frags - Flags for fragmentation
  * @retval 0 on success, else failure
  */
int serial_ll_rx_handler(interface_buffer_handle_t * buf_handle)
{

#define SERIAL_ALLOC_REALLOC_RDATA() \
	do { \
		if(!r.data) { \
			r.data = (uint8_t *)g_h.funcs->_h_malloc(buf_handle->payload_len); \
		} else { \
			r.data = (uint8_t *)g_h.funcs->_h_realloc(r.data, r.len + buf_handle->payload_len); \
		} \
		if (!r.data) { \
			ESP_LOGE(TAG, "Failed to allocate serial data"); \
			goto serial_buff_cleanup; \
		} \
	} while(0);

	serial_ll_handle_t * serial_ll_hdl = NULL;
	uint8_t *serial_buf = NULL;
	interface_buffer_handle_t new_buf_handle = {0};

	/* Check valid handle and length */
	if (!buf_handle || !buf_handle->payload_len) {
		ESP_LOGE(TAG, "%s:%u Invalid parameters", __func__, __LINE__);
		goto serial_buff_cleanup;
	}

	serial_ll_hdl = get_serial_ll_handle(buf_handle->if_num);

	/* Is serial interface up */
	if ((! serial_ll_hdl) || (serial_ll_hdl->state != ACTIVE)) {
		ESP_LOGD(TAG, "Serial interface not active (possibly shutting down), discarding packet");
		goto serial_buff_cleanup_silent;
	}


	/* Accumulate fragments */
	if (buf_handle->flag & MORE_FRAGMENT) {

		ESP_LOGD(TAG, "Fragment!!!");
		SERIAL_ALLOC_REALLOC_RDATA();

		g_h.funcs->_h_memcpy((r.data + r.len), buf_handle->payload, buf_handle->payload_len);
		r.len += buf_handle->payload_len;
		return 0;
	}

	SERIAL_ALLOC_REALLOC_RDATA();

	/* No or last fragment */
	g_h.funcs->_h_memcpy((r.data + r.len), buf_handle->payload, buf_handle->payload_len);
	r.len += buf_handle->payload_len;

	serial_buf = (uint8_t *)g_h.funcs->_h_malloc(r.len);
	if(!serial_buf) {
		ESP_LOGE(TAG, "Malloc failed, drop pkt");
		goto serial_buff_cleanup;
	}
	g_h.funcs->_h_memcpy(serial_buf, r.data, r.len);

	/* form new buf handle for processing of serial msg */
	new_buf_handle.if_type = ESP_SERIAL_IF;
	new_buf_handle.if_num = buf_handle->if_num;
	new_buf_handle.payload_len = r.len;
	new_buf_handle.payload = serial_buf;
	new_buf_handle.priv_buffer_handle = serial_buf;
	new_buf_handle.free_buf_handle = g_h.funcs->_h_free;

	/* clear old buf handle */
	//H_FREE_PTR_WITH_FUNC(buf_handle->free_buf_handle, buf_handle->priv_buffer_handle);


	r.len = 0;
	g_h.funcs->_h_free(r.data);
	r.data = NULL;

	ESP_LOGV(TAG, "before ENQ for ll_read");
	/* send to serial queue */
	if (g_h.funcs->_h_queue_item(serial_ll_hdl->queue,
		    &new_buf_handle, HOSTED_BLOCK_MAX)) {
		ESP_LOGE(TAG, "Failed send serialif queue[%u]", new_buf_handle.if_num);
		goto serial_buff_cleanup;
	}
	ESP_LOGV(TAG, "after ENQ for ll_read");

	/* Indicate higher layer about data ready for consumption */
	if (serial_ll_hdl->serial_rx_callback) {
		(*serial_ll_hdl->serial_rx_callback) ();
	} else {
		goto serial_buff_cleanup;
	}

	return 0;

serial_buff_cleanup:
	ESP_LOGE(TAG, "Err occurred, discard current buffer");

serial_buff_cleanup_silent:
	/* Common cleanup path - used for both errors and expected shutdown */
	H_FREE_PTR_WITH_FUNC(buf_handle->free_buf_handle, buf_handle->priv_buffer_handle);

	r.len = 0;

	H_FREE_PTR_WITH_FUNC(new_buf_handle.free_buf_handle, new_buf_handle.priv_buffer_handle);

	g_h.funcs->_h_free(r.data);
	return -1;
}

/** Exported Functions **/

/**
  * @brief create and return new serial interface
  * @param  serial_rx_callback - callback to be invoked on rx data
  * @retval serial_ll_hdl - output handle of serial interface
  */
serial_ll_handle_t * serial_ll_init(void(*serial_rx_callback)(void))
{
	serial_ll_handle_t  * serial_ll_hdl = NULL;

	/* Check if more serial interfaces be created */
	if ((conn_num+1) < MAX_SERIAL_INTF) {

		serial_ll_hdl = (serial_ll_handle_t *)g_h.funcs->_h_malloc(sizeof(serial_ll_handle_t));
		if (! serial_ll_hdl) {
			ESP_LOGE(TAG, "Serial interface - malloc failed");
			return NULL;
		}

		serial_ll_hdl->if_type = ESP_SERIAL_IF;
		serial_ll_hdl->if_num  = conn_num;
		serial_ll_hdl->queue   = to_serial_ll_intf_queue[conn_num];
		serial_ll_hdl->state   = INIT;
		serial_ll_hdl->fops    = &serial_ll_fops;
		serial_ll_hdl->serial_rx_callback   = serial_rx_callback;
		interface_handle_g[conn_num] = serial_ll_hdl;
		conn_num++;

	} else {
		ESP_LOGE(TAG, "Number of serial interface connections overflow");
		return NULL;
	}

	return serial_ll_hdl;
}
