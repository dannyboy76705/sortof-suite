/*
 * sortof-minhash - file list in similarity order (MinHash), part of the sortof-suite
 *
 * Scans the sources, computes a MinHash digest per file (128 functions over 8-byte
 * shingles), orders greedily so similar files end up adjacent, then refines with
 * 2-opt sweeps. Prints the ordered list (stdout or -f). Pipe it into an archiver:
 *
 *   $ sortof-minhash dir | tar -T- --no-recursion -cf - | zstd -9 > out.tar.zst
 *
 * MinHash replaced nilsimsa after a head-to-head on a 12.6 GB DOS collection:
 * 1.17% smaller under zstd -9, 1.26% under xz. Shuffling the input order alone
 * moves a result by up to ~0.25%. The order depends on the input order (the greedy
 * pass starts at the first file): feed a sorted list, e.g.
 *   sortof-natural dir | sortof-minhash -
 *
 * Build: see the Makefile (gcc -O2 -pthread, links sortof-common.c).
 *
 * a sort, sort of.)
 *
 * MIT License
 *
 * Copyright (c) 2026 Daniel Lee Witzel
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "sortof-common.h"
#include <sched.h>
#if defined(__AVX2__)
#include <immintrin.h>   /* only when built with -mavx2 / -march=native */
#elif defined(__SSE2__)
#include <emmintrin.h>   /* baseline on x86-64: no -march flag needed */
#endif

#include "sortof-sketch.h"

/* ---------------------------------------------------------------- */
/* The digest interface                                              */
/* ---------------------------------------------------------------- */

typedef minhash_t hasher_t;
#define hasher_init(h)           minhash_init(h)
#define hasher_update(h, d, n)   minhash_update((h), (d), (n))
#define hasher_digest(h, out)    minhash_digest((h), (out))
#define digest_compare(d1, d2)   minhash_compare((d1), (d2))

/* fnv1a64_update and FNV_* come from sortof-common.h */
static int hash_file(const char *path, unsigned char *digest_out,
                      uint64_t *dup_hash_out, uint64_t *dup_len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    hasher_t mh;
    hasher_init(&mh);
    uint64_t fh = FNV_OFFSET_64;
    uint64_t total = 0;
    unsigned char buf[1 << 16];
    size_t r;
    size_t carry = 0;
    while ((r = fread(buf + carry, 1, sizeof(buf) - carry, f)) > 0) {
        if (id_mode) {
            size_t have = carry + r, use = have & ~(size_t)3;
            uint32_t tmp[(1 << 14)];
            memcpy(tmp, buf, use);
            minhash_update_ids(&mh, tmp, use / 4);
            carry = have - use;
            memmove(buf, buf + use, carry);
            fh = fnv1a64_update(fh, (const unsigned char *)tmp, use);
            total += r;
            continue;
        }
        hasher_update(&mh, buf, r);
        fh = fnv1a64_update(fh, buf, r);
        total += r;
    }
    fclose(f);
    hasher_digest(&mh, digest_out);
    *dup_hash_out = fh;
    *dup_len_out = total;
    return 0;
}

/* ---------------------------------------------------------------- */
/* EBML/Matroska-aware hashing (-m)                                 */
/*                                                                   */
/* Matroska/WebM (.mkv/.webm) files carry EBML container framing --*/
/* EBML header, Segment, SeekHead, Info, Tracks, Cues, etc -- ahead */
/* of and around the actual codec payload. For small per-frame or   */
/* per-GOP files, that framing can be a large enough fraction of    */
/* total bytes (and near-identical across files from the same       */
/* encoding pipeline) that it distorts similarity scoring:           */
/* files end up looking more alike than their actual video content  */
/* warrants. This walks just enough EBML structure to find Cluster  */
/* elements (the actual frame/GOP payload) and hashes only those,   */
/* skipping every other element on purpose.                         */
/* ---------------------------------------------------------------- */

#define EBML_ID_HEADER  0x1A45DFA3u
#define EBML_ID_SEGMENT 0x18538067u
#define EBML_ID_CLUSTER 0x1F43B675u

/* Reads an EBML element ID at buf[*pos] (kept with its length-marker
 * bits, matching how IDs like 0x1A45DFA3 are conventionally written).
 * Advances *pos past it. Returns 0 on success, -1 if invalid/short. */
static int ebml_read_id(const unsigned char *buf, size_t len, size_t *pos,
                         uint32_t *id_out) {
    if (*pos >= len) return -1;
    unsigned char b0 = buf[*pos];
    int idlen;
    if (b0 & 0x80) idlen = 1;
    else if (b0 & 0x40) idlen = 2;
    else if (b0 & 0x20) idlen = 3;
    else if (b0 & 0x10) idlen = 4;
    else return -1; /* IDs longer than 4 bytes aren't used in practice */
    if (*pos + (size_t)idlen > len) return -1;
    uint32_t id = 0;
    for (int i = 0; i < idlen; i++) id = (id << 8) | buf[*pos + i];
    *pos += (size_t)idlen;
    *id_out = id;
    return 0;
}

/* Reads an EBML vint size field at buf[*pos] (length-marker bits
 * stripped from the returned value, unlike element IDs). Sets
 * *unknown if this is an EBML "unknown size" marker (all value bits
 * set to 1, legal for streamed/unfinalized elements); callers here
 * treat that as "runs to the end of the enclosing element", which is
 * a reasonable simplification for finalized single-cluster files but
 * not a fully general EBML reader. */
