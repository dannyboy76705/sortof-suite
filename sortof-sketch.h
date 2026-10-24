/*
 * sortof-sketch.h - the MinHash sketch used by sortof-minhash and sortof-cdcsort
 * (128 minimums over 8-byte shingles; the score of two sketches is how many of the
 * 128 minimums match). Part of the sortof-suite.
 *
 * MIT License, Copyright (c) 2026 Daniel Lee Witzel
 */
#ifndef SORTOF_SKETCH_H
#define SORTOF_SKETCH_H
#include <stdint.h>
#include <string.h>
#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif

/* ---------------------------------------------------------------- */
/* MinHash                                                          */
/*                                                                   */
/* LZ-family compressors (zstd, xz) save space where files share    */
/* literal, reusable byte sequences. MinHash estimates how much two */
/* files share: it slides an 8-byte shingle window across each and  */
/* tracks, for NUM_FUNCS independent hash functions, the minimum    */
/* hash value seen. Two files sharing more literal shingles are     */
/* more likely to land on the same minimum for a given function;    */
/* the similarity score is just how many of the NUM_FUNCS minimums  */
/* match exactly (0..NUM_FUNCS, higher = more alike) -- the         */
/* standard MinHash/Jaccard estimator.                               */
/*                                                                   */
/* MinHash as a TECHNIQUE is standard, but this specific choice of  */
/* shingle size and function count is ours: treat the scores as a   */
/* heuristic. What validates it is compression: on a 12.6 GB DOS    */
/* collection its order gave archives 1.17% smaller than nilsimsa's */
/* under zstd -9 and 1.26% smaller under xz.                        */
/* ---------------------------------------------------------------- */

#define MINHASH_SHINGLE_K   8
#ifndef MINHASH_NUM_FUNCS
#define MINHASH_NUM_FUNCS   128         /* 32 before; 128 ordered better in every test (see below). Build with -DMINHASH_NUM_FUNCS=N; a multiple of 8 */
#endif
#define MINHASH_DIGEST_BYTES (MINHASH_NUM_FUNCS * 4)

/* NUM_FUNCS distinct odd 64-bit seeds, generated from the golden-
 * ratio constant used in splitmix64 (public domain): reproducible
 * from source, and the first N are the same whatever the count.
 *
 * 128 instead of 32: the score is a count of matching minimums, so its error falls as 1/sqrt(N).
 * On 1,025 synthetic files (25 families of related versions, edits at 0.05%-3%), 128 beat 32 on
 * three shuffled inputs and one fixed one: mean true Jaccard of neighbouring files 0.781 -> 0.791,
 * xz output 0.2%-1.3% smaller. The price is 4x the hashing work (the hash loop runs N
 * functions per byte) and 4x the digest memory, and a compare that is 4x the bytes. Not yet
 * measured on real frames. Build with -DMINHASH_NUM_FUNCS=32 for the old speed. Filled once
 * by minhash_seeds_init() before any thread starts, then only read. */
#define MH_SEED(i) ((uint64_t)(2 * (uint64_t)(i) + 1) * 0x9E3779B97F4A7C15ULL)
static uint64_t minhash_seeds[MINHASH_NUM_FUNCS];
static inline void minhash_seeds_init(void) {
    for (int i = 0; i < MINHASH_NUM_FUNCS; i++) minhash_seeds[i] = MH_SEED(i);
}
_Static_assert(MINHASH_NUM_FUNCS % 8 == 0 && MINHASH_NUM_FUNCS >= 8, "the digest is compared up to eight lanes at a time");
_Static_assert(MINHASH_SHINGLE_K == 8, "the shingle is the 64-bit rolling window in minhash_t");

/* splitmix64's finalizer/avalanche step (public domain) -- a fast,
 * well-mixing 64-bit integer hash, used here to derive NUM_FUNCS
 * roughly-independent hash functions from one 8-byte shingle. */
