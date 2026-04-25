#include "managers/views/offline_map_screen.h"
#include "managers/views/offline_map_jpeg_stb.h"
#include "gui/screen_layout.h"
#include "gui/theme_palette_api.h"
#include "gui/lvgl_safe.h"
#include "managers/display_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/settings_manager.h"
#include "managers/views/app_gallery_screen.h"
#include "sdkconfig.h"
#include "cJSON.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_err.h"
#include <stdbool.h>
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <math.h>
#include <stdint.h>
#include <errno.h>

/* Same LVGL file API as the rest of GhostESP (fopen-based stdio; see CONFIG_LV_USE_FS_STDIO). */
#if LV_USE_PNG && LV_USE_FS_STDIO
#ifndef CONFIG_LV_FS_STDIO_LETTER
#define MAP_FS_CH 'S'
#else
#define MAP_FS_CH ((char)CONFIG_LV_FS_STDIO_LETTER)
#endif
#endif

#define MAP_SOURCE    "satellite"
#define MAP_TILES_REL "/maps/" MAP_SOURCE "/tiles"

static const char *TAG = "OfflineMap";

/* Slippy map: tiles/{z}/{x}/{y}.png (same as tdeck-meshtastic_tiles output). */
#define MAP_GRID 3
#define META_MAX 4096
#define PATH_PLAIN_MAX 200
/* LVGL path: "S:" + s_path_plain (NUL-terminated, max PATH_PLAIN_MAX-1 chars) */
#define PATH_LVGL_MAX (PATH_PLAIN_MAX + 2)
#define LBL_MAX 88
/* Max raw PNG size per tile (typical 256×256 satellite ~50–200 KB). */
#define MAX_TILE_PNG_BYTES (512 * 1024)
/* Bounds for initial "focus on tiles" directory scan (avoid long walks on large caches). */
#define FOCUS_MAX_X_DIRS  384
#define FOCUS_MAX_Y_FILES 512
#define FOCUS_MAX_SAMPLES   20000

/* Set to 0 in a compile flag or here to silence per-tile ESP_LOGI lines (errors still log). */
#ifndef GHOST_OFFLINE_MAP_VERBOSE
#define GHOST_OFFLINE_MAP_VERBOSE 1
#endif

static lv_obj_t *s_root;
static lv_obj_t *s_map_cont;
static lv_obj_t *s_hud;
static lv_obj_t *s_cells[MAP_GRID * MAP_GRID];
static lv_obj_t *s_imgs[MAP_GRID * MAP_GRID];
/** Shown when a map tile file is missing for that cell. */
static lv_obj_t *s_no_tile_lbl[MAP_GRID * MAP_GRID];
static lv_obj_t *s_lbl;
static int s_zoom;
static int s_map_cell_w;
static int s_map_cell_h;
/** Per-slot pixel size (grid tracks differ when w%3 / h%3 ≠ 0). */
static int s_map_cell_w_slot[MAP_GRID * MAP_GRID];
static int s_map_cell_h_slot[MAP_GRID * MAP_GRID];
static int s_base_tx;
static int s_base_ty;
static int s_min_z;
static int s_max_z;
/* True if this screen mounted SD; only then unmount on leave. */
static bool s_mounted_by_us;
static bool s_jit_suspended;
/* Shared display+SD SPI: read PNG into RAM, unmount, then lv_img from lv_img_dsc_t (LV_IMG_SRC_VARIABLE). */
static bool s_stream_mode;
static lv_img_dsc_t *s_tile_ram[MAP_GRID * MAP_GRID];
/** Slippy identity for each RAM dsc (-1 z = no tile / freed). Used to skip SD read when panning. */
static int s_tile_ram_z[MAP_GRID * MAP_GRID];
static int s_tile_ram_tx[MAP_GRID * MAP_GRID];
static int s_tile_ram_ty[MAP_GRID * MAP_GRID];
static char s_path_plain[9][PATH_PLAIN_MAX];
static char s_path_lvgl[9][PATH_LVGL_MAX];

void offline_map_view_create(void);
void offline_map_view_destroy(void);

static uint32_t theme_bg(void) {
  return theme_palette_get_background(settings_get_menu_theme(&G_Settings));
}

static uint32_t theme_muted(void) {
  return theme_palette_get_text_muted(settings_get_menu_theme(&G_Settings));
}

static int clamp_i(int v, int lo, int hi) {
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

static bool file_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void latlon_to_tile(double lat, double lon, int z, int *out_x, int *out_y) {
  double n = (double)(1u << (unsigned)z);
  double xf = (lon + 180.0) / 360.0 * n;
  double latr = lat * (double)M_PI / 180.0;
  double yf = (1.0 - log(tan(latr) + 1.0 / cos(latr)) / (double)M_PI) / 2.0 * n;
  *out_x = (int)floor(xf);
  *out_y = (int)floor(yf);
}

static void load_metadata_defaults(void) {
  s_min_z = 10;
  s_max_z = 12;
  s_zoom = 11;
  s_base_tx = 0;
  s_base_ty = 0;
  /* Sample bbox center (Houston) — overridden when metadata.json is present. */
  latlon_to_tile(29.75, -95.35, s_zoom, &s_base_tx, &s_base_ty);
  s_base_tx -= 1;
  s_base_ty -= 1;
}

static void load_map_metadata(void) {
  char path[sizeof(GHOSTESP_SD_ROOT) + 128];
  snprintf(path, sizeof(path), "%s" MAP_TILES_REL "/metadata.json", GHOSTESP_SD_ROOT);
  load_metadata_defaults();
  if (!file_exists(path)) {
    return;
  }
  FILE *f = fopen(path, "rb");
  if (!f) {
    return;
  }
  char *buf = malloc(META_MAX);
  if (!buf) {
    fclose(f);
    return;
  }
  size_t n = fread(buf, 1, META_MAX - 1, f);
  buf[n] = '\0';
  fclose(f);
  cJSON *root = cJSON_Parse(buf);
  free(buf);
  if (!root) {
    return;
  }
  cJSON *jz = cJSON_GetObjectItem(root, "minzoom");
  cJSON *jz2 = cJSON_GetObjectItem(root, "maxzoom");
  if (cJSON_IsNumber(jz) && cJSON_IsNumber(jz2)) {
    s_min_z = (int)jz->valuedouble;
    s_max_z = (int)jz2->valuedouble;
  }
  cJSON *b = cJSON_GetObjectItem(root, "bounds");
  if (cJSON_IsArray(b) && cJSON_GetArraySize(b) >= 4) {
    const cJSON *jw = cJSON_GetArrayItem(b, 0);
    const cJSON *js = cJSON_GetArrayItem(b, 1);
    const cJSON *je = cJSON_GetArrayItem(b, 2);
    const cJSON *jn = cJSON_GetArrayItem(b, 3);
    if (cJSON_IsNumber(jw) && cJSON_IsNumber(js) && cJSON_IsNumber(je) && cJSON_IsNumber(jn)) {
      double w = jw->valuedouble;
      double s = js->valuedouble;
      double e = je->valuedouble;
      double n0 = jn->valuedouble;
      double mlat = (s + n0) / 2.0;
      double mlon = (w + e) / 2.0;
      int tzx = 0, tzy = 0;
      s_zoom = clamp_i((s_min_z + s_max_z) / 2, s_min_z, s_max_z);
      latlon_to_tile(mlat, mlon, s_zoom, &tzx, &tzy);
      s_base_tx = tzx - 1;
      s_base_ty = tzy - 1;
    }
  }
  cJSON_Delete(root);
}

static bool dir_name_is_uint(const char *name, unsigned long *out) {
  if (!name || !out) {
    return false;
  }
  if (name[0] == '\0' || (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))) {
    return false;
  }
  char *end = NULL;
  unsigned long v = strtoul(name, &end, 10);
  if (end == name || *end != '\0' || v > (unsigned long)INT_MAX) {
    return false;
  }
  *out = v;
  return true;
}