static int ebml_read_size(const unsigned char *buf, size_t len, size_t *pos,
                           uint64_t *size_out, int *unknown) {
    if (*pos >= len) return -1;
    unsigned char b0 = buf[*pos];
    int sizelen;
    unsigned char mask;
    if (b0 & 0x80) { sizelen = 1; mask = 0x7F; }
    else if (b0 & 0x40) { sizelen = 2; mask = 0x3F; }
    else if (b0 & 0x20) { sizelen = 3; mask = 0x1F; }
    else if (b0 & 0x10) { sizelen = 4; mask = 0x0F; }
    else if (b0 & 0x08) { sizelen = 5; mask = 0x07; }
    else if (b0 & 0x04) { sizelen = 6; mask = 0x03; }
    else if (b0 & 0x02) { sizelen = 7; mask = 0x01; }
    else if (b0 & 0x01) { sizelen = 8; mask = 0x00; }
    else return -1;
    if (*pos + (size_t)sizelen > len) return -1;
    uint64_t val = (uint64_t)(b0 & mask);
    for (int i = 1; i < sizelen; i++) val = (val << 8) | buf[*pos + i];
    *pos += (size_t)sizelen;
    uint64_t all_ones = ((uint64_t)1 << (7 * sizelen)) - 1;
    *unknown = (val == all_ones);
    *size_out = val;
    return 0;
}

/* Finds top-level Cluster elements (direct children of Segment) and
 * appends each one's (start,end) payload byte range to the starts and
 * ends output arrays (realloc'd as needed; caller frees both). Returns 1 if this looked
 * like EBML and at least one Cluster was found, 0 otherwise (caller
 * should fall back to hashing the raw bytes). This is algorithm-
 * agnostic on purpose -- it only finds the byte ranges to hash, so
 * the caller can feed them into MinHash (or anything else)
 * without duplicating the EBML-walking logic per algorithm. */
static int ebml_find_clusters(const unsigned char *buf, size_t len,
                               size_t **starts, size_t **ends, size_t *count) {
    *starts = NULL; *ends = NULL; *count = 0;
    if (len < 4 || buf[0] != 0x1A || buf[1] != 0x45 ||
        buf[2] != 0xDF || buf[3] != 0xA3) {
        return 0; /* not EBML */
    }

    size_t pos = 0;
    {
        uint32_t id; uint64_t size; int unknown;
        if (ebml_read_id(buf, len, &pos, &id) != 0 || id != EBML_ID_HEADER) return 0;
        if (ebml_read_size(buf, len, &pos, &size, &unknown) != 0) return 0;
        pos += unknown ? (len - pos) : (size_t)size; /* skip EBML header */
    }

    uint32_t seg_id; uint64_t seg_size; int seg_unknown;
    if (pos >= len) return 0;
    if (ebml_read_id(buf, len, &pos, &seg_id) != 0 || seg_id != EBML_ID_SEGMENT) return 0;
    if (ebml_read_size(buf, len, &pos, &seg_size, &seg_unknown) != 0) return 0;
    size_t seg_end = seg_unknown ? len : pos + (size_t)seg_size;
    if (seg_end > len) seg_end = len;

    size_t cap = 0;
    while (pos < seg_end) {
        uint32_t child_id; uint64_t child_size; int child_unknown;
        size_t elem_start = pos;
        if (ebml_read_id(buf, len, &pos, &child_id) != 0) break;
        if (ebml_read_size(buf, len, &pos, &child_size, &child_unknown) != 0) break;
        size_t payload_start = pos;
        size_t payload_end = child_unknown ? seg_end : pos + (size_t)child_size;
        if (payload_end > seg_end) payload_end = seg_end;

        if (child_id == EBML_ID_CLUSTER) {
            if (*count == cap) {
                cap = cap ? cap * 2 : 4;
                *starts = realloc(*starts, cap * sizeof(size_t));
                *ends = realloc(*ends, cap * sizeof(size_t));
            }
            (*starts)[*count] = payload_start;
            (*ends)[*count] = payload_end;
            (*count)++;
        }
        /* SeekHead, Info, Tracks, Cues, Attachments, Chapters, Tags,
           and anything else are skipped on purpose. */

        pos = payload_end;
        if (pos <= elem_start) break; /* guard against zero/bad progress */
    }
    return *count > 0;
}

/* EBML-aware variant of hash_file: falls back to the plain streaming
 * hasher for anything that isn't EBML (checked by magic bytes), so
 * non-Matroska files are completely unaffected by -m. For files that
 * are EBML, this buffers the whole file in memory to walk its
 * structure -- fine for the small single-frame/single-GOP files this
 * is meant for, but be aware it will fully buffer whatever you point
 * it at, so it isn't meant for very large multi-GOP MKV files. */
static int hash_file_ebml_aware(const char *path, unsigned char *digest_out,
                                 uint64_t *dup_hash_out, uint64_t *dup_len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    unsigned char magic[4];
    size_t got = fread(magic, 1, 4, f);
    int is_ebml = (got == 4 && magic[0] == 0x1A && magic[1] == 0x45 &&
                   magic[2] == 0xDF && magic[3] == 0xA3);
    if (!is_ebml) {
        fclose(f);
        return hash_file(path, digest_out, dup_hash_out, dup_len_out);
    }

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return hash_file(path, digest_out, dup_hash_out, dup_len_out); }
    long fsize = ftell(f);
    if (fsize < 0) { fclose(f); return hash_file(path, digest_out, dup_hash_out, dup_len_out); }
    rewind(f);

    unsigned char *buf = malloc((size_t)fsize);
    if (!buf) { fclose(f); return hash_file(path, digest_out, dup_hash_out, dup_len_out); }
    size_t rd = fread(buf, 1, (size_t)fsize, f);
    fclose(f);

    hasher_t mh;
    hasher_init(&mh);
    uint64_t fh = FNV_OFFSET_64;
    uint64_t total = 0;

    size_t *starts, *ends, range_count;
    if (ebml_find_clusters(buf, rd, &starts, &ends, &range_count)) {
        for (size_t i = 0; i < range_count; i++) {
            size_t s = starts[i], e = ends[i];
            hasher_update(&mh, buf + s, e - s);
            fh = fnv1a64_update(fh, buf + s, e - s);
            total += (e - s);
        }
    } else {
        /* Structure wasn't what we expected -- hash the raw buffered
           bytes instead of producing nothing. */
        hasher_update(&mh, buf, rd);
        fh = fnv1a64_update(fh, buf, rd);
        total = rd;
    }
    free(starts);
    free(ends);
    free(buf);
    hasher_digest(&mh, digest_out);
    *dup_hash_out = fh;
    *dup_len_out = total;
    return 0;
}

