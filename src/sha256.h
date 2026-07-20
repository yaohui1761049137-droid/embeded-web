/* sha256.h — Compact SHA-256 implementation (public domain) */
#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>
#include <stddef.h>

#define SHA256_DIGEST_SIZE 32
#define SHA256_BLOCK_SIZE  64

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buf[64];
} sha256_ctx;

void sha256_init(sha256_ctx *ctx);
void sha256_update(sha256_ctx *ctx, const uint8_t *data, size_t len);
void sha256_final(sha256_ctx *ctx, uint8_t digest[SHA256_DIGEST_SIZE]);

/* One-shot: hash a buffer, output hex string (65 bytes: 64 hex + NUL) */
void sha256_hex(const uint8_t *data, size_t len, char hex[65]);

#endif /* SHA256_H */
