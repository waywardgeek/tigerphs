/*
   TwoCats C API header file

   Written in 2014 by Bill Cox <waywardgeek@gmail.com>

   To the extent possible under law, the author(s) have dedicated all copyright and
   related and neighboring rights to this software to the public domain worldwide. This
   software is distributed without any warranty.

   You should have received a copy of the CC0 Public Domain Dedication along with this
   software. If not, see <http://creativecommons.org/publicdomain/zero/1.0/>.
*/

#include <stdint.h>
#include <stdbool.h>

/*
    This is the TwoCats password hashing scheme.  Most users will find the simple
    TwoCats_HashPassword interface sufficient.  More advanced users who want control over
    both runtime and memory hashing size should use TwoCats_HashPasswordFull.  If you
    know what you are doing and want bare metal control, use TwoCats_HashPasswordExtended.

    For all of these functions, these are the restrictions on sizes:

    1 <= hashSize <= min(8160, blockSize) -- blockSize defaults to 16384
    memCost <= 30
    timeCost <= 30
    multiplies <= 8
    1 <= prallelism <= 255
    startMemCost <= stopMemCost <= 30
    oldMemCost < newMemCost <= 30
    32 <= subBlockSize <= blockSize <= 2^20 -- both must be powers of 2

    NULL values and 0 lengths are legal for all variable sized inputs.

    Preferably, passwords and any other secret data are passed in fixed sized buffers.
    This insures that SHA256 can not leak length information.

    Preferably clearPassword is set to true so that the password buffer can be overwritten
    with 0's at the beginning of hashing rather than by the user afterward.
*/


// The default password hashing interface.  On success, a 32-byte password hash is written,
// and true is returned.  otherwise false is returned, and hash and password are unchanged.
// Each increment of memCost doubles difficulty.  The memory hashed = 2^memCost KiB.
// If clearPassword is set, the password buffer is set to 0's early during the hashing.

bool TwoCats_HashPassword( uint8_t hash[32],
                           uint8_t *password,   uint32_t passwordSize,
                           const uint8_t *salt, uint32_t saltSize,
                           uint8_t memCost,     bool clearPassword);


// The full password hashing interface.  On success, true is returned, otherwise false.
// Memory hashed = 2^memCost KiB.  Repetitions of hashing = 2^timeCost.  Number of
// threads used = parallelism.  The password buffer is set to 0's early during the
// hashing if clearPassword is set.

bool TwoCats_HashPasswordFull( uint8_t *hash,       uint32_t hashSize,
                               uint8_t *password,   uint32_t passwordSize,
                               const uint8_t *salt, uint32_t saltSize,
                               uint8_t memCost,     uint8_t timeCost,
                               uint8_t parallelism, bool clearPassword);


/* These values make reasonable defaults when using the extended interface */

#define TWOCATS_KEYSIZE 32
#define TWOCATS_PARALLELISM 2
#define TWOCATS_BLOCKSIZE 16384
#define TWOCATS_SUBBLOCKSIZE 64
#define TWOCATS_TIMECOST 0
#define TWOCATS_MULTIPLIES 3

/*
   This is the extended password hashing interface for those who know what they are doing.
   Consider running twocats-guessparams to find reasonalbe default values for a given
   memory cost for your specific machine.

   Data can be any application specific data, such as a secondary key or application name,
   or a concatenation of various data.

   startMemCost is normally equal to stopMemCost, unless a password hash has been
   strengthened using TwoCats_UpdatePassword.
   
   stopMemCost is the main memory hashing difficulty parameter, which causes 2^stopMemCost
   KiB of memory to be hashed.  Each increment doubles memory and difficulty.
   
   timeCost causes the inner loop to repeat 2^timeCost times, repeatedly hashing blocks
   that most likely fit in on-chip cache.  It can be used to increase runtime for a given
   memory size, and to reduce DRAM bandwidth while increasing cache bandwidth.  For memory
   sizes large enough to require external DRAM, it is ideally it is set as high as
   possible without increasing runtime significantly. For memory sizes that fit in on-chip
   cache, timeCost needs to be set high enough to provide the desired security, which is
   aproximately runtime*memory.

   multiplies is used to force attackers to run each guess as long as you do.  It should
   be set as high as possible without increasing runtime significantly.  3 is a reasonable
   default for hashing memory sizes larger than the CPU cache size, 2 is reasonable for
   L2/L3 cache sizes, and 1 is good for L1 cache sizes, or when timeCost is set high
   enough to cause the runtime to increase significantly.  For CPUs without hardware
   multiplication or on-chip data cache, multiplies should be set to 0.

   parallelism is the number of threads used in parallel to hash the password.  A
   reasonable number is half the CPU cores you expect to have idle at any time, but it
   must be at least 1.  Each thread hashes memory so fast, you likely will maximize memory
   bandwidth with only 2 threads.  Higher values can be used on multi-CPU servers with
   more than two memory banks to increase password security.

   If clearPassword is true, the password is set to 0's  early in hashing, and if
   clearData is set, the data input is set to 0's early in hashing.
*/

bool TwoCats_HashPasswordExtended( uint8_t *hash,        uint32_t hashSize,
                                   uint8_t *password,    uint32_t passwordSize,
                                   const uint8_t *salt,  uint32_t saltSize,
                                   uint8_t *data,        uint32_t dataSize,
                                   uint8_t startMemCost, uint8_t stopMemCost,
                                   uint8_t timeCost,     uint8_t multiplies,
                                   uint8_t parallelism,
                                   uint32_t blockSize,   uint32_t subBlockSize,
                                   bool clearPassword, bool clearData);

// Update an existing password hash to a more difficult level of memCost.
bool TwoCats_UpdatePassword(uint8_t *hash, uint32_t hashSize, uint8_t oldMemCost,
        uint8_t newMemCost, uint8_t timeCost, uint8_t multiplies, uint8_t parallelism,
        uint32_t blockSize, uint32_t subBlockSize);

// Client-side portion of work for server-relief mode.  hashSize must be <= 255*32
bool TwoCats_ClientHashPassword(uint8_t *hash, uint32_t hashSize, uint8_t *password, uint32_t passwordSize,
    const uint8_t *salt, uint32_t saltSize, uint8_t *data, uint32_t dataSize, uint8_t startMemCost,
    uint8_t stopMemCost, uint8_t timeCost, uint8_t multiplies, uint8_t parallelism,
    uint32_t blockSize, uint32_t subBlockSize, bool clearPassword, bool clearData);

// Server portion of work for server-relief mode.
void TwoCats_ServerHashPassword(uint8_t *hash, uint8_t hashSize);

// Find parameter settings on this machine for a given desired runtime and maximum memory
// usage.  maxMem is in KiB.  Runtime with be typically +/- 50% and memory will be <= maxMem.
void TwoCats_FindCostParameters(uint32_t milliSeconds, uint32_t maxMem, uint8_t *memCost,
    uint8_t *timeCost, uint8_t *multplies);

// This is the prototype required for the password hashing competition.
int PHS(void *out, size_t outlen, const void *in, size_t inlen, const void *salt, size_t saltlen,
    unsigned int t_cost, unsigned int m_cost);