typedef struct {
    char **paths;
    unsigned char *digests;
    size_t digest_bytes;
    uint64_t *dup_hash;
    uint64_t *dup_len;
    size_t start, end;
    int quiet;
    int mkv_aware;
    atomic_size_t *done_counter;
} hash_job_t;

static void *hash_worker(void *arg) {
    hash_job_t *j = (hash_job_t *)arg;
    for (size_t i = j->start; i < j->end; i++) {
        unsigned char *slot = j->digests + i * j->digest_bytes;
        int rc = j->mkv_aware
            ? hash_file_ebml_aware(j->paths[i], slot, &j->dup_hash[i], &j->dup_len[i])
            : hash_file(j->paths[i], slot, &j->dup_hash[i], &j->dup_len[i]);
        if (rc != 0) {
            if (!j->quiet) {
                fprintf(stderr, "\nsortof: warning: cannot read %s: %s\n",
                        j->paths[i], strerror(errno));
            }
            memset(slot, 0, j->digest_bytes);
            j->dup_hash[i] = 0;
            j->dup_len[i] = 0;
        }
        atomic_fetch_add(j->done_counter, 1);
    }
    return NULL;
}

static void hash_all(char **paths, size_t n, unsigned char *digests, size_t digest_bytes,
                      uint64_t *dup_hash, uint64_t *dup_len,
                      int nthreads, int quiet, int mkv_aware) {
    if (n == 0) return;
    if (nthreads < 1) nthreads = 1;
    if ((size_t)nthreads > n) nthreads = (int)n;

    atomic_size_t done_counter = 0;
    pthread_t *th = malloc(sizeof(pthread_t) * (size_t)nthreads);
    hash_job_t *jobs = malloc(sizeof(hash_job_t) * (size_t)nthreads);
    size_t chunk = (n + (size_t)nthreads - 1) / (size_t)nthreads;
    int launched = 0;

    for (int t = 0; t < nthreads; t++) {
        size_t start = (size_t)t * chunk;
        if (start >= n) break;
        size_t end = start + chunk;
        if (end > n) end = n;
        jobs[t].paths = paths;
        jobs[t].digests = digests;
        jobs[t].digest_bytes = digest_bytes;
        jobs[t].dup_hash = dup_hash;
        jobs[t].dup_len = dup_len;
        jobs[t].start = start;
        jobs[t].end = end;
        jobs[t].quiet = quiet;
        jobs[t].mkv_aware = mkv_aware;
        jobs[t].done_counter = &done_counter;
        pthread_create(&th[t], NULL, hash_worker, &jobs[t]);
        launched++;
    }

    if (!quiet) {
        size_t seen;
        do {
            struct timespec nap = {0, 50 * 1000 * 1000}; /* 50ms */
            nanosleep(&nap, NULL);
            seen = atomic_load(&done_counter);
            progress_bar("hashing", seen, n);
        } while (seen < n);
    }

    for (int t = 0; t < launched; t++) pthread_join(th[t], NULL);
    free(th);
    free(jobs);
}

/* ---------------------------------------------------------------- */
/* Exact duplicate detection (over the same bytes hashed above)     */
/* ---------------------------------------------------------------- */

typedef struct {
    size_t idx;
    uint64_t len;
    uint64_t fh;
} dup_key_t;

static int dup_key_cmp(const void *a, const void *b) {
    const dup_key_t *ka = a, *kb = b;
    if (ka->len != kb->len) return (ka->len < kb->len) ? -1 : 1;
    if (ka->fh != kb->fh) return (ka->fh < kb->fh) ? -1 : 1;
    return 0;
}

/* Groups files by (content length, FNV-1a fingerprint). Prints a
 * one-line summary (respecting quiet) of how many files are exact
 * duplicates of at least one other file, and how many duplicate sets
 * that forms. Does not print individual file paths or group contents
 * -- just the totals the person asked for. */
static void report_duplicates(uint64_t *dup_hash, uint64_t *dup_len,
                               size_t n, int quiet) {
    if (n < 2) {
        if (!quiet) fprintf(stderr, "sortof: 0 duplicate files (0 sets)\n");
        return;
    }
    dup_key_t *keys = malloc(sizeof(dup_key_t) * n);
    for (size_t i = 0; i < n; i++) {
        keys[i].idx = i;
        keys[i].len = dup_len[i];
        keys[i].fh = dup_hash[i];
    }
    qsort(keys, n, sizeof(dup_key_t), dup_key_cmp);

    size_t dup_files = 0, dup_sets = 0;
    size_t i = 0;
    while (i < n) {
        size_t j = i + 1;
        while (j < n && keys[j].len == keys[i].len && keys[j].fh == keys[i].fh) j++;
        size_t group_size = j - i;
        if (group_size > 1) {
            dup_files += group_size;
            dup_sets++;
        }
        i = j;
    }
    free(keys);

    if (!quiet) {
        fprintf(stderr, "sortof: %zu duplicate file%s found in %zu set%s of identical content\n",
                dup_files, dup_files == 1 ? "" : "s",
                dup_sets, dup_sets == 1 ? "" : "s");
    }
}

/* ---------------------------------------------------------------- */
/* A small thread pool for the ordering                             */
/* ---------------------------------------------------------------- */

/* The ordering is a chain of decisions (which file next; which 2-opt move), but each
 * decision is a scan of independent compares. The scans are split across threads and the
 * answers reduced so that the result is exactly what the serial scan finds (see
 * greedy_order and twoopt_pass): the list is identical for any thread count, and -t 1 is
 * the plain serial code.
 *
 * A scan can take only tens of microseconds, less than waking a thread through a
 * condition variable costs, so the workers wait by spinning (yielding now and then, so
 * that more threads than cores still make progress). The pool lives only while one batch
 * is ordered, and only when a scan is big enough to be worth splitting: each thread must
 * get at least SORTOF_PAR_MIN compares of a scan, so small lists stay serial and keep no
 * idle threads spinning. */

