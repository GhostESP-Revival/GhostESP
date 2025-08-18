/*
 * 1. Both chips boot up and automatically start listening for peers
 * 2. Once connected, use 'commsend <command>' to send commands
 */

#include "core/esp_comm_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "core/serial_manager.h"
#include "soc/uart_pins.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_mac.h"
#include <string.h>
#include <stdio.h>
#include "managers/views/terminal_screen.h"
#include "managers/ap_manager.h"

#define COMM_BUFFER_SIZE 128
#define COMM_PACKET_SIZE 64
#define DISCOVERY_INTERVAL_MS 3000
#define HANDSHAKE_TIMEOUT_MS 3000
#define COMMAND_TIMEOUT_MS 500

#if defined(CONFIG_IDF_TARGET_ESP32)
#define DEFAULT_TX_PIN GPIO_NUM_17
#define DEFAULT_RX_PIN GPIO_NUM_16
#else
#define DEFAULT_TX_PIN GPIO_NUM_6
#define DEFAULT_RX_PIN GPIO_NUM_7
#endif
#define DEFAULT_BAUD_RATE 921600

static const char* TAG = "esp_comm_manager";

typedef enum {
    PACKET_TYPE_DISCOVERY = 0x01,
    PACKET_TYPE_HANDSHAKE_REQ = 0x02,
    PACKET_TYPE_HANDSHAKE_ACK = 0x03,
    PACKET_TYPE_COMMAND = 0x04,
    PACKET_TYPE_RESPONSE = 0x05,
    PACKET_TYPE_PING = 0x06,
    PACKET_TYPE_PONG = 0x07
} packet_type_t;

typedef enum {
    PARSE_STATE_IDLE = 0,
    PARSE_STATE_START_BYTE,
    PARSE_STATE_TYPE,
    PARSE_STATE_DATA,
    PARSE_STATE_CHECKSUM
} packet_parse_state_t;

typedef struct {
    uint8_t start_byte;
    uint8_t type;
    uint8_t length;
    uint8_t data[COMM_PACKET_SIZE - 4];
} __attribute__((packed)) comm_packet_t;

typedef struct {
    char command[33];
    char data[COMM_PACKET_SIZE - 32];
} comm_command_t;

typedef struct {
    gpio_num_t tx_pin;
    gpio_num_t rx_pin;
    uint32_t baud_rate;
    
    comm_state_t state;
    comm_role_t role;
    comm_peer_t peer;
    
    QueueHandle_t rx_packet_queue;
    QueueHandle_t tx_queue;
    QueueHandle_t command_queue;
    TaskHandle_t rx_task_handle;
    TaskHandle_t tx_task_handle;
    TaskHandle_t protocol_task_handle;
    TaskHandle_t command_executor_task_handle;
    TimerHandle_t discovery_timer;
    TimerHandle_t ping_timer;
    
    comm_command_callback_t command_callback;
    void* callback_user_data;
    
    char chip_name[32];
    uint8_t chip_id[6];
    
    bool initialized;
    bool is_executing_remote_cmd;
    
    packet_parse_state_t parse_state;
    comm_packet_t partial_packet;
    uint8_t data_bytes_received;
} esp_comm_manager_t;

static esp_comm_manager_t* s_comm_manager = NULL;
static comm_command_callback_t s_pending_callback = NULL;
static void* s_pending_callback_user_data = NULL;

// Forward declarations for functions referenced before their definitions
static void tx_task(void* arg);
static void rx_task(void* arg);
static void protocol_task(void* arg);
static void command_executor_task(void* arg);
static void discovery_timer_callback(TimerHandle_t xTimer);
static void send_discovery_packet(void);
static void send_handshake_request(const char* peer_name);
static void send_handshake_ack(void);

static uint8_t calculate_checksum(const uint8_t* data, size_t len) {
    uint8_t checksum = 0;
    for (size_t i = 0; i < len; i++) {
        checksum ^= data[i];
    }
    return checksum;
}

