#include "managers/views/music_visualizer.h"
#include "managers/views/main_menu_screen.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <lvgl.h>
#include <math.h>
#include "esp_log.h"
#include "driver/i2s.h"

#define NUM_PARTICLES 5
#define ANIMATION_INTERVAL_MS 5 // Approximately 30 FPS

#define I2S_NUM         (0)
#define I2S_SAMPLE_RATE (16000)
#define I2S_BCK_IO      (43) // CLK pin
#define I2S_DATA_IN_IO  (46) // DATA pin
#define MIC_SENSITIVITY 1  // Increase for more sensitive, decrease for less
#define MIC_NOISE_FLOOR 20000  // Try 30, 50, 100, etc.
#define MAX_MIC_AMPLITUDE 12000  // Tune this value for your environment

static const char *TAG = "MusicVisualizer";

void init_pdm_microphone(void);
static void update_amplitudes_from_mic(void);

lv_timer_t *animation_timer = NULL;

typedef struct {
  lv_obj_t *obj;
  int x;        // Current x position
  int y;        // Current y position
  int velocity; // Horizontal velocity
} Particle;

typedef struct {
  int bars[NUM_BARS]; // Amplitude data for each bar
} AmplitudeData;

Particle particles[NUM_PARTICLES];
MusicVisualizerView view;
lv_obj_t *root;
QueueHandle_t amplitudeQueue;

int target_amplitudes[NUM_BARS] = {0};
int current_amplitudes[NUM_BARS] = {0};

void handle_hardware_input_music_callback(InputEvent *event) {
  if (event->type == INPUT_TYPE_TOUCH) {
    ESP_LOGI(TAG, "Touch event");
    display_manager_switch_view(&main_menu_view);
  } else if (event->type == INPUT_TYPE_JOYSTICK) {
    ESP_LOGI(TAG, "Joystick event");

    int button = event->data.joystick_index;
    if (button == 1) {
      display_manager_switch_view(&main_menu_view);
    }
  } else if (event->type == INPUT_TYPE_KEYBOARD){ 
    ESP_LOGW(TAG, "keyboard event");
    uint8_t key = event->data.key_value;
    if (key == 27 || key == '`'){
    display_manager_switch_view(&main_menu_view);
    }
  }
}

void get_music_visualizer_callback(void **callback) {
  *callback = music_visualizer_view.input_callback;
}

View music_visualizer_view = {
    .root = NULL,
    .create = music_visualizer_view_create,
    .destroy = music_visualizer_destroy,
    .input_callback = handle_hardware_input_music_callback,
    .name = "Music Visualizer",
    .get_hardwareinput_callback = get_music_visualizer_callback};

void animation_timer_callback(lv_timer_t *timer);

