#ifndef AUDIO_RECEIVER_MANAGER_H
#define AUDIO_RECEIVER_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the audio receiver manager on the S3 side.
 *
 * Sets up the GhostLink stream handler and decoder pipeline.
 *
 * @return ESP_OK on success
 */
esp_err_t audio_receiver_manager_init(void);

/**
 * @brief Deinitialize the audio receiver.
 */
void audio_receiver_manager_deinit(void);

/**
 * @brief Check if the receiver is initialized.
 */
bool audio_receiver_manager_is_initialized(void);

/**
 * @brief Start the audio receiver - enables stream handler to process packets.
 */
esp_err_t audio_receiver_manager_start(void);

/**
 * @brief Stop the audio receiver - disables stream handler and flushes state.
 */
void audio_receiver_manager_stop(void);

/**
 * @brief Pause receiver output immediately without discarding buffered data.
 *
 * Decoding stops and the I2S tail is flushed, but the ring buffer, decoder
 * state and playback position are preserved so a later resume() continues
 * from exactly the same audible point.
 */
void audio_receiver_manager_pause(void);

/**
 * @brief Resume receiver output from the exact point it was paused.
 *
 * Continues decoding the preserved ring buffer/decoder state; no flush,
 * no prebuffer wait. Only a fresh stream should use start() instead.
 */
void audio_receiver_manager_resume(void);

/**
 * @brief Flush the decoder state and ring buffer without stopping.
 *
 * Resets the MP3 decoder and clears the ring buffer to recover from
 * corruption or large jumps. The receiver remains active.
 */
void audio_receiver_manager_flush(void);

/**
 * @brief Feed MP3 stream data into the local receiver pipeline.
 *
 * Used when this board plays back through its own speaker instead of
 * streaming to a GhostLink peer. Writes as much data as the ring buffer
 * accepts and returns the accepted byte count (0 = full or inactive;
 * callers should retry after a short delay).
 *
 * @param data Pointer to MP3 stream bytes
 * @param len  Number of bytes to write
 * @return Number of bytes accepted
 */
size_t audio_receiver_manager_write_stream(const uint8_t *data, size_t len);

/**
 * @brief Set the DAC sample rate based on decoded MP3 info.
 *
 * @param sample_rate Detected sample rate in Hz
 * @return ESP_OK on success
 */
esp_err_t audio_receiver_manager_set_sample_rate(uint32_t sample_rate);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_RECEIVER_MANAGER_H
