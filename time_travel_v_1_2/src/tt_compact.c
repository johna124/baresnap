#include "tt_types.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern int tt_store_reader_init(void);
extern int tt_store_reader_next(TtDeltaHeader *h, char *p, size_t ps, uint8_t **pl, size_t *plsz);
extern void tt_store_reader_free(void);
extern int tt_store_write(const TtDeltaHeader *h, const char *path, const uint8_t *payload);
extern void tt_store_free(void);
extern int tt_store_init(const char *store_dir);
extern int tt_delta_encode(const uint8_t *old_data, size_t old_size, const uint8_t *new_data, size_t new_size, uint8_t **delta_out, size_t *delta_size_out);
extern int tt_delta_decode(const uint8_t *old_data, size_t old_size, const uint8_t *delta_data, size_t delta_size, size_t expected_new_size, uint8_t **new_out, size_t *new_size_out);

typedef struct { TtDeltaHeader hdr; char path[TT_PATH_MAX]; uint8_t *payload; size_t payload_size; size_t seq; } CompactRecord;
typedef struct { char path[TT_PATH_MAX]; CompactRecord *records; size_t count, cap; } CompactGroup;
typedef struct { CompactGroup *groups; size_t count, cap; } CompactSet;

static void compact_record_destroy(CompactRecord *r) {
    free(r->payload); r->payload = NULL; r->payload_size = 0;
}

static void compact_set_destroy(CompactSet *cs) {
    if (!cs) return;
    for (size_t i = 0; i < cs->count; ++i) {
        CompactGroup *g = &cs->groups[i];
        for (size_t j = 0; j < g->count; ++j) compact_record_destroy(&g->records[j]);
        free(g->records);
    }
    free(cs->groups);
    memset(cs, 0, sizeof *cs);
}

static CompactGroup *compact_set_touch(CompactSet *cs, const char *path) {
    for (size_t i = 0; i < cs->count; ++i)
        if (strcmp(cs->groups[i].path, path) == 0) return &cs->groups[i];
    if (cs->count == cs->cap) {
        size_t ncap = cs->cap ? cs->cap * 2 : 16;
        CompactGroup *ng = realloc(cs->groups, ncap * sizeof(CompactGroup));
        if (!ng) return NULL;
        cs->groups = ng; cs->cap = ncap;
    }
    CompactGroup *g = &cs->groups[cs->count++];
    memset(g, 0, sizeof *g);
    snprintf(g->path, sizeof g->path, "%s", path);
    return g;
}

static int compact_read_all(CompactSet *cs) {
    memset(cs, 0, sizeof *cs);
    if (tt_store_reader_init() != 0) return -1;
    size_t seq = 0;
    for (;;) {
        TtDeltaHeader hdr; char path[TT_PATH_MAX]; uint8_t *pl = NULL; size_t plsz = 0;
        int rc = tt_store_reader_next(&hdr, path, sizeof path, &pl, &plsz);
        if (rc == 0) break;
        if (rc < 0) { tt_store_reader_free(); compact_set_destroy(cs); return -1; }
        CompactGroup *g = compact_set_touch(cs, path);
        if (!g) { free(pl); tt_store_reader_free(); compact_set_destroy(cs); return -1; }
        if (g->count == g->cap) {
            size_t ncap = g->cap ? g->cap * 2 : 8;
            CompactRecord *nr = realloc(g->records, ncap * sizeof(CompactRecord));
            if (!nr) { free(pl); tt_store_reader_free(); compact_set_destroy(cs); return -1; }
            g->records = nr; g->cap = ncap;
        }
        CompactRecord *r = &g->records[g->count++];
        r->hdr = hdr;
        snprintf(r->path, sizeof r->path, "%s", path);
        r->payload = pl; r->payload_size = plsz; r->seq = seq++;
    }
    tt_store_reader_free();
    return 0;
}

/* Reconstruye el estado final de la cadena [start..end]; la base debe ser CREATE. */
static int collapse_replay(CompactRecord *recs, size_t start, size_t end,
                           uint8_t **out_state, size_t *out_size) {
    if (recs[start].hdr.event_type != TT_EV_CREATE) return -1;
    size_t bsz = recs[start].payload_size;
    uint8_t *state = malloc(bsz ? bsz : 1);
    if (!state) return -1;
    if (bsz && recs[start].payload) memcpy(state, recs[start].payload, bsz);
    size_t state_size = bsz;
    for (size_t i = start + 1; i <= end; ++i) {
        CompactRecord *r = &recs[i];
        if (r->hdr.event_type == TT_EV_DELETE) { free(state); return -1; }
        if (r->hdr.event_type == TT_EV_CREATE) {
            free(state);
            size_t s = r->payload_size;
            state = malloc(s ? s : 1);
            if (!state) return -1;
            if (s && r->payload) memcpy(state, r->payload, s);
            state_size = s;
            continue;
        }
        /* MODIFY */
        if (r->hdr.delta_size == 0) {
            free(state); state = malloc(1);
            if (!state) return -1;
            state_size = 0;
            continue;
        }
        if (!r->payload) { free(state); return -1; }
        uint8_t *ns = NULL; size_t nss = 0;
        if (tt_delta_decode(state, state_size, r->payload, r->payload_size,
                            r->hdr.file_size, &ns, &nss) != 0) { free(state); return -1; }
        free(state); state = ns; state_size = nss;
    }
    *out_state = state; *out_size = state_size;
    return 0;
}

