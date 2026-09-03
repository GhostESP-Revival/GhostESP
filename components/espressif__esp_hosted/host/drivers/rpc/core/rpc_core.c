/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "rpc_core.h"
#include "esp_hosted_rpc.h"
#include "serial_if.h"
#include "serial_drv.h"
#include <unistd.h>
#include "esp_task.h"
#include "port_esp_hosted_host_config.h"
#include "port_esp_hosted_host_log.h"
#include "esp_hosted_rpc.pb-c.h"

static const char *TAG = "rpc_core";


#define RPC_LIB_STATE_INACTIVE      0
#define RPC_LIB_STATE_INIT          1
#define RPC_LIB_STATE_READY         2


struct rpc_lib_context {
	int state;
};

typedef void (*rpc_rx_ind_t)(void);
typedef void (*rpc_tx_ind_t)(void);

static queue_handle_t rpc_rx_q = NULL;
static queue_handle_t rpc_tx_q = NULL;

static void * rpc_rx_thread_hdl;
static void * rpc_tx_thread_hdl;
static void * rpc_tx_sem;
static struct rpc_lib_context rpc_lib_ctxt;

/* uid to link between requests and responses */
/* uids are incrementing values from 1 onwards.
 * 0 means not a valid id */
static uint32_t uid = 0;

/* structures used to keep track of response semaphores
 * and callbacks via their uid */
typedef struct {
	uint32_t uid;
	void * sem;
} sync_rsp_t;

typedef struct {
	uint32_t uid;
	rpc_rsp_cb_t cb;
	void * timer_hdl;
	ctrl_cmd_t *app_req;  /* Store request so we can free it on response or timeout */
} async_rsp_t;

/* rpc response callbacks
 * These will be updated per rpc request received
 * 1. If application wants to use synchrounous, i.e. Wait till the response received
 *    after current rpc request is sent or timeout occurs,
 *    application will pass this callback in request as NULL.
 * 2. If application wants to use `asynchronous`, i.e. Just send the request and
 *    unblock for next processing, application will assign function pointer in
 *    rpc request, which will be registered here.
 *    When the response comes, the this registered callback function will be called
 *    with input as response
 */
#define MAX_SYNC_RPC_TRANSACTIONS  H_MAX_SYNC_RPC_REQUESTS
#define MAX_ASYNC_RPC_TRANSACTIONS H_MAX_ASYNC_RPC_REQUESTS

static sync_rsp_t sync_rsp_table[MAX_SYNC_RPC_TRANSACTIONS] = { 0 };
static async_rsp_t async_rsp_table[MAX_ASYNC_RPC_TRANSACTIONS] = { 0 };

/* rpc event callbacks
 * These will be updated when user registers event callback
 * using `set_event_callback` API
 * 1. If application does not register event callback,
 *    Events received from ESP32 will be dropped
 * 2. If application registers event callback,
 *    and when registered event is received from ESP32,
 *    event callback will be called asynchronously
 */
static rpc_evt_cb_t rpc_evt_cb_table[RPC_ID__Event_Max - RPC_ID__Event_Base] = { NULL };


static int call_event_callback(ctrl_cmd_t *app_event);
static int is_async_resp_callback_available(ctrl_cmd_t *app_resp);
static int is_sync_resp_sem_available(uint32_t uid);
static int clear_async_resp_callback(async_rsp_t *async_rsp_item);
static int call_async_resp_callback(ctrl_cmd_t *app_resp);
static int set_async_resp_callback(ctrl_cmd_t *app_req, rpc_rsp_cb_t resp_cb, void *timer_hdl);
static int set_sync_resp_sem(ctrl_cmd_t *app_req);
static int wait_for_sync_response(ctrl_cmd_t *app_req);
static void rpc_async_timeout_handler(void *arg);
static int post_sync_resp_sem(ctrl_cmd_t *app_resp);


/* Open serial interface
 * This function may fail if the ESP32 kernel module is not loaded
 **/
static int serial_init(void)
{
	if (transport_pserial_open()) {
		return FAILURE;
	}
	return SUCCESS;
}

/* close serial interface */
static int serial_deinit(void)
{
	if (transport_pserial_close()) {
		return FAILURE;
	}
	return SUCCESS;
}

static inline void set_rpc_lib_state(int state)
{
	rpc_lib_ctxt.state = state;
}

static inline int is_rpc_lib_ready(void)
{
	if (rpc_lib_ctxt.state >= RPC_LIB_STATE_READY)
		return 1;
	return 0;
}

static inline int is_rpc_lib_inactive(void)
{
	if (rpc_lib_ctxt.state == RPC_LIB_STATE_INACTIVE)
		return 1;
	return 0;
}

/* RPC TX indication */
static void rpc_tx_ind(void)
{
	ESP_LOGV(TAG, "posting rpc tx semaphore");
	g_h.funcs->_h_post_semaphore(rpc_tx_sem);
}

/* Returns CALLBACK_AVAILABLE if a non NULL RPC event
 * callback is available. It will return failure -
 *     MSG_ID_OUT_OF_ORDER - if request msg id is unsupported
 *     CALLBACK_NOT_REGISTERED - if aync callback is not available
 **/
