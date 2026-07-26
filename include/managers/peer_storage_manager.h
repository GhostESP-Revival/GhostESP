#ifndef PEER_STORAGE_MANAGER_H
#define PEER_STORAGE_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Peer-backed SD storage for boards that have no local SD slot but are paired
 * over GhostLink with a board that does (e.g. somethingsomething2 / Banshee S3
 * streams file bytes to somethingsomething / Banshee C5, which owns the SD).
 *
 * The protocol rides on COMM_STREAM_CHANNEL_STORAGE. Frames are small
 * null-padded text records that both sides parse with sscanf/strtok:
 *
 *   request -> peer:
 *     "OPEN|<mode>|<path>\0"        mode = "w" | "a" | "wb" | "ab"
 *     "WRITE|<handle>|<nbytes>\0"   followed by <nbytes> raw bytes in the
 *                                  NEXT stream packet on the same channel.
 *     "CLOSE|<handle>\0"
 *     "MKDIR|<path>\0"
 *     "STAT|<path>\0"              (optional)
 *
 *   peer -> request:
 *     "OK|<handle>\0"               in response to OPEN
 *     "OK\0"                        in response to WRITE/CLOSE/MKDIR
 *     "ERR|<code>\0"                code is a short token such as "open",
 *                                  "write", "nomem", "badhandle", "path"
 *     "STAT|<size>\0"               in response to STAT
 *
 * The caller-side API is a thin synchronous wrapper. It owns a single in-flight
 * request at a time (peer_ota_manager has the same constraint because the
 * process-wide response callback slot is reused). Use peer_storage_begin/end
 * to bracket a session; nested sessions on the same task are unsupported.
 *
 * On boards that DO have a local SD, peer_storage_* transparently falls back
 * to the local sd_card_* helpers, so callers can use one code path everywhere.
 */

typedef enum {
    PEER_STORAGE_OK = 0,
    PEER_STORAGE_ERR_NOT_CONNECTED,
    PEER_STORAGE_ERR_TIMEOUT,
    PEER_STORAGE_ERR_PROTOCOL,
    PEER_STORAGE_ERR_PEER,
    PEER_STORAGE_ERR_LOCAL,
    PEER_STORAGE_ERR_NO_MEM,
    PEER_STORAGE_ERR_BAD_ARG,
} peer_storage_err_t;

/*
 * True on the peer that owns a writable SD (somethingsomething today). Only
 * this board registers the handler in main.c. Equivalent of
 * peer_ota_manager_is_supported().
 */
bool peer_storage_manager_is_peer(void);

/*
 * True on a board that should act as a *client* of peer storage: it has no
 * local SD AND GhostLink is available. somethingsomething2 is the only such
 * board today. Use this to decide whether to call peer_storage_* or local
 * sd_card_* directly (the wrapper already handles it, but callers sometimes
 * want to short-circuit entirely when neither path is available).
 */
bool peer_storage_manager_is_client(void);

/*
 * Install the peer-side stream handler. Call once during boot on the board
 * where peer_storage_manager_is_peer() is true. Idempotent.
 */
esp_err_t peer_storage_manager_init_peer(void);

/*
 * Caller-side session. Returns PEER_STORAGE_OK on success. Subsequent
 * open/write/close/mkdir calls are serialized by an internal mutex.
 *
 *   peer_storage_err_t e;
 *   int h;
 *   if ((e = peer_storage_begin()) != PEER_STORAGE_OK) return;
 *   if ((e = peer_storage_open("/mnt/ghostesp/scans/arp_TS.jsonl", "w", &h)) == PEER_STORAGE_OK) {
 *       peer_storage_write(h, buf, len);
 *       peer_storage_close(h);
 *   }
 *   peer_storage_end();
 *
 * On boards with local SD, begin/end are no-ops and open/write/close hit
 * local fopen/fwrite/fclose instead of the RPC channel.
 */
peer_storage_err_t peer_storage_begin(void);
void peer_storage_end(void);

peer_storage_err_t peer_storage_open(const char *path, const char *mode, int *out_handle);
peer_storage_err_t peer_storage_write(int handle, const void *data, size_t len);
peer_storage_err_t peer_storage_close(int handle);
peer_storage_err_t peer_storage_mkdir(const char *path);

/*
 * Convenience: open+write+close in one call. Useful for one-shot dumps of an
 * in-RAM buffer (e.g. a JSONL scan report). Returns the final status.
 */
peer_storage_err_t peer_storage_save_file(const char *path, const void *data, size_t len);

/*
 * Convenience: append a chunk to an opened-or-new file. If *handle < 0 the
 * file is opened in append mode and the handle is stored; otherwise the chunk
 * is written to the existing handle. Caller owns *handle and must close it
 * eventually via peer_storage_close() or peer_storage_end(). On error
 * *handle is reset to -1.
 */
peer_storage_err_t peer_storage_append_chunk(const char *path, const void *data, size_t len, int *handle);

#endif // PEER_STORAGE_MANAGER_H