void music_visualizer_view_create() {
  init_pdm_microphone();

  display_manager_fill_screen(lv_color_black());

  root = lv_obj_create(lv_scr_act());
  music_visualizer_view.root = root;
  lv_obj_set_style_bg_color(music_visualizer_view.root, lv_color_black(),
                            LV_PART_MAIN);
  lv_obj_set_size(music_visualizer_view.root, LV_HOR_RES, LV_VER_RES);
  lv_obj_set_scrollbar_mode(music_visualizer_view.root, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_column(music_visualizer_view.root, LV_HOR_RES / 24, 0);
  lv_obj_set_style_bg_opa(music_visualizer_view.root, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(music_visualizer_view.root, 0, 0);
  lv_obj_set_style_pad_all(music_visualizer_view.root, 0, 0);
  lv_obj_set_style_radius(music_visualizer_view.root, 0, 0);

  const lv_font_t *track_label_font;
  const lv_font_t *artist_label_font;

  if (LV_HOR_RES <= 128) {
    track_label_font = &lv_font_montserrat_12;
    artist_label_font = &lv_font_montserrat_10;
  } else if (LV_HOR_RES <= 240) {
    track_label_font = &lv_font_montserrat_16;
    artist_label_font = &lv_font_montserrat_12;
  } else {
    track_label_font = &lv_font_montserrat_24;
    artist_label_font = &lv_font_montserrat_16;
  }

  int label_x_offset = LV_HOR_RES / 12;
  int label_y_offset = LV_VER_RES / 8;
  int bar_width = LV_HOR_RES / (NUM_BARS * 2);
  int bar_spacing = LV_HOR_RES / (NUM_BARS + 2);
  int bar_y_offset = LV_VER_RES / 4;

  view.track_label = lv_label_create(music_visualizer_view.root);
  lv_label_set_text(view.track_label, "Ghost ESP");
  lv_obj_set_style_text_font(view.track_label, track_label_font, LV_PART_MAIN);
  lv_obj_set_style_text_color(view.track_label, lv_color_white(), LV_PART_MAIN);
  lv_obj_align(view.track_label, LV_ALIGN_BOTTOM_LEFT, label_x_offset,
               -label_y_offset);

  view.artist_label = lv_label_create(music_visualizer_view.root);
  lv_label_set_text(view.artist_label, "Spooky");
  lv_obj_set_style_text_font(view.artist_label, artist_label_font,
                             LV_PART_MAIN);
  lv_obj_set_style_text_color(view.artist_label, lv_color_white(),
                              LV_PART_MAIN);
  lv_obj_align_to(view.artist_label, view.track_label, LV_ALIGN_OUT_BOTTOM_LEFT,
                  0, lv_font_get_line_height(track_label_font) / 4);

  for (int i = 0; i < NUM_BARS; i++) {
    view.bars[i] = lv_obj_create(music_visualizer_view.root);
    lv_obj_set_size(view.bars[i], bar_width, 1);
    lv_obj_align(view.bars[i], LV_ALIGN_BOTTOM_LEFT,
                 label_x_offset + (bar_spacing * i), -bar_y_offset);

    lv_obj_set_style_radius(view.bars[i], 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(view.bars[i], LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(view.bars[i], lv_color_make(147, 112, 219),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(view.bars[i], lv_color_make(147, 112, 219),
                                   LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(view.bars[i], LV_GRAD_DIR_VER, LV_PART_MAIN);
  }

  for (int i = 0; i < NUM_PARTICLES; i++) {
    particles[i].obj = lv_obj_create(music_visualizer_view.root);
    lv_obj_set_size(particles[i].obj, 1, 1);
    lv_obj_set_style_radius(particles[i].obj, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(particles[i].obj, lv_color_white(), LV_PART_MAIN);
    particles[i].x = 0;
    particles[i].y = rand() % LV_VER_RES;
    particles[i].velocity = 1 + rand() % 3;
    lv_obj_align(particles[i].obj, LV_ALIGN_TOP_LEFT, particles[i].x,
                 particles[i].y);
  }

  display_manager_add_status_bar(LV_VER_RES > 320 ? "Rave Mode" : "Rave");

  amplitudeQueue = xQueueCreate(10, sizeof(AmplitudeData));
  animation_timer =
      lv_timer_create(animation_timer_callback, ANIMATION_INTERVAL_MS, NULL);
}

void animation_timer_callback(lv_timer_t *timer) {
    update_amplitudes_from_mic();

  AmplitudeData amplitudeData;
  bool dataAvailable =
      xQueueReceive(amplitudeQueue, &amplitudeData, 0) == pdTRUE;

  if (dataAvailable) {
    // Only log the first bar height for summary
    for (int i = 0; i < NUM_BARS; i++) {
      lv_obj_set_height(view.bars[i], amplitudeData.bars[i]);
    }
  }

  for (int i = 0; i < NUM_PARTICLES; i++) {
    particles[i].x += particles[i].velocity;
    if (particles[i].x > LV_HOR_RES) {
      particles[i].x = 0;
      particles[i].y = rand() % LV_VER_RES;
      particles[i].velocity = 1 + rand() % 3;
    }
    lv_obj_set_pos(particles[i].obj, particles[i].x, particles[i].y);
  }
}

void music_visualizer_view_update(const uint8_t *amplitudes,
                                  const char *track_name,
                                  const char *artist_name) {

  if (music_visualizer_view.root) {
    if (strcmp(lv_label_get_text(view.track_label), track_name) != 0) {
      lv_label_set_text(view.track_label, track_name);
    }
    if (strcmp(lv_label_get_text(view.artist_label), artist_name) != 0) {
      lv_label_set_text(view.artist_label, artist_name);
    }

    AmplitudeData amplitudeData;
    for (int i = 0; i < NUM_BARS; i++) {
      amplitudeData.bars[i] = amplitudes[i];
    }
    xQueueSend(amplitudeQueue, &amplitudeData, portMAX_DELAY);
  }
}

void music_visualizer_destroy(void) {

  if (animation_timer) {
    lv_timer_del(animation_timer);
    animation_timer = NULL;
  }

  if (root) {
    lv_obj_del(root);
    root = NULL;
    music_visualizer_view.root = NULL;
  }
  if (amplitudeQueue) {
    vQueueDelete(amplitudeQueue);
    amplitudeQueue = NULL;
  }
}

void init_pdm_microphone(void) {
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = I2S_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_I2S, // <-- Change this line
        .intr_alloc_flags = 0,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCK_IO,
        .ws_io_num = -1, // Not used for PDM
        .data_out_num = -1,
        .data_in_num = I2S_DATA_IN_IO
    };

    i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM, &pin_config);
}

static void update_amplitudes_from_mic(void) {
    int16_t mic_buffer[128];
    size_t bytes_read = 0;
    esp_err_t err = i2s_read(I2S_NUM, mic_buffer, sizeof(mic_buffer), &bytes_read, 0);
    ESP_LOGI(TAG, "i2s_read: err=%d, bytes_read=%d", err, (int)bytes_read);

    ESP_LOGI(TAG, "Sample[0]=%d Sample[1]=%d Sample[2]=%d", mic_buffer[0], mic_buffer[1], mic_buffer[2]);

    int16_t min = mic_buffer[0], max = mic_buffer[0];
for (int i = 1; i < 128; i++) {
    if (mic_buffer[i] < min) min = mic_buffer[i];
    if (mic_buffer[i] > max) max = mic_buffer[i];
}
ESP_LOGI(TAG, "Buffer min: %d, max: %d", min, max);

    uint8_t amplitudes[NUM_BARS] = {0};
    int samples_per_bar = sizeof(mic_buffer)/sizeof(mic_buffer[0]) / NUM_BARS;

    for (int i = 0; i < NUM_BARS; i++) {
        int32_t sum = 0;
        for (int j = 0; j < samples_per_bar; j++) {
            int idx = i * samples_per_bar + j;
            sum += abs(mic_buffer[idx]);
        }
        float avg = (float)sum / samples_per_bar;
        // Use avg - noise floor
        float amplitude = avg - MIC_NOISE_FLOOR;
        if (amplitude < 0) amplitude = 0;
        if (amplitude > MAX_MIC_AMPLITUDE) amplitude = MAX_MIC_AMPLITUDE;
        float scaled = (amplitude / MAX_MIC_AMPLITUDE) * LV_VER_RES;
        amplitudes[i] = (uint16_t)scaled;

        if (i == 0) ESP_LOGI(TAG, "Bar 0 amplitude: %d (raw: %.2f, scaled: %.2f)", amplitudes[i], amplitude, scaled);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    music_visualizer_view_update(amplitudes, "Ghost ESP", "Spooky");
}