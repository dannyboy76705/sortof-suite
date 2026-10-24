/*
 * sortof-cdcsort - file list with identical files chained behind their first instance
 *               (part of the sortof-suite)
 *
 * Keeps the list in the order it is given, except that every file whose content is
 * byte for byte (same length, same 64-bit FNV hash) another file's comes right behind the FIRST
 * file of its set, in the order they appeared. Everything else stays where it was.
 * A compressor with a small window (tar | zstd -9 sees only a few MB back) then finds the second
 * copy right next to the first. Only regular, non-empty files that share a size with another
 * file are read. A wrong match could only change the order, never the data.
 *
 *   $ cdcstore-list -k REPO | sortof-cdcsort - | tar -T- -cf - | zstd -9
 *
 * A source is a directory (scanned), a file, or - (a list on stdin). The order given is kept.
 *
 * NEAR MATCHES (-s PCT, default 50; -s 0 = exact duplicates only): chunks of a patched file are not identical to the old ones but share
 * almost all their bytes. Unless -s 0, every file that is not an exact duplicate gets a MinHash
 * sketch (the one sortof-minhash uses); a file whose sketch is at least PCT percent like an
 * EARLIER file's is moved behind it (behind the most similar such file; a file with several
 * look-alikes brings them along, in their original order). Files with no look-alike keep
 * their place. Candidates come from banding the sketch (no all-pairs comparison), so it
 * scales to millions of files; -m limits the candidates tried per band. The sketches need
 * 512 bytes per file in memory, and every file is read once.
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
#include "sortof-sketch.h"

typedef struct {
    char **paths;
    const size_t *cand;
    size_t ncand;
    uint64_t *hash, *len;
    atomic_size_t next, done;
    int quiet;
} dupscan_job_t;

static void *dupscan_worker(void *arg) {
    dupscan_job_t *j = (dupscan_job_t *)arg;
    unsigned char *buf = malloc(1 << 16);
    for (;;) {
        size_t k = atomic_fetch_add(&j->next, 1);
        if (k >= j->ncand) break;
        size_t i = j->cand[k];
        FILE *f = buf ? fopen(j->paths[i], "rb") : NULL;
        j->hash[i] = 0;
        j->len[i] = 0;
        if (!f) {
            if (!j->quiet) fprintf(stderr, "\nsortof-cdcsort: warning: cannot read %s: %s\n", j->paths[i], strerror(errno));
        } else {
            uint64_t fh = FNV_OFFSET_64, total = 0;
            size_t r;
            while ((r = fread(buf, 1, 1 << 16, f)) > 0) {
                fh = fnv1a64_update(fh, buf, r);
                total += r;
            }
            fclose(f);
            j->hash[i] = fh;
            j->len[i] = total;
        }
        atomic_fetch_add(&j->done, 1);
    }
    free(buf);
    return NULL;
}

typedef struct { uint64_t size; size_t idx; } sizekey_t;

static int sizekey_cmp(const void *a, const void *b) {
    const sizekey_t *x = a, *y = b;
    if (x->size != y->size) return x->size < y->size ? -1 : 1;
    return x->idx < y->idx ? -1 : (x->idx > y->idx);
}

static void scan_duplicates(char **paths, size_t n, int nthreads, int quiet,
                            uint64_t *hash, uint64_t *len) {
    sizekey_t *keys = malloc((n ? n : 1) * sizeof(sizekey_t));
    size_t *cand = malloc((n ? n : 1) * sizeof(size_t));
    size_t m = 0, ncand = 0;
    for (size_t i = 0; i < n; i++) {
        struct stat st;
        hash[i] = 0;
        len[i] = 0;
        if (keys && lstat(paths[i], &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
            keys[m].size = (uint64_t)st.st_size;
            keys[m].idx = i;
            m++;
        }
    }
    if (!keys || !cand) { free(keys); free(cand); return; }
    qsort(keys, m, sizeof(sizekey_t), sizekey_cmp);
    for (size_t a = 0; a < m;) {
        size_t b = a + 1;
        while (b < m && keys[b].size == keys[a].size) b++;
        if (b - a > 1) for (size_t k = a; k < b; k++) cand[ncand++] = keys[k].idx;
        a = b;
    }
    free(keys);
    if (ncand) {
        dupscan_job_t job;
        job.paths = paths; job.cand = cand; job.ncand = ncand; job.hash = hash; job.len = len; job.quiet = quiet;
        atomic_init(&job.next, 0);
        atomic_init(&job.done, 0);
        if (nthreads < 1) nthreads = 1;
        if ((size_t)nthreads > ncand) nthreads = (int)ncand;
        pthread_t *th = malloc(sizeof(pthread_t) * (size_t)nthreads);
        int launched = 0;
        if (th) for (int t = 0; t < nthreads; t++) if (pthread_create(&th[launched], NULL, dupscan_worker, &job) == 0) launched++;
        if (launched == 0) dupscan_worker(&job);              /* no threads to be had: do it here */
        else {
            while (atomic_load(&job.done) < ncand) {
                if (!quiet) progress_bar("dup scan", atomic_load(&job.done), ncand);
                struct timespec ts = {0, 50 * 1000 * 1000};
                nanosleep(&ts, NULL);
            }
            if (!quiet) progress_bar("dup scan", ncand, ncand);
            for (int t = 0; t < launched; t++) pthread_join(th[t], NULL);
        }
        free(th);
    }
    free(cand);
}

