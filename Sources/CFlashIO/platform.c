/* Platform layer: Darwin-specific flash friendly syscalls with portable
 * fallbacks (Linux is used for CI / unit tests). */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "flashio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/clonefile.h>
#endif

#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

const char *fio_strerror(int code) {
    switch (code) {
    case FIO_OK:        return "ok";
    case FIO_ENOTFOUND: return "key not found";
    case FIO_ECORRUPT:  return "corrupt or truncated data";
    case FIO_ELOCKED:   return "store is locked by another opener";
    case FIO_EINVAL:    return "invalid argument";
    case FIO_ENOMEM:    return "out of memory";
    case FIO_EPOISONED: return "store disabled after an earlier I/O failure";
    default:
        if (code < 0 && code > -1000) return strerror(-code);
        return "unknown error";
    }
}

void fio_free(void *p) { free(p); }

/* ------------------------------------------------------------------ */
/* CRC32C (Castagnoli). ARMv8 CRC instructions on Apple silicon / iOS, */
/* table driven otherwise.                                             */
/* ------------------------------------------------------------------ */
#if defined(__ARM_FEATURE_CRC32)
uint32_t fio_crc32c(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (len && ((uintptr_t)p & 7)) { crc = __crc32cb(crc, *p++); len--; }
    while (len >= 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        crc = __crc32cd(crc, v);
        p += 8; len -= 8;
    }
    while (len--) crc = __crc32cb(crc, *p++);
    return ~crc;
}
#else
#include <pthread.h>
static uint32_t crc_table[8][256];
static pthread_once_t crc_once = PTHREAD_ONCE_INIT;

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
        crc_table[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++)
        for (int t = 1; t < 8; t++)
            crc_table[t][i] = (crc_table[t - 1][i] >> 8) ^ crc_table[0][crc_table[t - 1][i] & 0xFF];
}