int is_event_callback_registered(int event)
{
	int event_cb_tbl_idx = event - RPC_ID__Event_Base;

	if ((event<=RPC_ID__Event_Base) || (event>=RPC_ID__Event_Max)) {
		ESP_LOGW(TAG, "Could not identify event[%u]", event);
		return MSG_ID_OUT_OF_ORDER;
	}

	if (rpc_evt_cb_table[event_cb_tbl_idx]) {
		ESP_LOGV(TAG, "event id [0x%x]: callback %p", event, rpc_evt_cb_table[event_cb_tbl_idx]);
		return CALLBACK_AVAILABLE;
	}
	ESP_LOGD(TAG, "event id [0x%x]: No callback available", event);

	return CALLBACK_NOT_REGISTERED;
}


static int process_rpc_tx_msg(ctrl_cmd_t *app_req)
{
	Rpc   req = {0};
	uint32_t  tx_len = 0;
	uint8_t  *tx_data = NULL;
	int       ret = SUCCESS;
	int32_t   failure_status = 0;

	req.msg_type = RPC_TYPE__Req;

	/* 1. Protobuf msg init */
	rpc__init(&req);

	req.msg_id = app_req->msg_id;
	req.uid = app_req->uid;
	ESP_LOGI(TAG, "<-- RPC_Req  [0x%x], uid %ld", app_req->msg_id, app_req->uid);
	/* payload case is exact match to msg id in esp_hosted_config.pb-c.h */
	req.payload_case = (Rpc__PayloadCase) app_req->msg_id;

	if (compose_rpc_req(&req, app_req, &failure_status)) {
		ESP_LOGE(TAG, "compose_rpc_req failed for [0x%x]", app_req->msg_id);
		goto fail_req;
	}

	/* 3. Protobuf msg size */
	tx_len = rpc__get_packed_size(&req);
	if (!tx_len) {
		ESP_LOGE(TAG, "Invalid tx length");
		failure_status = RPC_ERR_PROTOBUF_ENCODE;
		goto fail_req;
	}

	/* 4. Allocate protobuf msg */
	HOSTED_CALLOC(uint8_t, tx_data, tx_len, fail_req0);

	/* 5. Assign response callback, if valid */
	if (app_req->rpc_rsp_cb) {

		/* 5.1 Start timeout for response for async only
		* For sync procedures, g_h.funcs->_h_get_semaphore takes care to
		* handle timeout situations */
		ESP_LOGI(TAG, "starting async resp timer for req[%u]",req.msg_id);
		void *timer_hdl = g_h.funcs->_h_timer_start("rpc_async_timeout_timer", SEC_TO_MILLISEC(app_req->rsp_timeout_sec), H_TIMER_TYPE_ONESHOT,
				rpc_async_timeout_handler, app_req);
		if (!timer_hdl) {
			ESP_LOGE(TAG, "Failed to start async resp timer");
			goto fail_req;
		}

		/* 5.2 set async resp callback */
		ESP_LOGD(TAG, "setting async resp callback for req[%u]",req.msg_id);
		ret = set_async_resp_callback(app_req, app_req->rpc_rsp_cb, timer_hdl);
		if (ret < 0) {
			ESP_LOGE(TAG, "could not set callback for req[%u]",req.msg_id);
			failure_status = RPC_ERR_SET_ASYNC_CB;
			goto fail_req;
		}
	}

	/* 7. Pack in protobuf and send the request */
	rpc__pack(&req, tx_data);
	ESP_LOGD(TAG, "sending rpc req[%u]",req.msg_id);
	if (transport_pserial_send(tx_data, tx_len)) {
		ESP_LOGE(TAG, "Send RPC req[0x%x] failed",req.msg_id);
		failure_status = RPC_ERR_TRANSPORT_SEND;
		goto fail_req;
	}

	ESP_LOGD(TAG, "Sent RPC_Req[0x%x]",req.msg_id);


	/* 8. Free hook for application */
	H_FREE_PTR_WITH_FUNC(app_req->app_free_buff_func, app_req->app_free_buff_hdl);

	/* 9. Cleanup */
	HOSTED_FREE(tx_data);
	RPC_FREE_BUFFS();
	return SUCCESS;
fail_req0:
	failure_status = RPC_ERR_MEMORY_FAILURE;
fail_req:


	ESP_LOGW(TAG, "fail1");
	if (app_req->rpc_rsp_cb) {
		/* 11. In case of async procedure,
		 * Let application know of failure using callback itself
		 **/
		ctrl_cmd_t *app_resp = NULL;

		HOSTED_CALLOC(ctrl_cmd_t, app_resp, sizeof(ctrl_cmd_t), fail_req2);

		app_resp->msg_type = RPC_TYPE__Resp;
		app_resp->msg_id = (app_req->msg_id - RPC_ID__Req_Base + RPC_ID__Resp_Base);
		app_resp->resp_event_status = failure_status;

		/* 12. In async procedure, it is important to get
		 * some kind of acknowledgement to user when
		 * request is already sent and success return to the caller
		 */
		app_req->rpc_rsp_cb(app_resp);
	} else {
		/* for sync procedure, put failed response into receive queue
		 * so application is aware of transmit failure.
		 * Prevents timeout waiting for a response that will never come
		 * as request was never sent
		 */
		ESP_LOGW(TAG, "RPC Sync proc failed");

		ctrl_cmd_t *app_resp = NULL;

		HOSTED_CALLOC(ctrl_cmd_t, app_resp, sizeof(ctrl_cmd_t), fail_req2);

		app_resp->msg_type = RPC_TYPE__Resp;
		app_resp->msg_id = (app_req->msg_id - RPC_ID__Req_Base + RPC_ID__Resp_Base);
		app_resp->resp_event_status = failure_status;

		// same as process_rpc_rx_msg() for failed condition
		esp_queue_elem_t elem = {0};
		elem.buf = app_resp;
		elem.buf_len = sizeof(ctrl_cmd_t);

		if (g_h.funcs->_h_queue_item(rpc_rx_q, &elem, HOSTED_BLOCK_MAX)) {
			ESP_LOGE(TAG, "RPC Q put fail");
		} else if (CALLBACK_AVAILABLE == is_sync_resp_sem_available(app_resp->uid)) {
			ESP_LOGV(TAG, "trigger semaphore to react to failed message uid %ld", app_resp->uid);
			post_sync_resp_sem(app_resp);
		} else {
			ESP_LOGE(TAG, "no sync resp callback to react to failed message uid %ld", app_resp->uid);
		}
	}

fail_req2:
	ESP_LOGW(TAG, "fail2");
	/* 13. Cleanup */
	H_FREE_PTR_WITH_FUNC(app_req->app_free_buff_func, app_req->app_free_buff_hdl);

	HOSTED_FREE(tx_data);
	RPC_FREE_BUFFS();

	/* app_req lifecycle on failure:
	 * - Async requests: Check if registered in callback table
	 *   - If registered: Will be freed on timeout or cleanup
	 *   - If not registered (early failure): Free here
	 * - Sync requests: NEVER free here! The calling application is waiting
	 *   and will free it in rpc_wait_and_parse_sync_resp() after getting
	 *   the error response we just queued. */
	if (app_req->rpc_rsp_cb) {
		/* Async request - check if it was registered */
		int found = 0;
		for (int i = 0; i < MAX_ASYNC_RPC_TRANSACTIONS; i++) {
			if (async_rsp_table[i].uid == app_req->uid) {
				found = 1;
				break;
			}
		}
		if (!found) {
			/* Not registered, free it here */
			HOSTED_FREE(app_req);
		}
		/* If found, it will be freed by timeout or cleanup */
	}
	/* Sync requests: Don't free - caller owns it */

	return FAILURE;
}

