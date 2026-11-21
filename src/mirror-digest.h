#pragma once
#include "blake3.h"

/* Local to the broker: the recovery store carries pixels/fds, never a digest.
 * Both sides hash the entire CPU-visible allocation, including row padding. */
static void mirror_digest(const void *pixels, size_t bytes, unsigned char digest[32])
{
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, pixels, bytes);
    blake3_hasher_finalize(&hasher, digest, 32);
}
