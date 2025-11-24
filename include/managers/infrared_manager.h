#ifndef INFRARED_MANAGER_H
#define INFRARED_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/rmt_types.h"
#include "driver/rmt_rx.h"

typedef struct {
    char name[32];
    bool is_raw;
    union {
        struct {
            uint32_t *timings;
            size_t timings_size;
            uint32_t frequency;
            float duty_cycle;
        } raw;
        struct {
            char protocol[32];
            uint32_t address;
            uint32_t command;
        } message;
    } payload;
} infrared_signal_t;

bool infrared_manager_init(void);
bool infrared_manager_read_file(const char *path, infrared_signal_t *signal);
void infrared_manager_free_signal(infrared_signal_t *signal);
bool infrared_manager_parse_buffer_single(const char *buf, infrared_signal_t *signal);

/**
 * @brief Read a JSON file containing an array of IR signal objects.
 * @param path Path to the JSON file.
 * @param signals Output pointer to allocated array of signals.
 * @param count Output count of signals.
 * @return true on success, false on failure.
 */
bool infrared_manager_read_list(const char *path, infrared_signal_t **signals, size_t *count);

/**
 * @brief Free a list of IR signals.
 * @param signals Array of IR signals to free.
 * @param count Number of signals in the array.
 */
void infrared_manager_free_list(infrared_signal_t *signals, size_t count);

/**
 * @brief Transmit an IR signal.
 * @param signal Pointer to the IR signal to transmit.
 * @return true on success, false on failure.
 */
bool infrared_manager_transmit(const infrared_signal_t *signal);

// Background task support (static allocation to avoid heap usage)
#define INFRARED_MANAGER_TASK_STACK_SIZE 512
bool infrared_manager_start_background_task(TaskFunction_t fn, void *arg);
void infrared_manager_stop_background_task(void);

// Async enqueue of a parsed IR signal for background transmit
bool infrared_manager_enqueue_signal(const infrared_signal_t *signal);

// RX Support
bool infrared_manager_rx_init(void);
void infrared_manager_rx_deinit(void);
// Returns true if signal received, false on timeout (timeout_ms < 0 for infinite)
bool infrared_manager_rx_receive(infrared_signal_t *signal, int timeout_ms);

#define IR_RX_MAX_SYMBOLS 128
typedef struct {
    size_t num_symbols;
    rmt_symbol_word_t symbols[IR_RX_MAX_SYMBOLS];
} infrared_rx_event_t;

rmt_channel_handle_t infrared_manager_get_rx_channel(void);
QueueHandle_t infrared_manager_get_rx_queue(void);

#endif // INFRARED_MANAGER_H