static bool send_packet(const comm_packet_t* packet) {
    if (!s_comm_manager || !packet) return false;
    if (s_comm_manager->tx_queue) {
        if (xQueueSend(s_comm_manager->tx_queue, packet, pdMS_TO_TICKS(10)) != pdPASS) {
            printf("W: TX queue full, dropped packet type 0x%02x\n", packet->type);
            return false;
        }
        return true;
    } else {
        // Direct transmit path used during scanning/handshake before TX task exists
        uart_write_bytes(UART_NUM_1, (const char*)packet, 3 + packet->length);
        uint8_t checksum = calculate_checksum((const uint8_t*)packet, 3 + packet->length);
        uart_write_bytes(UART_NUM_1, (const char*)&checksum, 1);
        if (packet->type == PACKET_TYPE_RESPONSE) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        return true;
    }
}

static void tx_task(void* arg) {
    esp_comm_manager_t* comm = (esp_comm_manager_t*)arg;
    comm_packet_t packet;

    while (1) {
        if (xQueueReceive(comm->tx_queue, &packet, portMAX_DELAY) == pdPASS) {
            uart_write_bytes(UART_NUM_1, (uint8_t*)&packet, 3 + packet.length);
            uint8_t checksum = calculate_checksum((uint8_t*)&packet, 3 + packet.length);
            uart_write_bytes(UART_NUM_1, &checksum, 1);
            if (packet.type == PACKET_TYPE_RESPONSE) {
                vTaskDelay(pdMS_TO_TICKS(2));
            }
        }
    }
}

