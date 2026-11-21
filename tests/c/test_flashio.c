#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
/* Unit + model-based tests for the FlashIO C core. Runs on Linux/macOS. */
#include "flashio.h"

#include <dirent.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); failures++; return; } } while (0)
#define CHECK_RC(x) do { int _rc = (x); if (_rc != FIO_OK) { fprintf(stderr, "%s:%d: %s -> %d (%s)\n", __FILE__, __LINE__, #x, _rc, fio_strerror(_rc)); failures++; return; } } while (0)

static char root[512];

static void rm_rf(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    char p[1024];
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        unlink(p);
    }
    closedir(d);
    rmdir(dir);
}

static void fresh_dir(char *out, size_t cap, const char *name) {
    snprintf(out, cap, "%s/%s", root, name);
    rm_rf(out);
}

static int get_str(fio_store *s, const char *k, char *buf, size_t cap) {
    void *v; size_t n;
    int rc = fio_store_get(s, k, strlen(k), &v, &n);
    if (rc != FIO_OK) return rc;
    if (n >= cap) n = cap - 1;
    memcpy(buf, v, n);
    buf[n] = 0;
    fio_free(v);
    return FIO_OK;
}

static int put_str(fio_store *s, const char *k, const char *v, fio_durability d) {
    return fio_store_put(s, k, strlen(k), v, strlen(v), d);
}

/* ------------------------------------------------------------------ */
static void test_crc(void) {
    CHECK(fio_crc32c(0, "123456789", 9) == 0xE3069283u);
    uint32_t a = fio_crc32c(0, "1234", 4);
    CHECK(fio_crc32c(a, "56789", 5) == 0xE3069283u);
    CHECK(fio_crc32c(0, "", 0) == 0);
}

static void test_basic(void) {
    char dir[600]; fresh_dir(dir, sizeof dir, "basic");
    fio_store *s;
    CHECK_RC(fio_store_open(dir, NULL, &s));
    char buf[64];
    CHECK(get_str(s, "a", buf, sizeof buf) == FIO_ENOTFOUND);
    CHECK_RC(put_str(s, "a", "1", FIO_DURABILITY_NONE));
    CHECK_RC(put_str(s, "b", "2", FIO_DURABILITY_NONE));
    CHECK_RC(get_str(s, "a", buf, sizeof buf)); CHECK(!strcmp(buf, "1"));
    CHECK_RC(put_str(s, "a", "one", FIO_DURABILITY_BARRIER));
    CHECK_RC(get_str(s, "a", buf, sizeof buf)); CHECK(!strcmp(buf, "one"));
    CHECK_RC(fio_store_delete(s, "b", 1, FIO_DURABILITY_NONE));
    CHECK(get_str(s, "b", buf, sizeof buf) == FIO_ENOTFOUND);
    CHECK_RC(fio_store_put(s, "empty", 5, NULL, 0, FIO_DURABILITY_NONE));
    void *v; size_t n;
    CHECK_RC(fio_store_get(s, "empty", 5, &v, &n)); CHECK(n == 0); fio_free(v);
    CHECK(fio_store_put(s, "", 0, "x", 1, FIO_DURABILITY_NONE) == FIO_EINVAL);

    fio_store *s2;
    CHECK(fio_store_open(dir, NULL, &s2) == FIO_ELOCKED);

    fio_stats st; fio_store_stats(s, &st);
    CHECK(st.keys == 2);
    fio_store_close(s);

    CHECK_RC(fio_store_open(dir, NULL, &s));
    CHECK_RC(get_str(s, "a", buf, sizeof buf)); CHECK(!strcmp(buf, "one"));
    CHECK(get_str(s, "b", buf, sizeof buf) == FIO_ENOTFOUND);
    CHECK_RC(fio_store_get(s, "empty", 5, &v, &n)); CHECK(n == 0); fio_free(v);
    fio_store_stats(s, &st);
    CHECK(st.keys == 2);
    fio_store_close(s);
}

