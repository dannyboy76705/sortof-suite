/*
 * sortof-common.h - what the sortof-suite programs share: reading the list of
 * sources, natural orders, a progress bar.
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
#ifndef SORTOF_COMMON_H
#define SORTOF_COMMON_H
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>

/* 64-bit FNV-1a, computed alongside the MinHash digest over exactly
 * the same bytes (whole file normally, or just Cluster payloads when
 * -m is active). This is for EXACT duplicate detection -- MinHash is
 * a fuzzy/locality-sensitive hash and isn't meant to prove equality,
 * only similarity. Paired with a length check, FNV-1a collisions on
 * real (non-adversarial) file content are negligible in practice. */
#define FNV_OFFSET_64 0xcbf29ce484222325ULL
#define FNV_PRIME_64  0x100000001b3ULL

static inline uint64_t fnv1a64_update(uint64_t h, const unsigned char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= FNV_PRIME_64;
    }
    return h;
}


typedef struct {
    char **paths;
    size_t count;
    size_t cap;
} pathlist_t;

void progress_bar(const char *label, size_t done, size_t total);

void pathlist_init(pathlist_t *pl);
void pathlist_push(pathlist_t *pl, const char *p);
void scan_dir(const char *dir, pathlist_t *pl, int quiet);
size_t pathlist_dedupe(pathlist_t *pl);
void read_stdin_list(pathlist_t *pl);

/* Checks every source (directory, regular file, or - for a list on stdin, once), reads
 * them all into pl and drops repeated paths. Returns 0, or 1 after saying what is wrong
 * (nothing has been written yet). prog is the program name for messages. */
int collect_sources(const char *prog, int nsrc, char **src, pathlist_t *pl, int quiet);

void order_natural(char **paths, size_t n, int by_extension);

/* PINNED FILES: paths that lead the output exactly as given and are never sorted, hashed or read. A cdcstore repository's
 * index is pinned by itself: any input path whose file name is cdcstore.idx (cdcstore-list -k prints it first) comes out
 * first, so `cdcstore-list -k REPO | sortof-cdcsort -` is the whole job. -p FILE (any number) pins other paths, such as
 * an mjpegstore set's specs.txt. collect_sources leaves pinned paths out of the list and every program writes them first
 * (pin_write): the -p ones in the order given, then the cdcstore.idx ones in the order they came. Matching is on the
 * path as written. */
void pin_add(const char *path);
int  pin_is(const char *path);
size_t pin_count(void);
void pin_write(FILE *out);

/* Writes the list to out_path (or stdout) and frees it. 0 or 1. */
int write_list(const char *prog, pathlist_t *pl, const char *out_path, int quiet);

/* The whole program of sortof-natural (by_extension 0) and sortof-extension (1). */
int natural_main(int argc, char **argv, const char *prog, int by_extension);

#endif