static bool file_name_is_y_png(const char *name, unsigned long *out_y) {
  if (!name || !out_y) {
    return false;
  }
  const char *dot = strrchr(name, '.');
  if (!dot || dot == name) {
    return false;
  }
  if (strcasecmp(dot, ".png") != 0) {
    return false;
  }
  char *end = NULL;
  unsigned long y = strtoul(name, &end, 10);
  if (end == name || end != dot) {
    return false;
  }
  if (y > (unsigned long)INT_MAX) {
    return false;
  }
  *out_y = y;
  return true;
}

/* If any tile PNGs exist at zoom z, set bounding box in tile space (inclusive). */
static bool scan_tiles_bbox_at_z(int z, int *min_x, int *min_y, int *max_x, int *max_y, int *out_count) {
  if (!min_x || !min_y || !max_x || !max_y || !out_count) {
    return false;
  }
  *out_count = 0;
  char zpath[PATH_PLAIN_MAX];
  if (snprintf(zpath, sizeof(zpath), "%s" MAP_TILES_REL "/%d", GHOSTESP_SD_ROOT, z) >= (int)sizeof(zpath)) {
    return false;
  }
  struct stat st;
  if (stat(zpath, &st) != 0 || !S_ISDIR(st.st_mode)) {
    return false;
  }
  DIR *dz = opendir(zpath);
  if (!dz) {
    return false;
  }
  *min_x = INT_MAX;
  *min_y = INT_MAX;
  *max_x = INT_MIN;
  *max_y = INT_MIN;
  int x_cols = 0;
  int samples = 0;
  int dents = 0;
  struct dirent *ex;
  while ((ex = readdir(dz)) != NULL && dents < 20000 && samples < FOCUS_MAX_SAMPLES) {
    dents++;
    unsigned long xu;
    if (!dir_name_is_uint(ex->d_name, &xu)) {
      continue;
    }
    char xpath[PATH_PLAIN_MAX];
    if (snprintf(xpath, sizeof(xpath), "%s/%lu", zpath, xu) >= (int)sizeof(xpath)) {
      continue;
    }
    if (stat(xpath, &st) != 0 || !S_ISDIR(st.st_mode)) {
      continue;
    }
    if (x_cols >= FOCUS_MAX_X_DIRS) {
      break;
    }
    x_cols++;
    DIR *dx = opendir(xpath);
    if (!dx) {
      continue;
    }
    int y_n = 0;
    struct dirent *ey;
    while ((ey = readdir(dx)) != NULL && y_n < FOCUS_MAX_Y_FILES && samples < FOCUS_MAX_SAMPLES) {
      unsigned long yu;
      if (!file_name_is_y_png(ey->d_name, &yu)) {
        continue;
      }
      char fpath[PATH_PLAIN_MAX + 8];
      if (snprintf(fpath, sizeof(fpath), "%s/%s", xpath, ey->d_name) >= (int)sizeof(fpath)) {
        continue;
      }
      if (stat(fpath, &st) != 0 || !S_ISREG(st.st_mode)) {
        continue;
      }
      int xi = (int)xu;
      int yi = (int)yu;
      if (xi < *min_x) {
        *min_x = xi;
      }
      if (xi > *max_x) {
        *max_x = xi;
      }
      if (yi < *min_y) {
        *min_y = yi;
      }
      if (yi > *max_y) {
        *max_y = yi;
      }
      (*out_count)++;
      samples++;
    }
    closedir(dx);
  }
  closedir(dz);
  if (*out_count < 1) {
    return false;
  }
  if (*min_x < INT_MAX && *min_y < INT_MAX && *max_x >= *min_x && *max_y >= *min_y) {
    return true;
  }
  return false;
}

/* Prefer current s_zoom; otherwise pick the first level (min..max) that has on-disk tiles. */
static void focus_initial_view_on_existing_tiles(void) {
  if (!sd_card_exists(GHOSTESP_SD_ROOT)) {
    return;
  }
  if (s_min_z > s_max_z) {
    return;
  }
  int z0 = clamp_i(s_zoom, s_min_z, s_max_z);
  /* First try metadata/default zoom, then any level that has tiles. */
  for (int pass = 0; pass < 2; pass++) {
    for (int z = s_min_z; z <= s_max_z; z++) {
      if (pass == 0) {
        if (z != z0) {
          continue;
        }
      } else {
        if (z == z0) {
          continue;
        }
      }
      int min_x, min_y, max_x, max_y, cnt;
      if (!scan_tiles_bbox_at_z(z, &min_x, &min_y, &max_x, &max_y, &cnt) || cnt < 1) {
        continue;
      }
      s_zoom = z;
      int cx = (min_x + max_x) / 2;
      int cy = (min_y + max_y) / 2;
      s_base_tx = cx - 1;
      s_base_ty = cy - 1;
      ESP_LOGI(TAG, "map focus: z=%d center tile (%d,%d) bbox x[%d..%d] y[%d..%d] (%d png in sample)", z, cx, cy, min_x, max_x, min_y, max_y, cnt);
      return;
    }
  }
}

static void set_tile_path(int slot, int z, int tx, int ty) {
  if (slot < 0 || slot >= MAP_GRID * MAP_GRID) {
    return;
  }
  int pl = snprintf(s_path_plain[slot], PATH_PLAIN_MAX, "%s" MAP_TILES_REL "/%d/%d/%d.png", GHOSTESP_SD_ROOT, z, tx, ty);
  if (pl < 0 || pl >= PATH_PLAIN_MAX) {
    s_path_plain[slot][0] = '\0';
    s_path_lvgl[slot][0] = '\0';
    return;
  }
#if LV_USE_PNG && LV_USE_FS_STDIO
  (void)snprintf(s_path_lvgl[slot], PATH_LVGL_MAX, "%c:%.*s", MAP_FS_CH, (int)(PATH_PLAIN_MAX - 1), s_path_plain[slot]);
#else
  s_path_lvgl[slot][0] = '\0';
#endif
}

/* Walk chunks from byte 8 so CgBI / non-standard chunks before IHDR still work (iOS/Apple exports). */
static bool map_png_read_ihdr(const uint8_t *p, size_t len, int *ow, int *oh) {
  if (!p || !ow || !oh || len < 33) {
    return false;
  }
  if (p[0] != 0x89U || p[1] != 0x50U || p[2] != 0x4eU || p[3] != 0x47U) {
    return false;
  }
  size_t o = 8U;
  for (unsigned n = 0; n < 64U; n++) {
    if (o + 12U > len) {
      return false;
    }
    uint32_t ch_len = ((uint32_t)p[o] << 24) | ((uint32_t)p[o + 1] << 16) | ((uint32_t)p[o + 2] << 8) | (uint32_t)p[o + 3];
    if (ch_len > 1U + (uint32_t)MAX_TILE_PNG_BYTES) {
      return false;
    }
    if (o + 12U + (size_t)ch_len > len) {
      return false;
    }
    if (p[o + 4] == 'I' && p[o + 5] == 'H' && p[o + 6] == 'D' && p[o + 7] == 'R') {
      if (ch_len < 8U) {
        return false;
      }
      const uint8_t *d = p + o + 8U;
      uint32_t w = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | (uint32_t)d[3];
      uint32_t h = ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) | ((uint32_t)d[6] << 8) | (uint32_t)d[7];
      if (w < 1U || w > 4096U || h < 1U || h > 4096U) {
        return false;
      }
      *ow = (int)w;
      *oh = (int)h;
      return true;
    }
    o += 8U + (size_t)ch_len + 4U;
  }
  return false;
}

