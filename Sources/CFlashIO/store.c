/*
 * Log-structured key/value store.
 *
 * On-disk layout: <dir>/LOCK and <dir>/NNNNNNNNNN.seg segment files.
 * A segment is a sequence of frames:
 *
 *   frame  := magic:u32 crc:u32 payload_len:u32 count:u32 payload
 *   crc    := crc32c(payload_len, count, payload)
 *   payload:= op*
 *   op     := type:u8 klen:u32 vlen:u32 key[klen] value[vlen]
 *
 * All integers are little endian. A frame is the unit of atomicity: a write
 * batch becomes exactly one frame. Recovery replays segments in id order and
 * stops (truncating) at the first frame that is incomplete or fails its CRC.
 *
 * The in-memory index maps key -> (segment, value offset, value length).
 * Only the newest segment ("active") is appended to; older ones are sealed
 * and are reclaimed by compaction once enough of their records are dead.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "flashio.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define FRAME_MAGIC 0x314F4946u /* "FIO1" */
#define FRAME_HDR   16u
#define OP_HDR      9u
#define OP_PUT      1
#define OP_DEL      2
#define COMPACT_BATCH_BYTES (256u * 1024u)

static inline void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int reserve(uint8_t **buf, size_t *cap, size_t need) {
    if (need <= *cap) return FIO_OK;
    size_t nc = *cap ? *cap : 4096;
    while (nc < need) nc *= 2;
    uint8_t *nb = (uint8_t *)realloc(*buf, nc);
    if (!nb) return FIO_ENOMEM;
    *buf = nb;
    *cap = nc;
    return FIO_OK;
}

/* ================================================================== */
/* batch                                                              */
/* ================================================================== */
struct fio_batch {
    uint8_t *buf;
    size_t len, cap;
    uint32_t count;
    uint64_t user_bytes;
};

fio_batch *fio_batch_new(void) { return (fio_batch *)calloc(1, sizeof(fio_batch)); }

void fio_batch_clear(fio_batch *b) {
    if (!b) return;
    b->len = 0;
    b->count = 0;
    b->user_bytes = 0;
}

void fio_batch_free(fio_batch *b) {
    if (!b) return;
    free(b->buf);
    free(b);
}

size_t fio_batch_count(const fio_batch *b) { return b ? b->count : 0; }

static int batch_add(fio_batch *b, uint8_t type, const void *k, size_t kl,
                     const void *v, size_t vl) {
    if (!b || !k || kl == 0 || kl > FIO_MAX_KEY_SIZE) return FIO_EINVAL;
    if ((vl && !v) || vl > FIO_MAX_VALUE_SIZE) return FIO_EINVAL;
    size_t rec = OP_HDR + kl + vl;
    if (b->len + rec > (size_t)UINT32_MAX - FRAME_HDR) return FIO_EINVAL;
    if (reserve(&b->buf, &b->cap, b->len + rec) != FIO_OK) return FIO_ENOMEM;
    uint8_t *p = b->buf + b->len;
    p[0] = type;
    put_u32(p + 1, (uint32_t)kl);
    put_u32(p + 5, (uint32_t)vl);
    memcpy(p + OP_HDR, k, kl);
    if (vl) memcpy(p + OP_HDR + kl, v, vl);
    b->len += rec;
    b->count++;
    b->user_bytes += kl + vl;
    return FIO_OK;
}

int fio_batch_put(fio_batch *b, const void *key, size_t klen,
                  const void *val, size_t vlen) {
    return batch_add(b, OP_PUT, key, klen, val, vlen);
}

int fio_batch_delete(fio_batch *b, const void *key, size_t klen) {
    return batch_add(b, OP_DEL, key, klen, NULL, 0);
}

