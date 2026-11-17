#pragma once
#include <openssl/sha.h>
using mbedtls_sha256_context = SHA256_CTX;
inline void mbedtls_sha256_init(SHA256_CTX*) {}
inline void mbedtls_sha256_free(SHA256_CTX*) {}
inline int mbedtls_sha256_starts(SHA256_CTX* c, int) { return SHA256_Init(c) == 1 ? 0 : -1; }
inline int mbedtls_sha256_update(SHA256_CTX* c, const unsigned char* d, size_t n) { return SHA256_Update(c, d, n) == 1 ? 0 : -1; }
inline int mbedtls_sha256_finish(SHA256_CTX* c, unsigned char* d) { return SHA256_Final(d, c) == 1 ? 0 : -1; }
inline void mbedtls_sha256_clone(SHA256_CTX* c, const SHA256_CTX* s) { *c = *s; }
inline int mbedtls_sha256(const unsigned char* d, size_t n, unsigned char* h, int) { return SHA256(d, n, h) ? 0 : -1; }