/* "S:/mnt/…" from LVGL file src → "/mnt/…" for POSIX fopen (same as lv_fs stdio real path). */
static void map_lv_file_to_plain(const char *lv, char *out, size_t cap) {
  if (!out || cap < 2) {
    return;
  }
  out[0] = '\0';
  if (!lv) {
    return;
  }
  if (lv[0] && lv[1] == ':') {
    (void)snprintf(out, cap, "%s", &lv[2]);
  } else {
    (void)snprintf(out, cap, "%s", lv);
  }
}

static bool s_map_dims_fallback_warned;
static uint32_t s_map_load_seq;
static bool s_map_caps_logged;

static int map_img_slot_of(const lv_obj_t *img) {
  if (!img) {
    return -1;
  }
  for (int i = 0; i < MAP_GRID * MAP_GRID; i++) {
    if (s_imgs[i] == img) {
      return i;
    }
  }
  return -1;
}

static void offline_map_log_build_caps_once(void) {
  if (s_map_caps_logged) {
    return;
  }
  s_map_caps_logged = true;
  ESP_LOGI(TAG,
           "map: caps LV_USE_PNG=%d LV_USE_SJPG=%d LV_USE_FS_STDIO=%d stream_mode=%d root=%s tiles=" GHOSTESP_SD_ROOT
           MAP_TILES_REL "/{z}/{x}/{y}.png (JPEG data in .png needs SJPG)",
           (int)LV_USE_PNG,
#if LV_USE_SJPG
           1,
#else
           0,
#endif
           (int)LV_USE_FS_STDIO, s_stream_mode ? 1 : 0, GHOSTESP_SD_ROOT);
}

/* Native pixel size for zoom. Returns true if dimensions are from decoder / IHDR; false if 256×256 guess. */
static bool map_src_native_dims(const void *src, int *ow, int *oh) {
  if (!src || !ow || !oh) {
    *ow = 256;
    *oh = 256;
    return false;
  }
  /* RAM dsc: use header and raw PE scan first (LVGL PNG get_info for VARIABLE uses fixed IHDR offset). */
  {
    lv_img_src_t st = lv_img_src_get_type(src);
    if (st == LV_IMG_SRC_VARIABLE) {
      const lv_img_dsc_t *d = (const lv_img_dsc_t *)src;
      if (d->header.w >= 1U && d->header.h >= 1U) {
        *ow = (int)d->header.w;
        *oh = (int)d->header.h;
        return true;
      }
      if (d->data && d->data_size >= 24U && map_png_read_ihdr(d->data, d->data_size, ow, oh)) {
        return true;
      }
    } else if (st == LV_IMG_SRC_FILE) {
      const char *fn = (const char *)src;
      char plain[PATH_PLAIN_MAX];
      map_lv_file_to_plain(fn, plain, sizeof(plain));
      if (plain[0] != '\0') {
        /* Enough prefix for signature + CgBI + first IHDR (fixed 32B often misses IHDR). */
        uint8_t b[1536];
        FILE *fp = fopen(plain, "rb");
        if (fp) {
          size_t n = fread(b, 1, sizeof(b), fp);
          fclose(fp);
          if (map_png_read_ihdr(b, n, ow, oh)) {
            return true;
          }
        }
      }
    }
  }
  {
    lv_img_header_t hdr;
    if (lv_img_decoder_get_info(src, &hdr) == LV_RES_OK && hdr.w >= 1 && hdr.h >= 1) {
      *ow = (int)hdr.w;
      *oh = (int)hdr.h;
      return true;
    }
  }
  *ow = 256;
  *oh = 256;
  return false;
}

static void map_hud_update(void);

/* Keep the same map area centered when zoom changes:
 * center tile coordinate (in old z) is mapped to new z by 2^(dz),
 * then base top-left is recomputed from that center. */
static void map_set_zoom_keep_center(int requested_zoom) {
  int old_z = clamp_i(s_zoom, s_min_z, s_max_z);
  int new_z = clamp_i(requested_zoom, s_min_z, s_max_z);
  if (new_z == old_z) {
    s_zoom = new_z;
    return;
  }
  const double half_grid = (double)MAP_GRID * 0.5; /* 3x3 => 1.5 */
  const double old_center_tx = (double)s_base_tx + half_grid;
  const double old_center_ty = (double)s_base_ty + half_grid;
  const int dz = new_z - old_z;
  const double scale = ldexp(1.0, dz); /* exact 2^dz */
  const double new_center_tx = old_center_tx * scale;
  const double new_center_ty = old_center_ty * scale;
  s_base_tx = (int)floor(new_center_tx - half_grid);
  s_base_ty = (int)floor(new_center_ty - half_grid);
  s_zoom = new_z;
}

/* After lv_img_set_src: scale the tile to cover the cell (no letterboxing). Use max(z_w,z_h) so
 * the image fills the track; overflow is clipped by the cell (default clip, not OVERFLOW_VISIBLE).
 * Use LV_IMG_SIZE_MODE_REAL (see create) so object size matches zoomed draw area. */
static void map_img_set_zoom_for_cell(lv_obj_t *img) {
  if (!img || !lv_obj_is_valid(img) || s_map_cell_w < 1 || s_map_cell_h < 1) {
#if GHOST_OFFLINE_MAP_VERBOSE
    ESP_LOGW(TAG, "map: map_img_set_zoom_for_cell early out (img=%p valid=%d cell %dx%d)", (void *)img,
             (!img || !lv_obj_is_valid(img)) ? 0 : 1, s_map_cell_w, s_map_cell_h);
#endif
    return;
  }
  const void *src = lv_img_get_src(img);
  if (!src) {
#if GHOST_OFFLINE_MAP_VERBOSE
    ESP_LOGW(TAG, "map: slot %d img has NULL src", map_img_slot_of(img));
#endif
    return;
  }
  int slot = map_img_slot_of(img);
  int iw = 0;
  int ih = 0;
  bool have_native = map_src_native_dims(src, &iw, &ih);
  if (!have_native) {
    if (!s_map_dims_fallback_warned) {
      s_map_dims_fallback_warned = true;
      ESP_LOGW(TAG, "map: tile size unknown; zoom uses 256. For JPEG tiles enable CONFIG_LV_USE_SJPG=y. Path: " GHOSTESP_SD_ROOT
                       MAP_TILES_REL);
    }
  }
  if (iw < 1) {
    iw = 256;
  }
  if (ih < 1) {
    ih = 256;
  }
  int cw = s_map_cell_w;
  int ch = s_map_cell_h;
  if (slot >= 0 && slot < MAP_GRID * MAP_GRID && s_map_cell_w_slot[slot] >= 1 && s_map_cell_h_slot[slot] >= 1) {
    cw = s_map_cell_w_slot[slot];
    ch = s_map_cell_h_slot[slot];
  }
  uint32_t z_w = (uint32_t)cw * 256U / (uint32_t)iw;
  uint32_t z_h = (uint32_t)ch * 256U / (uint32_t)ih;
  uint32_t z = (z_w > z_h) ? z_w : z_h;
  if (z < 1U) {
    z = 1U;
  }
  if (z > 1024U) {
    z = 1024U;
  }
#if GHOST_OFFLINE_MAP_VERBOSE
  {
    lv_img_header_t dgi;
    memset(&dgi, 0, sizeof(dgi));
    lv_res_t gires = lv_img_decoder_get_info(src, &dgi);
    int st = (int)lv_img_src_get_type(src);
    if (st == (int)LV_IMG_SRC_VARIABLE) {
      const lv_img_dsc_t *vd = (const lv_img_dsc_t *)src;
      ESP_LOGI(
          TAG,
          "map: slot %d zoom: src=VAR dsc=%p dsz=%u hdr w=%d h=%d cf=%u native=%d->%dx%d z=%u cell=%dx%d dec_info %s w=%d h=%d cf=%d obj=%dx%d",
          slot, (void *)vd, (unsigned)vd->data_size, (int)vd->header.w, (int)vd->header.h, (unsigned)vd->header.cf,
          have_native ? 1 : 0, iw, ih, (unsigned)z, cw, ch,
          (gires == LV_RES_OK) ? "OK" : "INV", (int)dgi.w, (int)dgi.h, (int)dgi.cf, (int)lv_obj_get_width(img),
          (int)lv_obj_get_height(img));
    } else {
      ESP_LOGI(TAG,
               "map: slot %d zoom: src type=%d native=%d->%dx%d z=%u cell=%dx%d dec_info %s w=%d h=%d obj=%dx%d", slot, st,
               have_native ? 1 : 0, iw, ih, (unsigned)z, cw, ch,
               (gires == LV_RES_OK) ? "OK" : "INV", (int)dgi.w, (int)dgi.h, (int)lv_obj_get_width(img),
               (int)lv_obj_get_height(img));
    }
  }
#endif
  lv_img_set_zoom(img, (uint16_t)z);
  lv_obj_invalidate(img);
  lv_obj_update_layout(img);
  {
    lv_obj_t *p = lv_obj_get_parent(img);
    if (p) {
      lv_obj_update_layout(p);
    }
  }
  lv_obj_center(img);
  if (lv_obj_get_parent(img)) {
    lv_obj_invalidate(lv_obj_get_parent(img));
  }
}

