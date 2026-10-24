/*
 * sortof-palscore - scene finder / scene orderer for JPEG picture lists, by colour histogram and layout.
 *
 *   sortof-palscore [-t N] [-c CUT%] [-k K] [-j PCT] [-b PCT] [-g MAXSCENES] [-F] [-q]
 *            [-N] [-o ORDER.txt] [-v SCENES.tsv] [-w SIGFILE] LIST|-
 *
 * LIST is a list of file paths, one per line (natural order, as sortof-natural makes it). Every JPEG is
 * decoded at 1/8 scale (libjpeg then reads only the DC coefficients, so it is fast: a 720x480 picture
 * becomes a 90x60 thumbnail) and reduced to a small signature: coarse histograms of Y, Cb and Cr plus an
 * 8x6 grid of mean luma. Noise barely changes a signature, so scenes show up although their JPEG rows
 * never match byte for byte. Files that are not JPEGs (specs.txt) get no signature: they are put at the end of the order, in their input order.
 *
 * The signatures are kept in SIGFILE (-w), and reused on the next run if the list is the same (-F forces a
 * new pass), so tuning -c / -k / -j / -b takes seconds.
 *
 * Distance between two signatures: 0 (same) .. 100% (nothing in common), half histogram, half layout.
 *
 * What it prints:
 *   - the distance between neighbouring files (percentiles),
 *   - how many scenes each cut level gives (a file starts a scene when its distance to the last K files of
 *     the current scene is above the cut; -k, default 4, so a flash back and forth is not a cut),
 *   - for the chosen cut (-c, default 10%): how many scenes, and for each scene the most similar EARLIER
 *     scene (not its neighbour): how many have a look-alike within 2/5/10/20%, and how far back it is.
 *   - -o ORDER.txt: the list in scene order. Scenes stay whole and in natural order inside; after each
 *     scene the most similar unvisited scene follows if it is within -j % (default 5); otherwise, or if
 *     the natural next scene is within -b % (default 1) of that best one, the natural next scene follows.
 *   - -D (on by default, -N turns it off; used with -o): byte-identical files are chained right behind their first instance in that order. Every file
 *     is hashed while it is read for its signature (the hash is kept in SIGFILE), and matches are confirmed
 *     by comparing the files, so a hash collision can never merge two different files.
 *   - -v SCENES.tsv: one line per scene: number, first file, files, nearest earlier scene, distance %,
 *     files back.
 *
 * Build: gcc -O3 -march=native -Wall -pthread -o sortof-palscore sortof-palscore.c -ljpeg -lm
 *
 * MIT License, Copyright (c) 2026 Daniel Lee Witzel (see LICENSE)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <errno.h>
#include <setjmp.h>
#include <pthread.h>
#include <unistd.h>
#include <strings.h>
#include <jpeglib.h>
#include <sys/stat.h>
#include <limits.h>
#include "sortof-common.h"

#define NH 64          /* 32 Y + 16 Cb + 16 Cr */
#define NG 48          /* 8 x 6 grid */
typedef struct {
    uint64_t fhash;    /* hash of the whole file (for -D) */
    uint32_t fsize;    /* its size */
    uint16_t h[NH];    /* each of the three histograms sums to about 65535 */
    uint8_t g[NG];     /* mean luma per grid cell */
    uint8_t valid;     /* a signature was made */
    uint8_t hok;       /* the file was read and hashed */
    uint8_t pad[2];
} sig_t;

typedef struct { char magic[8]; uint32_t n; uint32_t size; uint64_t listhash; } sighdr_t;

static float sig_dist(const sig_t *a, const sig_t *b) {
    int l1 = 0, g = 0;
    for (int i = 0; i < NH; i++) l1 += abs((int)a->h[i] - (int)b->h[i]);
    for (int i = 0; i < NG; i++) g += abs((int)a->g[i] - (int)b->g[i]);
    return 0.5f * ((float)l1 / (6.0f * 65535.0f) + (float)g / (NG * 255.0f));
}

