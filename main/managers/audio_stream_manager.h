#ifndef AUDIO_STREAM_MANAGER_H
#define AUDIO_STREAM_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Audio stream state */
typedef enum {
    AUDIO_STREAM_STATE_IDLE = 0,
    AUDIO_STREAM_STATE_PLAYING,
    AUDIO_STREAM_STATE_PAUSED,
    AUDIO_STREAM_STATE_STOPPED
} audio_stream_state_t;

/**
 * @brief Initialize the audio stream manager.
 *
 * Scans the configured audio directory for MP3 files.
 *
 * @return ESP_OK on success
 */
esp_err_t audio_stream_manager_init(void);

/**
 * @brief Deinitialize the audio stream manager.
 */
void audio_stream_manager_deinit(void);

/**
 * @brief Get the current stream state.
 */
audio_stream_state_t audio_stream_manager_get_state(void);

/**
 * @brief Get the number of MP3 files found.
 */
int audio_stream_manager_get_file_count(void);

/**
 * @brief Get the filename at the given index.
 *
 * @param index File index
 * @return Filename string (internal pointer, do not free), or NULL
 */
const char *audio_stream_manager_get_filename(int index);

/**
 * @brief Get the index of the currently playing file.
 */
int audio_stream_manager_get_current_index(void);

/**
 * @brief Start playing the file at the given index.
 *
 * @param index File index (0-based)
 * @return ESP_OK on success
 */
esp_err_t audio_stream_manager_play(int index);

/**
 * @brief Pause playback.
 */
esp_err_t audio_stream_manager_pause(void);

/**
 * @brief Resume playback.
 */
esp_err_t audio_stream_manager_resume(void);

/**
 * @brief Stop playback.
 */
esp_err_t audio_stream_manager_stop(void);

/**
 * @brief Play the next file in the list.
 */
esp_err_t audio_stream_manager_next(void);

/**
 * @brief Play the previous file in the list.
 */
esp_err_t audio_stream_manager_prev(void);

/**
 * @brief Check if the audio stream manager is initialized.
 */
bool audio_stream_manager_is_initialized(void);

/**
 * @brief Whether the last audio directory scan could access SD storage.
 */
bool audio_stream_manager_sd_available(void);

/**
 * @brief Stream embedded MP3 data to the connected peer.
 *
 * Plays MP3 data directly from flash (embedded binary) via GhostLink.
 * Only works when a peer is connected.
 *
 * @param data Pointer to embedded MP3 data
 * @param len  Length of MP3 data in bytes
 * @return ESP_OK on success
 */
esp_err_t audio_stream_manager_play_embedded(const uint8_t *data, size_t len);

/**
 * @brief Get the current playback position in bytes.
 *
 * @return Current offset in bytes, or 0 if not playing
 */
size_t audio_stream_manager_get_position(void);

/**
 * @brief Get the total size of the current track in bytes.
 *
 * @return Total size in bytes, or 0 if unknown
 */
size_t audio_stream_manager_get_total_size(void);

/**
 * @brief Get the detected MP3 bitrate in kbps.
 *
 * Returns 0 until the first frame header has been parsed.
 *
 * @return Bitrate in kbps, or 0 if not yet detected
 */
uint16_t audio_stream_manager_get_bitrate(void);

/**
 * @brief Update sender-side pacing with receiver ringbuffer fill feedback.
 */
void audio_stream_manager_update_receiver_status(size_t fill_bytes, size_t capacity_bytes, uint32_t played_ms);

/**
 * @brief Get receiver-reported audible playback position in milliseconds.
 */
uint32_t audio_stream_manager_get_playback_ms(void);

/**
 * @brief Get estimated track duration in milliseconds.
 */
uint32_t audio_stream_manager_get_duration_ms(void);

/**
 * @brief Get latest receiver audio ringbuffer fill percent.
 */
uint8_t audio_stream_manager_get_receiver_buffer_percent(void);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_STREAM_MANAGER_H