static uint8_t *read_png_file_alloc(const char *path, size_t *out_len) {
  struct stat st;
  if (stat(path, &st) != 0) {
    ESP_LOGW(TAG, "map: read '%s' stat failed errno=%d", path, errno);
    return NULL;
  }
  if (!S_ISREG(st.st_mode)) {
    ESP_LOGW(TAG, "map: read '%s' not a regular file (mode=0%o)", path, (unsigned)st.st_mode);
    return NULL;
  }
  if (st.st_size <= 0 || st.st_size > (off_t)MAX_TILE_PNG_BYTES) {
    ESP_LOGW(TAG, "map: read '%s' size %ld invalid (max %d)", path, (long)st.st_size, MAX_TILE_PNG_BYTES);
    return NULL;
  }
  size_t n = (size_t)st.st_size;
  FILE *f = fopen(path, "rb");
  if (!f) {
    ESP_LOGW(TAG, "map: read '%s' fopen failed errno=%d", path, errno);
    return NULL;
  }
  uint8_t *buf = (uint8_t *)malloc(n);
  if (!buf) {
    ESP_LOGW(TAG, "map: read '%s' malloc(%zu) failed", path, n);
    fclose(f);
    return NULL;
  }
  size_t rd = fread(buf, 1, n, f);
  fclose(f);
  if (rd != n) {
    ESP_LOGW(TAG, "map: read '%s' short read %zu / %zu", path, rd, n);
    free(buf);
    return NULL;
  }
  *out_len = n;
#if GHOST_OFFLINE_MAP_VERBOSE
  ESP_LOGI(TAG, "map: read ok '%s' %zu bytes", path, n);
#endif
  return buf;
}

static bool map_bytes_look_like_png(const uint8_t *p, size_t len) {
  return p && len >= 8 && p[0] == 0x89U && p[1] == 0x50U && p[2] == 0x4eU && p[3] == 0x47U;
}

/* SOI + marker: many slippy caches store JPEG with a .png extension. */
static bool map_bytes_look_like_jpeg(const uint8_t *p, size_t len) {
  return p && len >= 3 && p[0] == 0xffU && p[1] == 0xd8U && p[2] == 0xffU;
}

