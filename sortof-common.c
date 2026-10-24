/*
 * sortof-common.c - shared by the sortof-suite programs (see sortof-common.h)
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

/* ---------------------------------------------------------------- */
/* Progress bar (stderr only, throttled to ~10 updates/sec)         */
/* ---------------------------------------------------------------- */

void progress_bar(const char *label, size_t done, size_t total) {
    static struct timespec last_print;
    static int have_last = 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (have_last && done != total) {
        double dt = (double)(now.tv_sec - last_print.tv_sec) +
                    (double)(now.tv_nsec - last_print.tv_nsec) / 1e9;
        if (dt < 0.1) return; /* throttle so huge n doesn't flood stderr */
    }
    last_print = now;
    have_last = 1;

    const int width = 30;
    double frac = total ? (double)done / (double)total : 1.0;
    if (frac > 1.0) frac = 1.0;
    int filled = (int)(frac * width);

    fprintf(stderr, "\r%-10s [", label);
    for (int i = 0; i < width; i++) fputc(i < filled ? '#' : '-', stderr);
    fprintf(stderr, "] %5.1f%% (%zu/%zu)", frac * 100.0, done, total);
    if (done >= total) fputc('\n', stderr);
    fflush(stderr);
}


/* ---------------------------------------------------------------- */
/* Natural orderings (-n, -e): no hashing, just a sort              */
/* ---------------------------------------------------------------- */

/* ASCII only, on purpose: the order must not depend on the locale, so the
 * same files give the same list on every machine. */
static inline int is_digit_c(unsigned char c) { return c >= '0' && c <= '9'; }
static inline unsigned char lower_c(unsigned char c) { return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c; }

/* Natural comparison of two byte ranges: runs of digits compare by value
 * (2 before 10, 007 equals 7), everything else bytewise, optionally
 * ignoring ASCII case. <0, 0 or >0. */
static int nat_cmp_n(const char *a, size_t al, const char *b, size_t bl, int fold) {
    size_t i = 0, j = 0;
    while (i < al && j < bl) {
        unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[j];
        if (is_digit_c(ca) && is_digit_c(cb)) {
            size_t ie = i, je = j, zi, zj, la, lb;
            int c;
            while (ie < al && is_digit_c((unsigned char)a[ie])) ie++;
            while (je < bl && is_digit_c((unsigned char)b[je])) je++;
            zi = i; zj = j;
            while (zi + 1 < ie && a[zi] == '0') zi++;      /* leading zeros do not count */
            while (zj + 1 < je && b[zj] == '0') zj++;
            la = ie - zi; lb = je - zj;
            if (la != lb) return la < lb ? -1 : 1;         /* more digits = bigger */
            c = memcmp(a + zi, b + zj, la);
            if (c) return c < 0 ? -1 : 1;
            i = ie; j = je;
        } else {
            if (fold) { ca = lower_c(ca); cb = lower_c(cb); }
            if (ca != cb) return ca < cb ? -1 : 1;
            i++; j++;
        }
    }
    if (i < al) return 1;
    if (j < bl) return -1;
    return 0;
}

/* Natural comparison of two paths one directory level at a time, so a
 * folder's files stay together and names sort among their siblings the
 * way a directory listing would. Repeated slashes count as one. */
static int path_cmp_n(const char *a, size_t al, const char *b, size_t bl) {
    size_t i = 0, j = 0;
    for (;;) {
        size_t ie, je;
        int c;
        while (i < al && a[i] == '/') i++;
        while (j < bl && b[j] == '/') j++;
        if (i >= al || j >= bl) break;
        ie = i; je = j;
        while (ie < al && a[ie] != '/') ie++;
        while (je < bl && b[je] != '/') je++;
        c = nat_cmp_n(a + i, ie - i, b + j, je - j, 0);
        if (c) return c;
        i = ie; j = je;
    }
    if (i < al) return 1;      /* the path that ends first sorts first */
    if (j < bl) return -1;
    return 0;
}

static int cmp_natural(const void *pa, const void *pb) {
    const char *a = *(char *const *)pa, *b = *(char *const *)pb;
    int c = path_cmp_n(a, strlen(a), b, strlen(b));
    return c ? c : strcmp(a, b);                           /* a total order, so the output is repeatable */
}

typedef struct {
    char *path;
    size_t dirlen;       /* bytes up to and including the last '/', 0 if none */
    size_t stemlen;      /* bytes of the file name before its extension */
    size_t extlen;       /* bytes after the last dot of the file name; 0 = no extension */
} fkey_t;

/* The extension is what follows the LAST dot of the file name. A leading dot
 * (.hidden) or a trailing one (name.) does not make an extension. */
static void fkey_init(fkey_t *k, char *path) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    k->path = path;
    k->dirlen = (size_t)(base - path);
    if (dot && dot != base && dot[1]) {
        k->stemlen = (size_t)(dot - base);
        k->extlen = strlen(dot + 1);
    } else {
        k->stemlen = strlen(base);
        k->extlen = 0;
    }
}