static void test_batch_atomic_and_torn_tail(void) {
    char dir[600]; fresh_dir(dir, sizeof dir, "torn");
    fio_store *s;
    CHECK_RC(fio_store_open(dir, NULL, &s));
    CHECK_RC(put_str(s, "keep", "yes", FIO_DURABILITY_BARRIER));
    fio_batch *b = fio_batch_new();
    for (int i = 0; i < 100; i++) {
        char k[16]; snprintf(k, sizeof k, "b%d", i);
        CHECK_RC(fio_batch_put(b, k, strlen(k), "v", 1));
    }
    CHECK_RC(fio_batch_delete(b, "keep", 4));
    CHECK_RC(fio_store_write(s, b, FIO_DURABILITY_BARRIER));
    fio_batch_free(b);
    fio_store_close(s);

    /* Simulate a torn write: chop 7 bytes off the end of the last frame. */
    char seg[700]; snprintf(seg, sizeof seg, "%s/0000000001.seg", dir);
    struct stat stbuf; CHECK(stat(seg, &stbuf) == 0);
    CHECK(truncate(seg, stbuf.st_size - 7) == 0);

    CHECK_RC(fio_store_open(dir, NULL, &s));
    char buf[16];
    /* the whole batch is gone (atomic), including its delete of "keep" */
    CHECK_RC(get_str(s, "keep", buf, sizeof buf)); CHECK(!strcmp(buf, "yes"));
    CHECK(get_str(s, "b0", buf, sizeof buf) == FIO_ENOTFOUND);
    CHECK(get_str(s, "b99", buf, sizeof buf) == FIO_ENOTFOUND);
    fio_stats st; fio_store_stats(s, &st);
    CHECK(st.recovered_truncated_bytes > 0);
    CHECK(st.keys == 1);
    /* the store keeps working after the repair */
    CHECK_RC(put_str(s, "after", "ok", FIO_DURABILITY_NONE));
    fio_store_close(s);
    CHECK_RC(fio_store_open(dir, NULL, &s));
    CHECK_RC(get_str(s, "after", buf, sizeof buf)); CHECK(!strcmp(buf, "ok"));
    fio_store_close(s);

    /* garbage appended after valid frames is also dropped */
    int fd = open(seg, O_WRONLY | O_APPEND);
    CHECK(fd >= 0);
    CHECK(write(fd, "FIO1garbagegarbagegarbage", 25) == 25);
    close(fd);
    CHECK_RC(fio_store_open(dir, NULL, &s));
    CHECK_RC(get_str(s, "after", buf, sizeof buf));
    fio_store_close(s);
}

static void test_compaction_and_tombstones(void) {
    char dir[600]; fresh_dir(dir, sizeof dir, "compact");
    fio_options o; fio_options_init(&o);
    o.segment_size = 64 << 10;
    o.auto_compact = 0;
    fio_store *s;
    CHECK_RC(fio_store_open(dir, &o, &s));

    char val[1000]; memset(val, 'x', sizeof val);
    CHECK_RC(put_str(s, "ghost", "boo", FIO_DURABILITY_NONE));
    /* fill several segments, overwriting the same small key space */
    for (int round = 0; round < 10; round++)
        for (int i = 0; i < 50; i++) {
            char k[16]; snprintf(k, sizeof k, "k%d", i);
            CHECK_RC(fio_store_put(s, k, strlen(k), val, sizeof val, FIO_DURABILITY_NONE));
        }
    CHECK_RC(fio_store_delete(s, "ghost", 5, FIO_DURABILITY_NONE));
    for (int i = 0; i < 60; i++) {
        char k[16]; snprintf(k, sizeof k, "k%d", i % 50);
        CHECK_RC(fio_store_put(s, k, strlen(k), val, sizeof val, FIO_DURABILITY_NONE));
    }
    fio_stats before; fio_store_stats(s, &before);
    CHECK(before.segments > 3);
    CHECK_RC(fio_store_compact(s));
    fio_stats after; fio_store_stats(s, &after);
    CHECK(after.compactions == 1);
    CHECK(after.total_bytes < before.total_bytes);
    CHECK(after.keys == 50);
    char buf[8];
    CHECK(get_str(s, "ghost", buf, sizeof buf) == FIO_ENOTFOUND);
    /* compact again until stable, then reopen: ghost must stay dead */
    CHECK_RC(fio_store_compact(s));
    fio_store_close(s);
    CHECK_RC(fio_store_open(dir, &o, &s));
    CHECK(get_str(s, "ghost", buf, sizeof buf) == FIO_ENOTFOUND);
    void *v; size_t n;
    CHECK_RC(fio_store_get(s, "k7", 2, &v, &n));
    CHECK(n == sizeof val && memcmp(v, val, n) == 0);
    fio_free(v);
    fio_store_stats(s, &after);
    CHECK(after.keys == 50);
    fio_store_close(s);
}

