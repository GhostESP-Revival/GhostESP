#ifndef INFRARED_MANAGER_H
#define INFRARED_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

/**
 * @brief Brute force transmit all signals in a file with a delay between each.
 * @param path Path to the file containing IR signals.
 * @param delay_ms Delay in milliseconds between transmissions.
 * @return true on success, false on failure.
 */
bool infrared_manager_bruteforce(const char *path, uint32_t delay_ms);

// Background task support (static allocation to avoid heap usage)
#define INFRARED_MANAGER_TASK_STACK_SIZE 512
bool infrared_manager_start_background_task(TaskFunction_t fn, void *arg);
void infrared_manager_stop_background_task(void);

// Async enqueue of a parsed IR signal for background transmit
bool infrared_manager_enqueue_signal(const infrared_signal_t *signal);

#endif // INFRARED_MANAGER_H