static void rx_task(void* arg) {
    esp_comm_manager_t* comm = (esp_comm_manager_t*)arg;
    uint8_t rx_buffer[COMM_BUFFER_SIZE];

    while (1) {
        int len = uart_read_bytes(UART_NUM_1, rx_buffer, COMM_BUFFER_SIZE, pdMS_TO_TICKS(100));
        if (len <= 0) continue;

        for (int i = 0; i < len; ++i) {
            uint8_t byte = rx_buffer[i];
            
            switch (comm->parse_state) {
                case PARSE_STATE_IDLE:
                    if (byte == 0xAA) {
                        comm->parse_state = PARSE_STATE_START_BYTE;
                        comm->partial_packet.start_byte = byte;
                    }
                    break;
                    
                case PARSE_STATE_START_BYTE:
                    comm->partial_packet.type = byte;
                    comm->parse_state = PARSE_STATE_TYPE;
                    break;
                    
                case PARSE_STATE_TYPE:
                    comm->partial_packet.length = byte;
                    if (byte > COMM_PACKET_SIZE - 4) {
                        comm->parse_state = PARSE_STATE_IDLE;
                        break;
                    }
                    comm->data_bytes_received = 0;
                    comm->parse_state = (byte == 0) ? PARSE_STATE_CHECKSUM : PARSE_STATE_DATA;
                    break;
                    
                case PARSE_STATE_DATA:
                    comm->partial_packet.data[comm->data_bytes_received++] = byte;
                    if (comm->data_bytes_received >= comm->partial_packet.length) {
                        comm->parse_state = PARSE_STATE_CHECKSUM;
                    }
                    break;
                    
                case PARSE_STATE_CHECKSUM:
                    {
                        uint8_t checksum = 0;
                        checksum ^= 0xAA;
                        checksum ^= comm->partial_packet.type;
                        checksum ^= comm->partial_packet.length;
                        for (int j = 0; j < comm->partial_packet.length; j++) {
                            checksum ^= comm->partial_packet.data[j];
                        }
                        
                        if (checksum == byte) {
                            if (comm->rx_packet_queue) {
                                if (xQueueSend(comm->rx_packet_queue, &comm->partial_packet, 0) != pdPASS) {
                                    printf("W: RX packet queue full, dropped packet type 0x%02x\n", comm->partial_packet.type);
                                }
                            } else {
                                // Minimal inline handling during scanning/handshake (no protocol task yet)
                                switch (comm->partial_packet.type) {
                                    case PACKET_TYPE_DISCOVERY:
                                        if (comm->state == COMM_STATE_SCANNING) {
                                            memcpy(comm->peer.chip_id, comm->partial_packet.data, 6);
                                            strncpy(comm->peer.chip_name, (char*)comm->partial_packet.data + 6, 32);
                                            comm->peer.chip_name[31] = '\0';
                                            printf("I: Discovered peer: %s\n", comm->peer.chip_name);
                                            ap_manager_add_log("I: Discovered peer (scan)\n");
                                            if (strcmp(comm->chip_name, comm->peer.chip_name) > 0) {
                                                printf("I: Peer has smaller name, I will initiate connection.\n");
                                                ap_manager_add_log("I: Peer has smaller name, I will initiate connection.\n");
                                                esp_comm_manager_connect_to_peer(comm->peer.chip_name);
                                            }
                                        }
                                        break;
                                    case PACKET_TYPE_HANDSHAKE_REQ:
                                        if (comm->state == COMM_STATE_SCANNING || comm->state == COMM_STATE_IDLE) {
                                            char requested_name[32];
                                            strncpy(requested_name, (char*)comm->partial_packet.data, 32);
                                            requested_name[31] = '\0';
                                            if (strcmp(requested_name, comm->chip_name) == 0) {
                                                comm->state = COMM_STATE_HANDSHAKE;
                                                comm->role = COMM_ROLE_SLAVE;
                                                if (comm->command_callback && !comm->command_queue) {
                                                    comm->command_queue = xQueueCreate(4, sizeof(comm_command_t));
                                                    if (comm->command_queue && !comm->command_executor_task_handle) {
                                                        xTaskCreate(command_executor_task, "comm_cmd_exec_task", 2048, comm, 5, &comm->command_executor_task_handle);
                                                    }
                                                }
                                                send_handshake_ack();
                                                comm->state = COMM_STATE_CONNECTED;
                                                if (comm->discovery_timer) {
                                                    xTimerStop(comm->discovery_timer, 0);
                                                    xTimerDelete(comm->discovery_timer, 0);
                                                    comm->discovery_timer = NULL;
                                                }
                                                // Bring up heavy resources now that we're connected
                                                if (!comm->tx_queue) {
                                                    comm->tx_queue = xQueueCreate(8, sizeof(comm_packet_t));
                                                }
                                                if (!comm->tx_task_handle && comm->tx_queue) {
                                                    xTaskCreate(tx_task, "comm_tx_task", 2048, comm, 11, &comm->tx_task_handle);
                                                }
                                                if (!comm->rx_packet_queue) {
                                                    comm->rx_packet_queue = xQueueCreate(12, sizeof(comm_packet_t));
                                                }
                                                if (!comm->protocol_task_handle && comm->rx_packet_queue) {
                                                    xTaskCreate(protocol_task, "comm_protocol_t", 3072, comm, 13, &comm->protocol_task_handle);
                                                }
                                                printf("I: Handshake complete - slave role\n");
                                                ap_manager_add_log("I: Handshake complete - slave role\n");
                                            }
                                        }
                                        break;
                                    case PACKET_TYPE_HANDSHAKE_ACK:
                                        if (comm->state == COMM_STATE_HANDSHAKE && comm->role == COMM_ROLE_MASTER) {
                                            comm->state = COMM_STATE_CONNECTED;
                                            if (comm->discovery_timer) {
                                                xTimerStop(comm->discovery_timer, 0);
                                                xTimerDelete(comm->discovery_timer, 0);
                                                comm->discovery_timer = NULL;
                                            }
                                            if (comm->command_callback && !comm->command_queue) {
                                                comm->command_queue = xQueueCreate(4, sizeof(comm_command_t));
                                                if (comm->command_queue && !comm->command_executor_task_handle) {
                                                    xTaskCreate(command_executor_task, "comm_cmd_exec_task", 2048, comm, 5, &comm->command_executor_task_handle);
                                                }
                                            }
                                            if (!comm->tx_queue) {
                                                comm->tx_queue = xQueueCreate(8, sizeof(comm_packet_t));
                                            }
                                            if (!comm->tx_task_handle && comm->tx_queue) {
                                                xTaskCreate(tx_task, "comm_tx_task", 2048, comm, 11, &comm->tx_task_handle);
                                            }
                                            if (!comm->rx_packet_queue) {
                                                comm->rx_packet_queue = xQueueCreate(12, sizeof(comm_packet_t));
                                            }
                                            if (!comm->protocol_task_handle && comm->rx_packet_queue) {
                                                xTaskCreate(protocol_task, "comm_protocol_t", 3072, comm, 13, &comm->protocol_task_handle);
                                            }
                                            printf("I: Handshake complete - master role\n");
                                            ap_manager_add_log("I: Handshake complete - master role\n");
                                        }
                                        break;
                                    default:
                                        // Unknown/ignored during scanning
                                        break;
                                }
                            }
                        }
                        comm->parse_state = PARSE_STATE_IDLE;
                    }
                    break;
            }
        }
    }
}