/* Process RPC msg (response or event) received from ESP32 */
static int process_rpc_rx_msg(Rpc * proto_msg, rpc_rx_ind_t rpc_rx_func)
{
	esp_queue_elem_t elem = {0};
	ctrl_cmd_t *app_resp = NULL;
	ctrl_cmd_t *app_event = NULL;

	/* 1. Check if valid proto msg */
	if (!proto_msg) {
		return FAILURE;
	}

	/* Note: free proto_msg at the end of processing*/

	/* 2. Check if it is event msg */
	if (proto_msg->msg_type == RPC_TYPE__Event) {
		/* Events are handled only asynchronously */
		ESP_LOGD(TAG, "Received Event [0x%x]", proto_msg->msg_id);
		/* check if callback is available.
		 * if not, silently drop the msg */
		if (CALLBACK_AVAILABLE ==
				is_event_callback_registered(proto_msg->msg_id)) {
			/* if event callback is registered, we need to
			 * parse the event into app structs and
			 * call the registered callback function
			 **/

			/* Allocate app struct for event */

			HOSTED_CALLOC(ctrl_cmd_t, app_event, sizeof(ctrl_cmd_t), free_buffers);

			/* Decode protobuf buffer of event and
			 * copy into app structures */
			if (rpc_parse_evt(proto_msg, app_event)) {
				ESP_LOGE(TAG, "failed to parse event");
				goto free_buffers;
			}

			/* callback to registered function */
			call_event_callback(app_event);
		} else
			goto free_buffers;

	/* 3. Check if it is response msg */
	} else if (proto_msg->msg_type == RPC_TYPE__Resp) {
		ESP_LOGD(TAG, "Received Resp [0x%x]", proto_msg->msg_id);
		/* RPC responses are handled asynchronously and
		 * asynchronpusly */

		/* Allocate app struct for response */
		HOSTED_CALLOC(ctrl_cmd_t, app_resp, sizeof(ctrl_cmd_t), free_buffers);

		/* Decode protobuf buffer of response and
		 * copy into app structures */
		if (rpc_parse_rsp(proto_msg, app_resp)) {
			ESP_LOGE(TAG, "failed to parse response, [0x%x]", proto_msg->msg_id);
			goto free_buffers;
		}

		/* Is callback is available,
		 * progress as async response */
		if (CALLBACK_AVAILABLE ==
			is_async_resp_callback_available(app_resp)) {

			/* User registered RPC async response callback
			 * function is available for this proto_msg,
			 * so call to that function should be done and
			 * return to select
			 */
			call_async_resp_callback(app_resp);
		} else {

			/* as RPC async response callback function is
			 * NOT available/registered, treat this response as
			 * synchronous response. forward this response to app
			 * using 'esp_queue' and help of semaphore
			 **/


			/* User is RESPONSIBLE to free memory from
			 * app_resp in case of async callbacks NOT provided
			 * to free memory, please refer CLEANUP_APP_MSG macro
			 **/
			elem.buf = app_resp;
			elem.buf_len = sizeof(ctrl_cmd_t);

			if (g_h.funcs->_h_queue_item(rpc_rx_q, &elem, HOSTED_BLOCK_MAX)) {
				ESP_LOGE(TAG, "RPC Q put fail");
				goto free_buffers;
			}

			/* Call up rx ind to unblock caller */
			if (CALLBACK_AVAILABLE == is_sync_resp_sem_available(app_resp->uid))
				post_sync_resp_sem(app_resp);
		}

	} else {
		/* 4. some unsupported msg, drop it */
		ESP_LOGE(TAG, "Incorrect RPC Msg Type[%u]",proto_msg->msg_type);
		goto free_buffers;
	}
	rpc__free_unpacked(proto_msg, NULL);
	proto_msg = NULL;
	return SUCCESS;

	/* 5. cleanup */
free_buffers:
	rpc__free_unpacked(proto_msg, NULL);
	proto_msg = NULL;
	HOSTED_FREE(app_event);
	HOSTED_FREE(app_resp);
	return RPC_ERR_PROTOBUF_DECODE;
}