/* ---------- decoding ---------- */
struct jerr { struct jpeg_error_mgr pub; jmp_buf jb; };
static void jerr_exit(j_common_ptr c) { longjmp(((struct jerr *)c->err)->jb, 1); }
static void jerr_quiet(j_common_ptr c, int lvl) { (void)c; (void)lvl; }

static int compute_sig(const unsigned char *buf, size_t size, sig_t *s) {
    memset(s, 0, sizeof(*s));
    struct jpeg_decompress_struct ci;
    struct jerr je;
    ci.err = jpeg_std_error(&je.pub);
    je.pub.error_exit = jerr_exit;
    je.pub.emit_message = jerr_quiet;
    JSAMPROW row = NULL;
    if (setjmp(je.jb)) {
        jpeg_destroy_decompress(&ci);
        free(row);
        memset(s, 0, sizeof(*s));
        return -1;
    }
    jpeg_create_decompress(&ci);
    jpeg_mem_src(&ci, (unsigned char *)buf, (unsigned long)size);
    jpeg_read_header(&ci, TRUE);
    ci.scale_num = 1;
    ci.scale_denom = 8;
    ci.dct_method = JDCT_IFAST;
    ci.do_fancy_upsampling = FALSE;
    ci.out_color_space = ci.num_components == 3 ? JCS_YCbCr : JCS_GRAYSCALE;
    jpeg_start_decompress(&ci);
    unsigned w = ci.output_width, h = ci.output_height;
    int nc = ci.output_components;
    row = malloc((size_t)w * nc);
    if (!row) longjmp(je.jb, 1);
    uint32_t hy[32] = {0}, hcb[16] = {0}, hcr[16] = {0}, gs[NG] = {0}, gn[NG] = {0};
    while (ci.output_scanline < h) {
        unsigned y = ci.output_scanline;
        JSAMPROW rp = row;
        jpeg_read_scanlines(&ci, &rp, 1);
        unsigned gy = y * 6 / h;
        for (unsigned x = 0; x < w; x++) {
            unsigned Y = row[x * nc], cb = 128, cr = 128;
            if (nc == 3) { cb = row[x * nc + 1]; cr = row[x * nc + 2]; }
            hy[Y >> 3]++; hcb[cb >> 4]++; hcr[cr >> 4]++;
            unsigned gi = gy * 8 + x * 8 / w;
            gs[gi] += Y; gn[gi]++;
        }
    }
    jpeg_finish_decompress(&ci);
    jpeg_destroy_decompress(&ci);
    free(row);
    double tot = (double)w * h;
    if (tot < 1) return -1;
    for (int i = 0; i < 32; i++) s->h[i] = (uint16_t)(hy[i] * 65535.0 / tot + 0.5);
    for (int i = 0; i < 16; i++) s->h[32 + i] = (uint16_t)(hcb[i] * 65535.0 / tot + 0.5);
    for (int i = 0; i < 16; i++) s->h[48 + i] = (uint16_t)(hcr[i] * 65535.0 / tot + 0.5);
    for (int i = 0; i < NG; i++) s->g[i] = gn[i] ? (uint8_t)(gs[i] / gn[i]) : 0;
    s->valid = 1;
    return 0;
}

static int looks_jpeg(const char *p) {
    size_t l = strlen(p);
    return (l > 4 && !strcasecmp(p + l - 4, ".jpg")) || (l > 5 && !strcasecmp(p + l - 5, ".jpeg"));
}

/* The whole file, up to 256 MB (anything bigger is left out of -D and of the signatures). */
static unsigned char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || sz > (256L << 20) || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    unsigned char *b = malloc(sz ? (size_t)sz : 1);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static uint64_t hash64(const unsigned char *p, size_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ULL ^ (uint64_t)n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h ^= w;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 32;
    }
    uint64_t t = 0;
    for (size_t k = 0; i < n; i++, k++) t |= (uint64_t)p[i] << (8 * k);
    h ^= t;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 29;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 32;
    return h;
}

static int files_equal(const char *a, const char *b) {
    size_t na, nb;
    unsigned char *x = slurp(a, &na), *y = slurp(b, &nb);
    int eq = x && y && na == nb && memcmp(x, y, na) == 0;
    free(x); free(y);
    return eq;
}