#ifndef SORTOF_PAR_MIN
#define SORTOF_PAR_MIN 1024
#endif

/* Test hook, never set in a normal build. Normally a scan gives up work whose result would be
 * thrown away: a thread past a known hit for a step, and the steps above a step that has a hit.
 * On one core the lowest chunk always runs first and the others quit at once, so two threads
 * never both report a hit; with this defined nothing is given up, so every thread reports its
 * own first hit for every step, which makes the choice of the lowest step and position testable
 * anywhere. */
#ifdef SORTOF_NO_EARLY_STOP
#define SORTOF_EARLY_STOP 0
#else
#define SORTOF_EARLY_STOP 1
#endif

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static inline void cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

static inline void spin_wait_step(unsigned *spins) {
    cpu_relax();
    if (++*spins >= 512) {
        *spins = 0;
        sched_yield();
    }
}

typedef void (*pool_fn)(void *ctx, int tid, int nthreads);

typedef struct pool pool_t;
typedef struct { pool_t *p; int tid; } pool_worker_arg;

struct pool {
    int nthreads;                 /* including the calling thread, which is thread 0 */
    pthread_t *th;
    pool_worker_arg *args;
    atomic_ulong gen;             /* bumped to start a job */
    atomic_int done;              /* workers finished with the current job */
    atomic_int quit;
    pool_fn fn;
    void *ctx;
};

static void *pool_worker(void *arg) {
    pool_worker_arg *w = (pool_worker_arg *)arg;
    pool_t *p = w->p;
    unsigned long seen = 0;
    for (;;) {
        unsigned spins = 0;
        unsigned long g;
        while ((g = atomic_load_explicit(&p->gen, memory_order_acquire)) == seen)
            spin_wait_step(&spins);
        seen = g;
        if (atomic_load_explicit(&p->quit, memory_order_acquire)) return NULL;
        p->fn(p->ctx, w->tid, p->nthreads);
        atomic_fetch_add_explicit(&p->done, 1, memory_order_release);
    }
}

/* NULL (run serially) when there is one thread or the list is too small to split. */
static pool_t *pool_start(int nthreads, size_t n) {
    if (nthreads < 2 || n < (size_t)SORTOF_PAR_MIN * (size_t)nthreads) return NULL;
    pool_t *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->th = calloc((size_t)nthreads, sizeof(pthread_t));
    p->args = calloc((size_t)nthreads, sizeof(pool_worker_arg));
    if (!p->th || !p->args) { free(p->th); free(p->args); free(p); return NULL; }
    atomic_init(&p->gen, 0);
    atomic_init(&p->done, 0);
    atomic_init(&p->quit, 0);
    p->nthreads = 1;
    for (int t = 1; t < nthreads; t++) {
        p->args[t].p = p;
        p->args[t].tid = t;
        if (pthread_create(&p->th[t], NULL, pool_worker, &p->args[t]) != 0) break;
        p->nthreads = t + 1;      /* thread ids stay contiguous if a create fails */
    }
    if (p->nthreads < 2) { free(p->th); free(p->args); free(p); return NULL; }
    return p;
}

/* Runs fn(ctx, tid, nthreads) on every thread, the caller being thread 0, and returns when all are done. */
static void pool_run(pool_t *p, pool_fn fn, void *ctx) {
    p->fn = fn;
    p->ctx = ctx;
    atomic_store_explicit(&p->done, 0, memory_order_relaxed);
    atomic_fetch_add_explicit(&p->gen, 1, memory_order_release);
#ifdef SORTOF_MAIN_LAST
    /* Test hook, never set in a normal build: the workers (higher positions) finish before the caller
     * (the lowest positions) starts, which is the order a one-core machine never produces on its own
     * and a multi-core one can at any time. It makes every step's pruning decisions see hits that come
     * from higher chunks. */
    {
        unsigned spins = 0;
        while (atomic_load_explicit(&p->done, memory_order_acquire) < p->nthreads - 1)
            spin_wait_step(&spins);
    }
    fn(ctx, 0, p->nthreads);
#else
    fn(ctx, 0, p->nthreads);
    unsigned spins = 0;
    while (atomic_load_explicit(&p->done, memory_order_acquire) < p->nthreads - 1)
        spin_wait_step(&spins);
#endif
}

static void pool_stop(pool_t *p) {
    if (!p) return;
    atomic_store_explicit(&p->quit, 1, memory_order_release);
    atomic_fetch_add_explicit(&p->gen, 1, memory_order_release);
    for (int t = 1; t < p->nthreads; t++) pthread_join(p->th[t], NULL);
    free(p->th);
    free(p->args);
    free(p);
}

/* ---------------------------------------------------------------- */
/* Greedy similarity ordering (O(n^2) comparisons, O(n) memory)     */
/* ---------------------------------------------------------------- */

static inline const unsigned char *digest_at(const unsigned char *digests,
                                              size_t digest_bytes, size_t i) {
    return digests + i * digest_bytes;
}

/* One thread's best candidate in a greedy scan. Padded so that two threads' results never
 * share a cache line. */
typedef struct { int score; size_t idx; int found; char pad[64]; } greedy_res_t;

typedef struct {
    const unsigned char *digests;
    size_t digest_bytes;
    const unsigned char *dc;      /* the current file's digest */
    const unsigned char *visited;
    size_t n;
    greedy_res_t *res;
} greedy_ctx_t;

/* The first most-similar unvisited file in [lo, hi) (ties go to the lowest index). The scan
 * stops at a perfect score: nothing can beat it, and scanning upward the first one found
 * is the lowest-index one. */