/* RPC rx thread
 * This is entry point for rpc messages received from ESP32 */
static void rpc_rx_thread(void const *arg)
{
	uint32_t buf_len = 0;

	rpc_rx_ind_t rpc_rx_func;
	rpc_rx_func = (rpc_rx_ind_t) arg;

	/* If serial interface is not available, exit */
	if (!serial_drv_open(SERIAL_IF_FILE)) {
		ESP_LOGE(TAG, "Exiting thread, handle invalid");
		return;
	}

	/* This queue should already be created
	 * if NULL, exit here */
	if (!rpc_rx_q) {
		ESP_LOGE(TAG, "Ctrl msg rx Q is not created");
		return;
	}

	/* Infinite loop to process incoming msg on serial interface */
	while (1) {
		uint8_t *buf = NULL;
		Rpc *resp = NULL;

		/* Block on read of protobuf encoded msg */
		if (!is_rpc_lib_ready()) {
			g_h.funcs->_h_sleep(1);
			continue;
		}
		buf = transport_pserial_read(&buf_len);

		if (!buf_len || !buf) {
			ESP_LOGE(TAG, "buf_len read = 0");
			goto free_bufs;
		}

		/* Decode protobuf */
		resp = rpc__unpack(NULL, buf_len, buf);
		if (!resp) {
			goto free_bufs;
		}
		/* Free the read buffer */
		HOSTED_FREE(buf);

		/* Send for further processing as event or response */
		ESP_LOGV(TAG, "Before process_rpc_rx_msg");
		process_rpc_rx_msg(resp, rpc_rx_func);
		ESP_LOGV(TAG, "after process_rpc_rx_msg");
		continue;

		/* Failed - cleanup */
free_bufs:
		HOSTED_FREE(buf);
		if (resp) {
			rpc__free_unpacked(resp, NULL);
			resp = NULL;
		}
	}
}

/* Async and sync request sends the rpc msg through this thread.
 * Async thread will register callback, which will be invoked in rpc_rx_thread, once received the response.
 * Sync thread will block for response (in its own context) after submission of ctrl_msg to rpc_tx_q */
static void rpc_tx_thread(void const *arg)
{
	ctrl_cmd_t *app_req = NULL;

	ESP_LOGD(TAG, "Starting tx thread");
	/* If serial interface is not available, exit */
	if (!serial_drv_open(SERIAL_IF_FILE)) {
		ESP_LOGE(TAG, "Exiting thread, handle invalid");
		return;
	}

	/* This queue should already be created
	 * if NULL, exit here */
	if (!rpc_tx_q) {
		ESP_LOGE(TAG, "RPC msg tx Q is not created");
		return;
	}

	/* Infinite loop to process incoming msg on serial interface */
	while (1) {
		ESP_LOGV(TAG, "Loop: Wait for next RPC request");

		/* 4.1 Block on read of protobuf encoded msg */
		if (!is_rpc_lib_ready()) {
			g_h.funcs->_h_sleep(1);
			ESP_LOGD(TAG, "%s:%u rpc lib not ready",__func__,__LINE__);
			continue;
		}

		ESP_LOGV(TAG, "Waiting for RPC TX semaphore");
		g_h.funcs->_h_get_semaphore(rpc_tx_sem, HOSTED_BLOCKING);
		ESP_LOGV(TAG, "RPC TX semaphore acquired");

		ESP_LOGV(TAG, "Dequeueing RPC TX Q");
		if (g_h.funcs->_h_dequeue_item(rpc_tx_q, &app_req, HOSTED_BLOCK_MAX)) {
			ESP_LOGE(TAG, "RPC TX Q Failed to dequeue");
			continue;
		}

		if (app_req) {
			ESP_LOGV(TAG, "Processing RPC TX msg");
			process_rpc_tx_msg(app_req);
			/* app_req lifecycle:
			 * - Async requests: Stored in async_rsp_table, freed when response arrives or timeout
			 * - Sync requests: Freed by caller in rpc_wait_and_parse_sync_resp() after response
			 * - Failed requests: Freed in process_rpc_tx_msg() failure path
			 * So we don't free anything here. */
		} else {
			ESP_LOGE(TAG, "RPC Tx Q empty or uninitialised");
			continue;
		}
	}
}


static int spawn_rpc_threads(void)
{
	/* create new thread for rpc RX path handling */
	rpc_rx_thread_hdl = g_h.funcs->_h_thread_create("rpc_rx", RPC_TASK_PRIO,
			RPC_TASK_STACK_SIZE, rpc_rx_thread, NULL);
	rpc_tx_thread_hdl = g_h.funcs->_h_thread_create("rpc_tx", RPC_TASK_PRIO,
			RPC_TASK_STACK_SIZE, rpc_tx_thread, NULL);
	if (!rpc_rx_thread_hdl || !rpc_tx_thread_hdl) {
		ESP_LOGE(TAG, "Thread creation failed for rpc_rx_thread");
		return FAILURE;
	}
	return SUCCESS;
}

