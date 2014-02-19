#define _POSIX_C_SOURCE 199309L // Otherwise nanosleep is not included
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <byteswap.h>
#include "tigerkdf.h"
#include "tigerkdf-impl.h"

// This include code copied from blake2s.c
#include <emmintrin.h>
#if defined(HAVE_SSSE3)
#include <tmmintrin.h>
#endif
#if defined(HAVE_SSE41)
#include <smmintrin.h>
#endif
#if defined(HAVE_AVX)
#include <immintrin.h>
#endif
#if defined(HAVE_XOP)
#include <x86intrin.h>
#endif

// This structure is shared among all threads.
struct TigerKDFCommonDataStruct {
    uint32_t *mem;
    uint32_t *multHashes;
    uint8_t *hash;
    uint32_t hashSize;
    uint32_t parallelism;
    uint32_t blocklen;
    uint32_t subBlocklen;
    uint32_t numblocks;
    uint32_t repetitions;
    uint32_t multipliesPerBlock;
    volatile uint32_t completedMultiplies;
};

// This structure is unique to each memory-hashing thread
struct TigerKDFContextStruct {
    struct TigerKDFCommonDataStruct *common;
    uint32_t p; // This is the memory-thread number
};

// Perform one crypt-strength hash on a 32-byte state.
static inline void hashState(uint32_t state[32]) {
    uint8_t buf[32];
    be32enc_vect(buf, state, 32);
    H(buf, 32, buf, 32, NULL, 0);
    be32dec_vect(state, buf, 32);
}

// Do low-bandwidth multplication hashing.
static void *multHash(void *commonPtr) {
    struct TigerKDFCommonDataStruct *c = (struct TigerKDFCommonDataStruct *)commonPtr;

    uint8_t *hash = c->hash;
    uint32_t hashSize = c->hashSize;
    uint32_t numblocks = c->numblocks;
    uint32_t repetitions = c->repetitions;
    uint32_t *multHashes = c->multHashes;
    uint32_t multipliesPerBlock = c->multipliesPerBlock;

    uint8_t s[sizeof(uint32_t)];
    be32enc(s, c->parallelism);
    uint8_t threadKey[32];
    uint32_t state[8];
    H(threadKey, 32, hash, hashSize, s, sizeof(uint32_t));
    be32dec_vect(state, threadKey, 32);
    uint32_t numMults = 0;
    for(uint32_t i = 0; i < numblocks*2; i++) {
        uint32_t j;
        for(j = 0; j < multipliesPerBlock * repetitions; j += 8) {
            // This is reversible, and will not lose entropy
            state[0] = (state[0]*(state[1] | 1)) ^ (state[2] >> 1);
            state[1] = (state[1]*(state[2] | 1)) ^ (state[3] >> 1);
            state[2] = (state[2]*(state[3] | 1)) ^ (state[4] >> 1);
            state[3] = (state[3]*(state[4] | 1)) ^ (state[5] >> 1);
            state[4] = (state[4]*(state[5] | 1)) ^ (state[6] >> 1);
            state[5] = (state[5]*(state[6] | 1)) ^ (state[7] >> 1);
            state[6] = (state[6]*(state[7] | 1)) ^ (state[0] >> 1);
            state[7] = (state[7]*(state[0] | 1)) ^ (state[1] >> 1);
            numMults += 8;
        }
        // Apply a crypt-strength hash to the state and broadcast the result
        hashState(state);
        for(j = 0; j < 8; j++) {
            multHashes[8*c->completedMultiplies + j] = state[j];
        }
        (c->completedMultiplies)++;
    }
    printf("total multiplies:%u\n", numMults);
    pthread_exit(NULL);
}

