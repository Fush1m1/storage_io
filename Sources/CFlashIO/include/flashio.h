/*
 * FlashIO — disk I/O middleware tuned for flash storage (iOS / APFS on NAND).
 *
 * Core ideas
 *   - Never overwrite in place: all mutations are appended to a log of
 *     segment files (sequential, page-aligned-ish writes are cheapest for a
 *     NAND FTL and for APFS copy-on-write).
 *   - Coalesce small writes in memory and issue them as large pwrite()s.
 *   - Make durability explicit and cheap: NONE (page cache), BARRIER
 *     (F_BARRIERFSYNC: ordering only), FULL (F_FULLFSYNC: drive cache flush).
 *   - Detect torn writes with CRC32C framing and truncate them on recovery.
 *   - Reclaim space with compaction that you can schedule (e.g. from a
 *     BGProcessingTask while charging) to control write amplification.
 *
 * All functions return FIO_OK (0) on success, a negative errno on OS errors,
 * or one of the FIO_E* codes below.
 */
#ifndef FLASHIO_H
#define FLASHIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- status codes (macros so that they import into Swift as Int32) ---- */
#define FIO_OK         0
#define FIO_ENOTFOUND  (-1001) /* key does not exist                        */
#define FIO_ECORRUPT   (-1002) /* malformed on-disk data / short read        */
#define FIO_ELOCKED    (-1003) /* store directory is opened by someone else  */
#define FIO_EINVAL     (-1004) /* invalid argument                           */
#define FIO_ENOMEM     (-1005) /* allocation failure                         */
#define FIO_EPOISONED  (-1006) /* an earlier write/sync failed; reopen store */

#define FIO_MAX_KEY_SIZE   65535u
#define FIO_MAX_VALUE_SIZE (1u << 30)

const char *fio_strerror(int code);

/* ---- durability ---- */
typedef enum fio_durability {
    /* Store APIs: the record stays in the in-process coalescing buffer until it
     * fills, fio_store_sync() or close. Lost if the process dies before that.
     * fio_store_sync(NONE) / helpers: data is in the OS page cache (survives
     * an app crash, not a kernel panic or power loss).                     */
    FIO_DURABILITY_NONE = 0,
    /* Darwin F_BARRIERFSYNC: everything written before is persisted before
     * anything written after. Much cheaper than FULL; enough for ordering
     * guarantees (e.g. "data before metadata"). Linux: fdatasync().        */
    FIO_DURABILITY_BARRIER = 1,
    /* Darwin F_FULLFSYNC: flush the device write cache. Expensive (ms),
     * costs flash wear. Use for "must never be lost" commits only.         */
    FIO_DURABILITY_FULL = 2,
} fio_durability;

/* ---- low level helpers (usable without the store) ---- */
uint32_t fio_crc32c(uint32_t crc, const void *data, size_t len);
int fio_fsync(int fd, fio_durability d);
int fio_fsync_dir(const char *dir_path, fio_durability d);
int fio_pwrite_all(int fd, const void *buf, size_t len, uint64_t off);
int fio_pread_all(int fd, void *buf, size_t len, uint64_t off);
/* Best-effort: reserve blocks without changing the file size. */
int fio_preallocate(int fd, uint64_t len);
/* Bypass the unified buffer cache for this fd (streaming / cold reads). */
int fio_set_nocache(int fd, int enable);
/* Read-ahead hint (F_RDADVISE / POSIX_FADV_WILLNEED). */
int fio_read_advise(int fd, uint64_t off, uint64_t len);
/* Replace `path` atomically: tmp file -> sync(d) -> rename -> dir sync(d). */
int fio_atomic_write_file(const char *path, const void *data, size_t len,
                          fio_durability d);
/* APFS clonefile(2) (zero bytes written to flash); falls back to a copy. */
int fio_clone_file(const char *src, const char *dst);
void fio_free(void *p);

/* ---- write batch: applied atomically (single CRC-protected frame) ---- */
typedef struct fio_batch fio_batch;
fio_batch *fio_batch_new(void);
int fio_batch_put(fio_batch *b, const void *key, size_t klen,
                  const void *val, size_t vlen);
int fio_batch_delete(fio_batch *b, const void *key, size_t klen);
size_t fio_batch_count(const fio_batch *b);
void fio_batch_clear(fio_batch *b);
void fio_batch_free(fio_batch *b);

/* ---- log-structured key/value store ---- */
typedef struct fio_options {
    uint64_t segment_size;        /* roll to a new segment file after this (default 8 MiB) */
    uint32_t write_buffer_size;   /* coalescing buffer; flushed when full (default 64 KiB = 4 NAND pages) */
    double   compaction_threshold;/* compact sealed segments whose garbage ratio >= this (default 0.5) */
    int      auto_compact;        /* run compaction automatically after a segment roll (default 1) */
    int      preallocate;         /* F_PREALLOCATE new segments (default 0; measure on your device) */
    fio_durability close_durability; /* sync level used by fio_store_close (default BARRIER) */
} fio_options;

void fio_options_init(fio_options *o);

typedef struct fio_stats {
    uint64_t keys;               /* live keys */
    uint64_t segments;           /* segment files */
    uint64_t live_bytes;         /* bytes of records that are still reachable */
    uint64_t total_bytes;        /* bytes in all segment files */
    uint64_t user_bytes;         /* key+value bytes submitted by the app */
    uint64_t log_bytes;          /* frame bytes appended for app writes */
    uint64_t compaction_bytes;   /* frame bytes appended by compaction */
    uint64_t physical_bytes;     /* bytes passed to pwrite() */
    uint64_t write_calls;        /* pwrite() batches issued */
    uint64_t barrier_syncs;
    uint64_t full_syncs;
    uint64_t compactions;
    uint64_t reclaimed_bytes;    /* segment bytes deleted by compaction */
    uint64_t recovered_truncated_bytes; /* torn tail bytes dropped at open */
} fio_stats;

typedef struct fio_store fio_store;

int  fio_store_open(const char *dir, const fio_options *opt, fio_store **out);
void fio_store_close(fio_store *s);   /* flushes with close_durability */

int fio_store_put(fio_store *s, const void *key, size_t klen,
                  const void *val, size_t vlen, fio_durability d);
int fio_store_delete(fio_store *s, const void *key, size_t klen,
                     fio_durability d);
int fio_store_write(fio_store *s, const fio_batch *b, fio_durability d);
/* *val is malloc'd; release with fio_free(). FIO_ENOTFOUND if absent. */
int fio_store_get(fio_store *s, const void *key, size_t klen,
                  void **val, size_t *vlen);
/* Flush the coalescing buffer and sync at level d. */
int fio_store_sync(fio_store *s, fio_durability d);
int fio_store_compact(fio_store *s);
int fio_store_stats(fio_store *s, fio_stats *out);

/* Iterate live keys (unordered). Do not call into the store from `fn`.
 * Return non-zero from `fn` to stop early. */
typedef int (*fio_key_fn)(const void *key, size_t klen, void *ctx);
int fio_store_foreach_key(fio_store *s, fio_key_fn fn, void *ctx);

#ifdef __cplusplus
}
#endif
#endif /* FLASHIO_H */