static void send_discovery_packet(void) {
    if (!s_comm_manager) return;
    
    comm_packet_t packet = {0};
    packet.start_byte = 0xAA;
    packet.type = PACKET_TYPE_DISCOVERY;
    packet.length = 38;
    
    memcpy(packet.data, s_comm_manager->chip_id, 6);
    strncpy((char*)packet.data + 6, s_comm_manager->chip_name, 31);
    ((char*)packet.data)[37] = '\0';
    
    send_packet(&packet);
}

static void send_handshake_request(const char* peer_name) {
    if (!s_comm_manager) return;
    
    comm_packet_t packet = {0};
    packet.start_byte = 0xAA;
    packet.type = PACKET_TYPE_HANDSHAKE_REQ;
    packet.length = 32;
    
    strncpy((char*)packet.data, peer_name, 32);
    
    send_packet(&packet);
}

static void send_handshake_ack(void) {
    if (!s_comm_manager) return;
    
    comm_packet_t packet = {0};
    packet.start_byte = 0xAA;
    packet.type = PACKET_TYPE_HANDSHAKE_ACK;
    packet.length = 0;
    
    send_packet(&packet);
}

static void command_executor_task(void* arg) {
    esp_comm_manager_t* comm = (esp_comm_manager_t*)arg;
    comm_command_t received_cmd;

    while (1) {
        if (xQueueReceive(comm->command_queue, &received_cmd, portMAX_DELAY) == pdPASS) {
            if (comm->command_callback) {
                // Temporarily set the remote command flag to indicate this is a remote command
                bool was_remote = esp_comm_manager_is_remote_command();
                esp_comm_manager_set_remote_command_flag(true);
                comm->command_callback(received_cmd.command, received_cmd.data, comm->callback_user_data);
                // Restore the previous remote command flag state
                esp_comm_manager_set_remote_command_flag(was_remote);
            }
        }
    }
}

