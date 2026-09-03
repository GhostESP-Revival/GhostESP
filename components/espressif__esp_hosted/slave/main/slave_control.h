/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __SLAVE_CONTROL__H__
#define __SLAVE_CONTROL__H__
#include <esp_err.h>
#include "esp_idf_version.h"
#include "interface.h"
#include "sdkconfig.h"

#include "esp_hosted_rpc.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  #define TIMEOUT_IN_SEC          (1000 / portTICK_PERIOD_MS)
#else
  #define TIMEOUT_IN_SEC          (1000 / portTICK_RATE_MS)
#endif

#define SSID_LENGTH             32
#define PASSWORD_LENGTH         64
#define VENDOR_OUI_BUF          3

#define SUCCESS                     0
#define FAILURE                     -1

#define mem_free(x)                 \
        {                           \
            if (x) {                \
                free(x);            \
                x = NULL;           \
            }                       \
        }


#define NTFY_TEMPLATE(NtFy_MsgId, NtFy_TyPe, NtFy_StRuCt, InIt_FuN)             \
  NtFy_TyPe *ntfy_payload = NULL;                                               \
  ntfy_payload = (NtFy_TyPe*)calloc(1,sizeof(NtFy_TyPe));                       \
  if (!ntfy_payload) {                                                          \
    ESP_LOGE(TAG,"Failed to allocate memory");                                  \
    return ESP_ERR_NO_MEM;                                                      \
  }                                                                             \
  InIt_FuN(ntfy_payload);                                                       \
  ntfy->payload_case = NtFy_MsgId;                                              \
  ntfy->NtFy_StRuCt = ntfy_payload;                                             \
  ntfy_payload->resp = SUCCESS;