// Add the last hashed data from each memory thread into the result and apply a
// crypto-strength hash to it.
static void combineHashes(uint8_t *hash, uint32_t hashSize, uint32_t *mem, uint32_t blocklen,
        uint32_t numblocks, uint32_t parallelism) {
    uint8_t data[hashSize];
    
    for(uint32_t p = 0; p < parallelism; p++) {
        uint64_t pos = 2*(p+1)*numblocks*(uint64_t)blocklen - hashSize/sizeof(uint32_t);
        be32enc_vect(data, mem + pos, hashSize);
        uint32_t i;
        for(i = 0; i < hashSize; i++) {
            hash[i] += data[i];
        }
    }
    H(hash, hashSize, hash, hashSize, NULL, 0);
}

// Convert a uint32_t[8] to two __m128i values.
static void convStateFromUint32ToM128i(uint32_t state[8], __m128i *v1, __m128i *v2) {
    *v1 = _mm_set_epi32(state[3], state[2], state[1], state[0]);
    *v2 = _mm_set_epi32(state[7], state[6], state[5], state[4]);
}

// Convert two __m128i to uint32_t[8].
static void convStateFromM128iToUint32(__m128i *v1, __m128i *v2, uint32_t state[8]) {
    uint32_t *p = (uint32_t *)v1;
    uint32_t i;
    for(i = 0; i < 4; i++) {
        state[i] = p[i];
    }
    p = (uint32_t *)v2;
    for(i = 0; i < 4; i++) {
        state[i+4] = p[i];
    }
}

// Hash three blocks together with fast SSE friendly hash function optimized for high memory bandwidth.
// Basically, it does for every 8 words:
//     for(i = 0; i < 8; i++) {
//         state[i] = ROTATE_RIGHT((state[i] + *p++) ^ *f++, 7);
//         *t++ = state[i];
//     
static inline void hashBlocks(uint32_t state[8], uint32_t *mem, uint32_t blocklen, uint32_t subBlocklen,
        uint64_t fromAddr, uint64_t toAddr, uint32_t repetitions) {
    __m128i s1;
    __m128i s2;
    convStateFromUint32ToM128i(state, &s1, &s2);
    uint64_t prevAddr = toAddr - blocklen;
    __m128i *m = (__m128i *)mem;
    uint32_t numSubBlocks = blocklen/subBlocklen;
    uint32_t mask = numSubBlocks - 1;
    __m128i shiftRightVal = _mm_set_epi32(25, 25, 25, 25);
    __m128i shiftLeftVal = _mm_set_epi32(7, 7, 7, 7);
    for(uint32_t r = 0; r < repetitions; r++) {
        __m128i *f = m + fromAddr/4;
        __m128i *t = m + toAddr/4;
        for(uint32_t i = 0; i < numSubBlocks; i++) {
            __m128i *p = m + prevAddr/4 + (subBlocklen/4)*(*(uint32_t *)f & mask);
            for(uint32_t j = 0; j < subBlocklen/8; j++) {
                s1 = _mm_add_epi32(s1, *p++);
                s1 = _mm_xor_si128(s1, *f++);
                // Rotate left 7
                s1 = _mm_or_si128(_mm_srl_epi32(s1, shiftRightVal), _mm_sll_epi32(s1, shiftLeftVal));
                *t++ = s1;
                s2 = _mm_add_epi32(s2, *p++);
                s2 = _mm_xor_si128(s2, *f++);
                // Rotate left 7
                s2 = _mm_or_si128(_mm_srl_epi32(s2, shiftRightVal), _mm_sll_epi32(s2, shiftLeftVal));
                *t++ = s2;
            }
        }
    }
    convStateFromM128iToUint32(&s1, &s2, state);
}

// Hash the multiply chain state into our state.  If the multiplies are falling behind, sleep for a while.
static void hashMultItoState(uint32_t iteration, struct TigerKDFCommonDataStruct *c, uint32_t *state) {
    while(iteration >= c->completedMultiplies) {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 100000; // 0.1ms
        nanosleep(&ts, NULL);
    }
    for(uint32_t i = 0; i < 8; i++) {
        state[i] ^= c->multHashes[iteration*8 + i];
    }
    hashState(state);
}

