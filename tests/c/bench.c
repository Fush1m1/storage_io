/* Rough comparison of common "save small records" patterns vs FlashIO.
 * Numbers on a dev box are only indicative; run on a real device for iOS. */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "flashio.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { N = 3000, VLEN = 200, GROUP = 100 };

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(void) {
    char root[256];
    snprintf(root, sizeof root, "%s/flashio-bench-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    mkdir(root, 0755);
    char val[VLEN];
    memset(val, 'v', sizeof val);
    char path[512];
    double t;

    /* 1. one file per record, atomically replaced, synced each time */
    snprintf(path, sizeof path, "%s/files", root);
    mkdir(path, 0755);
    t = now();
    for (int i = 0; i < N; i++) {
        char p[600];
        snprintf(p, sizeof p, "%s/%d.json", path, i % 300);
        fio_atomic_write_file(p, val, sizeof val, FIO_DURABILITY_BARRIER);
    }
    printf("file-per-record + sync each : %8.1f ms  (%d syncs)\n", (now() - t) * 1e3, 2 * N);

    /* 2. FlashIO, durable every write */
    fio_options o;
    fio_options_init(&o);
    fio_store *s;
    snprintf(path, sizeof path, "%s/store1", root);
    fio_store_open(path, &o, &s);
    t = now();
    for (int i = 0; i < N; i++) {
        char k[32];
        snprintf(k, sizeof k, "rec-%d", i % 300);
        fio_store_put(s, k, strlen(k), val, sizeof val, FIO_DURABILITY_BARRIER);
    }
    fio_stats st;
    fio_store_stats(s, &st);
    printf("FlashIO put + BARRIER each  : %8.1f ms  (%llu syncs, %llu pwrites)\n", (now() - t) * 1e3,
           (unsigned long long)st.barrier_syncs, (unsigned long long)st.write_calls);
    fio_store_close(s);

    /* 3. FlashIO, group commit every GROUP writes */
    snprintf(path, sizeof path, "%s/store2", root);
    fio_store_open(path, &o, &s);
    t = now();
    for (int i = 0; i < N; i++) {
        char k[32];
        snprintf(k, sizeof k, "rec-%d", i % 300);
        fio_store_put(s, k, strlen(k), val, sizeof val, FIO_DURABILITY_NONE);
        if ((i + 1) % GROUP == 0) fio_store_sync(s, FIO_DURABILITY_BARRIER);
    }
    fio_store_stats(s, &st);
    printf("FlashIO put + BARRIER /%-4d : %8.1f ms  (%llu syncs, %llu pwrites, %llu bytes)\n", GROUP,
           (now() - t) * 1e3, (unsigned long long)st.barrier_syncs,
           (unsigned long long)st.write_calls, (unsigned long long)st.physical_bytes);
    printf("  user bytes %llu -> log bytes %llu (overhead x%.2f)\n",
           (unsigned long long)st.user_bytes, (unsigned long long)st.log_bytes,
           (double)st.log_bytes / (double)st.user_bytes);
    fio_store_close(s);
    printf("(leftover files under %s; remove when done)\n", root);
    return 0;
}