/* ================================================================== */
/* store structures                                                   */
/* ================================================================== */
typedef struct fio_entry {
    struct fio_entry *next;
    uint64_t hash;
    uint64_t voff;   /* value offset inside segment file */
    uint32_t seg;    /* segment id holding the newest record for this key */
    uint32_t vlen;
    uint32_t rec;    /* size of that record (OP_HDR + klen + vlen) */
    uint32_t klen;
    uint8_t tomb;    /* newest record is a delete */
    uint8_t key[];
} fio_entry;

typedef struct fio_segment {
    uint32_t id;
    int fd;
    uint64_t size; /* logical size incl. still-buffered bytes (active only) */
    uint64_t live; /* bytes of records referenced by the index */
} fio_segment;

struct fio_store {
    pthread_rwlock_t lock;
    char *dir;
    int lockfd;
    fio_options opt;

    fio_entry **buckets;
    size_t nbuckets, nentries;
    uint64_t nkeys;

    fio_segment *segs; /* sorted by id; last one is active */
    size_t nsegs, capsegs;

    uint8_t *wbuf; /* coalescing buffer for the active fio_segment */
    size_t wlen, wcap;
    uint64_t flushed; /* bytes of active segment already on disk */

    fio_stats st;
    int err;        /* sticky error: store is poisoned */
    int compacting;
    int compact_pending;
};

static uint64_t hash_key(const void *k, size_t n) {
    const uint8_t *p = (const uint8_t *)k;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h ^ (h >> 29);
}

static fio_entry *lookup(fio_store *s, const void *k, size_t kl, uint64_t h) {
    for (fio_entry *e = s->buckets[h & (s->nbuckets - 1)]; e; e = e->next)
        if (e->hash == h && e->klen == kl && memcmp(e->key, k, kl) == 0) return e;
    return NULL;
}

static int grow_table(fio_store *s) {
    size_t nb = s->nbuckets * 2;
    fio_entry **b = (fio_entry **)calloc(nb, sizeof(fio_entry *));
    if (!b) return FIO_ENOMEM;
    for (size_t i = 0; i < s->nbuckets; i++) {
        fio_entry *e = s->buckets[i];
        while (e) {
            fio_entry *next = e->next;
            size_t j = e->hash & (nb - 1);
            e->next = b[j];
            b[j] = e;
            e = next;
        }
    }
    free(s->buckets);
    s->buckets = b;
    s->nbuckets = nb;
    return FIO_OK;
}

static fio_segment *find_seg(fio_store *s, uint32_t id) {
    size_t lo = 0, hi = s->nsegs;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (s->segs[mid].id == id) return &s->segs[mid];
        if (s->segs[mid].id < id) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

static fio_segment *active(fio_store *s) { return &s->segs[s->nsegs - 1]; }

static void seg_path(const fio_store *s, uint32_t id, char *buf, size_t cap) {
    snprintf(buf, cap, "%s/%010u.seg", s->dir, id);
}

static size_t path_cap(const fio_store *s) { return strlen(s->dir) + 32; }

static int push_seg(fio_store *s, uint32_t id, int fd, uint64_t size) {
    if (s->nsegs == s->capsegs) {
        size_t nc = s->capsegs ? s->capsegs * 2 : 8;
        fio_segment *ns = (fio_segment *)realloc(s->segs, nc * sizeof(fio_segment));
        if (!ns) return FIO_ENOMEM;
        s->segs = ns;
        s->capsegs = nc;
    }
    fio_segment *g = &s->segs[s->nsegs++];
    g->id = id; g->fd = fd; g->size = size; g->live = 0;
    return FIO_OK;
}

static int poison(fio_store *s, int rc) {
    if (rc != FIO_OK && s->err == FIO_OK) s->err = rc;
    return rc;
}

/* ================================================================== */
/* frame parsing / index application                                  */
/* ================================================================== */
static int validate_payload(const uint8_t *p, size_t len, uint32_t count) {
    size_t off = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (len - off < OP_HDR) return FIO_ECORRUPT;
        uint8_t type = p[off];
        uint64_t kl = get_u32(p + off + 1), vl = get_u32(p + off + 5);
        if ((type != OP_PUT && type != OP_DEL) || kl == 0 ||
            (type == OP_DEL && vl != 0))
            return FIO_ECORRUPT;
        if ((uint64_t)(len - off - OP_HDR) < kl + vl) return FIO_ECORRUPT;
        off += OP_HDR + kl + vl;
    }
    return off == len ? FIO_OK : FIO_ECORRUPT;
}