static void protocol_task(void* arg) {
    esp_comm_manager_t* comm = (esp_comm_manager_t*)arg;
    comm_packet_t packet;
    static char log_buffer[128];
    
    while(1) {
        if (xQueueReceive(comm->rx_packet_queue, &packet, pdMS_TO_TICKS(10)) == pdPASS) {
            switch(packet.type) {
                case PACKET_TYPE_DISCOVERY:
                    if (comm->state == COMM_STATE_SCANNING) {
                        memcpy(comm->peer.chip_id, packet.data, 6);
                        strncpy(comm->peer.chip_name, (char*)packet.data + 6, 32);
                        comm->peer.chip_name[31] = '\0';
                        printf("I: Discovered peer: %s\n", comm->peer.chip_name);
                        
                        snprintf(log_buffer, sizeof(log_buffer), "I: Discovered peer: %s\n", comm->peer.chip_name);
                        ap_manager_add_log(log_buffer);

                        if (strcmp(comm->chip_name, comm->peer.chip_name) > 0) {
                            printf("I: Peer has smaller name, I will initiate connection.\n");
                            ap_manager_add_log("I: Peer has smaller name, I will initiate connection.\n");
                            esp_comm_manager_connect_to_peer(comm->peer.chip_name);
                        }
                    }
                    break;
                    
                case PACKET_TYPE_HANDSHAKE_REQ:
                    if (comm->state == COMM_STATE_SCANNING || comm->state == COMM_STATE_IDLE) {
                        char requested_name[32];
                        strncpy(requested_name, (char*)packet.data, 32);
                        requested_name[31] = '\0';
                        if (strcmp(requested_name, comm->chip_name) == 0) {
                            comm->state = COMM_STATE_HANDSHAKE;
                            comm->role = COMM_ROLE_SLAVE;
                            if (comm->command_callback && !comm->command_queue) {
                                comm->command_queue = xQueueCreate(4, sizeof(comm_command_t));
                                if (comm->command_queue && !comm->command_executor_task_handle) {
                                    xTaskCreate(command_executor_task, "comm_cmd_exec_task", 2048, comm, 5, &comm->command_executor_task_handle);
                                }
                            }
                            send_handshake_ack();
                            comm->state = COMM_STATE_CONNECTED;
                            if (comm->discovery_timer) {
                                xTimerStop(comm->discovery_timer, 0);
                                xTimerDelete(comm->discovery_timer, 0);
                                comm->discovery_timer = NULL;
                            }
                            printf("I: Handshake complete - slave role\n");
                            ap_manager_add_log("I: Handshake complete - slave role\n");
                        }
                    }
                    break;
                    
                case PACKET_TYPE_HANDSHAKE_ACK:
                    if (comm->state == COMM_STATE_HANDSHAKE && comm->role == COMM_ROLE_MASTER) {
                        comm->state = COMM_STATE_CONNECTED;
                        printf("I: Handshake complete - master role\n");
                        ap_manager_add_log("I: Handshake complete - master role\n");

                        if (comm->command_callback && !comm->command_queue) {
                            comm->command_queue = xQueueCreate(4, sizeof(comm_command_t));
                            if (comm->command_queue && !comm->command_executor_task_handle) {
                                xTaskCreate(command_executor_task, "comm_cmd_exec_task", 2048, comm, 5, &comm->command_executor_task_handle);
                            }
                        }
                        if (comm->discovery_timer) {
                            xTimerStop(comm->discovery_timer, 0);
                            xTimerDelete(comm->discovery_timer, 0);
                            comm->discovery_timer = NULL;
                        }
                    }
                    break;
                    
                case PACKET_TYPE_COMMAND:
                    if (comm->state == COMM_STATE_CONNECTED && comm->command_callback) {
                        if (!comm->command_queue) {
                            comm->command_queue = xQueueCreate(4, sizeof(comm_command_t));
                            if (comm->command_queue && !comm->command_executor_task_handle) {
                                xTaskCreate(command_executor_task, "comm_cmd_exec_task", 2048, comm, 5, &comm->command_executor_task_handle);
                            }
                        }
                        comm_command_t cmd_to_queue;
                        memset(&cmd_to_queue, 0, sizeof(comm_command_t));
                        
                        strncpy(cmd_to_queue.command, (char*)packet.data, 32);
                        cmd_to_queue.command[32] = '\0';
                        
                        size_t cmd_len = strlen(cmd_to_queue.command);
                        size_t data_start = cmd_len + 1;
                        
                        if (packet.length > data_start) {
                            size_t data_len = packet.length - data_start;
                            if (data_len > sizeof(cmd_to_queue.data) - 1) {
                                data_len = sizeof(cmd_to_queue.data) - 1;
                            }
                            strncpy(cmd_to_queue.data, (char*)packet.data + data_start, data_len);
                            cmd_to_queue.data[data_len] = '\0';
                        }

                        if (comm->command_queue && xQueueSend(comm->command_queue, &cmd_to_queue, pdMS_TO_TICKS(10)) != pdPASS) {
                            printf("W: Command queue full, dropped command: %s\n", cmd_to_queue.command);
                        }
                    }
                    break;
                    
                case PACKET_TYPE_PING:
                    if (comm->state == COMM_STATE_CONNECTED) {
                        comm_packet_t pong = {0};
                        pong.start_byte = 0xAA;
                        pong.type = PACKET_TYPE_PONG;
                        pong.length = 0;
                        
                        send_packet(&pong);
                    }
                    break;
                    
                case PACKET_TYPE_RESPONSE:
                    if (comm->state == COMM_STATE_CONNECTED) {
                        size_t len = packet.length;
                        if (len >= sizeof(packet.data)) {
                            len = sizeof(packet.data) - 1;
                        }
                        ((char*)packet.data)[len] = '\0';
                        printf("ESP Comm Response: %s\n", (char*)packet.data);
                        snprintf(log_buffer, sizeof(log_buffer), "ESP Comm Response: %s\n", (char*)packet.data);
                        ap_manager_add_log(log_buffer);
                    }
                    break;
                    
                default:
                    printf("W: Unknown packet type: 0x%02x\n", packet.type);
                    break;
            }
        }
    }
}