/* cancel thread for rpc RX path handling */
static int cancel_rpc_threads(void)
{
	int ret1 = 0, ret2 =0;

	if (rpc_rx_thread_hdl)
		ret1 = g_h.funcs->_h_thread_cancel(rpc_rx_thread_hdl);

	if (rpc_tx_thread_hdl)
		ret2 = g_h.funcs->_h_thread_cancel(rpc_tx_thread_hdl);

	if (ret1 || ret2) {
		ESP_LOGE(TAG, "pthread_cancel rpc threads failed");
		return FAILURE;
	}

	return SUCCESS;
}


static const char *rpc_id_name(int id)
{
    const ProtobufCEnumValue *v =
        protobuf_c_enum_descriptor_get_value(&rpc_id__descriptor, id);

    return v ? v->name : "UNKNOWN";
}


/* This function will be only invoked in synchrounous rpc response path,
 * i.e. if rpc response callbcak is not available i.e. NULL
 * This function is called after sending synchrounous rpc request to wait
 * for the response using semaphores and esp_queue
 **/
static ctrl_cmd_t * get_response(int *read_len, ctrl_cmd_t *app_req)
{
	uint8_t * buf = NULL;
	esp_queue_elem_t elem = {0};
	int ret = 0;

	/* Any problems in response, return NULL */
	if (!read_len || !app_req) {
		ESP_LOGE(TAG, "Invalid input parameter");
		return NULL;
	}


	/* Wait for response */
	ret = wait_for_sync_response(app_req);
	if (ret) {
		if ((ret == RET_FAIL_TIMEOUT) || (errno == ETIMEDOUT))
			ESP_LOGW(TAG, "Timeout waiting for Resp for [0x%x](%s)", app_req->msg_id, rpc_id_name(app_req->msg_id));
		else
			ESP_LOGE(TAG, "ERR [%u] ret[%d] for [0x%x](%s)", errno, ret, app_req->msg_id, rpc_id_name(app_req->msg_id));
		return NULL;
	}

	/* Fetch response from `esp_queue` */
	if (g_h.funcs->_h_dequeue_item(rpc_rx_q, &elem, HOSTED_BLOCK_MAX)) {
		ESP_LOGE(TAG, "rpc Rx Q Failed to dequeue");
		return NULL;
	}

	if (elem.buf_len) {

		*read_len = elem.buf_len;
		buf = elem.buf;
		return (ctrl_cmd_t*)buf;

	} else {
		ESP_LOGE(TAG, "rpc Q empty or uninitialised");
		return NULL;
	}

	return NULL;
}

static int clear_async_resp_callback(async_rsp_t *async_rsp_item)
{
	if (async_rsp_item->uid != 0) {

		if (async_rsp_item->timer_hdl) {
			g_h.funcs->_h_timer_stop(async_rsp_item->timer_hdl);
			async_rsp_item->timer_hdl = NULL;
		}

		/* Free the request structure that was allocated by RPC_DEFAULT_REQ */
		HOSTED_FREE(async_rsp_item->app_req);

		async_rsp_item->uid = 0;
		async_rsp_item->cb = NULL;
		return SUCCESS;
	} else {
		ESP_LOGW(TAG, "async_rsp_item to be cleared already has uid 0");
	}
	return FAILURE;
}

/* Check and call rpc response asynchronous callback if available
 * else flag error
 *     MSG_ID_OUT_OF_ORDER - if response id is not understandable
 *     CALLBACK_NOT_REGISTERED - callback is not registered
 **/
static int call_async_resp_callback(ctrl_cmd_t *app_resp)
{
	int i;

	// msg_id of RPC_ID__Resp_Base now means Invalid RPC Request
	if ((app_resp->msg_id < RPC_ID__Resp_Base) ||
	    (app_resp->msg_id >= RPC_ID__Resp_Max)) {
		return MSG_ID_OUT_OF_ORDER;
	}

	for (i = 0; i < MAX_ASYNC_RPC_TRANSACTIONS; i++) {
		if (async_rsp_table[i].uid == app_resp->uid) {
			int ret = async_rsp_table[i].cb(app_resp);

			clear_async_resp_callback(&async_rsp_table[i]);

			return ret;
		}
	}

	return CALLBACK_NOT_REGISTERED;
}


static int post_sync_resp_sem(ctrl_cmd_t *app_resp)
{
	int i;

	// msg_id of RPC_ID__Resp_Base now means Invalid RPC Request
	if ((app_resp->msg_id < RPC_ID__Resp_Base) ||
	    (app_resp->msg_id >= RPC_ID__Resp_Max)) {
		return MSG_ID_OUT_OF_ORDER;
	}

	for (i = 0; i < MAX_SYNC_RPC_TRANSACTIONS; i++) {
		if (sync_rsp_table[i].uid == app_resp->uid) {
			return g_h.funcs->_h_post_semaphore(sync_rsp_table[i].sem);
		}
	}

	return CALLBACK_NOT_REGISTERED;
}


/* Check and call rpc event asynchronous callback if available
 * else flag error
 *     MSG_ID_OUT_OF_ORDER - if event id is not understandable
 *     CALLBACK_NOT_REGISTERED - callback is not registered
 **/
static int call_event_callback(ctrl_cmd_t *app_event)
{
	if ((app_event->msg_id <= RPC_ID__Event_Base) ||
	    (app_event->msg_id >= RPC_ID__Event_Max)) {
		return MSG_ID_OUT_OF_ORDER;
	}

	if (rpc_evt_cb_table[app_event->msg_id-RPC_ID__Event_Base]) {
		return rpc_evt_cb_table[app_event->msg_id-RPC_ID__Event_Base](app_event);
	}

	return CALLBACK_NOT_REGISTERED;
}