/* Apply already-validated ops located at file offset frame_off of seg. */
static int apply_ops(fio_store *s, fio_segment *seg, uint64_t frame_off,
                     const uint8_t *p, size_t len) {
    size_t off = 0;
    while (off < len) {
        uint8_t type = p[off];
        uint32_t kl = get_u32(p + off + 1), vl = get_u32(p + off + 5);
        const uint8_t *key = p + off + OP_HDR;
        uint32_t rec = OP_HDR + kl + vl;
        uint64_t h = hash_key(key, kl);
        fio_entry *e = lookup(s, key, kl, h);

        if (e) {
            fio_segment *old = find_seg(s, e->seg);
            if (old) old->live -= e->rec;
        }
        if (type == OP_PUT) {
            if (!e) {
                if (s->nentries >= s->nbuckets && grow_table(s) != FIO_OK) return FIO_ENOMEM;
                e = (fio_entry *)malloc(sizeof(fio_entry) + kl);
                if (!e) return FIO_ENOMEM;
                e->hash = h;
                e->klen = kl;
                memcpy(e->key, key, kl);
                size_t b = h & (s->nbuckets - 1);
                e->next = s->buckets[b];
                s->buckets[b] = e;
                s->nentries++;
                s->nkeys++;
            } else if (e->tomb) {
                s->nkeys++;
            }
            e->tomb = 0;
            e->voff = frame_off + FRAME_HDR + off + OP_HDR + kl;
            e->vlen = vl;
        } else {
            if (!e) { off += rec; continue; } /* nothing to shadow: pure garbage */
            if (!e->tomb) s->nkeys--;
            e->tomb = 1;
            e->voff = 0;
            e->vlen = 0;
        }
        e->seg = seg->id;
        e->rec = rec;
        seg->live += rec;
        off += rec;
    }
    return FIO_OK;
}

/* ================================================================== */
/* write path                                                         */
/* ================================================================== */
static int flush_buf(fio_store *s) {
    if (s->wlen == 0) return FIO_OK;
    fio_segment *a = active(s);
    int rc = fio_pwrite_all(a->fd, s->wbuf, s->wlen, s->flushed);
    if (rc != FIO_OK) return poison(s, rc);
    s->flushed += s->wlen;
    s->st.physical_bytes += s->wlen;
    s->st.write_calls++;
    s->wlen = 0;
    return FIO_OK;
}

static int do_sync(fio_store *s, int fd, fio_durability d) {
    if (d == FIO_DURABILITY_NONE) return FIO_OK;
    int rc = fio_fsync(fd, d);
    if (rc != FIO_OK) return poison(s, rc);
    if (d == FIO_DURABILITY_BARRIER) s->st.barrier_syncs++; else s->st.full_syncs++;
    return FIO_OK;
}

static int sync_locked(fio_store *s, fio_durability d) {
    int rc = flush_buf(s);
    if (rc == FIO_OK) rc = do_sync(s, active(s)->fd, d);
    return rc;
}

static int create_segment(fio_store *s, uint32_t id) {
    char *path = (char *)malloc(path_cap(s));
    if (!path) return FIO_ENOMEM;
    seg_path(s, id, path, path_cap(s));
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    int rc = fd < 0 ? -errno : FIO_OK;
    free(path);
    if (rc != FIO_OK) return rc;
    if (s->opt.preallocate) (void)fio_preallocate(fd, s->opt.segment_size);
    rc = push_seg(s, id, fd, 0);
    if (rc != FIO_OK) { close(fd); return rc; }
    s->flushed = 0;
    /* make the new directory entry persistent (cheaply, ordered) */
    rc = fio_fsync_dir(s->dir, FIO_DURABILITY_BARRIER);
    return rc;
}