typedef struct { char **names; sig_t *sigs; size_t n; size_t next; size_t bad; pthread_mutex_t mu; int quiet; } job_t;
static void *worker(void *arg) {
    job_t *j = arg;
    for (;;) {
        pthread_mutex_lock(&j->mu);
        size_t a = j->next;
        j->next += 64;
        pthread_mutex_unlock(&j->mu);
        if (a >= j->n) break;
        size_t b = a + 64 < j->n ? a + 64 : j->n;
        size_t bad = 0;
        for (size_t i = a; i < b; i++) {
            size_t sz = 0;
            unsigned char *buf = slurp(j->names[i], &sz);
            sig_t *sp = &j->sigs[i];
            if (!buf) { if (looks_jpeg(j->names[i])) bad++; continue; }
            if (looks_jpeg(j->names[i]) && compute_sig(buf, sz, sp) != 0) bad++;
            sp->fhash = hash64(buf, sz);
            sp->fsize = (uint32_t)sz;
            sp->hok = 1;
            free(buf);
        }
        pthread_mutex_lock(&j->mu);
        j->bad += bad;
        if (!j->quiet && (a / 64) % 200 == 0) { fprintf(stderr, "sortof-palscore: signatures %zu/%zu\r", b, j->n); }
        pthread_mutex_unlock(&j->mu);
    }
    return NULL;
}

/* ---------- scenes ---------- */
typedef struct { size_t first, last; } scene_t;

static size_t find_scenes(const sig_t *sg, size_t n, double cut, int K, scene_t *out /* may be NULL */) {
    size_t ns = 0;
    size_t ring[64];
    int rn = 0, rp = 0;
    size_t cur_first = 0;
    int started = 0;
    for (size_t i = 0; i < n; i++) {
        if (!sg[i].valid) continue;
        if (!started) { started = 1; ring[rp] = i; rp = (rp + 1) % K; rn = 1; continue; }
        double dm = 2.0;
        for (int r = 0; r < rn; r++) {
            double d = sig_dist(&sg[ring[r]], &sg[i]);
            if (d < dm) dm = d;
        }
        if (dm > cut) {
            if (out) { out[ns].first = cur_first; out[ns].last = i - 1; }
            ns++;
            cur_first = i;
            rn = 0; rp = 0;
        }
        ring[rp] = i; rp = (rp + 1) % K;
        if (rn < K) rn++;
    }
    if (started) {
        if (out) { out[ns].first = cur_first; out[ns].last = n - 1; }
        ns++;
    }
    return ns;
}

static const sig_t *g_dsig;
static int cmp_dup(const void *a, const void *b) {
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    const sig_t *p = &g_dsig[x], *q = &g_dsig[y];
    if (p->fhash != q->fhash) return p->fhash < q->fhash ? -1 : 1;
    if (p->fsize != q->fsize) return p->fsize < q->fsize ? -1 : 1;
    return x < y ? -1 : x > y;
}

static int cmp_f(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }

static uint64_t fnv(char **names, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        for (const char *p = names[i]; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
        h ^= 10; h *= 1099511628211ULL;
    }
    return h;
}

/* A set's specs.txt belongs in the archive with its pictures (a mount needs it), so it is implied by what this tool is for:
 * for every picture in the list, the specs.txt of its set (set/specs.txt for set/CLIP/bNNNNN/frame.jpg or set/CLIP/frame.jpg)
 * is added at the end of the list if it exists and the list does not hold it. Returns the new count. */