/* Set asynchronous rpc response callback from rpc **request**
 */
static int set_async_resp_callback(ctrl_cmd_t *app_req, rpc_rsp_cb_t resp_cb, void *timer_hdl)
{
	int i;

	int exp_resp_msg_id = (app_req->msg_id - RPC_ID__Req_Base + RPC_ID__Resp_Base);
	if (exp_resp_msg_id >= RPC_ID__Resp_Max) {
		ESP_LOGW(TAG, "Not able to map new request to resp id");
		return MSG_ID_OUT_OF_ORDER;
	}

	for (i = 0; i < MAX_ASYNC_RPC_TRANSACTIONS; i++) {
		if (!async_rsp_table[i].uid) {
			async_rsp_table[i].uid = app_req->uid;
			async_rsp_table[i].cb = resp_cb;
			async_rsp_table[i].timer_hdl = timer_hdl;
			async_rsp_table[i].app_req = app_req;  /* Store request for later cleanup */
			return CALLBACK_SET_SUCCESS;
		}
	}

	ESP_LOGE(TAG, "Async cb not registered: out of buffer space");
	return CALLBACK_NOT_REGISTERED;
}

/* Set synchronous rpc response semaphore
 * In case of asynchronous request, `rx_sem` will be NULL or disregarded
 * `rpc_rsp_cb_sem_table` will be updated with NULL for async
 * In case of synchronous request, valid callback will be cached
 * This sem will posted after receiving the mapping response
 **/
static int set_sync_resp_sem(ctrl_cmd_t *app_req)
{
	int exp_resp_msg_id = (app_req->msg_id - RPC_ID__Req_Base + RPC_ID__Resp_Base);
	int i;

	if (app_req->rx_sem)
		g_h.funcs->_h_destroy_semaphore(app_req->rx_sem);

	if (exp_resp_msg_id >= RPC_ID__Resp_Max) {
		ESP_LOGW(TAG, "Not able to map new request to resp id");
		return MSG_ID_OUT_OF_ORDER;
	} else if (!app_req->rpc_rsp_cb) {
		/* For sync, set sem */
		app_req->rx_sem = g_h.funcs->_h_create_semaphore(1);
		g_h.funcs->_h_get_semaphore(app_req->rx_sem, 0);

		for (i = 0; i < MAX_SYNC_RPC_TRANSACTIONS; i++) {
			if (!sync_rsp_table[i].uid) {
				ESP_LOGD(TAG, "Register sync sem %p for uid %ld", app_req->rx_sem, app_req->uid);
				sync_rsp_table[i].uid = app_req->uid;
				sync_rsp_table[i].sem = app_req->rx_sem;
				return CALLBACK_SET_SUCCESS;
			}
		}
		ESP_LOGE(TAG, "Symc sem not registered: out of buffer space");
		return CALLBACK_NOT_REGISTERED;
	} else {
		/* For async, nothing to be done */
		ESP_LOGD(TAG, "NOT Register sync sem for resp[0x%x]", exp_resp_msg_id);
		return CALLBACK_NOT_REGISTERED;
	}
}

static int wait_for_sync_response(ctrl_cmd_t *app_req)
{
	int timeout_sec = 0;
	int exp_resp_msg_id = 0;
	int ret = 0;
	int i;

	/* If timeout not specified, use default */
	if (!app_req->rsp_timeout_sec)
		timeout_sec = DEFAULT_RPC_RSP_TIMEOUT;
	else
		timeout_sec = app_req->rsp_timeout_sec;

	exp_resp_msg_id = (app_req->msg_id - RPC_ID__Req_Base + RPC_ID__Resp_Base);

	if (exp_resp_msg_id >= RPC_ID__Resp_Max) {
		ESP_LOGW(TAG, "Not able to map new request to resp id");
		return MSG_ID_OUT_OF_ORDER;
	}

	ESP_LOGV(TAG, "Wait for sync resp for Req[0x%x] with timer of %u sec",
			app_req->msg_id, timeout_sec);
	for (i = 0; i < MAX_SYNC_RPC_TRANSACTIONS; i++) {
		if (sync_rsp_table[i].uid == app_req->uid) {
			ret = g_h.funcs->_h_get_semaphore(sync_rsp_table[i].sem, SEC_TO_MILLISEC(timeout_sec));
			if (g_h.funcs->_h_destroy_semaphore(sync_rsp_table[i].sem)) {
				ESP_LOGE(TAG, "read sem rx for resp[0x%x] destroy failed", exp_resp_msg_id);
			}
			// clear table entry
			sync_rsp_table[i].uid = 0;
			sync_rsp_table[i].sem = NULL;
			return ret;
		}
	}
	ESP_LOGW(TAG, "Not able to map new request to resp id");
	return MSG_ID_OUT_OF_ORDER;
}

/* Is asynchronous rpc response callback from rpc response */
static int is_async_resp_callback_available(ctrl_cmd_t *app_resp)
{
	int i;

	// msg_id of RPC_ID__Resp_Base now means Invalid RPC Request
	if ((app_resp->msg_id < RPC_ID__Resp_Base) || (app_resp->msg_id >= RPC_ID__Resp_Max)) {
		ESP_LOGE(TAG, "resp id[0x%x] out of range", app_resp->msg_id);
		return MSG_ID_OUT_OF_ORDER;
	}

	for (i = 0; i < MAX_ASYNC_RPC_TRANSACTIONS; i++) {
		if (async_rsp_table[i].uid == app_resp->uid) {
			return CALLBACK_AVAILABLE;
		}
	}

	return CALLBACK_NOT_REGISTERED;
}