/* Build lv_img_dsc for raw file bytes. PNG: compressed + IHDR hints. JPEG: decode to RGB565 (stb) or SJPG baseline. */
static lv_img_dsc_t *alloc_tile_dsc(uint8_t *data, size_t len, const char *path_for_log) {
  const char *p = path_for_log ? path_for_log : "(ram)";

  if (map_bytes_look_like_png(data, len)) {
    lv_img_dsc_t *d = (lv_img_dsc_t *)calloc(1, sizeof(lv_img_dsc_t));
    if (!d) {
      ESP_LOGW(TAG, "map: alloc lv_img_dsc_t failed (path=%s)", p);
      free(data);
      return NULL;
    }
    d->data_size = (uint32_t)len;
    d->data = data;
    /* IHDR w/h; keep cf=UNKNOWN — do not set TRUE_COLOR on compressed PNG (built-in would mis-decode). */
    int ihdr_w = 0;
    int ihdr_h = 0;
    if (map_png_read_ihdr(data, len, &ihdr_w, &ihdr_h)) {
      if (ihdr_w > 2047) {
        ihdr_w = 2047;
      }
      if (ihdr_h > 2047) {
        ihdr_h = 2047;
      }
      d->header.w = (uint32_t)ihdr_w;
      d->header.h = (uint32_t)ihdr_h;
#if GHOST_OFFLINE_MAP_VERBOSE
      ESP_LOGI(TAG, "map: PNG IHDR %s %dx%d dsz=%u", p, ihdr_w, ihdr_h, (unsigned)len);
#endif
    } else {
      if (len >= 8) {
        ESP_LOGW(TAG, "map: PNG corrupt %s (len=%u head %02x%02x%02x%02x %02x%02x%02x%02x)", p, (unsigned)len, data[0],
                 data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
      } else {
        ESP_LOGW(TAG, "map: PNG too small %s len=%u", p, (unsigned)len);
      }
    }
    return d;
  }

  if (map_bytes_look_like_jpeg(data, len)) {
#if LV_COLOR_DEPTH == 16
    lv_img_dsc_t *decoded = offline_map_try_decode_jpeg_to_rgb565_dsc(data, len, p);
    if (decoded) {
      return decoded;
    }
    ESP_LOGW(TAG, "map: stb decode failed for %s — trying compressed + SJPG (baseline JPEG only)", p);
#endif
#if LV_USE_SJPG
    {
      lv_img_dsc_t *d = (lv_img_dsc_t *)calloc(1, sizeof(lv_img_dsc_t));
      if (!d) {
        ESP_LOGW(TAG, "map: alloc lv_img_dsc_t failed (path=%s)", p);
        free(data);
        return NULL;
      }
      d->data_size = (uint32_t)len;
      d->data = data;
      ESP_LOGI(TAG, "map: JPEG in %s (%u B) — compressed + SJPG/tjpgd (progressive JPEG needs RGB565 path above)", p,
               (unsigned)len);
      return d;
    }
#else
    ESP_LOGE(TAG,
             "map: %s is JPEG. Enable CONFIG_LV_USE_SJPG=y for baseline JPEG, or use LV_COLOR_DEPTH=16 for stb decode.",
             p);
    free(data);
    return NULL;
#endif
  }

  if (len >= 8) {
    ESP_LOGW(TAG, "map: unknown image format %s (len=%u head %02x%02x%02x%02x %02x%02x%02x%02x)", p, (unsigned)len, data[0],
             data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
  } else {
    ESP_LOGW(TAG, "map: unknown/short data %s len=%u", p, (unsigned)len);
  }
  free(data);
  return NULL;
}

static void tile_ram_clear_identity(int idx) {
  if (idx < 0 || idx >= MAP_GRID * MAP_GRID) {
    return;
  }
  s_tile_ram_z[idx] = -1;
  s_tile_ram_tx[idx] = 0;
  s_tile_ram_ty[idx] = 0;
}

static void tile_ram_set_identity(int idx, int z, int tx, int ty) {
  if (idx < 0 || idx >= MAP_GRID * MAP_GRID) {
    return;
  }
  s_tile_ram_z[idx] = z;
  s_tile_ram_tx[idx] = tx;
  s_tile_ram_ty[idx] = ty;
}

static void free_tile_ram_slot(int idx) {
  if (idx < 0 || idx >= MAP_GRID * MAP_GRID) {
    return;
  }
  if (!s_tile_ram[idx]) {
    return;
  }
  if (s_imgs[idx] && lv_obj_is_valid(s_imgs[idx])) {
    lv_img_set_src(s_imgs[idx], NULL);
  }
  void *raw = (void *)(uintptr_t)s_tile_ram[idx]->data;
  free(raw);
  free(s_tile_ram[idx]);
  s_tile_ram[idx] = NULL;
  tile_ram_clear_identity(idx);
}

static void free_all_tile_ram(void) {
  for (int i = 0; i < MAP_GRID * MAP_GRID; i++) {
    free_tile_ram_slot(i);
  }
}

static bool any_stream_tile_loaded(void) {
  for (int i = 0; i < MAP_GRID * MAP_GRID; i++) {
    if (s_tile_ram[i]) {
      return true;
    }
  }
  return false;
}

/* Read 9 cells into s_tile_ram using POSIX fopen. Reuses existing dsc when (z,x,y) already in RAM
 * (pan shifts grid: no SD read for tiles still on-screen). Detaches imgs before moving pointers. */
static void map_tiles_read_into_ram_dsc(void) {
  offline_map_log_build_caps_once();
  s_map_load_seq++;
  s_zoom = clamp_i(s_zoom, s_min_z, s_max_z);
#if GHOST_OFFLINE_MAP_VERBOSE
  {
    int have = 0;
    for (int i = 0; i < MAP_GRID * MAP_GRID; i++) {
      if (s_tile_ram[i]) {
        have++;
      }
    }
    (void)have;
    ESP_LOGI(TAG, "map: load #%u start z=%d base_tx=%d base_ty=%d (prev ram slots in use: %d)", s_map_load_seq, s_zoom,
             s_base_tx, s_base_ty, have);
  }
#endif
  for (int i = 0; i < MAP_GRID * MAP_GRID; i++) {
    if (s_imgs[i] && lv_obj_is_valid(s_imgs[i])) {
      lv_img_set_src(s_imgs[i], NULL);
    }
  }

  lv_img_dsc_t *old_ram[MAP_GRID * MAP_GRID];
  int old_z[MAP_GRID * MAP_GRID];
  int old_tx[MAP_GRID * MAP_GRID];
  int old_ty[MAP_GRID * MAP_GRID];
  bool old_used[MAP_GRID * MAP_GRID];
  memset(old_used, 0, sizeof(old_used));
  for (int i = 0; i < MAP_GRID * MAP_GRID; i++) {
    old_ram[i] = s_tile_ram[i];
    if (old_ram[i] && s_tile_ram_z[i] >= 0) {
      old_z[i] = s_tile_ram_z[i];
      old_tx[i] = s_tile_ram_tx[i];
      old_ty[i] = s_tile_ram_ty[i];
    } else {
      old_z[i] = -1;
      old_tx[i] = 0;
      old_ty[i] = 0;
    }
    s_tile_ram[i] = NULL;
    tile_ram_clear_identity(i);
  }

  int n_reused = 0;
  for (int idx = 0; idx < MAP_GRID * MAP_GRID; idx++) {
    int r = idx / MAP_GRID;
    int c = idx % MAP_GRID;
    int tx = s_base_tx + c;
    int ty = s_base_ty + r;
    const int z = s_zoom;
    set_tile_path(idx, z, tx, ty);

    bool reused = false;
    for (int j = 0; j < MAP_GRID * MAP_GRID; j++) {
      if (!old_ram[j] || old_used[j]) {
        continue;
      }
      if (old_z[j] == z && old_tx[j] == tx && old_ty[j] == ty) {
        s_tile_ram[idx] = old_ram[j];
        tile_ram_set_identity(idx, z, tx, ty);
        old_used[j] = true;
        reused = true;
        n_reused++;
#if GHOST_OFFLINE_MAP_VERBOSE
        ESP_LOGI(TAG, "map: cell %d tile z=%d (%d,%d) reuse from prev slot %d (no SD read)", idx, z, tx, ty, j);
#endif
        break;
      }
    }
    if (reused) {
      continue;
    }

    if (s_path_plain[idx][0] == '\0') {
#if GHOST_OFFLINE_MAP_VERBOSE
      ESP_LOGI(TAG, "map: cell %d tile (%d,%d) — empty path (path overflow?)", idx, tx, ty);
#endif
      continue;
    }
    if (!file_exists(s_path_plain[idx])) {
      ESP_LOGW(TAG, "map: cell %d path missing: %s", idx, s_path_plain[idx]);
      continue;
    }
    size_t len = 0;
    uint8_t *png = read_png_file_alloc(s_path_plain[idx], &len);
    if (!png) {
      continue;
    }
    lv_img_dsc_t *dsc = alloc_tile_dsc(png, len, s_path_plain[idx]);
    if (!dsc) {
      continue;
    }
    s_tile_ram[idx] = dsc;
    tile_ram_set_identity(idx, z, tx, ty);
#if GHOST_OFFLINE_MAP_VERBOSE
    ESP_LOGI(TAG, "map: cell %d tile (%d,%d) ram dsc=%p header w=%d h=%d dsz=%u", idx, tx, ty, (void *)dsc,
             (int)dsc->header.w, (int)dsc->header.h, (unsigned)dsc->data_size);
#endif
  }

  for (int j = 0; j < MAP_GRID * MAP_GRID; j++) {
    if (!old_ram[j] || old_used[j]) {
      continue;
    }
    void *raw = (void *)(uintptr_t)old_ram[j]->data;
    free(raw);
    free(old_ram[j]);
  }

  {
    int n = 0;
    for (int i = 0; i < MAP_GRID * MAP_GRID; i++) {
      if (s_tile_ram[i]) {
        n++;
      }
    }
    ESP_LOGI(TAG, "map: load #%u done: %d/%d slots in RAM (%d reused, %d read from SD)", s_map_load_seq, n,
             MAP_GRID * MAP_GRID, n_reused, n - n_reused);
  }
}

/* Call only while SD is mounted; uses fopen on plain paths. */
static bool stream_read_tiles_while_mounted(void) {
  map_tiles_read_into_ram_dsc();
  return any_stream_tile_loaded();
}

static void map_cell_set_no_tile_visible(int idx, bool show_placeholder) {
  if (idx < 0 || idx >= MAP_GRID * MAP_GRID) {
    return;
  }
  if (!s_no_tile_lbl[idx] || !lv_obj_is_valid(s_no_tile_lbl[idx])) {
    return;
  }
  if (show_placeholder) {
    lv_obj_clear_flag(s_no_tile_lbl[idx], LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_no_tile_lbl[idx], LV_OBJ_FLAG_HIDDEN);
  }
}

static void stream_apply_to_imgs(void) {
  for (int idx = 0; idx < MAP_GRID * MAP_GRID; idx++) {
    if (!s_imgs[idx] || !s_cells[idx]) {
#if GHOST_OFFLINE_MAP_VERBOSE
      ESP_LOGW(TAG, "map: apply cell %d: missing img or cell", idx);
#endif
      continue;
    }
    if (s_tile_ram[idx]) {
      const lv_img_dsc_t *d = s_tile_ram[idx];
#if GHOST_OFFLINE_MAP_VERBOSE
      ESP_LOGI(TAG, "map: apply cell %d: lv_img_set_src dsc=%p dsz=%u head w=%d h=%d cf=%u", idx, (void *)d,
               (unsigned)d->data_size, (int)d->header.w, (int)d->header.h, (unsigned)d->header.cf);
#endif
      lv_img_set_src(s_imgs[idx], s_tile_ram[idx]);
      {
        const void *after = lv_img_get_src(s_imgs[idx]);
        if (after != (const void *)s_tile_ram[idx]) {
          ESP_LOGE(TAG, "map: apply cell %d: get_src %p != dsc %p (LVGL rejected src?)", idx, (void *)after,
                   (void *)d);
        }
      }
      map_img_set_zoom_for_cell(s_imgs[idx]);
      lv_obj_clear_flag(s_imgs[idx], LV_OBJ_FLAG_HIDDEN);
      map_cell_set_no_tile_visible(idx, false);
    } else {
#if GHOST_OFFLINE_MAP_VERBOSE
      ESP_LOGI(TAG, "map: apply cell %d: no RAM tile — hidden + placeholder", idx);
#endif
      lv_obj_add_flag(s_imgs[idx], LV_OBJ_FLAG_HIDDEN);
      map_cell_set_no_tile_visible(idx, true);
    }
    lv_obj_invalidate(s_cells[idx]);
  }
}

/* Mount → read all tiles from SD → unmount (display works again on shared SPI). */
static void stream_refresh_from_sd(void) {
  bool disp = false;
  esp_err_t m = sd_card_mount_for_flush(&disp);
  if (m != ESP_OK) {
    ESP_LOGE(TAG, "map: stream refresh mount failed %s", esp_err_to_name(m));
    return;
  }
  ESP_LOGI(TAG, "map: stream refresh: mounted, reading tiles…");
  (void)stream_read_tiles_while_mounted();
  sd_card_unmount_after_flush(disp);
  stream_apply_to_imgs();
  map_hud_update();
}

static void map_hud_update(void) {
  if (!s_lbl) {
    return;
  }
  char line[LBL_MAX];
  const char *mode = s_stream_mode ? "stream" : "file";
  snprintf(line, sizeof(line), "z%2d  t %3d %3d  %s", s_zoom, s_base_tx, s_base_ty, mode);
  lv_label_set_text(s_lbl, line);
}

static void refresh_tiles(void) {
#if LV_USE_PNG && LV_USE_FS_STDIO
  if (s_stream_mode) {
    stream_refresh_from_sd();
    return;
  }
  /* Same as stream after mount: keep PNG in RAM with IHDR in dsc; do not use lv_img + "S:..."
   * or lv_fs for decode (VFS/decoder get_info can fail; fopen on /mnt/ghostesp/... still works). */
  if (!sd_card_exists(GHOSTESP_SD_ROOT)) {
    ESP_LOGW(TAG, "map: refresh_tiles: SD path not present (" GHOSTESP_SD_ROOT ")");
    return;
  }
  ESP_LOGI(TAG, "map: refresh_tiles (file mode, SD online)");
  map_tiles_read_into_ram_dsc();
  stream_apply_to_imgs();
  map_hud_update();
#endif
}

static void return_to_apps(void) { display_manager_switch_view(&apps_menu_view); }

static void handle_map_input_cb(InputEvent *e) {
  if (!e) {
    ESP_LOGW(TAG, "input: null event");
    return;
  }
  if (e->type == INPUT_TYPE_TOUCH) {
    lv_indev_data_t *d = &e->data.touch_data;
    ESP_LOGI(TAG, "touch: state=%d x=%d y=%d", (int)d->state, (int)d->point.x, (int)d->point.y);
    if (d->state == LV_INDEV_STATE_REL) {
      ESP_LOGI(TAG, "touch: release -> back to apps");
      return_to_apps();
    }
    return;
  }
  if (e->type == INPUT_TYPE_KEYBOARD) {
    uint8_t k = e->data.key_value;
    if (k == LV_KEY_ESC || k == 29 || k == '`' || k == 'q' || k == 'Q') {
      ESP_LOGI(TAG, "keyboard: 0x%02x -> back to apps", (unsigned)k);
      return_to_apps();
      return;
    }
    if (k == '[') {
      ESP_LOGI(TAG, "keyboard: [ zoom out");
      map_set_zoom_keep_center(s_zoom - 1);
    } else if (k == ']') {
      ESP_LOGI(TAG, "keyboard: ] zoom in");
      map_set_zoom_keep_center(s_zoom + 1);
    } else if (k == LV_KEY_LEFT || k == 'h' || k == 44) {
      ESP_LOGI(TAG, "keyboard: pan west");
      s_base_tx -= 1;
    } else if (k == LV_KEY_RIGHT || k == 'l' || k == 47) {
      ESP_LOGI(TAG, "keyboard: pan east");
      s_base_tx += 1;
    } else if (k == LV_KEY_UP || k == 'k' || k == ';' || k == 59) {
      ESP_LOGI(TAG, "keyboard: pan north");
      s_base_ty -= 1;
    } else if (k == LV_KEY_DOWN || k == 'j' || k == 46) {
      ESP_LOGI(TAG, "keyboard: pan south");
      s_base_ty += 1;
    } else {
      ESP_LOGI(TAG, "keyboard: unhandled 0x%02x", (unsigned)k);
      return;
    }
    refresh_tiles();
    return;
  }
  if (e->type == INPUT_TYPE_JOYSTICK) {
    int b = (int)e->data.joystick_index;
    /* Match terminal: 0/W 1/? 2/N 3/E 4/S — 1=zoom+. */
    if (b == 0) {
      ESP_LOGI(TAG, "joystick: %d pan west", b);
      s_base_tx -= 1;
    } else if (b == 3) {
      ESP_LOGI(TAG, "joystick: %d pan east", b);
      s_base_tx += 1;
    } else if (b == 2) {
      ESP_LOGI(TAG, "joystick: %d pan north", b);
      s_base_ty -= 1;
    } else if (b == 4) {
      ESP_LOGI(TAG, "joystick: %d pan south", b);
      s_base_ty += 1;
    } else if (b == 1) {
      ESP_LOGI(TAG, "joystick: %d zoom in", b);
      map_set_zoom_keep_center(s_zoom + 1);
    } else {
      ESP_LOGI(TAG, "joystick: unhandled %d", b);
      return;
    }
    refresh_tiles();
    return;
  }
  if (e->type == INPUT_TYPE_ENCODER) {
    if (e->data.encoder.button) {
      ESP_LOGI(TAG, "encoder: button (ignored)");
      return;
    }
    int dir = (int)e->data.encoder.direction;
    ESP_LOGI(TAG, "encoder: dir=%d (%s)", dir, dir > 0 ? "zoom in" : "zoom out");
    map_set_zoom_keep_center((dir > 0) ? (s_zoom + 1) : (s_zoom - 1));
    refresh_tiles();
    return;
  }
  if (e->type == INPUT_TYPE_EXIT_BUTTON) {
    ESP_LOGI(TAG, "exit button -> back to apps");
    return_to_apps();
    return;
  }
  ESP_LOGI(TAG, "input: type=%d (unknown) -> back to apps", (int)e->type);
  return_to_apps();
}

static void get_map_cb(void **c) { *c = handle_map_input_cb; }

#if !LV_USE_PNG || !LV_USE_FS_STDIO
/* No PNG/FS: instructions only */
static void no_cap_create(void) {
  lv_color_t bg = lv_color_hex(theme_bg());
  s_root = gui_screen_create_root(NULL, "Maps", bg, LV_OPA_COVER);
  offline_map_view.root = s_root;
  s_lbl = lv_label_create(s_root);
  lv_obj_set_width(s_lbl, LV_PCT(92));
  lv_label_set_long_mode(s_lbl, LV_LABEL_LONG_WRAP);
  lv_label_set_text(
      s_lbl,
      "Enable LVGL PNG + stdio FS (e.g. sdkconfig.defaults or menuconfig):\n"
      "  CONFIG_LV_USE_PNG=y\n"
      "  CONFIG_LV_USE_SJPG=y   (JPEG tiles misnamed .png — common for map caches)\n"
      "  CONFIG_LV_USE_FS_STDIO=y\n"
      "  CONFIG_LV_FS_STDIO_LETTER=83  (S: drive, fopen like rest of app)\n"
      "On SD, same layout as on PC under ghostesp/maps/:\n"
      "  " GHOSTESP_SD_ROOT "/maps/<source>/tiles/{z}/{x}/{y}.png\n"
      "  (this build uses: " MAP_SOURCE ")\n");
  lv_obj_set_style_text_color(s_lbl, lv_color_hex(theme_muted()), 0);
  lv_obj_align(s_lbl, LV_ALIGN_CENTER, 0, 0);
}

static void no_cap_destroy(void) {
  lvgl_obj_del_safe(&s_root);
  offline_map_view.root = NULL;
  s_lbl = NULL;
}
#endif

void offline_map_view_create(void) {
#if !LV_USE_PNG || !LV_USE_FS_STDIO
  (void)TAG;
  no_cap_create();
  return;
#else
  s_mounted_by_us = false;
  s_jit_suspended = false;
  s_stream_mode = false;
  s_map_cont = NULL;
  s_hud = NULL;
  for (int i = 0; i < MAP_GRID * MAP_GRID; i++) {
    s_cells[i] = NULL;
    s_imgs[i] = NULL;
    s_no_tile_lbl[i] = NULL;
    s_tile_ram[i] = NULL;
    tile_ram_clear_identity(i);
  }
  s_lbl = NULL;

  bool was_mounted = sd_card_manager.is_initialized;
  esp_err_t m = sd_card_mount_for_flush(&s_jit_suspended);
  if (m == ESP_OK && !was_mounted) {
    s_mounted_by_us = true;
  } else {
    s_mounted_by_us = false;
  }

  if (m != ESP_OK || !sd_card_exists(GHOSTESP_SD_ROOT)) {
    ESP_LOGW(TAG, "SD not ready: %s", esp_err_to_name(m));
  } else {
    load_map_metadata();
    focus_initial_view_on_existing_tiles();
  }

  /*
   * Shared display+SD SPI: JIT mount suspends LVGL. Read tile bytes into RAM, then unmount so the
   * panel works again. Tiles use lv_img + in-memory lv_img_dsc_t (PNG or JPEG via SJPG).
   */
  s_stream_mode = (m == ESP_OK && s_jit_suspended);
  if (m == ESP_OK && s_stream_mode) {
    if (!stream_read_tiles_while_mounted()) {
      ESP_LOGE(TAG, "Stream tile read failed");
    }
    sd_card_unmount_after_flush(s_jit_suspended);
    s_jit_suspended = false;
    s_mounted_by_us = false;
  }

  bool show_err = (m != ESP_OK);
  if (m == ESP_OK && s_stream_mode && !any_stream_tile_loaded()) {
    show_err = true;
    m = ESP_ERR_NOT_FOUND;
  } else if (m == ESP_OK && !s_stream_mode && !sd_card_exists(GHOSTESP_SD_ROOT)) {
    show_err = true;
  }

  if (show_err) {
    lv_color_t bg = lv_color_hex(theme_bg());
    s_root = gui_screen_create_root(NULL, "Maps", bg, LV_OPA_COVER);
    offline_map_view.root = s_root;
    s_lbl = lv_label_create(s_root);
    if (m == ESP_ERR_NOT_FOUND) {
      lv_label_set_text(s_lbl, "Maps: no tile PNGs in " GHOSTESP_SD_ROOT MAP_TILES_REL
                              " (check z/x/y). Use stream: SD unmounted after read.");
    } else {
      lv_label_set_text(s_lbl, "SD not found. Put maps under " GHOSTESP_SD_ROOT " (e.g. ghostesp/maps/... on the card)");
    }
    lv_obj_set_width(s_lbl, LV_PCT(92));
    lv_label_set_long_mode(s_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_lbl, lv_color_hex(theme_muted()), 0);
    lv_obj_align(s_lbl, LV_ALIGN_CENTER, 0, 0);
    if (s_mounted_by_us) {
      sd_card_unmount_after_flush(s_jit_suspended);
      s_mounted_by_us = false;
    }
    free_all_tile_ram();
    return;
  }

  s_root = gui_screen_create_root(NULL, "Maps", lv_color_hex(theme_bg()), LV_OPA_COVER);
  offline_map_view.root = s_root;
  lv_obj_set_style_pad_all(s_root, 0, 0);

  int inner_h = LV_VER_RES;
  if (inner_h > 100) {
    inner_h -= 24;
  }
  int w = (lv_obj_get_width(s_root) > 0) ? lv_obj_get_width(s_root) : LV_HOR_RES;
  if (w < 1) {
    w = 1;
  }
  if (inner_h < 1) {
    inner_h = 1;
  }
  /* Distribute w%3 / h%3 remainder across tracks so grid height/width match inner_h and no 1px seams. */
  lv_coord_t col_px[MAP_GRID];
  lv_coord_t row_px[MAP_GRID];
  {
    const int bw = w / MAP_GRID;
    const int rw = w % MAP_GRID;
    const int bh = inner_h / MAP_GRID;
    const int rh = inner_h % MAP_GRID;
    for (int i = 0; i < MAP_GRID; i++) {
      col_px[i] = (lv_coord_t)(bw + (i < rw ? 1 : 0));
      row_px[i] = (lv_coord_t)(bh + (i < rh ? 1 : 0));
    }
  }
  int cell_w = w / MAP_GRID;
  if (cell_w < 1) {
    cell_w = 1;
  }
  int cell_h = inner_h / MAP_GRID;
  if (cell_h < 1) {
    cell_h = 1;
  }
  s_map_cell_w = cell_w;
  s_map_cell_h = cell_h;

  s_map_cont = lv_obj_create(s_root);
  lv_obj_set_size(s_map_cont, w, inner_h);
  lv_obj_set_pos(s_map_cont, 0, 0);
  lv_obj_set_style_bg_opa(s_map_cont, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(s_map_cont, lv_color_hex(0x101010), 0);
  lv_obj_set_style_pad_all(s_map_cont, 0, 0);
  lv_obj_set_style_pad_row(s_map_cont, 0, 0);
  lv_obj_set_style_pad_column(s_map_cont, 0, 0);
  lv_obj_set_style_border_width(s_map_cont, 0, 0);
  lv_obj_set_style_radius(s_map_cont, 0, 0);
  lv_obj_set_style_shadow_width(s_map_cont, 0, 0);
  lv_obj_set_scrollbar_mode(s_map_cont, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(s_map_cont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_layout(s_map_cont, LV_LAYOUT_GRID);
  {
    static lv_coord_t col_d[4] = {0, 0, 0, LV_GRID_TEMPLATE_LAST};
    static lv_coord_t row_d[4] = {0, 0, 0, LV_GRID_TEMPLATE_LAST};
    col_d[0] = col_px[0];
    col_d[1] = col_px[1];
    col_d[2] = col_px[2];
    row_d[0] = row_px[0];
    row_d[1] = row_px[1];
    row_d[2] = row_px[2];
    lv_obj_set_grid_dsc_array(s_map_cont, col_d, row_d);
  }

  int n = 0;
  for (int r = 0; r < MAP_GRID; r++) {
    for (int c = 0; c < MAP_GRID; c++) {
      s_map_cell_w_slot[n] = (int)col_px[c];
      s_map_cell_h_slot[n] = (int)row_px[r];
      s_cells[n] = lv_obj_create(s_map_cont);
      lv_obj_set_size(s_cells[n], (int)col_px[c], (int)row_px[r]);
      lv_obj_set_style_pad_all(s_cells[n], 0, 0);
      lv_obj_set_style_pad_row(s_cells[n], 0, 0);
      lv_obj_set_style_pad_column(s_cells[n], 0, 0);
      lv_obj_set_style_border_width(s_cells[n], 0, 0);
      /* Match map: theme defaults add radius/shadow; gaps come from grid pad_row/pad_column on parent. */
      lv_obj_set_style_radius(s_cells[n], 0, 0);
      lv_obj_set_style_shadow_width(s_cells[n], 0, 0);
      lv_obj_set_style_outline_width(s_cells[n], 0, 0);
      lv_obj_set_style_bg_color(s_cells[n], lv_color_hex(0x101010), 0);
      lv_obj_set_style_bg_opa(s_cells[n], LV_OPA_COVER, 0);
      lv_obj_set_scrollbar_mode(s_cells[n], LV_SCROLLBAR_MODE_OFF);
      lv_obj_clear_flag(s_cells[n], LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_clear_flag(s_cells[n], LV_OBJ_FLAG_OVERFLOW_VISIBLE);
      lv_obj_set_grid_cell(s_cells[n], LV_GRID_ALIGN_STRETCH, c, 1, LV_GRID_ALIGN_STRETCH, r, 1);
      s_imgs[n] = lv_img_create(s_cells[n]);
      /* Must match map_img_set_zoom_for_cell: object size = transformed (scaled) size to avoid clip bugs. */
      lv_img_set_size_mode(s_imgs[n], LV_IMG_SIZE_MODE_REAL);
      lv_obj_set_style_radius(s_imgs[n], 0, 0);
      lv_obj_set_style_bg_opa(s_imgs[n], LV_OPA_TRANSP, 0);
      lv_obj_set_style_pad_all(s_imgs[n], 0, 0);
      lv_obj_set_style_img_opa(s_imgs[n], LV_OPA_COVER, 0);
      lv_obj_set_scrollbar_mode(s_imgs[n], LV_SCROLLBAR_MODE_OFF);
      lv_obj_clear_flag(s_imgs[n], LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_center(s_imgs[n]);
      lv_img_set_antialias(s_imgs[n], true);

      s_no_tile_lbl[n] = lv_label_create(s_cells[n]);
      lv_label_set_text(s_no_tile_lbl[n], "No tile\nfound");
      lv_label_set_long_mode(s_no_tile_lbl[n], LV_LABEL_LONG_WRAP);
      lv_obj_set_width(s_no_tile_lbl[n], LV_MAX((int)col_px[c] - 4, 1));
      lv_obj_set_style_pad_all(s_no_tile_lbl[n], 0, 0);
      lv_obj_set_style_bg_opa(s_no_tile_lbl[n], LV_OPA_TRANSP, 0);
      lv_obj_set_style_radius(s_no_tile_lbl[n], 0, 0);
      lv_obj_set_style_border_width(s_no_tile_lbl[n], 0, 0);
      lv_obj_set_style_shadow_width(s_no_tile_lbl[n], 0, 0);
      lv_obj_set_style_text_align(s_no_tile_lbl[n], LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_text_color(s_no_tile_lbl[n], lv_color_hex(theme_muted()), 0);
      {
        const lv_font_t *nf =
            ((int)col_px[c] < 100 || (int)row_px[r] < 100) ? &lv_font_montserrat_8 : &lv_font_montserrat_10;
        lv_obj_set_style_text_font(s_no_tile_lbl[n], nf, 0);
      }
      lv_obj_center(s_no_tile_lbl[n]);
      lv_obj_add_flag(s_no_tile_lbl[n], LV_OBJ_FLAG_HIDDEN);
      /* Image must paint above a themed label background (if any); label stays hidden when tiles load. */
      lv_obj_move_foreground(s_imgs[n]);

      n++;
    }
  }

  s_hud = lv_obj_create(s_root);
  lv_obj_set_size(s_hud, w, 24);
  lv_obj_set_pos(s_hud, 0, inner_h);
  lv_obj_set_style_bg_opa(s_hud, LV_OPA_60, 0);
  lv_obj_set_style_bg_color(s_hud, lv_color_hex(0x000000), 0);
  lv_obj_set_style_pad_left(s_hud, 4, 0);
  s_lbl = lv_label_create(s_hud);
  lv_obj_set_style_text_color(s_lbl, lv_color_hex(theme_muted()), 0);
  lv_obj_set_width(s_lbl, w - 8);
  lv_label_set_long_mode(s_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_align(s_lbl, LV_ALIGN_LEFT_MID, 0, 0);

  if (s_stream_mode) {
    stream_apply_to_imgs();
    map_hud_update();
  } else {
    refresh_tiles();
  }
  {
    int nt = 0;
    for (int i = 0; i < MAP_GRID * MAP_GRID; i++) {
      if (s_tile_ram[i]) {
        nt++;
      }
    }
    ESP_LOGI(TAG, "map: view ready: stream_mode=%d ram_tiles=%d/%d cell=%dx%d", s_stream_mode ? 1 : 0, nt,
             MAP_GRID * MAP_GRID, s_map_cell_w, s_map_cell_h);
  }
#endif
}

void offline_map_view_destroy(void) {
#if !LV_USE_PNG || !LV_USE_FS_STDIO
  no_cap_destroy();
  return;
#else
  {
    bool susp = s_jit_suspended;
    bool un = s_mounted_by_us;
    s_mounted_by_us = false;
    s_jit_suspended = false;
    s_stream_mode = false;
    free_all_tile_ram();
    lvgl_obj_del_safe(&s_root);
    offline_map_view.root = NULL;
    s_hud = NULL;
    s_map_cont = NULL;
    for (int i = 0; i < MAP_GRID * MAP_GRID; i++) {
      s_cells[i] = NULL;
      s_imgs[i] = NULL;
      s_no_tile_lbl[i] = NULL;
    }
    s_lbl = NULL;
    if (un) {
      sd_card_unmount_after_flush(susp);
    }
  }
#endif
}

View offline_map_view = {
    .root = NULL,
    .create = offline_map_view_create,
    .destroy = offline_map_view_destroy,
    .input_callback = handle_map_input_cb,
    .name = "Maps",
    .get_hardwareinput_callback = get_map_cb,
};