static int roll(fio_store *s) {
    /* Seal the active segment. The barrier guarantees that a later FULL
     * sync on any newer segment also covers this one, and that compaction
     * output is ordered before the unlink of its victims. */
    int rc = sync_locked(s, FIO_DURABILITY_BARRIER);
    if (rc != FIO_OK) return rc;
    rc = create_segment(s, active(s)->id + 1);
    if (rc != FIO_OK) return poison(s, rc);
    if (!s->compacting) s->compact_pending = 1;
    return FIO_OK;
}

static int append_frame(fio_store *s, const uint8_t *payload, size_t plen,
                        uint32_t count, int from_compaction) {
    size_t flen = FRAME_HDR + plen;
    if (active(s)->size > 0 && active(s)->size + flen > s->opt.segment_size) {
        int rc = roll(s);
        if (rc != FIO_OK) return rc;
    }
    if (reserve(&s->wbuf, &s->wcap, s->wlen + flen) != FIO_OK) return FIO_ENOMEM;

    uint8_t *f = s->wbuf + s->wlen;
    put_u32(f, FRAME_MAGIC);
    put_u32(f + 8, (uint32_t)plen);
    put_u32(f + 12, count);
    memcpy(f + FRAME_HDR, payload, plen);
    put_u32(f + 4, fio_crc32c(0, f + 8, flen - 8));

    fio_segment *a = active(s);
    uint64_t frame_off = a->size;
    a->size += flen;
    s->wlen += flen;
    if (from_compaction) s->st.compaction_bytes += flen; else s->st.log_bytes += flen;

    int rc = apply_ops(s, a, frame_off, payload, plen);
    if (rc != FIO_OK) return poison(s, rc); /* index is now inconsistent */

    if (s->wlen >= s->opt.write_buffer_size) return flush_buf(s);
    return FIO_OK;
}

/* ================================================================== */
/* compaction                                                         */
/* ================================================================== */
static int compact_one(fio_store *s, uint32_t vid, uint32_t min_remaining,
                       fio_batch *b) {
    fio_segment *v = find_seg(s, vid);
    if (!v || v->live == 0) return FIO_OK; /* nothing references it */
    uint64_t vsize = v->size;
    uint8_t *mem = NULL;
    int rc = FIO_OK;

    if (v->live > 0) {
        /* One large sequential read through an uncached fd, so cold data
         * does not evict the app's hot pages. */
        char *path = (char *)malloc(path_cap(s));
        if (!path) return FIO_ENOMEM;
        seg_path(s, vid, path, path_cap(s));
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        free(path);
        if (fd < 0) return -errno;
        (void)fio_set_nocache(fd, 1);
        mem = (uint8_t *)malloc(vsize ? vsize : 1);
        rc = mem ? fio_pread_all(fd, mem, vsize, 0) : FIO_ENOMEM;
        close(fd);
        if (rc != FIO_OK) { free(mem); return rc; }
    }

    for (size_t i = 0; i < s->nbuckets && rc == FIO_OK; i++) {
        fio_entry **pp = &s->buckets[i];
        while (*pp && rc == FIO_OK) {
            fio_entry *e = *pp;
            if (e->seg != vid) { pp = &e->next; continue; }
            if (!e->tomb) {
                if (e->voff + e->vlen > vsize) { rc = FIO_ECORRUPT; break; }
                rc = fio_batch_put(b, e->key, e->klen, mem + e->voff, e->vlen);
            } else if (min_remaining < vid) {
                /* an older segment may still hold a value: keep shadowing it */
                rc = fio_batch_delete(b, e->key, e->klen);
            } else {
                *pp = e->next; /* nothing older survives: drop tombstone */
                free(e);
                s->nentries--;
                continue;
            }
            pp = &e->next;
            if (rc == FIO_OK && b->len >= COMPACT_BATCH_BYTES) {
                /* rewriting existing keys never inserts, so the table
                 * layout we are iterating stays intact */
                rc = append_frame(s, b->buf, b->len, b->count, 1);
                fio_batch_clear(b);
            }
        }
    }
    if (rc == FIO_OK && b->count) {
        rc = append_frame(s, b->buf, b->len, b->count, 1);
        fio_batch_clear(b);
    }
    free(mem);
    return rc;
}