static int compact_flat_cmp(const void *a, const void *b) {
    const CompactRecord *ra = a, *rb = b;
    if (ra->hdr.timestamp_ns < rb->hdr.timestamp_ns) return -1;
    if (ra->hdr.timestamp_ns > rb->hdr.timestamp_ns) return 1;
    if (ra->seq < rb->seq) return -1;
    if (ra->seq > rb->seq) return 1;
    return 0;
}

int tt_compact_run(TtDaemon *d) {
    if (!d || !d->store_dir[0]) return -1;
    CompactSet cs;
    if (compact_read_all(&cs) != 0) return -1;
    if (cs.count == 0) { compact_set_destroy(&cs); return 0; }

    int collapsed = 0;
    for (size_t gi = 0; gi < cs.count; ++gi) {
        CompactGroup *g = &cs.groups[gi];
        if (g->count <= (size_t)TT_COMPACT_THRESHOLD) continue;

        CompactRecord *nr = calloc(g->count, sizeof(CompactRecord));
        if (!nr) continue;
        size_t n = 0, run_start = 0;

        for (size_t i = 0; i <= g->count; ++i) {
            int is_del = (i < g->count && g->records[i].hdr.event_type == TT_EV_DELETE);
            int is_end = (i == g->count);
            if (!is_del && !is_end) continue;

            size_t run_len = i - run_start;
            int did = 0;
            if (run_len > (size_t)TT_COMPACT_THRESHOLD &&
                g->records[run_start].hdr.event_type == TT_EV_CREATE) {
                uint8_t *state = NULL; size_t state_size = 0;
                if (collapse_replay(g->records, run_start, i - 1, &state, &state_size) == 0) {
                    CompactRecord *base = &g->records[run_start];
                    uint8_t *super = NULL; size_t super_size = 0;
                    int full_copy = 1;
                    if (state_size > 0 && base->payload && base->payload_size > 0 &&
                        tt_delta_encode(base->payload, base->payload_size, state, state_size,
                                        &super, &super_size) == 0 &&
                        super_size > 0 && super_size < state_size) {
                        full_copy = 0;
                    } else { free(super); super = NULL; }

                    nr[n++] = *base;                     /* la base se mueve */
                    CompactRecord *sr = &nr[n++];
                    memset(sr, 0, sizeof *sr);
                    sr->hdr = g->records[i - 1].hdr;     /* timestamp del último */
                    sr->hdr.path_len = (uint32_t)strlen(g->path);
                    sr->hdr.file_size = (uint64_t)state_size;   /* FIX: tamaño REAL */
                    snprintf(sr->path, sizeof sr->path, "%s", g->path);
                    sr->seq = g->records[i - 1].seq;
                    if (full_copy) {
                        sr->hdr.event_type = TT_EV_CREATE;      /* FIX: tipo correcto */
                        sr->payload = state; state = NULL;
                        sr->payload_size = state_size;
                        sr->hdr.delta_size = (uint32_t)state_size;
                    } else {
                        sr->hdr.event_type = TT_EV_MODIFY;
                        sr->payload = super; super = NULL;
                        sr->payload_size = super_size;
                        sr->hdr.delta_size = (uint32_t)super_size;
                    }
                    for (size_t k = run_start + 1; k < i; ++k)
                        compact_record_destroy(&g->records[k]);
                    free(state); free(super);
                    did = 1; collapsed++;
                }
            }
            if (!did)
                for (size_t k = run_start; k < i; ++k) nr[n++] = g->records[k];
            if (is_del) nr[n++] = g->records[i];
            run_start = i + 1;
        }
        free(g->records);
        g->records = nr; g->count = n; g->cap = n;
    }

    if (collapsed == 0) { compact_set_destroy(&cs); return 0; }

    /* Reescritura ordenada por timestamp */
    size_t total = 0;
    for (size_t gi = 0; gi < cs.count; ++gi) total += cs.groups[gi].count;
    CompactRecord *flat = malloc((total ? total : 1) * sizeof(CompactRecord));
    if (!flat) { compact_set_destroy(&cs); return -1; }
    size_t f = 0;
    for (size_t gi = 0; gi < cs.count; ++gi) {
        CompactGroup *g = &cs.groups[gi];
        for (size_t i = 0; i < g->count; ++i) flat[f++] = g->records[i];
        free(g->records); g->records = NULL; g->count = g->cap = 0;
    }
    free(cs.groups); cs.groups = NULL; cs.count = cs.cap = 0;
    if (total > 1) qsort(flat, total, sizeof(CompactRecord), compact_flat_cmp);

    DIR *dir = opendir(d->store_dir);
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            size_t ln = strlen(de->d_name);
            if (ln > 4 && strcmp(de->d_name + ln - 4, ".ttd") == 0) {
                char p[TT_PATH_MAX + 64];
                snprintf(p, sizeof p, "%s/%s", d->store_dir, de->d_name);
                unlink(p);
            }
        }
        closedir(dir);
    }
    tt_store_free();
    if (tt_store_init(d->store_dir) != 0) {
        for (size_t i = 0; i < total; ++i) compact_record_destroy(&flat[i]);
        free(flat);
        return -1;
    }
    int werr = 0;
    for (size_t i = 0; i < total; ++i)
        if (tt_store_write(&flat[i].hdr, flat[i].path, flat[i].payload) != 0) { werr = 1; break; }
    for (size_t i = 0; i < total; ++i) compact_record_destroy(&flat[i]);
    free(flat);
    return werr ? -1 : collapsed;
}