typedef struct { uint64_t len, hash; size_t pos, idx; } dupkey_t;
typedef struct { size_t a, b, firstpos; } dupgroup_t;

static int dupkey_cmp(const void *x, const void *y) {
    const dupkey_t *a = x, *b = y;
    if (a->len != b->len) return a->len < b->len ? -1 : 1;
    if (a->hash != b->hash) return a->hash < b->hash ? -1 : 1;
    return a->pos < b->pos ? -1 : (a->pos > b->pos);
}

static int dupgroup_cmp(const void *x, const void *y) {
    const dupgroup_t *a = x, *b = y;
    return a->firstpos < b->firstpos ? -1 : (a->firstpos > b->firstpos);
}

/* ord[pos] is the index of the file at position pos; hash[] and len[] are by file index.
 * Returns a new order of the same n indexes with every set of identical files placed
 * first: the sets in order of first appearance, the members of a set by position, then
 * all the other files in their order. NULL if out of memory (the caller keeps the
 * order it had). */
static size_t *apply_dup_order(const size_t *ord, size_t n, const uint64_t *hash, const uint64_t *len,
                               size_t *nsets, size_t *nfiles, uint64_t *redundant_bytes, int inline_mode) {
    size_t *pos = malloc((n ? n : 1) * sizeof(size_t));
    dupkey_t *k = malloc((n ? n : 1) * sizeof(dupkey_t));
    dupgroup_t *g = malloc((n ? n : 1) * sizeof(dupgroup_t));
    size_t *out = malloc((n ? n : 1) * sizeof(size_t));
    unsigned char *emitted = calloc(n ? n : 1, 1);
    size_t m = 0, ng = 0, w = 0;
    *nsets = 0; *nfiles = 0; *redundant_bytes = 0;
    if (!pos || !k || !g || !out || !emitted) { free(pos); free(k); free(g); free(out); free(emitted); return NULL; }
    for (size_t p = 0; p < n; p++) pos[ord[p]] = p;
    for (size_t i = 0; i < n; i++)
        if (len[i] > 0) { k[m].len = len[i]; k[m].hash = hash[i]; k[m].pos = pos[i]; k[m].idx = i; m++; }
    qsort(k, m, sizeof(dupkey_t), dupkey_cmp);
    for (size_t a = 0; a < m;) {                          /* runs of identical (length, hash): the sets */
        size_t b = a + 1;
        while (b < m && k[b].len == k[a].len && k[b].hash == k[a].hash) b++;
        if (b - a > 1) {
            g[ng].a = a; g[ng].b = b; g[ng].firstpos = k[a].pos;         /* k[a] is the first member by position */
            ng++;
            *redundant_bytes += (uint64_t)(b - a - 1) * k[a].len;
            *nfiles += b - a;
        }
        a = b;
    }
    *nsets = ng;
    if (inline_mode) {
        /* -D: every set stays where its first file is: the first file, then the other members right after it,
         * by position; everything else keeps its place. */
        size_t *sa = malloc((n ? n : 1) * sizeof(size_t)), *sb = malloc((n ? n : 1) * sizeof(size_t));
        if (!sa || !sb) { free(sa); free(sb); free(pos); free(k); free(g); free(out); free(emitted); return NULL; }
        for (size_t i = 0; i < n; i++) sa[i] = (size_t)-1;
        for (size_t r = 0; r < ng; r++) { sa[k[g[r].a].idx] = g[r].a; sb[k[g[r].a].idx] = g[r].b; }
        for (size_t p = 0; p < n; p++) {
            size_t i = ord[p];
            if (emitted[i]) continue;
            out[w++] = i; emitted[i] = 1;
            if (sa[i] != (size_t)-1)
                for (size_t x = sa[i] + 1; x < sb[i]; x++) { out[w++] = k[x].idx; emitted[k[x].idx] = 1; }
        }
        free(sa); free(sb); free(pos); free(k); free(g); free(emitted);
        return out;
    }
    qsort(g, ng, sizeof(dupgroup_t), dupgroup_cmp);       /* sets in order of first appearance */
    for (size_t r = 0; r < ng; r++)
        for (size_t x = g[r].a; x < g[r].b; x++) { out[w++] = k[x].idx; emitted[k[x].idx] = 1; }
    for (size_t p = 0; p < n; p++) if (!emitted[ord[p]]) out[w++] = ord[p];
    free(pos); free(k); free(g); free(emitted);
    return out;
}