static int compact_locked(fio_store *s) {
    if (s->nsegs < 2) return FIO_OK;
    s->compact_pending = 0;

    size_t nsealed = s->nsegs - 1, nv = 0;
    uint32_t *victims = (uint32_t *)malloc(nsealed * sizeof(uint32_t));
    if (!victims) return FIO_ENOMEM;
    uint32_t min_remaining = active(s)->id;
    for (size_t i = 0; i < nsealed; i++) {
        fio_segment *g = &s->segs[i];
        double garbage = g->size ? 1.0 - (double)g->live / (double)g->size : 1.0;
        if (garbage >= s->opt.compaction_threshold) victims[nv++] = g->id;
        else if (g->id < min_remaining) min_remaining = g->id;
    }
    if (nv == 0) { free(victims); return FIO_OK; }

    s->compacting = 1;
    fio_batch b = {0};
    int rc = FIO_OK;
    for (size_t i = 0; i < nv && rc == FIO_OK; i++)
        rc = compact_one(s, victims[i], min_remaining, &b);
    free(b.buf);
    /* rewritten records must be persistent before the victims disappear */
    if (rc == FIO_OK) rc = sync_locked(s, FIO_DURABILITY_BARRIER);
    s->compacting = 0;
    if (rc != FIO_OK) { free(victims); return poison(s, rc); }

    char *path = (char *)malloc(path_cap(s));
    if (!path) { free(victims); return FIO_ENOMEM; }
    for (size_t i = 0; i < nv; i++) { /* ascending id order */
        fio_segment *g = find_seg(s, victims[i]);
        if (!g) continue;
        seg_path(s, g->id, path, path_cap(s));
        uint64_t sz = g->size;
        close(g->fd);
        size_t idx = (size_t)(g - s->segs);
        memmove(&s->segs[idx], &s->segs[idx + 1], (s->nsegs - idx - 1) * sizeof(fio_segment));
        s->nsegs--;
        if (unlink(path) != 0 && errno != ENOENT) { rc = poison(s, -errno); break; }
        /* Order the unlinks: an older victim must never outlive a newer one
         * whose dropped tombstones were shadowing it. */
        rc = fio_fsync_dir(s->dir, FIO_DURABILITY_BARRIER);
        if (rc != FIO_OK) { poison(s, rc); break; }
        s->st.reclaimed_bytes += sz;
    }
    free(path);
    free(victims);
    if (rc == FIO_OK) s->st.compactions++;
    return rc;
}

/* ================================================================== */
/* open / recovery                                                    */
/* ================================================================== */
void fio_options_init(fio_options *o) {
    if (!o) return;
    o->segment_size = 8u << 20;
    o->write_buffer_size = 64u << 10;
    o->compaction_threshold = 0.5;
    o->auto_compact = 1;
    o->preallocate = 0;
    o->close_durability = FIO_DURABILITY_BARRIER;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static int parse_seg_name(const char *name, uint32_t *id) {
    if (strlen(name) != 14 || strcmp(name + 10, ".seg") != 0) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 10; i++) {
        if (name[i] < '0' || name[i] > '9') return 0;
        v = v * 10 + (uint64_t)(name[i] - '0');
    }
    if (v == 0 || v > UINT32_MAX) return 0;
    *id = (uint32_t)v;
    return 1;
}