static int cmp_by_extension(const void *pa, const void *pb) {
    const fkey_t *a = pa, *b = pb;
    const char *ab = a->path + a->dirlen, *bb = b->path + b->dirlen;
    int c = nat_cmp_n(ab + a->stemlen + (a->extlen ? 1 : 0), a->extlen,
                      bb + b->stemlen + (b->extlen ? 1 : 0), b->extlen, 1);    /* extension, ignoring case */
    if (c) return c;
    c = nat_cmp_n(ab, a->stemlen, bb, b->stemlen, 1);                          /* then the name, naturally */
    if (c) return c;
    c = path_cmp_n(a->path, a->dirlen, b->path, b->dirlen);                    /* then the folder */
    return c ? c : strcmp(a->path, b->path);
}

/* the list put in order in place: by extension (-e) or plain natural (-n) */
void order_natural(char **paths, size_t n, int by_extension) {
    if (!by_extension) {
        qsort(paths, n, sizeof(char *), cmp_natural);
        return;
    }
    fkey_t *keys = malloc(n * sizeof(fkey_t));
    if (!keys) {                                           /* out of memory: fall back to the plain order */
        qsort(paths, n, sizeof(char *), cmp_natural);
        return;
    }
    for (size_t i = 0; i < n; i++) fkey_init(&keys[i], paths[i]);
    qsort(keys, n, sizeof(fkey_t), cmp_by_extension);
    for (size_t i = 0; i < n; i++) paths[i] = keys[i].path;
    free(keys);
}

/* ---------------------------------------------------------------- */
/* Path collection                                                  */
/* ---------------------------------------------------------------- */

void pathlist_init(pathlist_t *pl) {
    pl->paths = NULL;
    pl->count = 0;
    pl->cap = 0;
}

void pathlist_push(pathlist_t *pl, const char *p) {
    if (pl->count == pl->cap) {
        pl->cap = pl->cap ? pl->cap * 2 : 1024;
        pl->paths = realloc(pl->paths, pl->cap * sizeof(char *));
    }
    pl->paths[pl->count++] = strdup(p);
}

void scan_dir(const char *dir, pathlist_t *pl, int quiet) {
    DIR *d = opendir(dir);
    if (!d) {
        if (!quiet) fprintf(stderr, "sortof: warning: cannot open %s: %s\n",
                             dir, strerror(errno));
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        char full[PATH_MAX];
        size_t dl = strlen(dir);
        snprintf(full, sizeof(full), "%s%s%s", dir, (dl && dir[dl - 1] == '/') ? "" : "/", ent->d_name);
        struct stat st;
        if (lstat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scan_dir(full, pl, quiet);
        } else if (S_ISREG(st.st_mode)) {
            pathlist_push(pl, full);
        }
    }
    closedir(d);
}

static char **dedupe_paths;                  /* qsort has no context argument */

static int dedupe_cmp(const void *a, const void *b) {
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    int c = strcmp(dedupe_paths[x], dedupe_paths[y]);
    return c ? c : (x < y ? -1 : (x > y));
}

/* Lists a path once however many times it came up (a directory and a file inside it, a
 * list and a directory, the same file twice), keeping the first and the order of the
 * rest. Only identical path text counts: ./a and a are different strings. Returns how
 * many repeats were dropped. */
size_t pathlist_dedupe(pathlist_t *pl) {
    size_t n = pl->count, removed = 0, w = 0;
    if (n < 2) return 0;
    size_t *idx = malloc(n * sizeof(size_t));
    unsigned char *drop = calloc(n, 1);
    if (!idx || !drop) { free(idx); free(drop); return 0; }
    for (size_t i = 0; i < n; i++) idx[i] = i;
    dedupe_paths = pl->paths;
    qsort(idx, n, sizeof(size_t), dedupe_cmp);
    for (size_t k = 1; k < n; k++)
        if (!strcmp(pl->paths[idx[k]], pl->paths[idx[k - 1]])) { drop[idx[k]] = 1; removed++; }
    for (size_t i = 0; i < n; i++) {
        if (drop[i]) free(pl->paths[i]);
        else pl->paths[w++] = pl->paths[i];
    }
    pl->count = w;
    free(idx);
    free(drop);
    return removed;
}

void read_stdin_list(pathlist_t *pl) {
    char line[PATH_MAX];
    while (fgets(line, sizeof(line), stdin)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (l) pathlist_push(pl, line);
    }
}


