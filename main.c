/*
   TigerPHS main wrapper.

   Written in 2014 by Bill Cox <waywardgeek@gmail.com>

   To the extent possible under law, the author(s) have dedicated all copyright
   and related and neighboring rights to this software to the public domain
   worldwide. This software is distributed without any warranty.

   You should have received a copy of the CC0 Public Domain Dedication along with
   this software. If not, see <http://creativecommons.org/publicdomain/zero/1.0/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <string.h>
#include <getopt.h>
#include "tigerphs.h"
#include "tigerphs-impl.h"

static void usage(char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, (char *)format, ap);
    va_end(ap);
    fprintf(stderr, "\nUsage: tigerphs [OPTIONS]\n"
        "    -h hashSize     -- The output derived key length in bytes\n"
        "    -p password     -- Set the password to hash\n"
        "    -s salt         -- Set the salt.  Salt must be in hexidecimal\n"
        "    -m memCost      -- The amount of memory to use in KB\n"
        "    -t timeCost     -- Parallelism parameter, typically the number of threads\n"
        "    -M multiplies   -- The number of multiplies per 32 bytes of hashing\n"
        "    -P parallelism  -- Parallelism parameter, typically the number of threads\n");
    exit(1);
}

static uint32_t readuint32_t(char flag, char *arg) {
    char *endPtr;
    char *p = arg;
    uint32_t value = strtol(p, &endPtr, 0);
    if(*p == '\0' || *endPtr != '\0') {
        usage("Invalid integer for parameter -%c", flag);
    }
    return value;
}

// Read a 2-character hex byte.
static bool readHexByte(uint8_t *dest, char *value) {
    char c = toupper((uint8_t)*value++);
    uint8_t byte;
    if(c >= '0' && c <= '9') {
        byte = c - '0';
    } else if(c >= 'A' && c <= 'F') {
        byte = c - 'A' + 10;
    } else {
        return false;
    }
    byte <<= 4;
    c = toupper((uint8_t)*value);
    if(c >= '0' && c <= '9') {
        byte |= c - '0';
    } else if(c >= 'A' && c <= 'F') {
        byte |= c - 'A' + 10;
    } else {
        return false;
    }
    *dest = byte;
    return true;
}

static uint8_t *readHexSalt(char *p, uint32_t *saltLength) {
    uint32_t length = strlen(p);
    if(length & 1) {
        usage("hex salt string must have an even number of digits.\n");
    }
    *saltLength = strlen(p) >> 1;
    uint8_t *salt = malloc(*saltLength*sizeof(uint8_t));
    if(salt == NULL) {
        usage("Unable to allocate salt");
    }
    uint8_t *dest = salt;
    while(*p != '\0' && readHexByte(dest++, p)) {
        p += 2;
    }
    return salt;
}

int main(int argc, char **argv) {
    uint32_t derivedKeySize = TIGERPHS_KEYSIZE;
    uint8_t parallelism = TIGERPHS_PARALLELISM;
    uint8_t memCost = TIGERPHS_MEMCOST;
    uint8_t *salt = (uint8_t *)"salt";
    uint32_t saltSize = 4;
    uint8_t *password = (uint8_t *)"password";
    uint32_t passwordSize = 8;
    uint8_t timeCost = TIGERPHS_TIMECOST;
    uint8_t multiplies = TIGERPHS_MULTIPLIES;

    char c;
    while((c = getopt(argc, argv, "h:p:s:m:M:t:P:b:B:")) != -1) {
        switch (c) {
        case 'h':
            derivedKeySize = readuint32_t(c, optarg);
            break;
        case 'p':
            password = (uint8_t *)optarg;
            passwordSize = strlen(optarg);
            break;
        case 's':
            salt = readHexSalt(optarg, &saltSize);
            break;
        case 'm':
            memCost = readuint32_t(c, optarg);
            break;
        case 'M':
            multiplies = readuint32_t(c, optarg);
            break;
        case 't':
            timeCost = readuint32_t(c, optarg);
            break;
        case 'P':
            parallelism = readuint32_t(c, optarg);
            break;
        default:
            usage("Invalid argumet");
        }
    }
    if(optind != argc) {
        usage("Extra parameters not recognised\n");
    }

    printf("memCost:%u timeCost:%u multiplies:%u parallelism:%u password:%s salt:%s\n",
        memCost, timeCost, multiplies, parallelism, password, salt);
    uint8_t *derivedKey = (uint8_t *)calloc(derivedKeySize, sizeof(uint8_t));
    if(!TigerPHS_HashPassword(derivedKey, derivedKeySize, password, passwordSize, salt, saltSize,
            NULL, 0, memCost, memCost, timeCost, multiplies, parallelism, false, false)) {
        fprintf(stderr, "Key stretching failed.\n");
        return 1;
    }
    printHex("", derivedKey, derivedKeySize);
    return 0;
}