static int is_sync_resp_sem_available(uint32_t uid)
{
	int i;

	for (i = 0; i < MAX_SYNC_RPC_TRANSACTIONS; i++) {
		if (sync_rsp_table[i].uid == uid) {
			return CALLBACK_AVAILABLE;
		}
	}
	return CALLBACK_NOT_REGISTERED;
}

/* Set rpc event callback
 * `rpc_evt_cb_table` will be updated with NULL by default
 * when user sets event callback, user provided function pointer
 * will be registered with user function
 * If user does not register event callback,
 * events received from ESP32 will be dropped
 **/
int set_event_callback(int event, rpc_rsp_cb_t event_cb)
{
	int event_cb_tbl_idx = event - RPC_ID__Event_Base;

	if ((event<=RPC_ID__Event_Base) || (event>=RPC_ID__Event_Max)) {
		ESP_LOGW(TAG, "Could not identify event[0x%x]", event);
		return MSG_ID_OUT_OF_ORDER;
	}
	rpc_evt_cb_table[event_cb_tbl_idx] = event_cb;
	return CALLBACK_SET_SUCCESS;
}

/* Assign NULL event callback */
int reset_event_callback(int event)
{
	return set_event_callback(event, NULL);
}

/* This is only used in synchrounous rpc
 * When request is sent without async callback, this function will be called
 * It will wait for rpc response or timeout for rpc response
 **/
ctrl_cmd_t * rpc_wait_and_parse_sync_resp(ctrl_cmd_t *app_req)
{
	ctrl_cmd_t * rx_buf = NULL;
	int rx_buf_len = 0;

	rx_buf = get_response(&rx_buf_len, app_req);
	if (!rx_buf || !rx_buf_len) {
		ESP_LOGE(TAG, "Response not received for [0x%x](%s)", app_req->msg_id, rpc_id_name(app_req->msg_id));
		if (rx_buf) {
			HOSTED_FREE(rx_buf);
		}
	}
	HOSTED_FREE(app_req);
	return rx_buf;
}


/* This function is called for async procedure
 * Timer started when async rpc req is received
 * But there was no response in due time, this function will
 * be called to send error to application
 * */
//static void rpc_async_timeout_handler(void const *arg)
static void rpc_async_timeout_handler(void *arg)
{
	/* Please Note: Be careful while porting this to MCU.
	 * rpc_async_timeout_handler should only be invoked after the timer has expired.
	 * timer should not expire incorrect duration (Check os_wrapper layer for
	 * correct seconds to milliseconds or ticks etc depending upon the platform
	 * */
	ctrl_cmd_t *app_req = (ctrl_cmd_t *)arg;

	if (!app_req || !app_req->rpc_rsp_cb) {
	  if (!app_req)
		ESP_LOGE(TAG, "NULL app_req");

	  if (!app_req->rpc_rsp_cb)
		ESP_LOGE(TAG, "NULL app_req->resp_cb");
	  return;
	}

	ESP_LOGW(TAG, "ASYNC Timeout for req [0x%x](%s)", app_req->msg_id, rpc_id_name(app_req->msg_id));
	rpc_rsp_cb_t func = app_req->rpc_rsp_cb;
	uint32_t req_uid = app_req->uid;
	ctrl_cmd_t *app_resp = NULL;
	HOSTED_CALLOC(ctrl_cmd_t, app_resp, sizeof(ctrl_cmd_t), free_buffers);
	app_resp->msg_id = app_req->msg_id - RPC_ID__Req_Base + RPC_ID__Resp_Base;
	app_resp->msg_type = RPC_TYPE__Resp;
	app_resp->resp_event_status = RPC_ERR_REQUEST_TIMEOUT;

	/* Clear the async callback table entry (this frees app_req) */
	for (int i = 0; i < MAX_ASYNC_RPC_TRANSACTIONS; i++) {
		if (async_rsp_table[i].uid == req_uid) {
			clear_async_resp_callback(&async_rsp_table[i]);
			break;
		}
	}

	/* call func pointer to notify failure */
	func(app_resp);
free_buffers:
	return;
}

/* This is entry level function when rpc request APIs are used
 * This function will encode rpc request in protobuf and send to ESP32
 * It will copy application structure `ctrl_cmd_t` to
 * protobuf rpc req `Rpc`
 **/
int rpc_send_req(ctrl_cmd_t *app_req)
{
	if (!app_req) {
		ESP_LOGE(TAG, "Invalid param in rpc_send_req");
		return FAILURE;
	}

	if (!rpc_tx_q) {
		ESP_LOGW(TAG, "RPC not initialized or transport down, failing fast");
		return FAILURE;
	}


	uid++;
	// handle rollover in uid value
	if (!uid)
		uid++;
	app_req->uid = uid;

	ESP_LOGD(TAG, "app_req msgid[0x%x] with uid %" PRIu32, app_req->msg_id, app_req->uid);
	if (!app_req->rpc_rsp_cb) {
		/* sync proc only */
		if (set_sync_resp_sem(app_req)) {
			ESP_LOGE(TAG, "could not set sync resp sem for req[0x%x]",app_req->msg_id);
			goto fail_req;
		}
	}

	app_req->msg_type = RPC_TYPE__Req;

	ESP_LOGV(TAG, "queueing rpc tx q with uid %" PRIu32, app_req->uid);
	if (g_h.funcs->_h_queue_item(rpc_tx_q, &app_req, HOSTED_BLOCK_MAX)) {
	  ESP_LOGE(TAG, "Failed to new app rpc req[0x%x] in tx queue", app_req->msg_id);
	  goto fail_req;
	}

	rpc_tx_ind();

	/* TODO : commenting, Review again to avoid duable free */
	//H_FREE_PTR_WITH_FUNC(app_req->app_free_buff_func, app_req->app_free_buff_hdl);

	return SUCCESS;

fail_req:
	if (app_req->rx_sem)
		g_h.funcs->_h_destroy_semaphore(app_req->rx_sem);

	H_FREE_PTR_WITH_FUNC(app_req->app_free_buff_func, app_req->app_free_buff_hdl);
	HOSTED_FREE(app_req);

	return FAILURE;
}