static void greedy_scan(const greedy_ctx_t *c, size_t lo, size_t hi, greedy_res_t *r) {
    const int maxs = MINHASH_NUM_FUNCS;
    r->score = -1000;
    r->idx = 0;
    r->found = 0;
    for (size_t i = lo; i < hi; i++) {
        if (c->visited[i]) continue;
        int s = digest_compare(c->dc, digest_at(c->digests, c->digest_bytes, i));
        if (!r->found || s > r->score) {
            r->score = s;
            r->idx = i;
            r->found = 1;
            if (s == maxs) break;
        }
    }
}

static void greedy_job(void *vc, int tid, int nt) {
    greedy_ctx_t *c = (greedy_ctx_t *)vc;
    size_t chunk = (c->n + (size_t)nt - 1) / (size_t)nt;
    size_t lo = (size_t)tid * chunk, hi = lo + chunk;
    if (lo > c->n) lo = c->n;
    if (hi > c->n) hi = c->n;
    greedy_scan(c, lo, hi, &c->res[tid]);
}

/* Nearest-neighbour construction: from the current file, take the most similar file not yet
 * used (ties go to the lowest index). With a pool the scan is cut into contiguous chunks and
 * the chunks' answers combined lowest chunk first with a strict '>', which is the serial rule. */
static size_t *greedy_order(unsigned char *digests, size_t digest_bytes, size_t n, int quiet,
                             pool_t *pool) {
    if (n == 0) return NULL;
    size_t *order = malloc(sizeof(size_t) * n);
    unsigned char *visited = calloc(n, 1);
    greedy_res_t *res = pool ? calloc((size_t)pool->nthreads, sizeof *res) : NULL;
    if (pool && !res) pool = NULL;           /* out of memory: do it serially */

    order[0] = 0;
    visited[0] = 1;
    size_t cur = 0;

    for (size_t step = 1; step < n; step++) {
        greedy_ctx_t c;
        greedy_res_t best;
        c.digests = digests;
        c.digest_bytes = digest_bytes;
        c.dc = digest_at(digests, digest_bytes, cur);
        c.visited = visited;
        c.n = n;
        c.res = res;
        if (pool && n - step >= (size_t)SORTOF_PAR_MIN * (size_t)pool->nthreads) {
            pool_run(pool, greedy_job, &c);
            best.score = -1000;
            best.idx = 0;
            best.found = 0;
            for (int t = 0; t < pool->nthreads; t++) {
                if (res[t].found && (!best.found || res[t].score > best.score)) {
                    best.score = res[t].score;
                    best.idx = res[t].idx;
                    best.found = 1;
                }
            }
        } else {
            greedy_scan(&c, 0, n, &best);
        }
        order[step] = best.idx;
        visited[best.idx] = 1;
        cur = best.idx;
        if (!quiet) progress_bar("ordering", step, n - 1);
    }
    free(res);
    free(visited);
    return order;
}

#define NO_HIT ((size_t)-1)

/* The 2-opt sweep is cache-blocked. Scanning for one outer step streams every later digest
 * through the cache; on a list too big for the cache (a million files is 512 MB of digests)
 * that is limited by memory bandwidth, not by compares, and extra threads share the same
 * memory channel and gain nothing. So SORTOF_GROUP consecutive outer steps are scanned
 * together, SORTOF_TILE positions at a time: each tile's digests are fetched once and used
 * for the whole group. */
#ifndef SORTOF_GROUP
#define SORTOF_GROUP 8      /* outer steps scanned together; memory traffic falls by about this factor */
#endif
#ifndef SORTOF_TILE
#define SORTOF_TILE 1024    /* positions per tile: 512 B of digest each (256 KB a tile), so it should fit a core's L2 */
#endif

typedef struct { size_t j; int c1, c2; } step_hit_t;
/* one thread's first hit for each step of the group; padded so two threads' rows never share a line */
typedef struct { step_hit_t h[SORTOF_GROUP]; char pad[64]; } opt_row_t;

typedef struct {
    const unsigned char *digests;
    size_t digest_bytes;
    const size_t *order;
    const int *edge;
    size_t i0;                              /* the group's first outer step */
    int g;                                  /* its steps: i0 .. i0+g-1 */
    size_t step_lo[SORTOF_GROUP];           /* the first position step k scans (a step resuming after a move starts later) */
    size_t rlo, jend;                       /* positions any step scans: [rlo, jend) */
    int parallel;
    atomic_size_t first_hit[SORTOF_GROUP];  /* parallel: the lowest position at which step k has a hit so far */
    atomic_int low_step;                    /* parallel: the lowest step with a hit so far (g: none) */
    opt_row_t *res;                         /* one row per thread */
} opt_ctx_t;

static inline void atomic_min_size(atomic_size_t *p, size_t v) {
    size_t cur = atomic_load_explicit(p, memory_order_relaxed);
    while (v < cur && !atomic_compare_exchange_weak_explicit(p, &cur, v, memory_order_relaxed,
                                                             memory_order_relaxed)) {
    }
}

static inline void atomic_min_int(atomic_int *p, int v) {
    int cur = atomic_load_explicit(p, memory_order_relaxed);
    while (v < cur && !atomic_compare_exchange_weak_explicit(p, &cur, v, memory_order_relaxed,
                                                             memory_order_relaxed)) {
    }
}

/* One thread's share of a group: the positions [lo, hi) of the whole range, tile by tile, and
 * for each tile every step of the group. Records, for each step, the first position in this
 * thread's share where reversing order[i+1..j] improves the two boundary edges (the same test
 * as always: the old edges are cached, and a first new edge that cannot reach the old total
 * even with the best second one is skipped without the second compare).
 *
 * Two shortcuts, both only ever skipping work whose result would be thrown away:
 *  - once some thread has a hit for step k at position h, a thread at a tile past h gives up on k;
 *  - once a step has a hit, the steps above it are not scanned further: after that move they
 *    have to be redone from the changed order anyway. */