// Bit-reversal function derived from Catena's version.
uint32_t reverse(uint32_t x, const uint8_t n)
{
    if(n == 0) {
        return 0;
    }
    x = bswap_32(x);
    x = ((x & 0x0f0f0f0f) << 4) | ((x & 0xf0f0f0f0) >> 4);
    x = ((x & 0x33333333) << 2) | ((x & 0xcccccccc) >> 2);
    x = ((x & 0x55555555) << 1) | ((x & 0xaaaaaaaa) >> 1);
    return x >> (32 - n);
}

// Hash memory without doing any password dependent memory addressing to thwart cache-timing-attacks.
// Use Solar Designer's sliding-power-of-two window, with Catena's bit-reversal.
static void *hashWithoutPassword(void *contextPtr) {
    struct TigerKDFContextStruct *ctx = (struct TigerKDFContextStruct *)contextPtr;
    struct TigerKDFCommonDataStruct *c = ctx->common;

    uint32_t *mem = c->mem;
    uint8_t *hash = c->hash;
    uint32_t hashSize = c->hashSize;
    uint32_t p = ctx->p;
    uint32_t blocklen = c->blocklen;
    uint32_t numblocks = c->numblocks;
    uint32_t repetitions = c->repetitions;

    uint64_t start = 2*p*(uint64_t)numblocks*blocklen;
    uint8_t threadKey[blocklen*sizeof(uint32_t)];
    uint8_t s[sizeof(uint32_t)];
    be32enc(s, p);
    H(threadKey, blocklen*sizeof(uint32_t), hash, hashSize, s, sizeof(uint32_t));
    be32dec_vect(mem + start, threadKey, blocklen*sizeof(uint32_t));
    uint32_t state[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    uint32_t mask = 1;
    uint32_t numBits = 0;
    uint64_t toAddr = start + blocklen;
    for(uint32_t i = 1; i < numblocks; i++) {
        if(mask << 1 <= i) {
            mask <<= 1;
            numBits++;
        }
        uint32_t reversePos = reverse(i, numBits);
        if(reversePos + mask < i) {
            reversePos += mask;
        }
        uint64_t fromAddr = start + (uint64_t)blocklen*reversePos;
        hashBlocks(state, mem, blocklen, blocklen, fromAddr, toAddr, repetitions);
        hashMultItoState(i, c, state);
        toAddr += blocklen;
    }
    pthread_exit(NULL);
}

// Hash memory with dependent memory addressing to thwart TMTO attacks.
static void *hashWithPassword(void *contextPtr) {
    struct TigerKDFContextStruct *ctx = (struct TigerKDFContextStruct *)contextPtr;
    struct TigerKDFCommonDataStruct *c = ctx->common;

    uint32_t *mem = c->mem;
    uint32_t parallelism = c->parallelism;
    uint32_t p = ctx->p;
    uint32_t blocklen = c->blocklen;
    uint32_t subBlocklen = c->subBlocklen;
    uint32_t numblocks = c->numblocks;
    uint32_t repetitions = c->repetitions;

    uint64_t start = (2*p + 1)*(uint64_t)numblocks*blocklen;
    uint32_t state[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    uint64_t toAddr = start;
    for(uint32_t i = 0; i < numblocks; i++) {
        uint64_t v = state[0];
        uint64_t v2 = v*v >> 32;
        uint64_t v3 = v*v2 >> 32;
        uint32_t distance = (i + numblocks - 1)*v3 >> 32;
        uint64_t fromAddr;
        if(distance < i) {
            fromAddr = start + (i - 1 - distance)*(uint64_t)blocklen;
        } else {
            uint32_t q = (p + i) % parallelism;
            uint32_t b = numblocks - 1 - (distance - i);
            fromAddr = (2*numblocks*q + b)*(uint64_t)blocklen;
        }
        hashBlocks(state, mem, blocklen, subBlocklen, fromAddr, toAddr, repetitions);
        hashMultItoState(i, c, state);
        toAddr += blocklen;
    }
    pthread_exit(NULL);
}

// The TigerKDF password hashing function.  MemSize is in KiB.
bool TigerKDF(uint8_t *hash, uint32_t hashSize, uint32_t memSize, uint32_t multipliesPerBlock, uint8_t startGarlic,
        uint8_t stopGarlic, uint32_t blockSize, uint32_t subBlockSize, uint32_t parallelism, uint32_t repetitions,
        bool skipLastHash) {
    // Compute sizes
    uint64_t memlen = (1 << 10)*(uint64_t)memSize/sizeof(uint32_t);
    uint32_t blocklen = blockSize/sizeof(uint32_t);
    uint32_t numblocks = (memlen/(2*parallelism*blocklen)) << startGarlic;
    memlen = (2*parallelism*(uint64_t)numblocks*blocklen) << (stopGarlic - startGarlic);
    multipliesPerBlock = 8*(multipliesPerBlock/8);
    if(multipliesPerBlock == 0) {
        multipliesPerBlock = 8;
    }
    // Allocate memory
    uint32_t *mem;
    if(posix_memalign((void *)&mem,  32, memlen*sizeof(uint32_t))) {
        return false;
    }
    pthread_t multThread;
    pthread_t *memThreads = (pthread_t *)malloc(parallelism*sizeof(pthread_t));
    if(memThreads == NULL) {
        return false;
    }
    struct TigerKDFContextStruct *c = (struct TigerKDFContextStruct *)malloc(
            parallelism*sizeof(struct TigerKDFContextStruct));
    if(c == NULL) {
        return false;
    }
    struct TigerKDFCommonDataStruct common;
    uint32_t *multHashes = (uint32_t *)malloc(8*sizeof(uint32_t)*memlen/blocklen);
    if(multHashes == NULL) {
        return false;
    }
    // Fill out the common constant data used in all threads
    common.multHashes = multHashes;
    common.multipliesPerBlock = multipliesPerBlock;
    common.hash = hash;
    common.hashSize = hashSize;
    common.mem = mem;
    common.blocklen = blocklen;
    common.subBlocklen = subBlockSize != 0? subBlockSize/sizeof(uint32_t) : blocklen;
    common.parallelism = parallelism;
    common.repetitions = repetitions;
    common.multHashes = multHashes;
    common.multipliesPerBlock = multipliesPerBlock;
    // Iterate through the levels of garlic
    for(uint8_t i = startGarlic; i <= stopGarlic; i++) {
        common.numblocks = numblocks;
        common.completedMultiplies = 0;
        // Start the multiplication chain hashing thread
        int rc = pthread_create(&multThread, NULL, multHash, (void *)&common);
        if(rc) {
            fprintf(stderr, "Unable to start threads\n");
            return false;
        }
        // Start the memory threads for the first "pure" loop
        uint32_t p;
        for(p = 0; p < parallelism; p++) {
            c[p].common = &common;
            c[p].p = p;
            rc = pthread_create(&memThreads[p], NULL, hashWithoutPassword, (void *)(c + p));
            if(rc) {
                fprintf(stderr, "Unable to start threads\n");
                return false;
            }
        }
        for(p = 0; p < parallelism; p++) {
            (void)pthread_join(memThreads[p], NULL);
        }
        // Start the memory threads for the second "dirty" loop
        for(p = 0; p < parallelism; p++) {
            int rc = pthread_create(&memThreads[p], NULL, hashWithPassword, (void *)(c + p));
            if(rc) {
                fprintf(stderr, "Unable to start threads\n");
                return false;
            }
        }
        for(p = 0; p < parallelism; p++) {
            (void)pthread_join(memThreads[p], NULL);
        }
        (void)pthread_join(multThread, NULL);
        // Combine all the memory thread hashes with a crypto-strength hash
        combineHashes(hash, hashSize, mem, blocklen, numblocks, parallelism);
        // Double the memory for the next round of garlic
        numblocks *= 2;
        if(i < stopGarlic || !skipLastHash) {
            // For server relief mode, skip doing this last hash
            H(hash, hashSize, hash, hashSize, &i, 1);
        }
    }
    // The light is green, the trap is clean
    free(multHashes);
    free(c);
    free(memThreads);
    free(mem);
    return true;
}