static void report_dup_order(size_t nsets, size_t nfiles, uint64_t redundant, int quiet, int inline_mode) {
    if (quiet) return;
    fprintf(stderr, inline_mode ? "sortof-cdcsort: %zu duplicate file%s in %zu set%s (%.1f MiB of redundant copies), placed after their first file\n" : "sortof-cdcsort: %zu duplicate file%s in %zu set%s (%.1f MiB of redundant copies), placed first\n",
            nfiles, nfiles == 1 ? "" : "s", nsets, nsets == 1 ? "" : "s",
            (double)redundant / (1024.0 * 1024.0));
}


/* ---------------------------------------------------------------- near matches (-s) */

typedef struct {
    char **paths; const size_t *elig; size_t n;
    uint32_t *dig; unsigned char *ok;
    atomic_size_t next, done;
} sk_job_t;

static void *sk_worker(void *a) {
    sk_job_t *j = a;
    unsigned char *buf = malloc(1 << 16);
    for (;;) {
        size_t k = atomic_fetch_add(&j->next, 1);
        if (k >= j->n) break;
        FILE *f = buf ? fopen(j->paths[j->elig[k]], "rb") : NULL;
        minhash_t m;
        minhash_init(&m);
        j->ok[k] = 0;
        if (f) {
            size_t r;
            while ((r = fread(buf, 1, 1 << 16, f)) > 0) minhash_update(&m, buf, r);
            fclose(f);
            j->ok[k] = 1;
        }
        minhash_digest(&m, (unsigned char *)(j->dig + k * MINHASH_NUM_FUNCS));
        atomic_fetch_add(&j->done, 1);
    }
    free(buf);
    return NULL;
}

typedef struct { uint64_t key; uint32_t k; } bandent_t;
static int bandent_cmp(const void *x, const void *y) {
    const bandent_t *a = x, *b = y;
    if (a->key != b->key) return a->key < b->key ? -1 : 1;
    return a->k < b->k ? -1 : (a->k > b->k);
}
typedef struct { uint64_t len, hash; size_t idx; } exkey_t;
static int exkey_cmp(const void *x, const void *y) {
    const exkey_t *a = x, *b = y;
    if (a->len != b->len) return a->len < b->len ? -1 : 1;
    if (a->hash != b->hash) return a->hash < b->hash ? -1 : 1;
    return a->idx < b->idx ? -1 : (a->idx > b->idx);
}
static uint64_t bandmix(uint64_t h, uint32_t v) {
    h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    h *= 0xff51afd7ed558ccdULL;
    return h ^ (h >> 33);
}