static int cleanup_sync_async_timer_table(void)
{
	for (int i = 0; i < MAX_ASYNC_RPC_TRANSACTIONS; i++) {
		if (async_rsp_table[i].timer_hdl) {
			g_h.funcs->_h_timer_stop(async_rsp_table[i].timer_hdl);
		}
		/* Free any pending async request structures */
		if (async_rsp_table[i].app_req) {
			HOSTED_FREE(async_rsp_table[i].app_req);
		}
		async_rsp_table[i].timer_hdl = NULL;
		async_rsp_table[i].uid = 0;
		async_rsp_table[i].cb = NULL;
		async_rsp_table[i].app_req = NULL;
	}

	for (int i = 0; i < MAX_SYNC_RPC_TRANSACTIONS; i++) {
		if (sync_rsp_table[i].sem) {
			g_h.funcs->_h_get_semaphore(sync_rsp_table[i].sem, 0);
			g_h.funcs->_h_destroy_semaphore(sync_rsp_table[i].sem);
		}
		sync_rsp_table[i].uid = 0;
		sync_rsp_table[i].sem = NULL;
	}

	return SUCCESS;
}

/* De-init hosted rpc lib */
int rpc_core_deinit(void)
{
	int ret = SUCCESS;

	if (is_rpc_lib_inactive())
		return ret;

	set_rpc_lib_state(RPC_LIB_STATE_INACTIVE);

	/* Drain rpc_rx_q before destroying to prevent buffer leaks */
	if (rpc_rx_q) {
		esp_queue_elem_t elem;
		/* Flush all remaining items and free their buffers */
		while (g_h.funcs->_h_dequeue_item(rpc_rx_q, &elem, 0) == 0) {
			if (elem.buf) {
				g_h.funcs->_h_free(elem.buf);
			}
		}
		g_h.funcs->_h_destroy_queue(rpc_rx_q);
		rpc_rx_q = NULL;
	}

	/* Drain rpc_tx_q before destroying to prevent buffer leaks */
	if (rpc_tx_q) {
		void *buf_ptr;
		/* Flush all remaining items and free their buffers */
		while (g_h.funcs->_h_dequeue_item(rpc_tx_q, &buf_ptr, 0) == 0) {
			if (buf_ptr) {
				g_h.funcs->_h_free(buf_ptr);
			}
		}
		g_h.funcs->_h_destroy_queue(rpc_tx_q);
		rpc_tx_q = NULL;
	}

	if (rpc_tx_sem) {
		if (g_h.funcs->_h_destroy_semaphore(rpc_tx_sem)) {
			ret = FAILURE;
			ESP_LOGE(TAG, "read sem tx deinit failed");
		}
		rpc_tx_sem = NULL;
	}

	cleanup_sync_async_timer_table();

	if (cancel_rpc_threads()) {
		ret = FAILURE;
		ESP_LOGE(TAG, "cancel rpc rx thread failed");
	}

	if (serial_deinit()) {
		ret = FAILURE;
		ESP_LOGE(TAG, "Serial de-init failed");
	}
	return ret;
}

/* Init hosted rpc lib */
int rpc_core_init(void)
{
	int ret = SUCCESS;

	/* semaphore init */
	rpc_tx_sem = g_h.funcs->_h_create_semaphore(MAX_SYNC_RPC_TRANSACTIONS +
			MAX_ASYNC_RPC_TRANSACTIONS);
	if (!rpc_tx_sem) {
		ESP_LOGE(TAG, "sem init failed, exiting");
		goto free_bufs;
	}

	/* Get semaphore for first time */
	g_h.funcs->_h_get_semaphore(rpc_tx_sem, 0);

	/* serial init */
	if (serial_init()) {
		ESP_LOGE(TAG, "Failed to serial_init");
		goto free_bufs;
	}

	rpc_rx_q = g_h.funcs->_h_create_queue(RPC_RX_QUEUE_SIZE,
			sizeof(esp_queue_elem_t));

	rpc_tx_q = g_h.funcs->_h_create_queue(RPC_TX_QUEUE_SIZE,
			sizeof(void *));
	if (!rpc_rx_q || !rpc_tx_q) {
		ESP_LOGE(TAG, "Failed to create app rpc msg Q");
		goto free_bufs;
	}

	/* thread init */
	if (spawn_rpc_threads())
		goto free_bufs;

	/* state init */
	set_rpc_lib_state(RPC_LIB_STATE_INIT);
	return ret;

free_bufs:
	rpc_core_deinit();
	return FAILURE;
}

int rpc_core_start(void)
{
	set_rpc_lib_state(RPC_LIB_STATE_READY);
	return SUCCESS;
}

int rpc_core_stop(void)
{
	set_rpc_lib_state(RPC_LIB_STATE_INIT);
	return SUCCESS;
}