static int load_segment(fio_store *s, uint32_t id) {
    char *path = (char *)malloc(path_cap(s));
    if (!path) return FIO_ENOMEM;
    seg_path(s, id, path, path_cap(s));
    int fd = open(path, O_RDWR | O_CLOEXEC);
    free(path);
    if (fd < 0) return -errno;

    struct stat stbuf;
    if (fstat(fd, &stbuf) != 0) { int e = -errno; close(fd); return e; }
    uint64_t size = (uint64_t)stbuf.st_size;
    int rc = push_seg(s, id, fd, 0);
    if (rc != FIO_OK) { close(fd); return rc; }
    fio_segment *g = active(s);

    uint8_t *mem = (uint8_t *)malloc(size ? size : 1);
    if (!mem) return FIO_ENOMEM;
    if (size) (void)fio_read_advise(fd, 0, size);
    rc = fio_pread_all(fd, mem, size, 0);
    if (rc != FIO_OK) { free(mem); return rc; }

    uint64_t off = 0;
    while (size - off >= FRAME_HDR) {
        const uint8_t *f = mem + off;
        if (get_u32(f) != FRAME_MAGIC) break;
        uint64_t plen = get_u32(f + 8);
        uint32_t count = get_u32(f + 12);
        if (size - off - FRAME_HDR < plen) break;                /* torn */
        if (fio_crc32c(0, f + 8, 8 + plen) != get_u32(f + 4)) break; /* torn */
        if (validate_payload(f + FRAME_HDR, plen, count) != FIO_OK) break;
        rc = apply_ops(s, g, off, f + FRAME_HDR, plen);
        if (rc != FIO_OK) { free(mem); return rc; }
        off += FRAME_HDR + plen;
    }
    free(mem);
    if (off < size) {
        if (ftruncate(fd, (off_t)off) != 0) return -errno;
        s->st.recovered_truncated_bytes += size - off;
    }
    g->size = off;
    return FIO_OK;
}

static void free_store(fio_store *s) {
    if (!s) return;
    for (size_t i = 0; i < s->nbuckets; i++) {
        fio_entry *e = s->buckets[i];
        while (e) { fio_entry *n = e->next; free(e); e = n; }
    }
    free(s->buckets);
    for (size_t i = 0; i < s->nsegs; i++) close(s->segs[i].fd);
    free(s->segs);
    free(s->wbuf);
    if (s->lockfd >= 0) { flock(s->lockfd, LOCK_UN); close(s->lockfd); }
    free(s->dir);
    pthread_rwlock_destroy(&s->lock);
    free(s);
}