/* Builds the new order. dh/dl: exact-duplicate hashes (dl == 0: not read). Returns it, or NULL. */
static size_t *near_order(const char *prog, char **paths, size_t n, const uint64_t *dh, const uint64_t *dl,
                          double pct, unsigned maxcand, uint64_t minsize, int nthreads, int quiet) {
    const size_t NONE = (size_t)-1;
    size_t *lead = malloc(n * sizeof(size_t)), *fnext = malloc(n * sizeof(size_t)), *ftail = malloc(n * sizeof(size_t));
    exkey_t *ek = malloc((n + 1) * sizeof(exkey_t));
    if (!lead || !fnext || !ftail || !ek) return NULL;
    size_t m = 0;
    for (size_t i = 0; i < n; i++) { lead[i] = i; fnext[i] = NONE; ftail[i] = i; if (dl[i] > 0) { ek[m].len = dl[i]; ek[m].hash = dh[i]; ek[m].idx = i; m++; } }
    qsort(ek, m, sizeof(exkey_t), exkey_cmp);
    size_t nexact = 0;
    for (size_t a = 0; a < m;) {
        size_t b = a + 1;
        while (b < m && ek[b].len == ek[a].len && ek[b].hash == ek[a].hash) b++;
        size_t first = ek[a].idx;
        for (size_t x = a + 1; x < b; x++) { lead[ek[x].idx] = first; fnext[ftail[first]] = ek[x].idx; ftail[first] = ek[x].idx; nexact++; }
        a = b;
    }
    free(ek);

    /* files that get a sketch: leaders that are regular files of at least minsize bytes */
    size_t *elig = malloc((n + 1) * sizeof(size_t)), *kof = malloc(n * sizeof(size_t));
    size_t ne = 0;
    for (size_t i = 0; i < n; i++) {
        struct stat st;
        kof[i] = NONE;
        if (lead[i] != i) continue;
        if (lstat(paths[i], &st) == 0 && S_ISREG(st.st_mode) && (uint64_t)st.st_size >= minsize) { kof[i] = ne; elig[ne++] = i; }
    }
    size_t *parent = malloc((ne + 1) * sizeof(size_t));
    uint8_t *bestsc = calloc(ne + 1, 1);
    for (size_t k = 0; k < ne; k++) parent[k] = NONE;
    size_t nlinked = 0;
    double simsum = 0;
    if (ne >= 2) {
        uint32_t *dig = aligned_alloc(64, ((ne * MINHASH_DIGEST_BYTES) + 63) / 64 * 64);
        unsigned char *ok = calloc(ne, 1);
        if (!dig || !ok) { fprintf(stderr, "%s: out of memory for %zu sketches\n", prog, ne); return NULL; }
        minhash_seeds_init();
        sk_job_t job;
        job.paths = paths; job.elig = elig; job.n = ne; job.dig = dig; job.ok = ok;
        atomic_init(&job.next, 0); atomic_init(&job.done, 0);
        if (nthreads < 1) nthreads = 1;
        pthread_t *th = malloc(sizeof(pthread_t) * (size_t)nthreads);
        int launched = 0;
        if (th) for (int t = 0; t < nthreads; t++) if (pthread_create(&th[launched], NULL, sk_worker, &job) == 0) launched++;
        if (launched == 0) sk_worker(&job);
        else {
            while (atomic_load(&job.done) < ne) {
                if (!quiet) progress_bar("sketches", atomic_load(&job.done), ne);
                struct timespec ts = {0, 50 * 1000 * 1000};
                nanosleep(&ts, NULL);
            }
            if (!quiet) progress_bar("sketches", ne, ne);
            for (int t = 0; t < launched; t++) pthread_join(th[t], NULL);
        }
        free(th);
        /* bands: rows per band r, b = 128 / r bands; the largest r that still catches a pair at the threshold 90% of the time */
        const double sfrac = pct / 100.0;
        int rr = 1;
        static const int rs[] = {32, 16, 8, 4, 2, 1};
        for (size_t x = 0; x < sizeof(rs) / sizeof(*rs); x++) {
            double p = 1.0, sr = 1.0;
            for (int q = 0; q < rs[x]; q++) sr *= sfrac;
            int nb = MINHASH_NUM_FUNCS / rs[x];
            for (int q = 0; q < nb; q++) p *= (1.0 - sr);
            if (1.0 - p >= 0.9) { rr = rs[x]; break; }
        }
        int nb = MINHASH_NUM_FUNCS / rr;
        if (!quiet) fprintf(stderr, "%s: %zu files sketched; near-match search: %d bands of %d values, up to %u candidates each\n", prog, ne, nb, rr, maxcand);
        bandent_t *be = malloc(ne * sizeof(bandent_t));
        if (!be) return NULL;
        const unsigned thr = (unsigned)(pct * MINHASH_NUM_FUNCS / 100.0 + 0.999999);
        for (int t = 0; t < nb; t++) {
            for (size_t k = 0; k < ne; k++) {
                uint64_t h = 0x243F6A8885A308D3ULL + (uint64_t)t;
                for (int q = 0; q < rr; q++) h = bandmix(h, dig[k * MINHASH_NUM_FUNCS + (size_t)t * (size_t)rr + (size_t)q]);
                be[k].key = h; be[k].k = (uint32_t)k;
            }
            qsort(be, ne, sizeof(bandent_t), bandent_cmp);
            size_t g = 0;
            for (size_t e = 0; e < ne; e++) {
                if (e == 0 || be[e].key != be[e - 1].key) g = e;
                uint32_t k = be[e].k;
                if (!ok[k]) continue;
                size_t lo = e > g + maxcand ? e - maxcand : g;
                for (size_t c = e; c-- > lo;) {
                    uint32_t j = be[c].k;
                    if (!ok[j]) continue;
                    int sc = minhash_compare((const unsigned char *)(dig + (size_t)k * MINHASH_NUM_FUNCS), (const unsigned char *)(dig + (size_t)j * MINHASH_NUM_FUNCS));
                    if ((unsigned)sc >= thr && (parent[k] == NONE || sc > bestsc[k] || (sc == bestsc[k] && j > parent[k]))) { parent[k] = j; bestsc[k] = (uint8_t)sc; }
                }
            }
        }
        free(be); free(dig); free(ok);
        for (size_t k = 0; k < ne; k++) if (parent[k] != NONE) { nlinked++; simsum += bestsc[k] * 100.0 / MINHASH_NUM_FUNCS; }
    }
    if (!quiet) {
        fprintf(stderr, "%s: %zu exact duplicates chained; %zu of %zu other files are at least %.0f%% like an earlier file%s", prog, nexact, nlinked, ne, pct, nlinked ? "" : "\n");
        if (nlinked) fprintf(stderr, " (mean %.1f%%) and moved behind it\n", simsum / (double)nlinked);
    }
    /* children lists, newest first, so pushing them in this order pops the earliest first */
    size_t *child = malloc((ne + 1) * sizeof(size_t)), *sib = malloc((ne + 1) * sizeof(size_t));
    for (size_t k = 0; k < ne; k++) child[k] = NONE;
    for (size_t k = 0; k < ne; k++) if (parent[k] != NONE) { sib[k] = child[parent[k]]; child[parent[k]] = k; }
    size_t *out = malloc((n + 1) * sizeof(size_t)), w = 0;
    size_t *stack = malloc((ne + 1) * sizeof(size_t));
    if (!out || !stack) return NULL;
    for (size_t i = 0; i < n; i++) {
        if (lead[i] != i) continue;
        size_t k0 = kof[i];
        if (k0 != NONE && parent[k0] != NONE) continue;           /* comes out behind its look-alike */
        size_t sp = 0;
        stack[sp++] = i;
        while (sp) {
            size_t u = stack[--sp];
            for (size_t f = u; f != NONE; f = fnext[f]) out[w++] = f;
            size_t ku = kof[u];
            if (ku != NONE)
                for (size_t c = child[ku]; c != NONE; c = sib[c]) stack[sp++] = elig[c];
        }
    }
    free(lead); free(fnext); free(ftail); free(elig); free(kof); free(parent); free(bestsc); free(child); free(sib); free(stack);
    if (w != n) { fprintf(stderr, "%s: internal error: %zu of %zu files placed\n", prog, w, n); free(out); return NULL; }
    return out;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] <source>...\n"
        "       find /some/dir -type f | %s [options] -\n"
        "A source is a directory (scanned), a regular file, or - (paths on stdin, once).\n"
        "The list keeps its order, except that files with identical content are placed right\n"
        "behind the first file of their set. Only files sharing a size with another are read.\n\n"
        "Options:\n"
        "  -s PCT   chain NEAR matches: a file at least PCT%% like (MinHash) an earlier file is moved behind it\n"
        "           (default 50; every file is read once for its sketch). -s 0: exact duplicates only\n"
        "  -m N     near-match candidates tried per band (default 16)\n"
        "  -e N     files smaller than N bytes get no sketch (default 512)\n"
        "  -p PATH  pin PATH: it comes first in the output and is never sorted or read (repeatable)\n"
        "  -t N     threads for reading, default = number of CPUs\n"
        "  -f PATH  write the list to PATH instead of stdout\n"
        "  -q       quiet\n"
        "  -h       this help\n",
        prog, prog);
}