static void twoopt_group_job(void *vc, int tid, int nt) {
    opt_ctx_t *c = (opt_ctx_t *)vc;
    step_hit_t *r = c->res[tid].h;
    const int g = c->g;
    const int maxs = MINHASH_NUM_FUNCS;
    const unsigned char *qi[SORTOF_GROUP], *qi1[SORTOF_GROUP];
    int ei[SORTOF_GROUP];
    for (int k = 0; k < g; k++) {
        r[k].j = NO_HIT;
        qi[k]  = digest_at(c->digests, c->digest_bytes, c->order[c->i0 + (size_t)k]);
        qi1[k] = digest_at(c->digests, c->digest_bytes, c->order[c->i0 + (size_t)k + 1]);
        ei[k]  = c->edge[c->i0 + (size_t)k];
    }
    size_t len = c->jend - c->rlo, chunk = (len + (size_t)nt - 1) / (size_t)nt;
    size_t lo = c->rlo + (size_t)tid * chunk, hi = lo + chunk;
    if (lo > c->jend) lo = c->jend;
    if (hi > c->jend) hi = c->jend;
    int top = g;                            /* the lowest step known to have a hit: steps above it are skipped */
    for (size_t a = lo; a < hi; a += SORTOF_TILE) {
        size_t b = a + SORTOF_TILE < hi ? a + SORTOF_TILE : hi;
        if (SORTOF_EARLY_STOP && c->parallel) {
            int gl = atomic_load_explicit(&c->low_step, memory_order_relaxed);
            if (gl < top) top = gl;
        }
        for (int k = 0; k < g; k++) {
            if (SORTOF_EARLY_STOP && k > top) break;
            if (r[k].j != NO_HIT) continue;
            if (SORTOF_EARLY_STOP && c->parallel &&
                a > atomic_load_explicit(&c->first_hit[k], memory_order_relaxed))
                continue;
            for (size_t j = a > c->step_lo[k] ? a : c->step_lo[k]; j < b; j++) {
                int old_score = ei[k] + c->edge[j];
                int c1 = digest_compare(qi[k], digest_at(c->digests, c->digest_bytes, c->order[j]));
                if (c1 + maxs <= old_score) continue;   /* cannot win */
                int c2 = digest_compare(qi1[k], digest_at(c->digests, c->digest_bytes, c->order[j + 1]));
                if (c1 + c2 > old_score) {
                    r[k].j = j;
                    r[k].c1 = c1;
                    r[k].c2 = c2;
                    if (k < top) top = k;
                    if (c->parallel) {
                        atomic_min_size(&c->first_hit[k], j);
                        atomic_min_int(&c->low_step, k);
                    }
                    break;
                }
            }
        }
    }
}

/* One full 2-opt sweep over the current order. For every pair of
 * non-adjacent "edges" (order[i],order[i+1]) and (order[j],order[j+1]),
 * check whether reversing the segment between them increases the
 * total similarity of the two boundary edges; if so, apply it
 * immediately (first-improvement strategy) and keep scanning.
 * Returns 1 if any improving move was applied this sweep, else 0.
 *
 * Same decisions as the plain three-compares-per-pair version, with
 * less work: edge[k] caches the score of the pair (order[k],
 * order[k+1]) so the old edges are never recomputed (a reversal turns
 * the cached edges inside the segment around and replaces the two at
 * its ends), and since the second new edge can add at most
 * the best possible score (MINHASH_NUM_FUNCS), a pair whose first new edge cannot reach the old
 * total even with that is skipped without the second compare.
 *
 * The outer steps are taken SORTOF_GROUP at a time (see twoopt_group_job). All of a group's
 * scans see the order as it was when the group started, which is exactly what the serial
 * sweep would have seen as long as no step of the group moves anything. So: if no step
 * finds a move, the whole group is done. If one does, the steps below the lowest such step
 * are done (nothing changed before them), that step makes its move and resumes scanning
 * from the position after it, and the steps above it are scanned again from the changed
 * order, which is what the serial sweep does next. The sequence of moves, and so the list,
 * is exactly the serial one. Most steps move nothing (on 12,000 real files, 93% in a first
 * sweep and over 99% after it), so most groups are one pass.
 *
 * With a pool, a group's range of positions is split across the threads; the lowest step
 * with a hit, and for it the lowest position, are the ones the serial scan would stop at. */