int fio_store_open(const char *dir, const fio_options *opt, fio_store **out) {
    if (!dir || !out) return FIO_EINVAL;
    *out = NULL;
    fio_store *s = (fio_store *)calloc(1, sizeof(fio_store));
    if (!s) return FIO_ENOMEM;
    s->lockfd = -1;
    if (pthread_rwlock_init(&s->lock, NULL) != 0) { free(s); return FIO_ENOMEM; }

    if (opt) s->opt = *opt; else fio_options_init(&s->opt);
    if (s->opt.segment_size < (64u << 10)) s->opt.segment_size = 64u << 10;
    if (s->opt.write_buffer_size < 4096) s->opt.write_buffer_size = 4096;
    if (!(s->opt.compaction_threshold > 0.0)) s->opt.compaction_threshold = 0.5;
    if (s->opt.compaction_threshold > 1.0) s->opt.compaction_threshold = 1.0;

    int rc = FIO_OK;
    s->dir = strdup(dir);
    s->nbuckets = 1024;
    s->buckets = (fio_entry **)calloc(s->nbuckets, sizeof(fio_entry *));
    if (!s->dir || !s->buckets) { rc = FIO_ENOMEM; goto fail; }

    if (mkdir(dir, 0755) != 0 && errno != EEXIST) { rc = -errno; goto fail; }

    char *lp = (char *)malloc(path_cap(s));
    if (!lp) { rc = FIO_ENOMEM; goto fail; }
    snprintf(lp, path_cap(s), "%s/LOCK", dir);
    s->lockfd = open(lp, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    free(lp);
    if (s->lockfd < 0) { rc = -errno; goto fail; }
    if (flock(s->lockfd, LOCK_EX | LOCK_NB) != 0) {
        rc = (errno == EWOULDBLOCK) ? FIO_ELOCKED : -errno;
        close(s->lockfd);
        s->lockfd = -1;
        goto fail;
    }

    DIR *d = opendir(dir);
    if (!d) { rc = -errno; goto fail; }
    uint32_t *ids = NULL;
    size_t nids = 0, capids = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        uint32_t id;
        if (!parse_seg_name(de->d_name, &id)) continue;
        if (nids == capids) {
            size_t nc = capids ? capids * 2 : 16;
            uint32_t *ni = (uint32_t *)realloc(ids, nc * sizeof(uint32_t));
            if (!ni) { rc = FIO_ENOMEM; break; }
            ids = ni;
            capids = nc;
        }
        ids[nids++] = id;
    }
    closedir(d);
    if (rc != FIO_OK) { free(ids); goto fail; }
    if (nids) qsort(ids, nids, sizeof(uint32_t), cmp_u32);
    for (size_t i = 0; i < nids && rc == FIO_OK; i++) rc = load_segment(s, ids[i]);
    free(ids);
    if (rc != FIO_OK) goto fail;

    if (s->nsegs == 0) {
        rc = create_segment(s, 1);
        if (rc != FIO_OK) goto fail;
    }
    s->flushed = active(s)->size;
    *out = s;
    return FIO_OK;

fail:
    free_store(s);
    return rc;
}

void fio_store_close(fio_store *s) {
    if (!s) return;
    pthread_rwlock_wrlock(&s->lock);
    if (s->err == FIO_OK) (void)sync_locked(s, s->opt.close_durability);
    pthread_rwlock_unlock(&s->lock);
    free_store(s);
}

/* ================================================================== */
/* public API                                                         */
/* ================================================================== */
static int write_batch_locked(fio_store *s, const fio_batch *b, fio_durability d) {
    if (s->err != FIO_OK) return FIO_EPOISONED;
    int rc = FIO_OK;
    if (b && b->count) {
        rc = append_frame(s, b->buf, b->len, b->count, 0);
        if (rc == FIO_OK) s->st.user_bytes += b->user_bytes;
    }
    /* NONE leaves the record in the coalescing buffer; it reaches the file
     * when the buffer fills, on fio_store_sync() or on close. */
    if (rc == FIO_OK && d != FIO_DURABILITY_NONE) rc = sync_locked(s, d);
    if (rc == FIO_OK && s->opt.auto_compact && s->compact_pending)
        rc = compact_locked(s);
    return rc;
}

int fio_store_write(fio_store *s, const fio_batch *b, fio_durability d) {
    if (!s) return FIO_EINVAL;
    pthread_rwlock_wrlock(&s->lock);
    int rc = write_batch_locked(s, b, d);
    pthread_rwlock_unlock(&s->lock);
    return rc;
}

int fio_store_put(fio_store *s, const void *key, size_t klen,
                  const void *val, size_t vlen, fio_durability d) {
    if (!s) return FIO_EINVAL;
    fio_batch b = {0};
    int rc = fio_batch_put(&b, key, klen, val, vlen);
    if (rc == FIO_OK) rc = fio_store_write(s, &b, d);
    free(b.buf);
    return rc;
}