#define RPC_TEMPLATE(RspTyPe, RspStRuCt, ReqType, ReqStruct, InIt_FuN)          \
  RspTyPe *resp_payload = NULL;                                                 \
  ReqType *req_payload = NULL;                                                  \
  if (!req || !resp || !req->ReqStruct) {                                       \
    ESP_LOGE(TAG, "Invalid parameters");                                        \
    return ESP_FAIL;                                                            \
  }                                                                             \
  req_payload = req->ReqStruct;                                                 \
  resp_payload = (RspTyPe *)calloc(1, sizeof(RspTyPe));                         \
  if (!resp_payload) {                                                          \
      ESP_LOGE(TAG, "Failed to alloc mem for resp.%s\n",#RspStRuCt);            \
      return ESP_ERR_NO_MEM;                                                    \
  }                                                                             \
  resp->RspStRuCt = resp_payload;                                               \
  InIt_FuN(resp_payload);                                                       \
  resp_payload->resp = SUCCESS;                                                 \


/* Simple is same above just, we dod not need req_payload unused warning */
#define RPC_TEMPLATE_SIMPLE(RspTyPe, RspStRuCt, ReqType, ReqStruct, InIt_FuN)   \
  RspTyPe *resp_payload = NULL;                                                 \
  if (!req || !resp) {                                                          \
    ESP_LOGE(TAG, "Invalid parameters");                                        \
    return ESP_FAIL;                                                            \
  }                                                                             \
  resp_payload = (RspTyPe *)calloc(1, sizeof(RspTyPe));                         \
  if (!resp_payload) {                                                          \
      ESP_LOGE(TAG, "Failed to alloc mem for resp.%s\n",#RspStRuCt);            \
      return ESP_ERR_NO_MEM;                                                    \
  }                                                                             \
  resp->RspStRuCt = resp_payload;                                               \
  InIt_FuN(resp_payload);                                                       \
  resp_payload->resp = SUCCESS;                                                 \

#define RPC_RESP_ASSIGN_FIELD(PaRaM)                                            \
  resp_payload->PaRaM = PaRaM

#define RPC_RET_FAIL_IF(ConDiTiOn) do {                                         \
  int rEt = (ConDiTiOn);                                                        \
  if (rEt) {                                                                    \
    resp_payload->resp = rEt;                                                   \
    ESP_LOGE(TAG, "%s:%u failed [%s] = [%d]", __func__,__LINE__,#ConDiTiOn, rEt); \
    return ESP_OK;                                                              \
  }                                                                             \
} while(0);


#define RPC_ALLOC_ELEMENT(TyPe,MsG_StRuCt,InIt_FuN) {                         \
  TyPe *NeW_AllocN = (TyPe *)calloc(1, sizeof(TyPe));                         \
  if (!NeW_AllocN) {                                                          \
    ESP_LOGI(TAG,"Failed to allocate memory for req.%s\n",#MsG_StRuCt);       \
    resp_payload->resp = RPC_ERR_MEMORY_FAILURE;                              \
    goto err;                                                                 \
  }                                                                           \
  MsG_StRuCt = NeW_AllocN;                                                    \
  InIt_FuN(MsG_StRuCt);                                                       \
}

#define NTFY_ALLOC_ELEMENT(TyPe,MsG_StRuCt,InIt_FuN) {                        \
  TyPe *NeW_AllocN = (TyPe *)calloc(1, sizeof(TyPe));                         \
  if (!NeW_AllocN) {                                                          \
    ESP_LOGI(TAG,"Failed to allocate memory for req.%s\n",#MsG_StRuCt);       \
    ntfy_payload->resp = RPC_ERR_MEMORY_FAILURE;                              \
    goto err;                                                                 \
  }                                                                           \
  MsG_StRuCt = NeW_AllocN;                                                    \
  InIt_FuN(MsG_StRuCt);                                                       \
}

#define NTFY_COPY_BYTES(dest, src, num)                                         \
  do {                                                                          \
    if (num) {                                                                  \
      dest.data = (uint8_t *)calloc(1, num);                                    \
      if (!dest.data) {                                                         \
        ESP_LOGE(TAG, "%s:%u Failed to duplicate bytes\n",__func__,__LINE__);   \
        ntfy_payload->resp = FAILURE;                                           \
        return ESP_OK;                                                          \
      }                                                                         \
      memcpy(dest.data, src, num);                                              \
      dest.len = num;                                                           \
    }                                                                           \
  } while(0)

#define NTFY_COPY_STR(dest, src, max_len)                                       \
  if (src) {                                                                    \
    dest.data = (uint8_t*)strndup((char*)src, max_len);                         \
    if (!dest.data) {                                                           \
      ESP_LOGE(TAG, "%s:%u Failed to duplicate bytes\n",__func__,__LINE__);     \
      ntfy_payload->resp = FAILURE;                                             \
      return ESP_OK;                                                            \
    }                                                                           \
    dest.len = min(max_len,strlen((char*)src)+1);                                 \
  }

#define RPC_REQ_COPY_BYTES(dest, src, num_bytes)                                \
  if (src.len && src.data)                                                      \
    memcpy((char*)dest, src.data, min(min(sizeof(dest), num_bytes), src.len));

#define RPC_REQ_COPY_STR RPC_REQ_COPY_BYTES

#define RPC_RESP_COPY_STR(dest, src, max_len)                                   \
  if (src) {                                                                    \
    dest.data = (uint8_t*)strndup((char*)src, max_len);                         \
    if (!dest.data) {                                                           \
      ESP_LOGE(TAG, "%s:%u Failed to duplicate bytes\n",__func__,__LINE__);     \
      resp_payload->resp = FAILURE;                                             \
      return ESP_OK;                                                            \
    }                                                                           \
    dest.len = min(max_len,strlen((char*)src)+1);                                 \
  }

#define RPC_RESP_COPY_BYTES_SRC_UNCHECKED(dest, src, num)                       \
  do {                                                                          \
    if (num) {                                                                  \
      dest.data = (uint8_t *)calloc(1, num);                                    \
      if (!dest.data) {                                                         \
        ESP_LOGE(TAG, "%s:%u Failed to duplicate bytes\n",__func__,__LINE__);   \
        resp_payload->resp = FAILURE;                                           \
        return ESP_OK;                                                          \
      }                                                                         \
      memcpy(dest.data, src, num);                                              \
      dest.len = num;                                                           \
    }                                                                           \
  } while(0)

#define RPC_RESP_COPY_BYTES(dest, src, num)                                     \
  if (src) {                                                                    \
    RPC_RESP_COPY_BYTES_SRC_UNCHECKED(dest, src, num);                          \
  }

#define RPC_COPY_STR(dest, src, max_len)                                        \
  if (src) {                                                                    \
    dest.data = (uint8_t*)strndup((char*)src, max_len);                         \
    if (!dest.data) {                                                           \
      ESP_LOGE(TAG, "%s:%u Failed to duplicate bytes\n",__func__,__LINE__);     \
      return FAILURE;                                                           \
    }                                                                           \
    dest.len = min(max_len,strlen((char*)src)+1);                                 \
  }

#define RPC_COPY_BYTES(dest, src, num)                                          \
  do {                                                                          \
    if (num) {                                                                  \
      dest.data = (uint8_t *)calloc(1, num);                                    \
      if (!dest.data) {                                                         \
        ESP_LOGE(TAG, "%s:%u Failed to duplicate bytes\n",__func__,__LINE__);   \
        return FAILURE;                                                         \
      }                                                                         \
      memcpy(dest.data, src, num);                                              \
      dest.len = num;                                                             \
    }                                                                           \
  } while(0)

esp_err_t data_transfer_handler(uint32_t session_id,const uint8_t *inbuf,
		ssize_t inlen,uint8_t **outbuf, ssize_t *outlen, void *priv_data);
esp_err_t rpc_evt_handler(uint32_t session_id,const uint8_t *inbuf,
		ssize_t inlen, uint8_t **outbuf, ssize_t *outlen, void *priv_data);
void send_event_to_host(int event_id);
void send_event_data_to_host(int event_id, void *data, int size);

#ifdef CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER
#include "esp_hosted_peer_data.h"
#endif

#include "esp_wifi.h"

#ifdef CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED
/* DHCP/DNS status structure */
typedef struct {
    int iface;
    int net_link_up;
    int dhcp_up;
    uint8_t dhcp_ip[64];
    uint8_t dhcp_nm[64];
    uint8_t dhcp_gw[64];
    int dns_up;
    uint8_t dns_ip[64];
    int dns_type;
} rpc_dhcp_dns_status_t;

/* Function declarations */

#endif /* CONFIG_ESP_HOSTED_NETWORK_SPLIT_ENABLED */

#endif /*__SLAVE_CONTROL__H__*/