static void discovery_timer_callback(TimerHandle_t xTimer) {
    if (s_comm_manager && s_comm_manager->state == COMM_STATE_SCANNING) {
        send_discovery_packet();
    }
}

void esp_comm_manager_init_with_defaults(void) {
    esp_comm_manager_init(DEFAULT_TX_PIN, DEFAULT_RX_PIN, DEFAULT_BAUD_RATE);
}

void esp_comm_manager_init(gpio_num_t tx_pin, gpio_num_t rx_pin, uint32_t baud_rate) {
    if (s_comm_manager) {
        printf("W: Already initialized\n");
        return;
    }

    s_comm_manager = malloc(sizeof(esp_comm_manager_t));
    if (!s_comm_manager) {
        printf("E: Failed to allocate memory\n");
        return;
    }
    
    memset(s_comm_manager, 0, sizeof(esp_comm_manager_t));

    s_comm_manager->tx_pin = tx_pin;
    s_comm_manager->rx_pin = rx_pin;
    s_comm_manager->baud_rate = baud_rate;
    s_comm_manager->state = COMM_STATE_IDLE;
    s_comm_manager->role = COMM_ROLE_MASTER;
    s_comm_manager->is_executing_remote_cmd = false;
    s_comm_manager->parse_state = PARSE_STATE_IDLE;
    s_comm_manager->data_bytes_received = 0;
    

    s_comm_manager->command_callback = s_pending_callback;
    s_comm_manager->callback_user_data = s_pending_callback_user_data;

    esp_read_mac(s_comm_manager->chip_id, ESP_MAC_WIFI_STA);
    snprintf(s_comm_manager->chip_name, sizeof(s_comm_manager->chip_name), 
             "ESP_%02X%02X%02X", 
             s_comm_manager->chip_id[3], 
             s_comm_manager->chip_id[4], 
             s_comm_manager->chip_id[5]);

    uart_config_t uart_config = {
        .baud_rate = baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (serial_manager_get_uart_num() == (int)UART_NUM_1) {
        serial_manager_deinit();
    } else if (serial_manager_get_uart_num() == (int)UART_NUM_0) {
        if ((int)tx_pin == U0TXD_GPIO_NUM || (int)rx_pin == U0RXD_GPIO_NUM) {
            serial_manager_deinit();
        }
    }
    uart_driver_install(UART_NUM_1, COMM_BUFFER_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_1, &uart_config);
    uart_set_pin(UART_NUM_1, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    s_comm_manager->tx_queue = NULL;
    s_comm_manager->rx_packet_queue = NULL;
    s_comm_manager->command_queue = NULL;
    s_comm_manager->command_executor_task_handle = NULL;
    s_comm_manager->tx_task_handle = NULL;
    s_comm_manager->protocol_task_handle = NULL;
    xTaskCreate(rx_task, "comm_rx_task", 2048, s_comm_manager, 12, &s_comm_manager->rx_task_handle);
    
    s_comm_manager->discovery_timer = xTimerCreate("discovery_timer", 
                                                   pdMS_TO_TICKS(DISCOVERY_INTERVAL_MS),
                                                   pdTRUE, NULL, discovery_timer_callback);
    
    s_comm_manager->initialized = true;
    
    s_comm_manager->state = COMM_STATE_SCANNING;
    if (!s_comm_manager->discovery_timer) {
        s_comm_manager->discovery_timer = xTimerCreate("discovery_timer",
                                                       pdMS_TO_TICKS(DISCOVERY_INTERVAL_MS),
                                                       pdTRUE, NULL, discovery_timer_callback);
    }
    if (s_comm_manager->discovery_timer) {
        xTimerStart(s_comm_manager->discovery_timer, 0);
    }

    printf("I: ESP Comm Manager initialized as '%s' on TX:%d RX:%d at %lu baud - Auto-listening for peers\n", 
             s_comm_manager->chip_name, tx_pin, rx_pin, (unsigned long)baud_rate);
}

bool esp_comm_manager_set_pins(gpio_num_t tx_pin, gpio_num_t rx_pin) {
    if (!s_comm_manager || !s_comm_manager->initialized) {
        printf("E: Not initialized\n");
        return false;
    }
    
    if (s_comm_manager->state != COMM_STATE_IDLE) {
        printf("W: Cannot change pins while connected or scanning\n");
        return false;
    }
    
    s_comm_manager->tx_pin = tx_pin;
    s_comm_manager->rx_pin = rx_pin;

    if (serial_manager_get_uart_num() == (int)UART_NUM_1) {
        serial_manager_deinit();
    } else if (serial_manager_get_uart_num() == (int)UART_NUM_0) {
        if ((int)tx_pin == U0TXD_GPIO_NUM || (int)rx_pin == U0RXD_GPIO_NUM) {
            serial_manager_deinit();
        }
    }

    uart_set_pin(UART_NUM_1, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    
    printf("I: Changed pins to TX:%d RX:%d\n", tx_pin, rx_pin);
    return true;
}

bool esp_comm_manager_start_discovery(void) {
    if (!s_comm_manager || !s_comm_manager->initialized) {
        printf("E: Not initialized\n");
        return false;
    }
    
    if (s_comm_manager->state != COMM_STATE_IDLE) {
        printf("W: Already in discovery or connected\n");
        return false;
    }

    // release heavy resources during discovery
    if (s_comm_manager->protocol_task_handle) {
        vTaskDelete(s_comm_manager->protocol_task_handle);
        s_comm_manager->protocol_task_handle = NULL;
    }
    if (s_comm_manager->command_executor_task_handle) {
        vTaskDelete(s_comm_manager->command_executor_task_handle);
        s_comm_manager->command_executor_task_handle = NULL;
    }
    if (s_comm_manager->rx_packet_queue) {
        vQueueDelete(s_comm_manager->rx_packet_queue);
        s_comm_manager->rx_packet_queue = NULL;
    }
    if (s_comm_manager->command_queue) {
        vQueueDelete(s_comm_manager->command_queue);
        s_comm_manager->command_queue = NULL;
    }
    if (s_comm_manager->tx_task_handle) {
        vTaskDelete(s_comm_manager->tx_task_handle);
        s_comm_manager->tx_task_handle = NULL;
    }
    if (s_comm_manager->tx_queue) {
        vQueueDelete(s_comm_manager->tx_queue);
        s_comm_manager->tx_queue = NULL;
    }

    s_comm_manager->state = COMM_STATE_SCANNING;
    if (!s_comm_manager->discovery_timer) {
        s_comm_manager->discovery_timer = xTimerCreate("discovery_timer",
                                                       pdMS_TO_TICKS(DISCOVERY_INTERVAL_MS),
                                                       pdTRUE, NULL, discovery_timer_callback);
    }
    if (s_comm_manager->discovery_timer) {
        xTimerStart(s_comm_manager->discovery_timer, 0);
    }
    
    printf("I: Started discovery as '%s'\n", s_comm_manager->chip_name);
    return true;
}

bool esp_comm_manager_connect_to_peer(const char* peer_name) {
    if (!s_comm_manager || !s_comm_manager->initialized || !peer_name) {
        printf("E: Invalid parameters\n");
        return false;
    }
    
    if (s_comm_manager->state != COMM_STATE_SCANNING) {
        printf("W: Not in scanning state\n");
        return false;
    }
    
    xTimerStop(s_comm_manager->discovery_timer, 0);
    s_comm_manager->state = COMM_STATE_HANDSHAKE;
    s_comm_manager->role = COMM_ROLE_MASTER;
    
    send_handshake_request(peer_name);
    
    printf("I: Connecting to peer: %s\n", peer_name);
    
    // Log to web UI
    char log_msg[64];
    snprintf(log_msg, sizeof(log_msg), "I: Connecting to peer: %s\n", peer_name);
    ap_manager_add_log(log_msg);
    return true;
}

bool esp_comm_manager_send_command(const char* command, const char* data) {
    if (!s_comm_manager || !s_comm_manager->initialized || !command) {
        printf("E: Invalid parameters\n");
        return false;
    }
    
    if (s_comm_manager->state != COMM_STATE_CONNECTED) {
        printf("W: Not connected\n");
        return false;
    }
    
    comm_packet_t packet = {0};
    packet.start_byte = 0xAA;
    packet.type = PACKET_TYPE_COMMAND;
    
    size_t cmd_len = strlen(command);
    if (cmd_len > 32) {
        cmd_len = 32;
    }
    
    strncpy((char*)packet.data, command, cmd_len);
    ((char*)packet.data)[cmd_len] = '\0';
    packet.length = cmd_len + 1;
    
    if (data) {
        size_t data_len = strlen(data);
        size_t max_data_len = COMM_PACKET_SIZE - packet.length - 4;
        if (data_len > max_data_len) {
            data_len = max_data_len;
        }
        strncpy((char*)packet.data + packet.length, data, data_len);
        packet.length += data_len;
    }
    
    bool result = send_packet(&packet);
    if (result) {
        printf("I: Sent command: %s\n", command);
        
        char log_msg[64];
        snprintf(log_msg, sizeof(log_msg), "I: Sent command: %s\n", command);
        ap_manager_add_log(log_msg);
    }
    
    return result;
}

bool esp_comm_manager_send_response(const uint8_t* data, size_t length) {
    if (!s_comm_manager || !s_comm_manager->initialized || !data) {
        printf("E: Invalid parameters for send_response\n");
        return false;
    }
    
    if (s_comm_manager->state != COMM_STATE_CONNECTED) {
        printf("W: Not connected, can't send response\n");
        return false;
    }
    
    comm_packet_t packet = {0};
    packet.start_byte = 0xAA;
    packet.type = PACKET_TYPE_RESPONSE;
    
    size_t data_len = length;
    size_t max_data_len = COMM_PACKET_SIZE - 4; 
    if (data_len > max_data_len) {
        data_len = max_data_len;
    }
    memcpy((char*)packet.data, data, data_len);
    packet.length = data_len;
    
    return send_packet(&packet);
}

bool esp_comm_manager_is_connected(void) {
    return s_comm_manager && s_comm_manager->state == COMM_STATE_CONNECTED;
}

comm_state_t esp_comm_manager_get_state(void) {
    return s_comm_manager ? s_comm_manager->state : COMM_STATE_ERROR;
}

void esp_comm_manager_set_command_callback(comm_command_callback_t callback, void* user_data) {
    if (s_comm_manager) {
        s_comm_manager->command_callback = callback;
        s_comm_manager->callback_user_data = user_data;
    } else {
        s_pending_callback = callback;
        s_pending_callback_user_data = user_data;
    }
}

void esp_comm_manager_set_remote_command_flag(bool is_remote) {
    if (s_comm_manager) {
        s_comm_manager->is_executing_remote_cmd = is_remote;
    }
}

bool esp_comm_manager_is_remote_command(void) {
    return s_comm_manager && s_comm_manager->is_executing_remote_cmd;
}

void esp_comm_manager_disconnect(void) {
    if (s_comm_manager) {
        if (s_comm_manager->discovery_timer) {
            xTimerStop(s_comm_manager->discovery_timer, 0);
        }
        s_comm_manager->state = COMM_STATE_IDLE;
        printf("I: Disconnected\n");
    }
}

void esp_comm_manager_deinit(void) {
    if (!s_comm_manager) return;

    uart_driver_delete(UART_NUM_1);
    
    if (s_comm_manager->discovery_timer) {
        xTimerDelete(s_comm_manager->discovery_timer, 0);
    }
    
    if (s_comm_manager->rx_task_handle) {
        vTaskDelete(s_comm_manager->rx_task_handle);
    }
    if (s_comm_manager->tx_task_handle) {
        vTaskDelete(s_comm_manager->tx_task_handle);
    }
    if (s_comm_manager->protocol_task_handle) {
        vTaskDelete(s_comm_manager->protocol_task_handle);
    }
    if (s_comm_manager->command_executor_task_handle) {
        vTaskDelete(s_comm_manager->command_executor_task_handle);
    }
    if (s_comm_manager->rx_packet_queue) {
        vQueueDelete(s_comm_manager->rx_packet_queue);
    }
    if (s_comm_manager->tx_queue) {
        vQueueDelete(s_comm_manager->tx_queue);
    }
    if (s_comm_manager->command_queue) {
        vQueueDelete(s_comm_manager->command_queue);
    }
    
    free(s_comm_manager);
    s_comm_manager = NULL;
    printf("I: ESP Comm Manager de-initialized\n");
} 