static int twoopt_pass(unsigned char *digests, size_t digest_bytes, size_t *order, size_t n,
                        const char *label, int quiet, pool_t *pool) {
    int improved = 0;
    if (n < 4) return 0;
    int *edge = malloc(n * sizeof(int));
    if (!edge) {
        fprintf(stderr, "sortof: warning: out of memory; skipping refinement\n");
        return 0;
    }
    opt_row_t *res = pool ? calloc((size_t)pool->nthreads, sizeof *res) : NULL;
    if (pool && !res) pool = NULL;           /* out of memory: do it on one thread */
    if (!pool) res = calloc(1, sizeof *res);
    if (!res) {
        fprintf(stderr, "sortof: warning: out of memory; skipping refinement\n");
        free(edge);
        return 0;
    }
    for (size_t k = 0; k + 1 < n; k++)
        edge[k] = digest_compare(digest_at(digests, digest_bytes, order[k]),
                                 digest_at(digests, digest_bytes, order[k + 1]));
    opt_ctx_t c;
    c.digests = digests;
    c.digest_bytes = digest_bytes;
    c.order = order;
    c.edge = edge;
    c.res = res;
    for (int k = 0; k < SORTOF_GROUP; k++) atomic_init(&c.first_hit[k], NO_HIT);
    atomic_init(&c.low_step, 0);
    const size_t last_i = n - 4; /* i ranges 0 .. n-4 inclusive (i+3 < n) */
    size_t i = 0, j0 = 2;        /* the step being worked on, and the position it scans from */
    while (i + 3 < n) {
        const int g = (int)(n - 3 - i < (size_t)SORTOF_GROUP ? n - 3 - i : (size_t)SORTOF_GROUP);
        c.i0 = i;
        c.g = g;
        c.step_lo[0] = j0;
        for (int k = 1; k < g; k++) c.step_lo[k] = i + (size_t)k + 2;
        c.rlo = c.step_lo[0];
        if (g > 1 && c.step_lo[1] < c.rlo) c.rlo = c.step_lo[1];
        c.jend = n - 1;                      /* positions j .. n-2: j+1 must exist */
        int hit_step = -1, c1 = 0, c2 = 0;
        size_t hit_j = NO_HIT;
        if (c.rlo < c.jend) {
            int nt = 1;
            if (pool && c.jend - c.rlo >= (size_t)SORTOF_PAR_MIN * (size_t)pool->nthreads) {
                nt = pool->nthreads;
                c.parallel = 1;
                for (int k = 0; k < g; k++)
                    atomic_store_explicit(&c.first_hit[k], NO_HIT, memory_order_relaxed);
                atomic_store_explicit(&c.low_step, g, memory_order_relaxed);
                pool_run(pool, twoopt_group_job, &c);
            } else {
                c.parallel = 0;
                twoopt_group_job(&c, 0, 1);
            }
            for (int k = 0; k < g && hit_step < 0; k++) {
                for (int t = 0; t < nt; t++) {
                    if (res[t].h[k].j < hit_j) {
                        hit_j = res[t].h[k].j;
                        c1 = res[t].h[k].c1;
                        c2 = res[t].h[k].c2;
                    }
                }
                if (hit_j != NO_HIT) hit_step = k;
            }
        }
        if (hit_step < 0) {                  /* no step of the group moves anything: all done */
            i += (size_t)g;
            j0 = i + 2;
            if (!quiet) progress_bar(label, i - 1 < last_i ? i - 1 : last_i, last_i);
            continue;
        }
        i += (size_t)hit_step;               /* the steps below it are done */
        {
            size_t j = hit_j;
            size_t lo = i + 1, hi = j;
            while (lo < hi) {
                size_t tmp = order[lo];
                order[lo] = order[hi];
                order[hi] = tmp;
                lo++; hi--;
            }
            lo = i + 1; hi = j - 1;          /* the edges inside the segment now run backwards */
            while (lo < hi) {
                int tmp = edge[lo];
                edge[lo] = edge[hi];
                edge[hi] = tmp;
                lo++; hi--;
            }
            edge[i] = c1;
            edge[j] = c2;
            improved = 1;
            j0 = j + 1;                      /* this step carries on after the move; the steps above are redone */
        }
        if (!quiet) progress_bar(label, i < last_i ? i : last_i, last_i);
    }
    free(res);
    free(edge);
    return improved;
}

/* Run up to max_sweeps full 2-opt sweeps, stopping early once a sweep
 * makes no improving move (i.e. it has converged to a local optimum). */
static int optimize_order(unsigned char *digests, size_t digest_bytes, size_t *order,
                           size_t n, int max_sweeps, int quiet, pool_t *pool) {
    if (max_sweeps <= 0) return 0;
    char label[24];
    int ran = 0;
    for (int s = 0; s < max_sweeps; s++) {
        snprintf(label, sizeof(label), "sweep %d", s + 1);
        int improved = twoopt_pass(digests, digest_bytes, order, n, label, quiet, pool);
        ran++;
        if (!improved) {
            if (!quiet) {
                fprintf(stderr, "sortof: converged after %d sweep(s)\n", s + 1);
            }
            break;
        }
    }
    return ran;
}


/* ---------------------------------------------------------------- */
/* main                                                              */
/* ---------------------------------------------------------------- */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] <source>...\n"
        "       find /some/dir -type f | %s [options] -\n"
        "A source is a directory (scanned), a regular file, or - (paths on stdin, once).\n"
        "A path that comes up more than once is listed once.\n\n"
        "Similarity order (MinHash): files sharing byte sequences end up next to each other.\n"
        "Options:\n"
        "  -b N     files per batch (default: unbounded, one batch, max accuracy)\n"
        "  -o N     max 2-opt refinement sweeps, default 50 (0 disables)\n"
        "  -I       the files are lists of 32-bit numbers (made by cdcmaster -I), not text:\n"
        "           each number is one set element\n"
        "  -m       Matroska-aware: for real .mkv/.webm files (magic bytes) hash only\n"
        "           the Cluster payload, skipping EBML header, SeekHead, Info, Tracks,\n"
        "           Cues. Other files are hashed normally\n"
        "  -t N     threads, default = number of CPUs (hashing and the ordering of big lists)\n"
        "  -p PATH  pin PATH: it comes first in the output and is never sorted or read (repeatable)\n"
        "  -f PATH  write the sorted list to PATH instead of stdout\n"
        "  -q       quiet (no progress messages on stderr)\n"
        "  -v       print a memory/behavior estimate before running\n"
        "  -h       show this help\n"
        "(For a plain name or extension order see sortof-natural and sortof-extension.)\n",
        prog, prog);
}

