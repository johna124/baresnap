#include "tt_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

extern int tt_store_init(const char *);
extern int tt_store_reader_init(void);
extern int tt_store_reader_next(TtDeltaHeader *, char *, size_t,
                                uint8_t **, size_t *);
extern void tt_store_reader_free(void);

static const char *evname(uint8_t e) {
    switch (e) {
        case 1: return "MODIFY";
        case 2: return "CREATE";
        case 3: return "DELETE";
        case 4: return "CLOSE_WRITE";
        default: return "UNKNOWN";
    }
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "uso: %s <dir_timetravel>\n", argv[0]); return 1; }
    if (tt_store_init(argv[1]) != 0) { perror("tt_store_init"); return 1; }
    if (tt_store_reader_init() != 0) { fprintf(stderr, "reader_init fail\n"); return 1; }

    TtDeltaHeader h;
    char path[4096];
    uint8_t *pl = NULL;
    size_t plsz = 0;
    int rc;
    while ((rc = tt_store_reader_next(&h, path, sizeof path, &pl, &plsz)) == 1) {
        time_t sec = (time_t)(h.timestamp_ns / 1000000000ULL);
        struct tm tm;
        localtime_r(&sec, &tm);
        char tsbuf[32];
        strftime(tsbuf, sizeof tsbuf, "%Y-%m-%d %H:%M:%S", &tm);
        printf("[%s.%03llu] %-12s size=%8llu delta=%6zu  %s\n",
               tsbuf,
               (unsigned long long)((h.timestamp_ns / 1000000ULL) % 1000),
               evname(h.event_type),
               (unsigned long long)h.file_size,
               plsz,
               path);
        free(pl);
        pl = NULL;
    }
    tt_store_reader_free();
    return 0;
}