#ifndef TT_TYPES_H
#define TT_TYPES_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#define TT_PATH_MAX          4096
#define TT_DEBOUNCE_MS       200
#define TT_TICK_MS           50
#define TT_RESCAN_MS         10000
#define TT_COMPACT_HOUR      3
#define TT_COMPACT_THRESHOLD 15
#define TT_MAX_PENDING       4096
#define TT_MAX_CAPTURE_SIZE  (64ULL * 1024ULL * 1024ULL)

typedef enum {
    TT_EV_CREATE = 1, TT_EV_MODIFY = 2, TT_EV_DELETE = 3, TT_EV_RENAME = 4
} TtEventType;

typedef struct __attribute__((packed)) {
    uint64_t timestamp_ns;
    uint8_t  event_type;
    uint32_t path_len;
    uint32_t delta_size;
    uint64_t file_size;
} TtDeltaHeader;

typedef struct {
    char     path[TT_PATH_MAX];
    uint64_t last_event_ns;
    uint8_t  event_type;
    int      pending_close;
    int      active;
} TtPendingEntry;

typedef struct {
    TtPendingEntry entries[TT_MAX_PENDING];
    size_t count;
} TtDebounce;

typedef struct { int wd; char path[TT_PATH_MAX]; } TtWatchEntry;

typedef struct {
    TtWatchEntry *entries;
    size_t count, cap;
} TtWatchMap;

typedef struct {
    char    path[TT_PATH_MAX];
    uint8_t *data;
    size_t  size;
    int     active;
    int     anchored;          /* el último estado del almacén == caché */
    int     baseline_pending;  /* sin historial: el 1er cambio guarda el original */
} TtStateEntry;

typedef struct {
    TtStateEntry *entries;
    size_t count, cap, active_count;
} TtStateCache;

typedef struct {
    char     watch_dir[TT_PATH_MAX];
    char     store_dir[TT_PATH_MAX];
    int      inotify_fd;
    int      timer_fd;
    int      root_wd;
    int      running;
    int      root_lost;
    int      rescan_needed;
    uint64_t rescan_accum_ms;
    uint64_t deltas_written;
    uint64_t bytes_stored;
    TtWatchMap   wmap;
    TtDebounce   debounce;
    TtStateCache cache;
} TtDaemon;

typedef void (*TtProcessCb)(TtDaemon *d, const char *full_path, uint8_t event_type, void *user);

static inline uint64_t tt_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#endif
