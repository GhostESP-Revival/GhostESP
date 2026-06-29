#include "managers/audio_stream_manager.h"

#ifdef CONFIG_HAS_AUDIO_PLAYER
#include "core/esp_comm_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/microphone/mic_visualizer.h"
#ifdef CONFIG_HAS_TLV320DAC_I2C
#include "tlv320dac3100.h"
#endif
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static const char *TAG = "AudioStream";

#define MAX_MP3_FILES     64
#define MAX_FILENAME_LEN  64
#define STREAM_CHUNK_SIZE 512
#define STREAM_READ_BURST_SIZE (256 * 1024)
#define STREAM_SEND_WAIT_MS 150
#define STREAM_TASK_STACK 8192
#define STREAM_TASK_PRIO  18
#define STREAM_INTER_PACKET_DELAY_MS 2
/* Baseline send rate; receiver fill feedback slows this when the S3 buffer is healthy. */
#define STREAM_MAX_BYTES_PER_SEC 40000
/* Let the receiver fill its decode buffer quickly, then switch to real-time media pacing. */
#define STREAM_PREFILL_BYTES (48 * 1024)
#define STREAM_FEEDBACK_STALE_MS 1500
#define STREAM_RX_TARGET_PERCENT 55
#define STREAM_RX_LOW_PERCENT 35
#define STREAM_RX_HIGH_PERCENT 75
#define STREAM_RX_PAUSE_DELAY_MS 20
#define STREAM_RX_MAX_PAUSE_MS 750
#define STREAM_MAX_ACCEPTED_BITRATE_KBPS 264

typedef struct {
    char (*filenames)[MAX_FILENAME_LEN];
    int file_count;
    int current_index;
    audio_stream_state_t state;
    size_t stream_offset;
    uint32_t stream_id;
    TaskHandle_t stream_task;
    SemaphoreHandle_t mutex;
    bool sd_available;
    bool initialized;
    bool embedded_source;
    const uint8_t *embedded_data;
    size_t embedded_len;
    uint16_t detected_bitrate_kbps;
    bool mic_visualizer_was_running;
    size_t receiver_fill_bytes;
    size_t receiver_capacity_bytes;
    uint32_t receiver_played_ms;
    TickType_t receiver_status_tick;
} audio_stream_ctx_t;

static audio_stream_ctx_t s_ctx = {0};
static volatile bool s_stream_task_exited = true;
static StackType_t *s_stream_task_stack = NULL;
static StaticTask_t *s_stream_task_tcb = NULL;
static TaskHandle_t s_play_precheck_task = NULL;
static StackType_t *s_play_precheck_stack = NULL;
static StaticTask_t *s_play_precheck_tcb = NULL;
#define STREAM_PLAY_PRECHECK_STACK 4096
#define STREAM_PLAY_PRECHECK_PRIO  5

typedef struct {
    int index;
    uint32_t stream_id;
} play_precheck_job_t;

typedef struct {
    const uint8_t *data;
    size_t len;
    uint32_t stream_id;
} play_embedded_precheck_job_t;

static void audio_stream_task(void *arg);
static TaskHandle_t create_audio_stream_task(uint32_t stream_id);
static int scan_mp3_files(void);
static bool is_mp3_file(const char *filename);
static uint16_t parse_mp3_bitrate(const uint8_t *data, size_t len);
static bool audio_sd_begin(bool *display_was_suspended, bool *did_mount, bool ensure_dirs);
static void audio_sd_end(bool display_was_suspended, bool did_mount);
static void audio_stream_play_precheck_task(void *arg);
static void audio_stream_play_embedded_precheck_task(void *arg);
static esp_err_t audio_stream_start_after_precheck(int index, uint16_t bitrate, size_t total_size);
static esp_err_t audio_stream_start_embedded_after_precheck(const uint8_t *data,
                                                            size_t len,
                                                            uint16_t bitrate);

static bool audio_sd_should_jit(void)
{
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    return strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0 ||
           strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething2") == 0;
#else
    return false;
#endif
}

static void audio_apply_one_shot_headphone_route(void)
{
#ifdef CONFIG_HAS_TLV320DAC_I2C
    esp_err_t ret = tlv320dac3100_detect_headphone_once();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "One-shot headphone route failed: %s", esp_err_to_name(ret));
    }
#endif
}

static void audio_pause_competing_streams(void)
{
    s_ctx.mic_visualizer_was_running = mic_visualizer_is_running();
    if (s_ctx.mic_visualizer_was_running) {
        ESP_LOGI(TAG, "Pausing MIC visualizer during audio stream");
        (void)mic_visualizer_stop();
    }
}

static void audio_resume_competing_streams(void)
{
    if (s_ctx.mic_visualizer_was_running) {
        s_ctx.mic_visualizer_was_running = false;
        ESP_LOGI(TAG, "Resuming MIC visualizer after audio stream");
        (void)mic_visualizer_start();
    }
}

static bool audio_sd_begin(bool *display_was_suspended, bool *did_mount, bool ensure_dirs)
{
    if (display_was_suspended) *display_was_suspended = false;
    if (did_mount) *did_mount = false;

    if (audio_sd_should_jit()) {
        esp_err_t mount_err = sd_card_mount_for_flush(display_was_suspended);
        if (mount_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to mount SD card for audio: %s", esp_err_to_name(mount_err));
            return false;
        }
        if (did_mount) *did_mount = true;
    }

    if (ensure_dirs) {
        esp_err_t dir_err = sd_card_setup_directory_structure();
        if (dir_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to ensure audio directory structure: %s", esp_err_to_name(dir_err));
        }
    }

    return true;
}

static void audio_sd_end(bool display_was_suspended, bool did_mount)
{
    if (did_mount) {
        sd_card_unmount_after_flush(display_was_suspended);
    }
}

static bool is_mp3_file(const char *filename)
{
    size_t len = strlen(filename);
    if (len < 4) return false;
    return (strcasecmp(filename + len - 4, ".mp3") == 0);
}