static inline uint64_t mix64(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

typedef struct {
    /* The last 8 bytes read, the newest in the top byte. Once 8 have been read the oldest is the low
     * byte, which is the same 8 bytes, in the same order, that a byte array holding the window and
     * copied into an integer on a little-endian machine gives: so the shingle values (and every
     * digest) are those the byte-array version produced, and now the same on any machine. */
    uint64_t window;
    int filled; /* bytes read so far, caps at K */
    uint32_t mins[MINHASH_NUM_FUNCS];
} minhash_t;

static inline void minhash_init(minhash_t *m) {
    m->window = 0;
    m->filled = 0;
    for (int i = 0; i < MINHASH_NUM_FUNCS; i++) m->mins[i] = UINT32_MAX;
}

/* Written so a compiler can do well with it, and checked against the plain byte-array version
 * (hashcheck.c): the state is worked on in local copies, because a char pointer may alias anything
 * and with the minimums in the struct the compiler must assume every store to one could change the
 * next byte to read, so it could neither keep them in registers nor vectorize the loop over them;
 * the window is one integer shifted along, not an array moved down a byte at a time; and the
 * minimum is a select, not a branch, so the NUM_FUNCS lanes are independent. */
static inline void minhash_update(minhash_t *m, const unsigned char *data, size_t len) {
    uint64_t w = m->window;
    int filled = m->filled;
    uint32_t mins[MINHASH_NUM_FUNCS];
    memcpy(mins, m->mins, sizeof mins);
    for (size_t k = 0; k < len; k++) {
        w = (w >> 8) | ((uint64_t)data[k] << 56);
        if (filled < MINHASH_SHINGLE_K && ++filled < MINHASH_SHINGLE_K) continue; /* window not full yet */

        const uint64_t base = mix64(w);
        for (int i = 0; i < MINHASH_NUM_FUNCS; i++) {
            const uint32_t v = (uint32_t)mix64(base ^ minhash_seeds[i]);
            mins[i] = v < mins[i] ? v : mins[i];
        }
    }
    m->window = w;
    m->filled = filled;
    memcpy(m->mins, mins, sizeof mins);
}

/* -I: the file is a list of little-endian 32-bit numbers (chunk ids from cdcmaster -I); each number is
 * one set element, hashed with every function. The element order does not matter. */
static int id_mode __attribute__((unused)) = 0;
static inline void minhash_update_ids(minhash_t *m, const uint32_t *ids, size_t n) {
    uint32_t mins[MINHASH_NUM_FUNCS];
    memcpy(mins, m->mins, sizeof mins);
    for (size_t k = 0; k < n; k++) {
        const uint64_t base = mix64((uint64_t)ids[k] + 0x9E3779B97F4A7C15ULL);
        for (int i = 0; i < MINHASH_NUM_FUNCS; i++) {
            const uint32_t v = (uint32_t)mix64(base ^ minhash_seeds[i]);
            mins[i] = v < mins[i] ? v : mins[i];
        }
    }
    memcpy(m->mins, mins, sizeof mins);
}

static inline void minhash_digest(const minhash_t *m, unsigned char out[MINHASH_DIGEST_BYTES]) {
    memcpy(out, m->mins, MINHASH_DIGEST_BYTES);
}

/* Similarity score: number of matching minimums out of NUM_FUNCS
 * (0..NUM_FUNCS, higher = more similar). Files shorter than the shingle
 * size never fill the window and keep all-UINT32_MAX digests, so
 * two such tiny/empty files will compare as maximally similar to
 * each other -- a deliberate, documented edge case, not a bug. */
#if defined(__AVX2__)
/* Eight lanes at a time (built with -mavx2 or -march=native, where the compiler defines
 * __AVX2__). Integer-exact: the score is identical to the SSE2 and scalar versions. */
static inline int minhash_compare(const unsigned char *d1, const unsigned char *d2) {
    __m256i acc = _mm256_setzero_si256();
    for (int i = 0; i < MINHASH_DIGEST_BYTES / 32; i++) {
        __m256i a = _mm256_loadu_si256((const __m256i *)(d1 + 32 * i));
        __m256i b = _mm256_loadu_si256((const __m256i *)(d2 + 32 * i));
        acc = _mm256_sub_epi32(acc, _mm256_cmpeq_epi32(a, b));
    }
    __m128i s = _mm_add_epi32(_mm256_castsi256_si128(acc), _mm256_extracti128_si256(acc, 1));
    s = _mm_add_epi32(s, _mm_srli_si128(s, 8));
    s = _mm_add_epi32(s, _mm_srli_si128(s, 4));
    return _mm_cvtsi128_si32(s);
}
#elif defined(__SSE2__)
/* Same count, four lanes at a time: _mm_cmpeq_epi32 gives -1 in each
 * lane whose two minimums are equal, and subtracting that from the
 * running total adds one. Integer-exact, so the score is identical to
 * the scalar loop below. */
static inline int minhash_compare(const unsigned char *d1, const unsigned char *d2) {
    __m128i acc = _mm_setzero_si128();
    for (int i = 0; i < MINHASH_DIGEST_BYTES / 16; i++) {
        __m128i a = _mm_loadu_si128((const __m128i *)(d1 + 16 * i));
        __m128i b = _mm_loadu_si128((const __m128i *)(d2 + 16 * i));
        acc = _mm_sub_epi32(acc, _mm_cmpeq_epi32(a, b));
    }
    acc = _mm_add_epi32(acc, _mm_srli_si128(acc, 8));
    acc = _mm_add_epi32(acc, _mm_srli_si128(acc, 4));
    return _mm_cvtsi128_si32(acc);
}
#else
static inline int minhash_compare(const unsigned char *d1, const unsigned char *d2) {
    uint32_t a[MINHASH_NUM_FUNCS], b[MINHASH_NUM_FUNCS];
    memcpy(a, d1, MINHASH_DIGEST_BYTES);
    memcpy(b, d2, MINHASH_DIGEST_BYTES);
    int matches = 0;
    for (int i = 0; i < MINHASH_NUM_FUNCS; i++) {
        if (a[i] == b[i]) matches++;
    }
    return matches;
}
#endif

#endif