int main(int argc, char **argv) {
    const char *prog = "sortof-cdcsort";
    int quiet = 0, opt;
    int nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (nthreads < 1) nthreads = 1;
    const char *out_path = NULL;
    double near_pct = 50;
    unsigned maxcand = 16;
    uint64_t minsize = 512;
    while ((opt = getopt(argc, argv, "p:s:m:e:t:f:qh")) != -1) {
        switch (opt) {
            case 'p': pin_add(optarg); break;
            case 's': near_pct = atof(optarg); if (near_pct < 0 || near_pct > 99) { fprintf(stderr, "%s: -s must be between 0 and 99\n", prog); return 1; } break;
            case 'm': maxcand = (unsigned)strtoul(optarg, NULL, 10); if (maxcand < 1) maxcand = 1; break;
            case 'e': minsize = strtoull(optarg, NULL, 10); break;
            case 't': nthreads = atoi(optarg); if (nthreads < 1) nthreads = 1; break;
            case 'f': out_path = optarg; break;
            case 'q': quiet = 1; break;
            case 'h': usage(prog); return 0;
            default: usage(prog); return 1;
        }
    }
    if (optind >= argc) { usage(prog); return 1; }
    pathlist_t pl;
    if (collect_sources(prog, argc - optind, argv + optind, &pl, quiet)) return 1;
    if (pl.count == 0) {
        if (!quiet) fprintf(stderr, "%s: no files found\n", prog);
        return 0;
    }
    size_t n = pl.count, nsets, nfiles, *ord = malloc(n * sizeof(size_t)), *nord = NULL;
    uint64_t *dh = malloc(n * sizeof(uint64_t)), *dl = malloc(n * sizeof(uint64_t)), redundant;
    if (ord && dh && dl) {
        for (size_t i = 0; i < n; i++) ord[i] = i;
        scan_duplicates(pl.paths, n, nthreads, quiet, dh, dl);
        if (near_pct > 0) nord = near_order(prog, pl.paths, n, dh, dl, near_pct, maxcand, minsize, nthreads, quiet);
        else {
            nord = apply_dup_order(ord, n, dh, dl, &nsets, &nfiles, &redundant, 1);
            if (nord) report_dup_order(nsets, nfiles, redundant, quiet, 1);
        }
    }
    if (!nord) { fprintf(stderr, "%s: out of memory\n", prog); return 1; }
    char **sorted = malloc(n * sizeof(char *));
    if (!sorted) { fprintf(stderr, "%s: out of memory\n", prog); return 1; }
    for (size_t i = 0; i < n; i++) sorted[i] = pl.paths[nord[i]];
    memcpy(pl.paths, sorted, n * sizeof(char *));
    free(sorted); free(ord); free(dh); free(dl); free(nord);
    return write_list(prog, &pl, out_path, quiet);
}