static void test_atomic_file_and_clone(void) {
    char dir[600]; fresh_dir(dir, sizeof dir, "files");
    mkdir(dir, 0755);
    char p[700], q[700];
    snprintf(p, sizeof p, "%s/state.json", dir);
    snprintf(q, sizeof q, "%s/copy.json", dir);
    CHECK_RC(fio_atomic_write_file(p, "{\"v\":1}", 7, FIO_DURABILITY_BARRIER));
    CHECK_RC(fio_atomic_write_file(p, "{\"v\":22}", 8, FIO_DURABILITY_FULL));
    CHECK_RC(fio_clone_file(p, q));
    char buf[32] = {0};
    int fd = open(q, O_RDONLY);
    CHECK(fd >= 0);
    CHECK(read(fd, buf, sizeof buf) == 8);
    close(fd);
    CHECK(!strcmp(buf, "{\"v\":22}"));
    /* no temp files left behind */
    DIR *d = opendir(dir); struct dirent *de; int n = 0;
    while ((de = readdir(d))) if (de->d_name[0] != '.') n++;
    closedir(d);
    CHECK(n == 2);
}

/* ------------------------------------------------------------------ */
/* Model-based randomized test                                        */
/* ------------------------------------------------------------------ */
#define NKEYS 300
typedef struct { uint8_t *v; size_t n; int present; } model_t;
static model_t model[NKEYS];

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }

static void model_set(int k, const uint8_t *v, size_t n) {
    free(model[k].v);
    model[k].v = malloc(n ? n : 1);
    memcpy(model[k].v, v, n);
    model[k].n = n;
    model[k].present = 1;
}

static void verify_all(fio_store *s, int *ok) {
    uint64_t live = 0;
    for (int k = 0; k < NKEYS; k++) {
        char key[16]; snprintf(key, sizeof key, "key-%d", k);
        void *v; size_t n;
        int rc = fio_store_get(s, key, strlen(key), &v, &n);
        if (model[k].present) {
            live++;
            if (rc != FIO_OK || n != model[k].n || memcmp(v, model[k].v, n) != 0) {
                fprintf(stderr, "mismatch on %s rc=%d\n", key, rc); *ok = 0;
            }
            if (rc == FIO_OK) fio_free(v);
        } else if (rc != FIO_ENOTFOUND) {
            fprintf(stderr, "%s should be absent rc=%d\n", key, rc); *ok = 0;
            if (rc == FIO_OK) fio_free(v);
        }
    }
    fio_stats st; fio_store_stats(s, &st);
    if (st.keys != live) { fprintf(stderr, "keys %llu != %llu\n", (unsigned long long)st.keys, (unsigned long long)live); *ok = 0; }
}