static int is_bucket(const char *s, size_t n)
{
    if (n < 2 || s[0] != 'b') return 0;
    for (size_t i = 1; i < n; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

static size_t add_specs(char ***namesp, size_t n)
{
    char **names = *namesp, **found = NULL;
    size_t nf = 0;
    char lastdir[PATH_MAX] = "";
    for (size_t i = 0; i < n; i++) {
        const char *p = names[i];
        size_t l = strlen(p);
        if (!((l > 4 && !strcasecmp(p + l - 4, ".jpg")) || (l > 5 && !strcasecmp(p + l - 5, ".jpeg")))) continue;
        char d[PATH_MAX];
        if (l >= sizeof d) continue;
        memcpy(d, p, l + 1);
        char *sl = strrchr(d, '/');
        if (!sl) continue;
        *sl = 0;                                              /* the folder of the picture */
        sl = strrchr(d, '/');
        if (sl && is_bucket(sl + 1, strlen(sl + 1))) { *sl = 0; sl = strrchr(d, '/'); }   /* ... or its bucket's parent: the clip */
        if (!sl) continue;
        *sl = 0;                                              /* the set */
        if (!strcmp(d, lastdir)) continue;
        snprintf(lastdir, sizeof lastdir, "%s", d);
        char sp[PATH_MAX + 16];
        snprintf(sp, sizeof sp, "%s/specs.txt", d[0] ? d : "");
        struct stat st;
        if (stat(sp, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        int have = 0;
        for (size_t k = 0; k < nf && !have; k++) have = !strcmp(found[k], sp);
        for (size_t k = 0; k < n && !have; k++) have = !strcmp(names[k], sp);
        if (have) continue;
        char **nfp = realloc(found, (nf + 1) * sizeof *found);
        if (!nfp) break;
        found = nfp;
        found[nf] = strdup(sp);
        if (found[nf]) nf++;
    }
    if (nf) {
        char **nn = realloc(names, (n + nf) * sizeof *names);
        if (nn) {
            names = nn;
            for (size_t k = 0; k < nf; k++) names[n++] = found[k];
            *namesp = names;
        } else for (size_t k = 0; k < nf; k++) free(found[k]);
    }
    free(found);
    return n;
}

static void usage(const char *p) {
    fprintf(stderr,
        "Usage: %s [-t N] [-c CUT%%] [-k K] [-j PCT] [-b PCT] [-g MAXSCENES] [-F] [-q]\n"
        "          [-o ORDER.txt] [-v SCENES.tsv] [-w SIGFILE] LIST|-|DIR...\n"
        "  DIR      a folder (or several): the list is made here in natural order, as sortof-natural would;\n"
        "           the signature file is DIR.bin next to the folder unless -w says otherwise\n"
        "  -p PATH  pin PATH: it comes first in the -o list and is never read (repeatable)\n"
        "  -w FILE  signatures (made if missing or if the list changed; -F forces); default: LIST.bin   -t threads\n"
        "  -c PCT   scene cut: distance above which a file starts a scene (default 10)   -k files looked back (4)\n"
        "  -j PCT   scene order: follow the most similar unvisited scene only if within PCT (default 5)\n"
        "  -b PCT   ... and prefer the natural next scene if within PCT of that best one (default 1)\n"
        "  -g N     most scenes for the look-alike search and the order (default 60000)\n"
        "  -D       (default) with -o: files that are byte for byte the same go right behind their first instance\n"
        "  -N       no duplicate chaining\n"
        "  -o FILE  write the list in scene order     -v FILE  write one line per scene\n", p);
}

int main(int argc, char **argv) {
    int nthreads = 4, K = 4, force = 0, quiet = 0, dupchain = 1;
    double cutpct = 10, jpct = 5, bpct = 1;
    size_t gmax = 60000;
    const char *sigpath = NULL, *orderpath = NULL, *scenepath = NULL;
    int c;
    while ((c = getopt(argc, argv, "p:t:c:k:j:b:g:Fo:v:w:qDNh")) != -1) {
        switch (c) {
        case 't': nthreads = atoi(optarg); break;
        case 'c': cutpct = atof(optarg); break;
        case 'k': K = atoi(optarg); break;
        case 'j': jpct = atof(optarg); break;
        case 'b': bpct = atof(optarg); break;
        case 'g': gmax = strtoul(optarg, NULL, 10); break;
        case 'F': force = 1; break;
        case 'D': dupchain = 1; break;
        case 'N': dupchain = 0; break;
        case 'p': pin_add(optarg); break;
        case 'o': orderpath = optarg; break;
        case 'v': scenepath = optarg; break;
        case 'w': sigpath = optarg; break;
        case 'q': quiet = 1; break;
        default: usage(argv[0]); return c == 'h' ? 0 : 1;
        }
    }
    struct stat st0;
    int from_dir = optind < argc && strcmp(argv[optind], "-") && stat(argv[optind], &st0) == 0 && S_ISDIR(st0.st_mode);
    if (optind >= argc || (!from_dir && optind + 1 != argc)) { usage(argv[0]); return 1; }
    char sigdefault[4200];
    if (!sigpath) {                                   /* no -w: the list's name plus .bin */
        if (!strcmp(argv[optind], "-")) { fprintf(stderr, "sortof-palscore: the list comes from stdin, so there is no name to make the signature file from: give -w FILE\n"); return 1; }
        size_t al = strlen(argv[optind]);
        while (from_dir && al > 1 && argv[optind][al - 1] == '/') argv[optind][--al] = 0;
        if (snprintf(sigdefault, sizeof(sigdefault), "%s.bin", argv[optind]) >= (int)sizeof(sigdefault)) { fprintf(stderr, "sortof-palscore: list name too long\n"); return 1; }
        sigpath = sigdefault;
    }
    if (nthreads < 1) nthreads = 1;
    if (K < 1) K = 1;
    if (K > 64) K = 64;

    /* read the list */
    char **names = NULL;
    size_t n = 0;
    if (from_dir) {
        pathlist_t pl;
        if (collect_sources("sortof-palscore", argc - optind, argv + optind, &pl, quiet)) return 1;
        order_natural(pl.paths, pl.count, 0);
        names = pl.paths; n = pl.count;
    } else {
    FILE *lf = strcmp(argv[optind], "-") ? fopen(argv[optind], "r") : stdin;
    if (!lf) { fprintf(stderr, "sortof-palscore: %s: %s\n", argv[optind], strerror(errno)); return 1; }
    size_t cap = 0;
    char *line = NULL;
    size_t lc = 0;
    ssize_t ll;
    while ((ll = getline(&line, &lc, lf)) > 0) {
        while (ll > 0 && (line[ll - 1] == '\n' || line[ll - 1] == '\r')) line[--ll] = 0;
        if (!ll) continue;
        if (n == cap) { cap = cap ? cap * 2 : 4096; names = realloc(names, cap * sizeof(*names)); if (!names) return 1; }
        if (pin_is(line)) continue;                 /* pinned: written first, never read */
        names[n++] = strdup(line);
    }
    free(line);
    if (lf != stdin) fclose(lf);
    }
    {
        size_t before = n;
        n = add_specs(&names, n);
        if (n > before && !quiet) fprintf(stderr, "sortof-palscore: added %zu specs.txt to the end of the list (a set's specs.txt goes with its pictures)\n", n - before);
    }
    if (!n) { fprintf(stderr, "sortof-palscore: the list is empty\n"); return 1; }

    sig_t *sg = calloc(n, sizeof(*sg));
    if (!sg) { fprintf(stderr, "sortof-palscore: out of memory\n"); return 1; }
    uint64_t lh = fnv(names, n);
    int have = 0;
    if (!force) {
        FILE *sf = fopen(sigpath, "rb");
        if (sf) {
            sighdr_t hd;
            if (fread(&hd, sizeof(hd), 1, sf) == 1 && !memcmp(hd.magic, "PALSIG02", 8) && hd.n == n &&
                hd.size == sizeof(sig_t) && hd.listhash == lh && fread(sg, sizeof(*sg), n, sf) == n) have = 1;
            fclose(sf);
        }
    }
    if (have) {
        if (!quiet) fprintf(stderr, "sortof-palscore: reusing signatures in %s\n", sigpath);
    } else {
        memset(sg, 0, n * sizeof(*sg));
        job_t job = { names, sg, n, 0, 0, PTHREAD_MUTEX_INITIALIZER, quiet };
        pthread_t th[64];
        if (nthreads > 64) nthreads = 64;
        for (int i = 0; i < nthreads; i++) pthread_create(&th[i], NULL, worker, &job);
        for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
        if (!quiet) fprintf(stderr, "sortof-palscore: signatures %zu/%zu    \n", n, n);
        if (job.bad) fprintf(stderr, "sortof-palscore: %zu JPEG files could not be read (left out of the scenes)\n", job.bad);
        FILE *sf = fopen(sigpath, "wb");
        if (sf) {
            sighdr_t hd;
            memcpy(hd.magic, "PALSIG02", 8);
            hd.n = (uint32_t)n; hd.size = sizeof(sig_t); hd.listhash = lh;
            fwrite(&hd, sizeof(hd), 1, sf);
            fwrite(sg, sizeof(*sg), n, sf);
            if (fclose(sf) != 0) fprintf(stderr, "sortof-palscore: could not write %s\n", sigpath);
        } else fprintf(stderr, "sortof-palscore: cannot write %s: %s\n", sigpath, strerror(errno));
    }
    size_t nvalid = 0;
    for (size_t i = 0; i < n; i++) nvalid += sg[i].valid;
    printf("files %zu, with a signature %zu\n", n, nvalid);
    if (nvalid < 2) { fprintf(stderr, "sortof-palscore: not enough JPEG files\n"); return 1; }

    /* neighbour distances */
    {
        float *d = malloc(nvalid * sizeof(float));
        size_t m = 0, prev = (size_t)-1;
        for (size_t i = 0; i < n; i++) {
            if (!sg[i].valid) continue;
            if (prev != (size_t)-1) d[m++] = sig_dist(&sg[prev], &sg[i]);
            prev = i;
        }
        qsort(d, m, sizeof(float), cmp_f);
        printf("distance between neighbouring files (%%):  median %.2f  90th %.2f  99th %.2f  99.9th %.2f  max %.2f\n",
               100 * d[m / 2], 100 * d[m * 90 / 100], 100 * d[m * 99 / 100], 100 * d[m * 999 / 1000], 100 * d[m - 1]);
        free(d);
    }
    printf("scenes for each cut level (K=%d):", K);
    static const double lv[] = {2, 3, 5, 7.5, 10, 15, 20, 30, 40};
    for (size_t i = 0; i < sizeof(lv) / sizeof(*lv); i++)
        printf("  %g%%:%zu", lv[i], find_scenes(sg, n, lv[i] / 100.0, K, NULL));
    printf("\n");

    /* scenes at the chosen cut */
    size_t ns = find_scenes(sg, n, cutpct / 100.0, K, NULL);
    scene_t *sc = malloc(ns * sizeof(*sc));
    find_scenes(sg, n, cutpct / 100.0, K, sc);
    size_t ones = 0;
    for (size_t s = 0; s < ns; s++) if (sc[s].last == sc[s].first) ones++;
    printf("cut %.1f%%: %zu scenes, %.1f files per scene on average, %zu of them one file\n", cutpct, ns, (double)n / ns, ones);
    if (ns > gmax) {
        printf("more than %zu scenes: raise -c (or -g) to look for look-alikes or make an order\n", gmax);
        return 0;
    }

    /* scene signatures: the average of the members */
    sig_t *rep = calloc(ns, sizeof(*rep));
    for (size_t s = 0; s < ns; s++) {
        uint64_t sh[NH] = {0}, sgr[NG] = {0};
        size_t cnt = 0;
        for (size_t i = sc[s].first; i <= sc[s].last; i++) {
            if (!sg[i].valid) continue;
            for (int k = 0; k < NH; k++) sh[k] += sg[i].h[k];
            for (int k = 0; k < NG; k++) sgr[k] += sg[i].g[k];
            cnt++;
        }
        if (!cnt) continue;
        for (int k = 0; k < NH; k++) rep[s].h[k] = (uint16_t)(sh[k] / cnt);
        for (int k = 0; k < NG; k++) rep[s].g[k] = (uint8_t)(sgr[k] / cnt);
        rep[s].valid = 1;
    }

    /* the most similar earlier (not neighbouring) scene for every scene */
    float *bestd = malloc(ns * sizeof(float));
    size_t *bestt = malloc(ns * sizeof(size_t));
    size_t within[4] = {0, 0, 0, 0}, considered = 0;
    float *back = malloc(ns * sizeof(float));
    size_t nb = 0;
    for (size_t s = 0; s < ns; s++) {
        bestd[s] = 2.0f; bestt[s] = (size_t)-1;
        if (s >= 2 && rep[s].valid)
            for (size_t t = 0; t + 1 < s; t++) {
                if (!rep[t].valid) continue;
                float d = sig_dist(&rep[s], &rep[t]);
                if (d < bestd[s]) { bestd[s] = d; bestt[s] = t; }
            }
        if (bestt[s] != (size_t)-1) {
            considered++;
            if (bestd[s] <= 0.02f) within[0]++;
            if (bestd[s] <= 0.05f) within[1]++;
            if (bestd[s] <= 0.10f) within[2]++;
            if (bestd[s] <= 0.20f) within[3]++;
            if (bestd[s] <= 0.10f) back[nb++] = (float)(sc[s].first - sc[bestt[s]].last);
        }
    }
    if (considered) {
        printf("scenes with a look-alike earlier (not the neighbour), of %zu: within 2%%: %zu (%.1f%%)  5%%: %zu (%.1f%%)  10%%: %zu (%.1f%%)  20%%: %zu (%.1f%%)\n",
               considered, within[0], 100.0 * within[0] / considered, within[1], 100.0 * within[1] / considered,
               within[2], 100.0 * within[2] / considered, within[3], 100.0 * within[3] / considered);
        if (nb) {
            qsort(back, nb, sizeof(float), cmp_f);
            printf("files back to the look-alike (within 10%%):  median %.0f  90th %.0f  max %.0f\n", back[nb / 2], back[nb * 9 / 10], back[nb - 1]);
        }
    }
    if (scenepath) {
        FILE *vf = fopen(scenepath, "w");
        if (vf) {
            fprintf(vf, "scene\tfirst\tfiles\tnearest_earlier\tdist_pct\tfiles_back\n");
            for (size_t s = 0; s < ns; s++) {
                if (bestt[s] == (size_t)-1) fprintf(vf, "%zu\t%zu\t%zu\t-\t-\t-\n", s, sc[s].first + 1, sc[s].last - sc[s].first + 1);
                else fprintf(vf, "%zu\t%zu\t%zu\t%zu\t%.2f\t%zu\n", s, sc[s].first + 1, sc[s].last - sc[s].first + 1, bestt[s], 100 * bestd[s], sc[s].first - sc[bestt[s]].last);
            }
            fclose(vf);
        }
    }

    /* scene order */
    if (orderpath) {
        char *visited = calloc(ns, 1);
        size_t *rem = malloc(ns * sizeof(size_t)), *pos = malloc(ns * sizeof(size_t));
        size_t nr = 0;
        for (size_t s = 0; s < ns; s++) { rem[nr] = s; pos[s] = nr; nr++; }
        FILE *of = fopen(orderpath, "w");
        if (!of) { fprintf(stderr, "sortof-palscore: cannot write %s: %s\n", orderpath, strerror(errno)); return 1; }
        pin_write(of);
        size_t *ord = malloc(n * sizeof(size_t)), nord = 0;
        if (!ord) { fprintf(stderr, "sortof-palscore: out of memory\n"); return 1; }
        size_t cur = 0, nn = 0, moves = 0, trans = 0;
        double movesum = 0;
        for (size_t step = 0; step < ns; step++) {
            for (size_t i = sc[cur].first; i <= sc[cur].last; i++) if (sg[i].valid) ord[nord++] = i;   /* non-pictures go last */
            visited[cur] = 1;
            size_t p = pos[cur], lastr = rem[--nr];
            rem[p] = lastr; pos[lastr] = p;
            if (!nr) break;
            while (visited[nn]) nn++;
            size_t best = (size_t)-1;
            float bd = 2.0f;
            for (size_t r = 0; r < nr; r++) {
                float d = sig_dist(&rep[cur], &rep[rem[r]]);
                if (d < bd || (d == bd && rem[r] < best)) { bd = d; best = rem[r]; }
            }
            float dn = sig_dist(&rep[cur], &rep[nn]);
            size_t pick = nn;
            if (best != nn && bd * 100 <= jpct && dn * 100 > bd * 100 + bpct) pick = best;
            trans++;
            if (pick != nn) { moves++; movesum += bd; }
            cur = pick;
        }
        /* -D: files that are byte for byte the same go right behind their first instance in this order */
        size_t moved = 0;
        if (dupchain) {
            size_t *cid = malloc(n * sizeof(size_t)), *srt = malloc(n * sizeof(size_t)), nh = 0;
            size_t *nxt = malloc(n * sizeof(size_t)), *lastm = malloc(n * sizeof(size_t)), *firstp = malloc(n * sizeof(size_t));
            if (!cid || !srt || !nxt || !lastm || !firstp) { fprintf(stderr, "sortof-palscore: out of memory\n"); return 1; }
            for (size_t i = 0; i < n; i++) { cid[i] = i; if (sg[i].hok) srt[nh++] = i; }
            g_dsig = sg;
            qsort(srt, nh, sizeof(size_t), cmp_dup);
            size_t unverified = 0;
            for (size_t a = 0; a < nh;) {
                size_t b = a + 1;
                while (b < nh && sg[srt[b]].fhash == sg[srt[a]].fhash && sg[srt[b]].fsize == sg[srt[a]].fsize) b++;
                if (b - a > 1) {
                    size_t reps[64], nr2 = 0;
                    for (size_t m = a; m < b; m++) {
                        size_t f = srt[m];
                        int hit = 0;
                        for (size_t r = 0; r < nr2; r++)
                            if (files_equal(names[reps[r]], names[f])) { cid[f] = reps[r]; hit = 1; break; }
                        if (!hit) { if (nr2 < 64) reps[nr2++] = f; else unverified++; }
                    }
                }
                a = b;
            }
            if (unverified) fprintf(stderr, "sortof-palscore: %zu files not compared (too many different files with one hash)\n", unverified);
            for (size_t i = 0; i < n; i++) { nxt[i] = (size_t)-1; lastm[i] = (size_t)-1; firstp[i] = (size_t)-1; }
            for (size_t p = 0; p < nord; p++) {
                size_t f = ord[p], c = cid[f];
                if (firstp[c] == (size_t)-1) { firstp[c] = p; lastm[c] = f; }
                else { nxt[lastm[c]] = f; lastm[c] = f; moved++; }
            }
            for (size_t p = 0; p < nord; p++) {
                size_t f = ord[p];
                if (firstp[cid[f]] != p) continue;
                for (size_t g = f; g != (size_t)-1; g = nxt[g]) fprintf(of, "%s\n", names[g]);
            }
            free(cid); free(srt); free(nxt); free(lastm); free(firstp);
        } else {
            for (size_t p = 0; p < nord; p++) fprintf(of, "%s\n", names[ord[p]]);
        }
        {   /* files without a signature (specs.txt, other non-JPEGs, unreadable pictures): at the end, in input order */
            size_t tail = 0;
            for (size_t i = 0; i < n; i++) if (!sg[i].valid) { fprintf(of, "%s\n", names[i]); tail++; }
            if (tail) printf("%zu file%s without a picture signature (specs.txt etc.) placed at the end\n", tail, tail == 1 ? "" : "s");
        }
        free(ord);
        if (fclose(of) != 0) { fprintf(stderr, "sortof-palscore: error writing %s\n", orderpath); return 1; }
        if (dupchain) printf("duplicates: %zu files moved behind an earlier identical file\n", moved);
        printf("order: %zu scenes, %zu of %zu steps jumped away from the natural next scene (mean distance %.2f%%) -> %s\n",
               ns, moves, trans, moves ? 100 * movesum / moves : 0.0, orderpath);
        free(visited); free(rem); free(pos);
    }
    return 0;
}