uint32_t fio_crc32c(uint32_t crc, const void *data, size_t len) {
    pthread_once(&crc_once, crc_init);
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (len >= 8) {
        uint32_t lo = crc ^ ((uint32_t)p[0] | (uint32_t)p[1] << 8 |
                             (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
        crc = crc_table[7][lo & 0xFF] ^ crc_table[6][(lo >> 8) & 0xFF] ^
              crc_table[5][(lo >> 16) & 0xFF] ^ crc_table[4][lo >> 24] ^
              crc_table[3][p[4]] ^ crc_table[2][p[5]] ^
              crc_table[1][p[6]] ^ crc_table[0][p[7]];
        p += 8; len -= 8;
    }
    while (len--) crc = (crc >> 8) ^ crc_table[0][(crc ^ *p++) & 0xFF];
    return ~crc;
}
#endif

/* ------------------------------------------------------------------ */
/* sync                                                                */
/* ------------------------------------------------------------------ */
int fio_fsync(int fd, fio_durability d) {
    switch (d) {
    case FIO_DURABILITY_NONE:
        return FIO_OK;
    case FIO_DURABILITY_BARRIER:
#if defined(__APPLE__)
#  if defined(F_BARRIERFSYNC)
        if (fcntl(fd, F_BARRIERFSYNC) == 0) return FIO_OK;
#  endif
        /* No barrier support on this FS: plain fsync() on Darwin does not
         * order writes, so keep the promise with a full flush. */
        if (fcntl(fd, F_FULLFSYNC) == 0) return FIO_OK;
        return fsync(fd) == 0 ? FIO_OK : -errno;
#else
        return fdatasync(fd) == 0 ? FIO_OK : -errno;
#endif
    case FIO_DURABILITY_FULL:
#if defined(__APPLE__)
        if (fcntl(fd, F_FULLFSYNC) == 0) return FIO_OK;
#endif
        return fsync(fd) == 0 ? FIO_OK : -errno;
    }
    return FIO_EINVAL;
}

int fio_fsync_dir(const char *dir_path, fio_durability d) {
    if (d == FIO_DURABILITY_NONE) return FIO_OK;
    int fd = open(dir_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    int rc = fio_fsync(fd, d);
    if (rc != FIO_OK) {
        /* Some filesystems reject syncing directories; fsync is the
         * portable minimum. */
        rc = fsync(fd) == 0 ? FIO_OK : -errno;
        if (rc == -EINVAL) rc = FIO_OK;
    }
    close(fd);
    return rc;
}

/* ------------------------------------------------------------------ */
/* positional I/O                                                      */
/* ------------------------------------------------------------------ */
int fio_pwrite_all(int fd, const void *buf, size_t len, uint64_t off) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len) {
        ssize_t n = pwrite(fd, p, len, (off_t)off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        p += n; len -= (size_t)n; off += (uint64_t)n;
    }
    return FIO_OK;
}

int fio_pread_all(int fd, void *buf, size_t len, uint64_t off) {
    uint8_t *p = (uint8_t *)buf;
    while (len) {
        ssize_t n = pread(fd, p, len, (off_t)off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (n == 0) return FIO_ECORRUPT;
        p += n; len -= (size_t)n; off += (uint64_t)n;
    }
    return FIO_OK;
}

/* ------------------------------------------------------------------ */
/* hints                                                               */
/* ------------------------------------------------------------------ */
int fio_preallocate(int fd, uint64_t len) {
#if defined(__APPLE__)
    fstore_t st;
    memset(&st, 0, sizeof st);
    st.fst_flags = F_ALLOCATECONTIG | F_ALLOCATEALL;
    st.fst_posmode = F_PEOFPOSMODE;
    st.fst_offset = 0;
    st.fst_length = (off_t)len;
    if (fcntl(fd, F_PREALLOCATE, &st) == 0) return FIO_OK;
    st.fst_flags = F_ALLOCATEALL;
    return fcntl(fd, F_PREALLOCATE, &st) == 0 ? FIO_OK : -errno;
#elif defined(__linux__)
    if (fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, (off_t)len) == 0) return FIO_OK;
    return -errno;
#else
    (void)fd; (void)len;
    return FIO_OK;
#endif
}

int fio_set_nocache(int fd, int enable) {
#if defined(__APPLE__)
    return fcntl(fd, F_NOCACHE, enable ? 1 : 0) == 0 ? FIO_OK : -errno;
#elif defined(POSIX_FADV_DONTNEED)
    if (!enable) return FIO_OK;
    int rc = posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    return rc == 0 ? FIO_OK : -rc;
#else
    (void)fd; (void)enable;
    return FIO_OK;
#endif
}

int fio_read_advise(int fd, uint64_t off, uint64_t len) {
#if defined(__APPLE__)
    struct radvisory ra;
    ra.ra_offset = (off_t)off;
    ra.ra_count = len > INT32_MAX ? INT32_MAX : (int)len;
    return fcntl(fd, F_RDADVISE, &ra) == 0 ? FIO_OK : -errno;
#elif defined(POSIX_FADV_WILLNEED)
    int rc = posix_fadvise(fd, (off_t)off, (off_t)len, POSIX_FADV_WILLNEED);
    return rc == 0 ? FIO_OK : -rc;
#else
    (void)fd; (void)off; (void)len;
    return FIO_OK;
#endif
}

/* ------------------------------------------------------------------ */
/* whole-file helpers                                                  */
/* ------------------------------------------------------------------ */
static char *dir_of(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return strdup(".");
    if (slash == path) return strdup("/");
    size_t n = (size_t)(slash - path);
    char *d = (char *)malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, path, n);
    d[n] = 0;
    return d;
}

int fio_atomic_write_file(const char *path, const void *data, size_t len,
                          fio_durability d) {
    static unsigned counter;
    if (!path || (!data && len)) return FIO_EINVAL;
    size_t cap = strlen(path) + 64;
    char *tmp = (char *)malloc(cap);
    if (!tmp) return FIO_ENOMEM;
    snprintf(tmp, cap, "%s.fio-tmp.%ld.%u", path, (long)getpid(),
             __atomic_fetch_add(&counter, 1, __ATOMIC_RELAXED));

    int rc;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) { rc = -errno; free(tmp); return rc; }

    rc = fio_pwrite_all(fd, data, len, 0);
    /* The sync below orders the file contents before the rename: after a
     * crash the reader sees either the old file or the complete new one. */
    if (rc == FIO_OK) rc = fio_fsync(fd, d);
    if (close(fd) != 0 && rc == FIO_OK) rc = -errno;
    if (rc == FIO_OK && rename(tmp, path) != 0) rc = -errno;
    if (rc != FIO_OK) {
        unlink(tmp);
        free(tmp);
        return rc;
    }
    free(tmp);

    char *dir = dir_of(path);
    if (!dir) return FIO_ENOMEM;
    rc = fio_fsync_dir(dir, d);
    free(dir);
    return rc;
}

static int copy_file(const char *src, const char *dst) {
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) return -errno;
    struct stat stbuf;
    if (fstat(in, &stbuf) != 0) { int e = -errno; close(in); return e; }
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, stbuf.st_mode & 0777);
    if (out < 0) { int e = -errno; close(in); return e; }

    enum { CHUNK = 1 << 20 };
    uint8_t *buf = (uint8_t *)malloc(CHUNK);
    int rc = buf ? FIO_OK : FIO_ENOMEM;
    uint64_t off = 0;
    while (rc == FIO_OK) {
        ssize_t n = pread(in, buf, CHUNK, (off_t)off);
        if (n < 0) { if (errno == EINTR) continue; rc = -errno; break; }
        if (n == 0) break;
        rc = fio_pwrite_all(out, buf, (size_t)n, off);
        off += (uint64_t)n;
    }
    free(buf);
    close(in);
    if (close(out) != 0 && rc == FIO_OK) rc = -errno;
    if (rc != FIO_OK) unlink(dst);
    return rc;
}

int fio_clone_file(const char *src, const char *dst) {
    if (!src || !dst) return FIO_EINVAL;
#if defined(__APPLE__)
    if (clonefile(src, dst, 0) == 0) return FIO_OK;
    if (errno != ENOTSUP && errno != EXDEV) return -errno;
#endif
    return copy_file(src, dst);
}