static void test_model(void) {
    char dir[600]; fresh_dir(dir, sizeof dir, "model");
    fio_options o; fio_options_init(&o);
    o.segment_size = 128 << 10;
    o.write_buffer_size = 16 << 10;
    o.compaction_threshold = 0.4;
    fio_store *s;
    CHECK_RC(fio_store_open(dir, &o, &s));
    uint8_t *val = malloc(70000);
    int ok = 1;

    for (int step = 0; step < 20000 && ok; step++) {
        uint32_t r = rnd() % 1000;
        int k = (int)(rnd() % NKEYS);
        char key[16]; snprintf(key, sizeof key, "key-%d", k);
        if (r < 550) {
            size_t n = (rnd() % 20 == 0) ? rnd() % 70000 : rnd() % 600;
            for (size_t i = 0; i < n; i++) val[i] = (uint8_t)(rnd());
            CHECK_RC(fio_store_put(s, key, strlen(key), val, n, (fio_durability)(rnd() % 3 == 0)));
            model_set(k, val, n);
        } else if (r < 750) {
            CHECK_RC(fio_store_delete(s, key, strlen(key), FIO_DURABILITY_NONE));
            model[k].present = 0;
        } else if (r < 850) {
            fio_batch *b = fio_batch_new();
            int cnt = 1 + rnd() % 20;
            for (int j = 0; j < cnt; j++) {
                int kk = (int)(rnd() % NKEYS);
                char kb[16]; snprintf(kb, sizeof kb, "key-%d", kk);
                if (rnd() % 4 == 0) { CHECK_RC(fio_batch_delete(b, kb, strlen(kb))); model[kk].present = 0; }
                else {
                    size_t n = rnd() % 300;
                    for (size_t i = 0; i < n; i++) val[i] = (uint8_t)rnd();
                    CHECK_RC(fio_batch_put(b, kb, strlen(kb), val, n));
                    model_set(kk, val, n);
                }
            }
            CHECK_RC(fio_store_write(s, b, FIO_DURABILITY_NONE));
            fio_batch_free(b);
        } else if (r < 990) {
            void *v; size_t n;
            int rc = fio_store_get(s, key, strlen(key), &v, &n);
            if (model[k].present) {
                CHECK(rc == FIO_OK && n == model[k].n && memcmp(v, model[k].v, n) == 0);
                fio_free(v);
            } else CHECK(rc == FIO_ENOTFOUND);
        } else if (r < 995) {
            CHECK_RC(fio_store_compact(s));
        } else {
            fio_store_close(s);
            CHECK_RC(fio_store_open(dir, &o, &s));
            verify_all(s, &ok);
        }
    }
    verify_all(s, &ok);
    CHECK_RC(fio_store_compact(s));
    fio_store_close(s);
    CHECK_RC(fio_store_open(dir, &o, &s));
    verify_all(s, &ok);
    fio_stats st; fio_store_stats(s, &st);
    printf("  model: keys=%llu segs=%llu live=%llu total=%llu compactions(session)=%llu\n",
           (unsigned long long)st.keys, (unsigned long long)st.segments,
           (unsigned long long)st.live_bytes, (unsigned long long)st.total_bytes,
           (unsigned long long)st.compactions);
    fio_store_close(s);
    free(val);
    CHECK(ok);
}


/* ------------------------------------------------------------------ */
typedef struct { fio_store *s; int id; int ok; } worker_t;

static void *worker(void *arg) {
    worker_t *w = (worker_t *)arg;
    char k[32], v[64];
    for (int i = 0; i < 3000; i++) {
        snprintf(k, sizeof k, "t%d-%d", w->id, i % 50);
        int n = snprintf(v, sizeof v, "val-%d-%d", w->id, i);
        if (fio_store_put(w->s, k, strlen(k), v, (size_t)n, FIO_DURABILITY_NONE) != FIO_OK) w->ok = 0;
        void *out; size_t on;
        if (fio_store_get(w->s, k, strlen(k), &out, &on) != FIO_OK) { w->ok = 0; continue; }
        if (on != (size_t)n || memcmp(out, v, on) != 0) w->ok = 0;
        fio_free(out);
        if (i % 500 == 0 && w->id == 0) fio_store_compact(w->s);
    }
    return NULL;
}

static void test_concurrency(void) {
    char dir[600]; fresh_dir(dir, sizeof dir, "conc");
    fio_options o; fio_options_init(&o);
    o.segment_size = 64 << 10;
    fio_store *s;
    CHECK_RC(fio_store_open(dir, &o, &s));
    pthread_t th[4]; worker_t w[4];
    for (int i = 0; i < 4; i++) { w[i] = (worker_t){s, i, 1}; pthread_create(&th[i], NULL, worker, &w[i]); }
    for (int i = 0; i < 4; i++) { pthread_join(th[i], NULL); CHECK(w[i].ok); }
    fio_stats st; fio_store_stats(s, &st);
    CHECK(st.keys == 200);
    fio_store_close(s);
}

int main(void) {
    const char *t = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/flashio-test-%d", t ? t : "/tmp", (int)getpid());
    mkdir(root, 0755);

    struct { const char *name; void (*fn)(void); } tests[] = {
        {"crc32c", test_crc},
        {"basic", test_basic},
        {"batch_atomic_and_torn_tail", test_batch_atomic_and_torn_tail},
        {"compaction_and_tombstones", test_compaction_and_tombstones},
        {"atomic_file_and_clone", test_atomic_file_and_clone},
        {"model", test_model},
        {"concurrency", test_concurrency},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int before = failures;
        tests[i].fn();
        printf("[%s] %s\n", failures == before ? " OK " : "FAIL", tests[i].name);
    }
    const char *dirs[] = {"basic", "torn", "compact", "files", "model", "conc"};
    for (size_t i = 0; i < 6; i++) { char d[600]; snprintf(d, sizeof d, "%s/%s", root, dirs[i]); rm_rf(d); }
    rmdir(root);
    printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