static TaskHandle_t create_audio_stream_task(uint32_t stream_id)
{
    if (!s_stream_task_stack) {
        s_stream_task_stack = (StackType_t *)heap_caps_malloc(
            STREAM_TASK_STACK * sizeof(StackType_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_stream_task_tcb) {
        s_stream_task_tcb = (StaticTask_t *)heap_caps_malloc(
            sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    if (s_stream_task_stack && s_stream_task_tcb) {
        TaskHandle_t handle = xTaskCreateStatic(audio_stream_task, "audio_stream",
                                                STREAM_TASK_STACK, (void *)(uintptr_t)stream_id,
                                                STREAM_TASK_PRIO, s_stream_task_stack, s_stream_task_tcb);
        if (handle) {
            ESP_LOGI(TAG, "Audio stream task created (%d bytes PSRAM stack)",
                     (int)(STREAM_TASK_STACK * sizeof(StackType_t)));
        }
        return handle;
    }

    ESP_LOGE(TAG, "Failed to allocate stream task resources");
    return NULL;
}

static int scan_mp3_files(void)
{
    s_ctx.file_count = 0;

    bool display_was_suspended = false;
    bool did_mount = false;
    s_ctx.sd_available = false;
    if (!audio_sd_begin(&display_was_suspended, &did_mount, true)) {
        return 0;
    }
    s_ctx.sd_available = true;

    const char *path = CONFIG_AUDIO_FILES_PATH;
    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGW(TAG, "Could not open audio dir '%s', trying to create", path);
        /* Try to create the directory */
        if (mkdir(path, 0755) != 0) {
            ESP_LOGW(TAG, "Could not create audio dir '%s'", path);
        }
        dir = opendir(path);
        if (!dir) {
            audio_sd_end(display_was_suspended, did_mount);
            return 0;
        }
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_ctx.file_count < MAX_MP3_FILES) {
        bool is_file = false;
        if (entry->d_type == DT_REG) {
            is_file = true;
        } else if (entry->d_type == DT_UNKNOWN) {
            /* FAT32 doesn't populate d_type; fall back to stat */
            char fullpath[512];
            snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);
            struct stat st;
            if (stat(fullpath, &st) == 0 && S_ISREG(st.st_mode)) {
                is_file = true;
            }
        }
        if (is_file && is_mp3_file(entry->d_name)) {
            strncpy(s_ctx.filenames[s_ctx.file_count], entry->d_name, MAX_FILENAME_LEN - 1);
            s_ctx.filenames[s_ctx.file_count][MAX_FILENAME_LEN - 1] = '\0';
            s_ctx.file_count++;
        }
    }
    closedir(dir);
    audio_sd_end(display_was_suspended, did_mount);

    /* Sort alphabetically by filename */
    for (int i = 0; i < s_ctx.file_count - 1; i++) {
        for (int j = i + 1; j < s_ctx.file_count; j++) {
            if (strcasecmp(s_ctx.filenames[i], s_ctx.filenames[j]) > 0) {
                char temp[MAX_FILENAME_LEN];
                memcpy(temp, s_ctx.filenames[i], MAX_FILENAME_LEN);
                memcpy(s_ctx.filenames[i], s_ctx.filenames[j], MAX_FILENAME_LEN);
                memcpy(s_ctx.filenames[j], temp, MAX_FILENAME_LEN);
            }
        }
    }

    ESP_LOGI(TAG, "Found %d MP3 files in '%s'", s_ctx.file_count, path);
    return s_ctx.file_count;
}

esp_err_t audio_stream_manager_init(void)
{
    if (s_ctx.initialized) {
        return ESP_OK;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.mutex = xSemaphoreCreateMutex();
    if (!s_ctx.mutex) {
        return ESP_ERR_NO_MEM;
    }

    s_ctx.filenames = (char (*)[MAX_FILENAME_LEN])heap_caps_calloc(MAX_MP3_FILES, MAX_FILENAME_LEN,
                                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ctx.filenames) {
        s_ctx.filenames = (char (*)[MAX_FILENAME_LEN])calloc(MAX_MP3_FILES, MAX_FILENAME_LEN);
        if (!s_ctx.filenames) {
            vSemaphoreDelete(s_ctx.mutex);
            s_ctx.mutex = NULL;
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGW(TAG, "Audio filename table using internal RAM fallback");
    } else {
        ESP_LOGI(TAG, "Audio filename table allocated from PSRAM: %d bytes",
                 MAX_MP3_FILES * MAX_FILENAME_LEN);
    }

    scan_mp3_files();

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "Audio stream manager initialized (%d files)", s_ctx.file_count);
    return ESP_OK;
}

void audio_stream_manager_deinit(void)
{
    if (!s_ctx.initialized) return;

    audio_stream_manager_stop();

    for (int i = 0; i < 10; i++) {
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        bool task_gone = (s_ctx.stream_task == NULL);
        xSemaphoreGive(s_ctx.mutex);
        if (task_gone) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    char (*filenames)[MAX_FILENAME_LEN] = s_ctx.filenames;
    s_ctx.filenames = NULL;
    if (s_ctx.mutex) {
        vSemaphoreDelete(s_ctx.mutex);
        s_ctx.mutex = NULL;
    }
    if (filenames) {
        free(filenames);
    }

    if (s_play_precheck_task) {
        vTaskDelete(s_play_precheck_task);
        s_play_precheck_task = NULL;
    }
    if (s_play_precheck_stack) {
        free(s_play_precheck_stack);
        s_play_precheck_stack = NULL;
    }
    if (s_play_precheck_tcb) {
        free(s_play_precheck_tcb);
        s_play_precheck_tcb = NULL;
    }
    if (s_stream_task_stack) {
        free(s_stream_task_stack);
        s_stream_task_stack = NULL;
    }
    if (s_stream_task_tcb) {
        free(s_stream_task_tcb);
        s_stream_task_tcb = NULL;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
}

audio_stream_state_t audio_stream_manager_get_state(void)
{
    return s_ctx.state;
}

int audio_stream_manager_get_file_count(void)
{
    return s_ctx.file_count;
}

const char *audio_stream_manager_get_filename(int index)
{
    if (index < 0 || index >= s_ctx.file_count) return NULL;
    return s_ctx.filenames[index];
}

int audio_stream_manager_get_current_index(void)
{
    return s_ctx.current_index;
}

esp_err_t audio_stream_manager_play(int index)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    if (index < 0 || index >= s_ctx.file_count) return ESP_ERR_INVALID_ARG;

    /* Take only the mutex long enough to bump stream_id/state and select the
     * target. The actual file I/O + bitrate probe runs on a low-priority
     * worker task to keep the LVGL tick stack safe. */
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);

    s_ctx.state = AUDIO_STREAM_STATE_STOPPED;
    s_ctx.stream_id++;
    uint32_t stream_id = s_ctx.stream_id;
    s_ctx.detected_bitrate_kbps = 0;
    s_ctx.receiver_fill_bytes = 0;
    s_ctx.receiver_capacity_bytes = 0;
    s_ctx.receiver_played_ms = 0;
    s_ctx.receiver_status_tick = 0;

    xSemaphoreGive(s_ctx.mutex);

    if (s_play_precheck_task) {
        vTaskDelete(s_play_precheck_task);
        s_play_precheck_task = NULL;
    }

    if (!s_play_precheck_stack) {
        s_play_precheck_stack = (StackType_t *)heap_caps_malloc(
            STREAM_PLAY_PRECHECK_STACK * sizeof(StackType_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_play_precheck_tcb) {
        s_play_precheck_tcb = (StaticTask_t *)heap_caps_malloc(
            sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    play_precheck_job_t *job = (play_precheck_job_t *)malloc(sizeof(*job));
    if (!job) {
        return ESP_ERR_NO_MEM;
    }
    job->index = index;
    job->stream_id = stream_id;

    if (s_play_precheck_stack && s_play_precheck_tcb) {
        s_play_precheck_task = xTaskCreateStatic(
            audio_stream_play_precheck_task, "audio_play_pre",
            STREAM_PLAY_PRECHECK_STACK, job, STREAM_PLAY_PRECHECK_PRIO,
            s_play_precheck_stack, s_play_precheck_tcb);
    } else {
        if (xTaskCreate(audio_stream_play_precheck_task, "audio_play_pre",
                        STREAM_PLAY_PRECHECK_STACK, job, STREAM_PLAY_PRECHECK_PRIO,
                        &s_play_precheck_task) != pdPASS) {
            s_play_precheck_task = NULL;
        }
    }

    if (!s_play_precheck_task) {
        free(job);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void audio_stream_play_precheck_task(void *arg)
{
    play_precheck_job_t *job = (play_precheck_job_t *)arg;
    int index = job->index;
    uint32_t stream_id = job->stream_id;
    free(job);

    char path[160];
    snprintf(path, sizeof(path), "%s/%s", CONFIG_AUDIO_FILES_PATH, s_ctx.filenames[index]);

    bool display_was_suspended = false;
    bool did_mount = false;
    /* Keep the scan small so PSRAM doesn't compete with the SD/flash bus and
     * starve the SysTick. Most MP3s have an ID3v2 tag < 16KB and a valid frame
     * header within the first frame. Allocate from PSRAM so the 8KB buffer
     * doesn't blow the task's internal stack. */
    const size_t scan_size = 8 * 1024;
    uint8_t *header_buf = (uint8_t *)heap_caps_malloc(scan_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!header_buf) {
        ESP_LOGE(TAG, "Precheck failed: cannot allocate %u bytes from PSRAM", (unsigned)scan_size);
        s_play_precheck_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    uint16_t bitrate = 0;
    uint16_t max_bitrate = 0;
    bool found_frame = false;
    size_t total_size = 0;

    if (!audio_sd_begin(&display_was_suspended, &did_mount, false)) {
        free(header_buf);
        s_play_precheck_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    FILE *f = fopen(path, "rb");
    if (f) {
        size_t header_len = fread(header_buf, 1, scan_size, f);

        /* Skip ID3v2 header if present so we land on the first real MP3 frame. */
        size_t scan_start = 0;
        if (header_len >= 10 && memcmp(header_buf, "ID3", 3) == 0) {
            uint32_t tag_size = ((uint32_t)(header_buf[6] & 0x7F) << 21) |
                                ((uint32_t)(header_buf[7] & 0x7F) << 14) |
                                ((uint32_t)(header_buf[8] & 0x7F) << 7)  |
                                ((uint32_t)(header_buf[9] & 0x7F));
            size_t tag_end = 10 + tag_size;
            if (tag_end > header_len) tag_end = header_len;
            scan_start = tag_end;
        }

        bitrate = parse_mp3_bitrate(header_buf + scan_start, header_len - scan_start);
        if (bitrate > 0) {
            found_frame = true;
            max_bitrate = bitrate;
        }

        fseek(f, 0, SEEK_END);
        total_size = (size_t)ftell(f);
        fclose(f);
    } else {
        ESP_LOGE(TAG, "Precheck failed: cannot open '%s'", path);
    }
    audio_sd_end(display_was_suspended, did_mount);

    if (f == NULL) {
        free(header_buf);
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        s_ctx.state = AUDIO_STREAM_STATE_IDLE;
        xSemaphoreGive(s_ctx.mutex);
        s_play_precheck_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (found_frame && max_bitrate > STREAM_MAX_ACCEPTED_BITRATE_KBPS) {
        ESP_LOGW(TAG, "Refusing %u kbps MP3; bitrate must be at most %u kbps",
                 (unsigned)max_bitrate, (unsigned)STREAM_MAX_ACCEPTED_BITRATE_KBPS);
        free(header_buf);
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        s_ctx.state = AUDIO_STREAM_STATE_IDLE;
        xSemaphoreGive(s_ctx.mutex);
        s_play_precheck_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (!found_frame) {
        /* Some MP3s (e.g. with Xing/LAME VBR headers, large ID3v2 tags, or
         * embedded tracks) don't have a parseable frame in the first 64KB.
         * Don't block playback in that case; the stream task will detect
         * the bitrate on the first frame it sends. */
        ESP_LOGW(TAG, "Bitrate not detected in precheck for '%s'; allowing playback",
                 s_ctx.filenames[index]);
        bitrate = 0;
    }

    audio_stream_start_after_precheck(index, bitrate, total_size);
    free(header_buf);
    s_play_precheck_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t audio_stream_start_after_precheck(int index, uint16_t bitrate, size_t total_size)
{
    /* Wait for any previous stream task to fully exit before reusing the
     * PSRAM stack. The wait must happen BEFORE taking the mutex, because
     * the old task needs the mutex to check stream_id and exit. */
    const TickType_t wait_start = xTaskGetTickCount();
    while (!s_stream_task_exited) {
        if ((xTaskGetTickCount() - wait_start) > pdMS_TO_TICKS(10000)) {
            ESP_LOGE(TAG, "Previous stream task stuck for 10s; proceeding anyway");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);

    s_ctx.current_index = index;
    s_ctx.stream_offset = 0;
    s_ctx.embedded_source = false;
    s_ctx.embedded_data = NULL;
    s_ctx.embedded_len = total_size;
    s_ctx.detected_bitrate_kbps = bitrate;

    s_ctx.state = AUDIO_STREAM_STATE_STOPPED;
    s_ctx.stream_id++;
    uint32_t stream_id = s_ctx.stream_id;

    xSemaphoreGive(s_ctx.mutex);

    ESP_LOGI(TAG, "Playing: %s", s_ctx.filenames[index]);

    audio_pause_competing_streams();
    audio_apply_one_shot_headphone_route();
    (void)esp_comm_manager_send_command("audio", "start");

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    s_ctx.state = AUDIO_STREAM_STATE_PLAYING;
    s_ctx.stream_task = create_audio_stream_task(stream_id);
    esp_err_t ret = s_ctx.stream_task ? ESP_OK : ESP_ERR_NO_MEM;
    if (!s_ctx.stream_task) {
        s_ctx.state = AUDIO_STREAM_STATE_IDLE;
    }
    xSemaphoreGive(s_ctx.mutex);
    return ret;
}

esp_err_t audio_stream_manager_pause(void)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    if (s_ctx.state == AUDIO_STREAM_STATE_PLAYING) {
        s_ctx.state = AUDIO_STREAM_STATE_PAUSED;
        (void)esp_comm_manager_send_command("audio", "pause");
        ESP_LOGI(TAG, "Paused");
    }
    xSemaphoreGive(s_ctx.mutex);
    return ESP_OK;
}

esp_err_t audio_stream_manager_resume(void)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    if (s_ctx.state == AUDIO_STREAM_STATE_PAUSED) {
        (void)esp_comm_manager_send_command("audio", "start");
        s_ctx.state = AUDIO_STREAM_STATE_PLAYING;
        ESP_LOGI(TAG, "Resumed");
    }
    xSemaphoreGive(s_ctx.mutex);
    return ESP_OK;
}

esp_err_t audio_stream_manager_stop(void)
{
    if (!s_ctx.initialized) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);

    s_ctx.state = AUDIO_STREAM_STATE_STOPPED;
    s_ctx.stream_id++;

    (void)esp_comm_manager_send_command("audio", "stop");
    audio_resume_competing_streams();

    xSemaphoreGive(s_ctx.mutex);

    /* Wait for stream task to exit */
    if (s_ctx.stream_task) {
        /* Give task time to notice stop and exit */
        for (int i = 0; i < 10 && s_ctx.stream_task; ++i) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    ESP_LOGI(TAG, "Stopped");
    return ESP_OK;
}

esp_err_t audio_stream_manager_next(void)
{
    if (!s_ctx.initialized || s_ctx.file_count == 0) return ESP_ERR_INVALID_STATE;
    int next = s_ctx.current_index + 1;
    if (next >= s_ctx.file_count) next = 0;
    return audio_stream_manager_play(next);
}

esp_err_t audio_stream_manager_prev(void)
{
    if (!s_ctx.initialized || s_ctx.file_count == 0) return ESP_ERR_INVALID_STATE;
    int prev = s_ctx.current_index - 1;
    if (prev < 0) prev = s_ctx.file_count - 1;
    return audio_stream_manager_play(prev);
}

bool audio_stream_manager_is_initialized(void)
{
    return s_ctx.initialized;
}

bool audio_stream_manager_sd_available(void)
{
    return s_ctx.sd_available;
}

esp_err_t audio_stream_manager_play_embedded(const uint8_t *data, size_t len)
{
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    if (!esp_comm_manager_is_connected()) return ESP_ERR_INVALID_STATE;

    if (!s_ctx.initialized) {
        esp_err_t init_err = audio_stream_manager_init();
        if (init_err != ESP_OK) return init_err;
    }

    /* Defer bitrate check + start to a worker task. Caller is often the LVGL
     * tick task; running file I/O or large parsing there overflows the stack. */
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    s_ctx.state = AUDIO_STREAM_STATE_STOPPED;
    s_ctx.stream_id++;
    uint32_t stream_id = s_ctx.stream_id;
    s_ctx.detected_bitrate_kbps = 0;
    s_ctx.receiver_fill_bytes = 0;
    s_ctx.receiver_capacity_bytes = 0;
    s_ctx.receiver_played_ms = 0;
    s_ctx.receiver_status_tick = 0;
    xSemaphoreGive(s_ctx.mutex);

    if (s_play_precheck_task) {
        vTaskDelete(s_play_precheck_task);
        s_play_precheck_task = NULL;
    }

    if (!s_play_precheck_stack) {
        s_play_precheck_stack = (StackType_t *)heap_caps_malloc(
            STREAM_PLAY_PRECHECK_STACK * sizeof(StackType_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_play_precheck_tcb) {
        s_play_precheck_tcb = (StaticTask_t *)heap_caps_malloc(
            sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    play_embedded_precheck_job_t *job = (play_embedded_precheck_job_t *)malloc(sizeof(*job));
    if (!job) {
        return ESP_ERR_NO_MEM;
    }
    job->data = data;
    job->len = len;
    job->stream_id = stream_id;

    if (s_play_precheck_stack && s_play_precheck_tcb) {
        s_play_precheck_task = xTaskCreateStatic(
            audio_stream_play_embedded_precheck_task, "audio_play_emb",
            STREAM_PLAY_PRECHECK_STACK, job, STREAM_PLAY_PRECHECK_PRIO,
            s_play_precheck_stack, s_play_precheck_tcb);
    } else {
        if (xTaskCreate(audio_stream_play_embedded_precheck_task, "audio_play_emb",
                        STREAM_PLAY_PRECHECK_STACK, job, STREAM_PLAY_PRECHECK_PRIO,
                        &s_play_precheck_task) != pdPASS) {
            s_play_precheck_task = NULL;
        }
    }

    if (!s_play_precheck_task) {
        free(job);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void audio_stream_play_embedded_precheck_task(void *arg)
{
    play_embedded_precheck_job_t *job = (play_embedded_precheck_job_t *)arg;
    uint16_t bitrate = parse_mp3_bitrate(job->data, job->len);
    if (bitrate > STREAM_MAX_ACCEPTED_BITRATE_KBPS && bitrate > 0) {
        ESP_LOGW(TAG, "Refusing %u kbps MP3; bitrate must be at most %u kbps",
                 (unsigned)bitrate, (unsigned)STREAM_MAX_ACCEPTED_BITRATE_KBPS);
        free(job);
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        s_ctx.state = AUDIO_STREAM_STATE_IDLE;
        xSemaphoreGive(s_ctx.mutex);
        s_play_precheck_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    const uint8_t *data = job->data;
    size_t len = job->len;
    free(job);

    audio_stream_start_embedded_after_precheck(data, len, bitrate);
    s_play_precheck_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t audio_stream_start_embedded_after_precheck(const uint8_t *data,
                                                             size_t len,
                                                             uint16_t bitrate)
{
    const TickType_t wait_start = xTaskGetTickCount();
    while (!s_stream_task_exited) {
        if ((xTaskGetTickCount() - wait_start) > pdMS_TO_TICKS(10000)) {
            ESP_LOGE(TAG, "Previous stream task stuck for 10s; proceeding anyway");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    s_ctx.embedded_source = true;
    s_ctx.embedded_data = data;
    s_ctx.embedded_len = len;
    s_ctx.current_index = -1;
    s_ctx.stream_offset = 0;
    s_ctx.detected_bitrate_kbps = bitrate;
    s_ctx.receiver_fill_bytes = 0;
    s_ctx.receiver_capacity_bytes = 0;
    s_ctx.receiver_played_ms = 0;
    s_ctx.receiver_status_tick = 0;
    s_ctx.state = AUDIO_STREAM_STATE_STOPPED;
    s_ctx.stream_id++;
    uint32_t stream_id = s_ctx.stream_id;
    xSemaphoreGive(s_ctx.mutex);

    ESP_LOGI(TAG, "Playing embedded audio (%u bytes)", (unsigned)len);
    audio_pause_competing_streams();
    audio_apply_one_shot_headphone_route();
    (void)esp_comm_manager_send_command("audio", "start");

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    s_ctx.state = AUDIO_STREAM_STATE_PLAYING;
    s_ctx.stream_task = create_audio_stream_task(stream_id);
    esp_err_t ret = s_ctx.stream_task ? ESP_OK : ESP_ERR_NO_MEM;
    if (!s_ctx.stream_task) {
        s_ctx.state = AUDIO_STREAM_STATE_IDLE;
        s_ctx.embedded_source = false;
    }
    xSemaphoreGive(s_ctx.mutex);
    return ret;
}

size_t audio_stream_manager_get_position(void)
{
    size_t offset = 0;
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    offset = s_ctx.stream_offset;
    xSemaphoreGive(s_ctx.mutex);
    return offset;
}

size_t audio_stream_manager_get_total_size(void)
{
    size_t total = 0;
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    if (s_ctx.embedded_source && s_ctx.embedded_data) {
        total = s_ctx.embedded_len;
    } else if (!s_ctx.embedded_source && s_ctx.current_index >= 0) {
        total = s_ctx.embedded_len;
    }
    xSemaphoreGive(s_ctx.mutex);
    return total;
}

uint16_t audio_stream_manager_get_bitrate(void)
{
    uint16_t br = 0;
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    br = s_ctx.detected_bitrate_kbps;
    xSemaphoreGive(s_ctx.mutex);
    return br;
}

void audio_stream_manager_update_receiver_status(size_t fill_bytes, size_t capacity_bytes, uint32_t played_ms)
{
    if (!s_ctx.initialized || !s_ctx.mutex || capacity_bytes == 0) return;

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    bool first_status = s_ctx.receiver_capacity_bytes == 0;
    s_ctx.receiver_fill_bytes = fill_bytes;
    s_ctx.receiver_capacity_bytes = capacity_bytes;
    s_ctx.receiver_played_ms = played_ms;
    s_ctx.receiver_status_tick = xTaskGetTickCount();
    xSemaphoreGive(s_ctx.mutex);

    if (first_status) {
        ESP_LOGI(TAG, "Audio receiver feedback active (%lu byte buffer)",
                 (unsigned long)capacity_bytes);
    }
}

uint32_t audio_stream_manager_get_playback_ms(void)
{
    uint32_t played_ms = 0;
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    played_ms = s_ctx.receiver_played_ms;
    xSemaphoreGive(s_ctx.mutex);
    return played_ms;
}

uint32_t audio_stream_manager_get_duration_ms(void)
{
    size_t total = audio_stream_manager_get_total_size();
    uint16_t bitrate = audio_stream_manager_get_bitrate();
    if (total == 0) return 0;
    if (bitrate == 0) bitrate = 128;

    uint32_t bytes_per_sec = (uint32_t)bitrate * 125;
    if (bytes_per_sec == 0) return 0;
    return (uint32_t)(((uint64_t)total * 1000) / bytes_per_sec);
}

uint8_t audio_stream_manager_get_receiver_buffer_percent(void)
{
    uint8_t percent = 0;
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    if (s_ctx.receiver_capacity_bytes > 0) {
        size_t pct = (s_ctx.receiver_fill_bytes * 100) / s_ctx.receiver_capacity_bytes;
        percent = (uint8_t)(pct > 100 ? 100 : pct);
    }
    xSemaphoreGive(s_ctx.mutex);
    return percent;
}

static bool audio_stream_get_receiver_status(size_t *fill_bytes,
                                             size_t *capacity_bytes,
                                             uint32_t *age_ms)
{
    bool fresh = false;
    TickType_t now = xTaskGetTickCount();

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    if (s_ctx.receiver_capacity_bytes > 0 && s_ctx.receiver_status_tick != 0) {
        if (fill_bytes) *fill_bytes = s_ctx.receiver_fill_bytes;
        if (capacity_bytes) *capacity_bytes = s_ctx.receiver_capacity_bytes;
        uint32_t status_age_ms = (uint32_t)((now - s_ctx.receiver_status_tick) * portTICK_PERIOD_MS);
        if (age_ms) *age_ms = status_age_ms;
        fresh = status_age_ms <= STREAM_FEEDBACK_STALE_MS;
    }
    xSemaphoreGive(s_ctx.mutex);

    return fresh;
}

static void audio_stream_apply_receiver_backpressure(void)
{
    uint32_t paused_ms = 0;

    while (paused_ms < STREAM_RX_MAX_PAUSE_MS) {
        size_t fill = 0;
        size_t capacity = 0;
        if (!audio_stream_get_receiver_status(&fill, &capacity, NULL) || capacity == 0) {
            return;
        }

        size_t target = (capacity * STREAM_RX_TARGET_PERCENT) / 100;
        size_t high = (capacity * STREAM_RX_HIGH_PERCENT) / 100;
        if (fill <= target) {
            return;
        }

        uint32_t delay_ms = STREAM_INTER_PACKET_DELAY_MS;
        if (fill >= high) {
            delay_ms = STREAM_RX_PAUSE_DELAY_MS;
        } else {
            size_t excess = fill - target;
            size_t range = high > target ? high - target : 1;
            delay_ms = 4 + (uint32_t)((excess * 16) / range);
        }

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        paused_ms += delay_ms;
    }
}

static bool audio_stream_receiver_needs_refill(bool *urgent)
{
    size_t fill = 0;
    size_t capacity = 0;
    if (urgent) *urgent = false;

    if (!audio_stream_get_receiver_status(&fill, &capacity, NULL) || capacity == 0) {
        return false;
    }

    size_t low = (capacity * STREAM_RX_LOW_PERCENT) / 100;
    size_t target = (capacity * STREAM_RX_TARGET_PERCENT) / 100;
    if (fill < low) {
        if (urgent) *urgent = true;
        return true;
    }

    return fill < target;
}

/* MPEG1 Layer 3 bitrate table (kbps) indexed by the 4-bit bitrate field */
static const uint16_t mp3_bitrates_mpeg1_l3[16] = {
    0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
};

/* MPEG2/2.5 Layer 3 bitrate table (kbps) */
static const uint16_t mp3_bitrates_mpeg2_l3[16] = {
    0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0
};

/**
 * @brief Parse the first MP3 frame header to determine bitrate (kbps).
 *
 * Scans the first ~4KB for a valid frame sync and returns the bitrate.
 * Returns 0 on failure (free-format, unknown, or not MP3).
 */
static uint16_t parse_mp3_bitrate(const uint8_t *data, size_t len)
{
    if (!data || len < 4) return 0;
    size_t scan_limit = len < 4096 ? len : 4096;
    for (size_t i = 0; i + 4 <= scan_limit; i++) {
        if (data[i] != 0xFF) continue;
        uint8_t b1 = data[i + 1];
        if ((b1 & 0xE0) != 0xE0) continue;
        uint8_t version = (b1 >> 3) & 0x03;
        uint8_t layer = (b1 >> 1) & 0x03;
        if (layer != 1) continue;
        uint8_t br_idx = (data[i + 2] >> 4) & 0x0F;
        if (br_idx == 0 || br_idx == 0x0F) continue;
        return (version == 3) ? mp3_bitrates_mpeg1_l3[br_idx] : mp3_bitrates_mpeg2_l3[br_idx];
    }
    return 0;
}

static void audio_stream_task(void *arg)
{
    s_stream_task_exited = false;
    uint32_t stream_id = (uint32_t)(uintptr_t)arg;
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    uint8_t *stream_buf = heap_caps_malloc(STREAM_READ_BURST_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!stream_buf) {
        stream_buf = malloc(STREAM_READ_BURST_SIZE);
    }
    if (!stream_buf) {
        ESP_LOGE(TAG, "Failed to allocate stream buffer");
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        s_ctx.state = AUDIO_STREAM_STATE_STOPPED;
        if (s_ctx.stream_id == stream_id && s_ctx.stream_task == self) {
            s_ctx.stream_task = NULL;
        }
        xSemaphoreGive(s_ctx.mutex);
        s_stream_task_exited = true;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Stream task started");
    bool waiting_logged = false;
    bool first_chunk_logged = false;
    bool pacing_started = false;
    TickType_t pacing_start_tick = 0;

    while (1) {
        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);

        if (s_ctx.state == AUDIO_STREAM_STATE_STOPPED || s_ctx.stream_id != stream_id) {
            xSemaphoreGive(s_ctx.mutex);
            break;
        }

        if (s_ctx.state == AUDIO_STREAM_STATE_PAUSED) {
            xSemaphoreGive(s_ctx.mutex);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (!esp_comm_manager_is_connected()) {
            if (!waiting_logged) {
                ESP_LOGW(TAG, "Waiting for GhostLink connection before streaming audio");
                waiting_logged = true;
                /* Flush receiver to prevent playing stale data after reconnect */
                (void)esp_comm_manager_send_command("audio", "flush");
            }
            xSemaphoreGive(s_ctx.mutex);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        waiting_logged = false;

        int index = s_ctx.current_index;
        size_t offset = s_ctx.stream_offset;
        bool is_embedded = s_ctx.embedded_source;
        const uint8_t *emb_data = s_ctx.embedded_data;
        size_t emb_len = s_ctx.embedded_len;
        char filename[MAX_FILENAME_LEN];

        if (!is_embedded) {
            if (index < 0 || index >= s_ctx.file_count) {
                s_ctx.state = AUDIO_STREAM_STATE_STOPPED;
                xSemaphoreGive(s_ctx.mutex);
                break;
            }
            strncpy(filename, s_ctx.filenames[index], sizeof(filename) - 1);
            filename[sizeof(filename) - 1] = '\0';
        }
        xSemaphoreGive(s_ctx.mutex);

        size_t bytes_read = 0;
        bool read_ok = false;

        if (is_embedded && emb_data) {
            if (offset < emb_len) {
                bytes_read = emb_len - offset;
                if (bytes_read > STREAM_READ_BURST_SIZE) bytes_read = STREAM_READ_BURST_SIZE;
                memcpy(stream_buf, emb_data + offset, bytes_read);
                read_ok = true;
            } else {
                bytes_read = 0;
                read_ok = true;
            }
        } else {
            char path[128];
            snprintf(path, sizeof(path), "%s/%s", CONFIG_AUDIO_FILES_PATH, filename);

            bool display_was_suspended = false;
            bool did_mount = false;
            if (audio_sd_begin(&display_was_suspended, &did_mount, false)) {
                FILE *f = fopen(path, "rb");
                if (f) {
                    if (fseek(f, (long)offset, SEEK_SET) == 0) {
                        bytes_read = fread(stream_buf, 1, STREAM_READ_BURST_SIZE, f);
                        read_ok = true;
                    }
                    fclose(f);
                } else {
                    ESP_LOGE(TAG, "Failed to open '%s'", path);
                }
                audio_sd_end(display_was_suspended, did_mount);
            }
        }

        if (!read_ok) {
            xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
            s_ctx.state = AUDIO_STREAM_STATE_STOPPED;
            if (s_ctx.stream_id == stream_id && s_ctx.stream_task == self) {
                s_ctx.stream_task = NULL;
            }
            xSemaphoreGive(s_ctx.mutex);
            break;
        }

        /* Detect bitrate from first chunk (for accurate progress + pacing) */
        if (offset == 0 && bytes_read > 0) {
            uint16_t br = parse_mp3_bitrate(stream_buf, bytes_read);
            if (br > 0) {
                xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
                if (s_ctx.detected_bitrate_kbps == 0) {
                    s_ctx.detected_bitrate_kbps = br;
                    ESP_LOGI(TAG, "Detected MP3 bitrate: %u kbps", (unsigned)br);
                }
                xSemaphoreGive(s_ctx.mutex);
            }
        }

        xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
        if (s_ctx.state == AUDIO_STREAM_STATE_STOPPED || s_ctx.stream_id != stream_id ||
            s_ctx.current_index != index || s_ctx.stream_offset != offset) {
            xSemaphoreGive(s_ctx.mutex);
            continue;
        }

        if (bytes_read == 0) {
            /* End of file - signal stop and exit cleanly */
            ESP_LOGI(TAG, "End of file reached");
            s_ctx.state = AUDIO_STREAM_STATE_STOPPED;
            s_ctx.stream_id++;
            bool is_self = (s_ctx.stream_task == self);
            if (is_self) {
                s_ctx.stream_task = NULL;
            }
            xSemaphoreGive(s_ctx.mutex);

            (void)esp_comm_manager_send_command("audio", "stop");
            audio_resume_competing_streams();

            free(stream_buf);
            stream_buf = NULL;

            s_stream_task_exited = true;
            vTaskDelete(NULL);
            return;
        }

        /* Send buffered chunks after unmounting so LVGL/display SPI gets time between SD bursts. */
        if (bytes_read > 0) {
            size_t sent_total = 0;
            bool paused_mid_chunk = false;
            xSemaphoreGive(s_ctx.mutex);

            while (sent_total < bytes_read) {
                xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
                if (s_ctx.state == AUDIO_STREAM_STATE_PAUSED && s_ctx.stream_id == stream_id &&
                    s_ctx.current_index == index && s_ctx.stream_offset == offset) {
                    s_ctx.stream_offset = offset + sent_total;
                    xSemaphoreGive(s_ctx.mutex);
                    paused_mid_chunk = true;
                    break;
                }
                bool should_stop = s_ctx.state == AUDIO_STREAM_STATE_STOPPED ||
                                   s_ctx.stream_id != stream_id ||
                                   s_ctx.current_index != index ||
                                   s_ctx.stream_offset != offset;
                xSemaphoreGive(s_ctx.mutex);
                if (should_stop) {
                    break;
                }

                size_t send_len = bytes_read - sent_total;
                if (send_len > STREAM_CHUNK_SIZE) {
                    send_len = STREAM_CHUNK_SIZE;
                }
                bool sent = esp_comm_manager_send_stream_wait(COMM_STREAM_CHANNEL_AUDIO,
                                                              stream_buf + sent_total,
                                                              send_len,
                                                              STREAM_SEND_WAIT_MS);
                if (sent && !first_chunk_logged) {
                    ESP_LOGI(TAG, "Audio stream started (%dKB bursts, %d byte packets)",
                             (int)(STREAM_READ_BURST_SIZE / 1024), (int)STREAM_CHUNK_SIZE);
                    first_chunk_logged = true;
                }
                if (!sent) {
                    ESP_LOGW(TAG, "GhostLink stream send failed");
                    /* Tell the receiver to flush its decoder to avoid playing garbage */
                    (void)esp_comm_manager_send_command("audio", "flush");
                    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
                    s_ctx.state = AUDIO_STREAM_STATE_STOPPED;
                    xSemaphoreGive(s_ctx.mutex);
                    break;
                }

                sent_total += send_len;

                audio_stream_apply_receiver_backpressure();

                bool urgent_refill = false;
                if (audio_stream_receiver_needs_refill(&urgent_refill)) {
                    vTaskDelay(pdMS_TO_TICKS(urgent_refill ? 1 : STREAM_INTER_PACKET_DELAY_MS));
                    continue;
                }

                /* Media-clock pacing: quickly fill the receiver prebuffer, then keep
                 * transmitted bytes aligned to elapsed playback time. This avoids
                 * burst-overflow while adapting to the detected MP3 bitrate. */
                size_t absolute_sent = offset + sent_total;
                if (absolute_sent <= STREAM_PREFILL_BYTES) {
                    vTaskDelay(pdMS_TO_TICKS(STREAM_INTER_PACKET_DELAY_MS));
                    continue;
                }

                uint32_t bps = STREAM_MAX_BYTES_PER_SEC;
                xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
                if (s_ctx.detected_bitrate_kbps > 0) {
                    uint32_t detected_bps = (uint32_t)s_ctx.detected_bitrate_kbps * 125;
                    if (detected_bps > bps) {
                        bps = detected_bps;
                    }
                }
                xSemaphoreGive(s_ctx.mutex);
                if (bps == 0) bps = STREAM_MAX_BYTES_PER_SEC;

                if (!pacing_started) {
                    pacing_started = true;
                    pacing_start_tick = xTaskGetTickCount();
                }

                size_t paced_bytes = absolute_sent - STREAM_PREFILL_BYTES;
                uint32_t target_ms = (uint32_t)(((uint64_t)paced_bytes * 1000) / bps);
                uint32_t elapsed_ms = (uint32_t)((xTaskGetTickCount() - pacing_start_tick) * portTICK_PERIOD_MS);
                if (target_ms > elapsed_ms) {
                    uint32_t delay_ms = target_ms - elapsed_ms;
                    if (delay_ms < STREAM_INTER_PACKET_DELAY_MS) {
                        delay_ms = STREAM_INTER_PACKET_DELAY_MS;
                    }
                    vTaskDelay(pdMS_TO_TICKS(delay_ms));
                } else {
                    vTaskDelay(pdMS_TO_TICKS(STREAM_INTER_PACKET_DELAY_MS));
                }
            }

            xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
            if (paused_mid_chunk) {
                xSemaphoreGive(s_ctx.mutex);
                continue;
            }
            if (sent_total != bytes_read || s_ctx.state == AUDIO_STREAM_STATE_STOPPED ||
                s_ctx.stream_id != stream_id || s_ctx.current_index != index ||
                s_ctx.stream_offset != offset) {
                xSemaphoreGive(s_ctx.mutex);
                break;
            }
            s_ctx.stream_offset = offset + bytes_read;
        }

        xSemaphoreGive(s_ctx.mutex);
    }

    ESP_LOGI(TAG, "Stream task exiting");
    free(stream_buf);
    audio_resume_competing_streams();
    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);
    if (s_ctx.stream_id == stream_id && s_ctx.stream_task == self) {
        s_ctx.stream_task = NULL;
    }
    xSemaphoreGive(s_ctx.mutex);
    s_stream_task_exited = true;
    vTaskDelete(NULL);
}

#else /* !CONFIG_HAS_AUDIO_PLAYER */

esp_err_t audio_stream_manager_init(void) { return ESP_ERR_NOT_SUPPORTED; }
void audio_stream_manager_deinit(void) {}
audio_stream_state_t audio_stream_manager_get_state(void) { return AUDIO_STREAM_STATE_IDLE; }
int audio_stream_manager_get_file_count(void) { return 0; }
const char *audio_stream_manager_get_filename(int index) { (void)index; return NULL; }
int audio_stream_manager_get_current_index(void) { return -1; }
esp_err_t audio_stream_manager_play(int index) { (void)index; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_stream_manager_pause(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_stream_manager_resume(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_stream_manager_stop(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_stream_manager_next(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_stream_manager_prev(void) { return ESP_ERR_NOT_SUPPORTED; }
bool audio_stream_manager_is_initialized(void) { return false; }
bool audio_stream_manager_sd_available(void) { return false; }
esp_err_t audio_stream_manager_play_embedded(const uint8_t *data, size_t len) { (void)data; (void)len; return ESP_ERR_NOT_SUPPORTED; }
size_t audio_stream_manager_get_position(void) { return 0; }
size_t audio_stream_manager_get_total_size(void) { return 0; }
uint16_t audio_stream_manager_get_bitrate(void) { return 0; }
void audio_stream_manager_update_receiver_status(size_t fill_bytes, size_t capacity_bytes, uint32_t played_ms) { (void)fill_bytes; (void)capacity_bytes; (void)played_ms; }
uint32_t audio_stream_manager_get_playback_ms(void) { return 0; }
uint32_t audio_stream_manager_get_duration_ms(void) { return 0; }
uint8_t audio_stream_manager_get_receiver_buffer_percent(void) { return 0; }

#endif /* CONFIG_HAS_AUDIO_PLAYER */