int fio_store_delete(fio_store *s, const void *key, size_t klen, fio_durability d) {
    if (!s || !key || klen == 0 || klen > FIO_MAX_KEY_SIZE) return FIO_EINVAL;
    pthread_rwlock_wrlock(&s->lock);
    int rc;
    fio_entry *e = lookup(s, key, klen, hash_key(key, klen));
    if (!e || e->tomb) {
        /* nothing to delete: write nothing (saves flash), still honour d */
        rc = s->err != FIO_OK ? FIO_EPOISONED
             : d != FIO_DURABILITY_NONE ? sync_locked(s, d) : FIO_OK;
    } else {
        fio_batch b = {0};
        rc = fio_batch_delete(&b, key, klen);
        if (rc == FIO_OK) rc = write_batch_locked(s, &b, d);
        free(b.buf);
    }
    pthread_rwlock_unlock(&s->lock);
    return rc;
}

int fio_store_get(fio_store *s, const void *key, size_t klen,
                  void **val, size_t *vlen) {
    if (!s || !key || !klen || !val || !vlen) return FIO_EINVAL;
    *val = NULL;
    *vlen = 0;
    pthread_rwlock_rdlock(&s->lock);
    int rc = FIO_OK;
    fio_entry *e = lookup(s, key, klen, hash_key(key, klen));
    if (s->err != FIO_OK) rc = FIO_EPOISONED;
    else if (!e || e->tomb) rc = FIO_ENOTFOUND;
    else {
        fio_segment *g = find_seg(s, e->seg);
        uint8_t *out = (uint8_t *)malloc(e->vlen ? e->vlen : 1);
        if (!g) rc = FIO_ECORRUPT;
        else if (!out) rc = FIO_ENOMEM;
        else if (e->vlen == 0)
            ; /* empty value: nothing to read */
        else if (g == active(s) && e->voff >= s->flushed)
            memcpy(out, s->wbuf + (e->voff - s->flushed), e->vlen);
        else
            rc = fio_pread_all(g->fd, out, e->vlen, e->voff);
        if (rc == FIO_OK) { *val = out; *vlen = e->vlen; }
        else free(out);
    }
    pthread_rwlock_unlock(&s->lock);
    return rc;
}

int fio_store_sync(fio_store *s, fio_durability d) {
    if (!s) return FIO_EINVAL;
    pthread_rwlock_wrlock(&s->lock);
    int rc = s->err != FIO_OK ? FIO_EPOISONED : sync_locked(s, d);
    pthread_rwlock_unlock(&s->lock);
    return rc;
}

int fio_store_compact(fio_store *s) {
    if (!s) return FIO_EINVAL;
    pthread_rwlock_wrlock(&s->lock);
    int rc = s->err != FIO_OK ? FIO_EPOISONED : compact_locked(s);
    pthread_rwlock_unlock(&s->lock);
    return rc;
}

int fio_store_stats(fio_store *s, fio_stats *out) {
    if (!s || !out) return FIO_EINVAL;
    pthread_rwlock_rdlock(&s->lock);
    *out = s->st;
    out->keys = s->nkeys;
    out->segments = s->nsegs;
    out->live_bytes = 0;
    out->total_bytes = 0;
    for (size_t i = 0; i < s->nsegs; i++) {
        out->live_bytes += s->segs[i].live;
        out->total_bytes += s->segs[i].size;
    }
    pthread_rwlock_unlock(&s->lock);
    return FIO_OK;
}

int fio_store_foreach_key(fio_store *s, fio_key_fn fn, void *ctx) {
    if (!s || !fn) return FIO_EINVAL;
    pthread_rwlock_rdlock(&s->lock);
    int stop = 0;
    for (size_t i = 0; i < s->nbuckets && !stop; i++)
        for (fio_entry *e = s->buckets[i]; e && !stop; e = e->next)
            if (!e->tomb) stop = fn(e->key, e->klen, ctx);
    pthread_rwlock_unlock(&s->lock);
    return FIO_OK;
}