static char **g_pins;
static size_t g_npins;
void pin_add(const char *path) {
    for (size_t i = 0; i < g_npins; i++) if (!strcmp(g_pins[i], path)) return;      /* once */
    char **n = realloc(g_pins, (g_npins + 1) * sizeof(*g_pins));
    if (!n) { fprintf(stderr, "sortof: out of memory\n"); exit(1); }
    g_pins = n;
    g_pins[g_npins] = strdup(path);
    if (!g_pins[g_npins]) { fprintf(stderr, "sortof: out of memory\n"); exit(1); }
    g_npins++;
}
/* Is this path pinned? A cdcstore.idx is pinned by what it is: the first time one is seen it is added to the pins. */
int pin_is(const char *path) {
    for (size_t i = 0; i < g_npins; i++) if (!strcmp(g_pins[i], path)) return 1;
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    if (!strcmp(b, "cdcstore.idx")) { pin_add(path); return 1; }
    return 0;
}
size_t pin_count(void) { return g_npins; }
void pin_write(FILE *out) {
    for (size_t i = 0; i < g_npins; i++) fprintf(out, "%s\n", g_pins[i]);
}
static void pin_filter(pathlist_t *pl) {
    size_t w = 0;
    for (size_t i = 0; i < pl->count; i++) {
        if (pin_is(pl->paths[i])) { free(pl->paths[i]); continue; }
        pl->paths[w++] = pl->paths[i];
    }
    pl->count = w;
}

int collect_sources(const char *prog, int nsrc, char **src, pathlist_t *pl, int quiet) {
    int stdin_given = 0;
    for (int a = 0; a < nsrc; a++) {
        struct stat st;
        if (!strcmp(src[a], "-")) {
            if (stdin_given) {
                fprintf(stderr, "%s: - (the list on stdin) is given twice\n", prog);
                return 1;
            }
            stdin_given = 1;
            continue;
        }
        if (stat(src[a], &st) != 0) {
            fprintf(stderr, "%s: cannot stat %s: %s\n", prog, src[a], strerror(errno));
            return 1;
        }
        if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
            fprintf(stderr, "%s: %s is neither a directory nor a regular file\n", prog, src[a]);
            return 1;
        }
    }
    pathlist_init(pl);
    for (int a = 0; a < nsrc; a++) {
        struct stat st;
        if (!strcmp(src[a], "-")) read_stdin_list(pl);
        else if (stat(src[a], &st) == 0 && S_ISDIR(st.st_mode)) scan_dir(src[a], pl, quiet);
        else pathlist_push(pl, src[a]);
    }
    size_t repeats = pathlist_dedupe(pl);
    if (repeats && !quiet)
        fprintf(stderr, "%s: note: %zu path%s came up more than once and %s listed once\n", prog,
                repeats, repeats == 1 ? "" : "s", repeats == 1 ? "is" : "are");
    pin_filter(pl);
    return 0;
}

int write_list(const char *prog, pathlist_t *pl, const char *out_path, int quiet) {
    FILE *out = stdout;
    if (out_path) {
        out = fopen(out_path, "w");
        if (!out) {
            fprintf(stderr, "%s: cannot open %s for writing: %s\n", prog, out_path, strerror(errno));
            return 1;
        }
    }
    pin_write(out);
    for (size_t i = 0; i < pl->count; i++) fprintf(out, "%s\n", pl->paths[i]);
    fflush(out);
    if (out_path) {
        fclose(out);
        if (!quiet) fprintf(stderr, "%s: wrote %zu file path(s) to %s\n", prog, pl->count + pin_count(), out_path);
    }
    for (size_t i = 0; i < pl->count; i++) free(pl->paths[i]);
    free(pl->paths);
    pl->paths = NULL; pl->count = 0;
    return 0;
}

static void natural_usage(const char *prog, int by_ext) {
    fprintf(stderr,
        "Usage: %s [options] <source>...\n"
        "       find /some/dir -type f | %s [options] -\n"
        "A source is a directory (scanned, recursively), a regular file (listed as it is), or -\n"
        "(paths on stdin, once). A path that comes up more than once is listed once.\n\n"
        "Writes the file list in %s, no hashing.\n\n"
        "Options:\n"
        "  -p PATH  pin PATH: it comes first in the output and is never sorted (repeatable)\n"
        "  -f PATH  write the list to PATH instead of stdout\n"
        "  -q       quiet\n"
        "  -h       this help\n",
        prog, prog,
        by_ext ? "natural order by extension: by extension (case ignored, none first), then\n"
                 "file name, then folder, so same-named files in different folders sit together"
               : "natural order: path by path, one folder level at a time, digit runs by value\n"
                 "(f2 before f10); every folder's files stay together");
}

int natural_main(int argc, char **argv, const char *prog, int by_extension) {
    int quiet = 0, opt;
    const char *out_path = NULL;
    while ((opt = getopt(argc, argv, "p:f:qh")) != -1) {
        switch (opt) {
            case 'p': pin_add(optarg); break;
            case 'f': out_path = optarg; break;
            case 'q': quiet = 1; break;
            case 'h': natural_usage(prog, by_extension); return 0;
            default: natural_usage(prog, by_extension); return 1;
        }
    }
    if (optind >= argc) { natural_usage(prog, by_extension); return 1; }
    pathlist_t pl;
    if (collect_sources(prog, argc - optind, argv + optind, &pl, quiet)) return 1;
    if (pl.count == 0) {
        if (!quiet) fprintf(stderr, "%s: no files found\n", prog);
        return 0;
    }
    order_natural(pl.paths, pl.count, by_extension);
    return write_list(prog, &pl, out_path, quiet);
}
