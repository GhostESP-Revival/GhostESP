#ifndef MIC_VISUALIZER_H
#define MIC_VISUALIZER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Initialize the MIC visualizer task
 * This starts a task that reads from the microphone and sends amplitude
 * data over GhostLink to a peer device with RGB LEDs.
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mic_visualizer_init(void);

/**
 * @brief Start the MIC visualizer transmission
 * Begins sending amplitude data over GhostLink
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mic_visualizer_start(void);

/**
 * @brief Stop the MIC visualizer transmission
 * Stops sending amplitude data
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mic_visualizer_stop(void);

/**
 * @brief Check if MIC visualizer is running
 * @return true if running, false otherwise
 */
bool mic_visualizer_is_running(void);

/**
 * @brief Latest microphone spectrum, for a local consumer (the Visualizer
 * screen) that animates from the Media I/O mic instead of a network stream.
 *
 * The mic task already computes this each capture loop, so a view can read it
 * without opening a second I2S channel.
 *
 * @param bands receives [bass, low_mid, high_mid, treble], each 0-255
 * @param level receives the smoothed overall amplitude 0-255 (may be NULL)
 * @return true once a capture has been analysed
 */
bool mic_visualizer_get_bands(uint8_t bands[4], uint8_t *level);

/**
 * @brief Deinitialize the MIC visualizer
 * Stops task and frees resources
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mic_visualizer_deinit(void);

#endif // MIC_VISUALIZER_H