int main(int argc, char **argv) {
    const char *prog = "sortof-minhash";
    size_t batch_size = 0;   /* 0 = unbounded; resolved after we know pl.count */
    int batch_explicit = 0;
    int max_sweeps = 50;
    int nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (nthreads < 1) nthreads = 1;
    int quiet = 0;
    int verbose = 0;
    int mkv_aware = 0;
    const char *out_path = NULL;
    int opt;

    while ((opt = getopt(argc, argv, "p:b:o:t:mIf:qvh")) != -1) {
        switch (opt) {
            case 'b':
                batch_size = (size_t)strtoull(optarg, NULL, 10);
                batch_explicit = 1;
                break;
            case 'o': max_sweeps = atoi(optarg); break;
            case 't': nthreads = atoi(optarg); if (nthreads < 1) nthreads = 1; break;
            case 'm': mkv_aware = 1; break;
            case 'I': id_mode = 1; break;
            case 'p': pin_add(optarg); break;
            case 'f': out_path = optarg; break;
            case 'q': quiet = 1; break;
            case 'v': verbose = 1; break;
            case 'h': usage(prog); return 0;
            default: usage(prog); return 1;
        }
    }
    if (optind >= argc) {
        usage(prog);
        return 1;
    }
    if (batch_explicit && batch_size < 1) batch_size = 1;
    minhash_seeds_init();
    const size_t digest_bytes = MINHASH_DIGEST_BYTES;

    pathlist_t pl;
    if (collect_sources(prog, argc - optind, argv + optind, &pl, quiet)) return 1;

    /* Printed unconditionally (even with -q) and before any real work starts, so a
     * forgotten -m/-f is visible right away. */
    fprintf(stderr, "%s: order: similarity (MinHash, %d functions) | MKV support: %s | outputting to %s\n",
            prog, MINHASH_NUM_FUNCS,
            mkv_aware ? "ENABLED (-m)" : "disabled (pass -m to strip Matroska container framing)",
            out_path ? out_path : "stdout");

    FILE *out = stdout;
    if (out_path) {
        out = fopen(out_path, "w");
        if (!out) {
            fprintf(stderr, "%s: cannot open %s for writing: %s\n", prog, out_path, strerror(errno));
            return 1;
        }
    }
    pin_write(out);

    if (pl.count == 0) {
        if (!quiet) fprintf(stderr, "%s: no files found\n", prog);
        if (out_path) fclose(out);
        return 0;
    }

    if (!batch_explicit) batch_size = pl.count; /* unbounded: one batch total */
    size_t nbatches = (pl.count + batch_size - 1) / batch_size;

    if (verbose) {
        /* Memory is O(batch_size): digest_bytes/digest plus a visited
           byte. Time is O(batch_size^2) per pass (greedy construction,
           and again for each 2-opt sweep) -- that's what actually
           limits how large a batch is practical, not memory. */
        double digest_mb = (double)(batch_size * digest_bytes) / (1024.0 * 1024.0);
        double comparisons_millions =
            ((double)batch_size * (double)batch_size) / 1e6;
        fprintf(stderr,
            "sortof-minhash: %zu files, batch size %zu -> %zu batch(es), %d thread(s)\n"
            "sortof-minhash: digest memory per batch: ~%.1f MB (%zu bytes/digest)\n"
            "sortof-minhash: greedy-sort comparisons per batch: ~%.0f million\n"
            "sortof-minhash: up to %d refinement sweep(s), ~%.0f million comparisons each\n",
            pl.count, batch_size, nbatches, nthreads,
            digest_mb, digest_bytes, comparisons_millions, max_sweeps, comparisons_millions);
    }

    unsigned char *digests = malloc(batch_size * digest_bytes);
    if (!digests) {
        fprintf(stderr, "sortof-minhash: out of memory allocating digest buffer\n");
        return 1;
    }
    /* Sized for the WHOLE run, not per-batch, so duplicate detection
     * works correctly even if you split into batches with -b -- a
     * duplicate pair split across two batches is still found, since
     * this is one array spanning every file, not reset per batch. */
    uint64_t *dup_hash_all = malloc(pl.count * sizeof(uint64_t));
    uint64_t *dup_len_all = malloc(pl.count * sizeof(uint64_t));
    if (!dup_hash_all || !dup_len_all) {
        fprintf(stderr, "sortof-minhash: out of memory allocating duplicate-tracking buffer\n");
        free(digests); free(dup_hash_all); free(dup_len_all);
        return 1;
    }

    size_t done = 0;
    size_t batch_num = 0;
    while (done < pl.count) {
        size_t n = pl.count - done;
        if (n > batch_size) n = batch_size;
        char **batch_paths = pl.paths + done;
        batch_num++;

        if (!quiet) {
            fprintf(stderr, "sortof-minhash: batch %zu/%zu: hashing %zu files...\n",
                    batch_num, nbatches, n);
        }
        double t0 = now_s();
        hash_all(batch_paths, n, digests, digest_bytes, dup_hash_all + done, dup_len_all + done,
                  nthreads, quiet, mkv_aware);
        double t_hash = now_s() - t0;

        if (!quiet) {
            fprintf(stderr, "sortof-minhash: batch %zu/%zu: ordering...\n",
                    batch_num, nbatches);
        }
        pool_t *pool = pool_start(nthreads, n);   /* NULL: serial (one thread, or too few files to split a scan) */
        int pool_threads = pool ? pool->nthreads : 1;
        t0 = now_s();
        size_t *order = greedy_order(digests, digest_bytes, n, quiet, pool);
        double t_greedy = now_s() - t0;
        t0 = now_s();
        int sweeps = optimize_order(digests, digest_bytes, order, n, max_sweeps, quiet, pool);
        double t_sweeps = now_s() - t0;
        pool_stop(pool);
        if (!quiet) {
            fprintf(stderr, "sortof-minhash: batch %zu/%zu took: hashing %.1f s, greedy %.1f s, %d sweep(s) %.1f s%s\n"
                            "sortof-minhash: the ordering used %d thread%s",
                    batch_num, nbatches, t_hash, t_greedy, sweeps, t_sweeps,
                    sweeps ? "" : " (none run)", pool_threads, pool_threads == 1 ? "" : "s");
            if (pool_threads == 1 && nthreads > 1)
                fprintf(stderr, " (-t %d, but a scan is split only when each thread gets at least %d compares: "
                                "a batch of at least %zu files)",
                        nthreads, SORTOF_PAR_MIN, (size_t)SORTOF_PAR_MIN * (size_t)nthreads);
            fprintf(stderr, "\n");
        }
        for (size_t i = 0; i < n; i++) {
            fprintf(out, "%s\n", batch_paths[order[i]]);
        }
        fflush(out);
        free(order);

        done += n;
    }

    report_duplicates(dup_hash_all, dup_len_all, pl.count, quiet);

    if (out_path) {
        fclose(out);
        if (!quiet) {
            fprintf(stderr, "sortof-minhash: wrote %zu file path(s) to %s\n", pl.count + pin_count(), out_path);
        }
    }

    free(digests);
    free(dup_hash_all);
    free(dup_len_all);
    for (size_t i = 0; i < pl.count; i++) free(pl.paths[i]);
    free(pl.paths);
    return 0;
}
