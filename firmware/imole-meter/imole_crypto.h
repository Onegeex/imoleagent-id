// imole_crypto.h - Ethereum-style crypto for the Imole ESP32 meter.
// keccak256 + secp256k1 ECDSA with recovery id (v), built on the mbedtls that ships with the
// ESP32 Arduino core (2.x and 3.x). Header-only so it drops into a Wokwi/Arduino sketch.
// Tested on a PC against ethers.js (see test_crypto.c in this folder).
#pragma once
#include <stdint.h>
#include <string.h>
#include "mbedtls/ecp.h"
#include "mbedtls/bignum.h"

#ifndef MBEDTLS_PRIVATE
#define MBEDTLS_PRIVATE(member) member
#endif

// ------------------------------------------------------------------ keccak256 (Ethereum)
static const uint64_t IM_RC[24] = {
  0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL, 0x8000000080008000ULL,
  0x000000000000808bULL, 0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL,
  0x000000000000008aULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
  0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL, 0x8000000000008003ULL,
  0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800aULL, 0x800000008000000aULL,
  0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL};
static const int IM_ROT[25] = {0, 1, 62, 28, 27, 36, 44, 6, 55, 20, 3, 10, 43, 25, 39, 41, 45, 15, 21, 8, 18, 2, 61, 56, 14};

static inline uint64_t im_rol(uint64_t x, int s) { return s ? (x << s) | (x >> (64 - s)) : x; }

static void im_keccakf(uint64_t A[25]) {
  for (int round = 0; round < 24; round++) {
    uint64_t C[5], D[5], B[25];
    for (int x = 0; x < 5; x++) C[x] = A[x] ^ A[x + 5] ^ A[x + 10] ^ A[x + 15] ^ A[x + 20];
    for (int x = 0; x < 5; x++) D[x] = C[(x + 4) % 5] ^ im_rol(C[(x + 1) % 5], 1);
    for (int i = 0; i < 25; i++) A[i] ^= D[i % 5];
    for (int x = 0; x < 5; x++)
      for (int y = 0; y < 5; y++) B[y + 5 * ((2 * x + 3 * y) % 5)] = im_rol(A[x + 5 * y], IM_ROT[x + 5 * y]);
    for (int x = 0; x < 5; x++)
      for (int y = 0; y < 5; y++) A[x + 5 * y] = B[x + 5 * y] ^ ((~B[(x + 1) % 5 + 5 * y]) & B[(x + 2) % 5 + 5 * y]);
    A[0] ^= IM_RC[round];
  }
}

static void keccak256(const uint8_t* in, size_t len, uint8_t out[32]) {
  uint64_t A[25];
  memset(A, 0, sizeof(A));
  const size_t rate = 136;
  uint8_t block[136];
  while (len >= rate) {
    for (int i = 0; i < 17; i++) { uint64_t w = 0; for (int b = 0; b < 8; b++) w |= (uint64_t)in[i * 8 + b] << (8 * b); A[i] ^= w; }
    im_keccakf(A); in += rate; len -= rate;
  }
  memset(block, 0, rate);
  memcpy(block, in, len);
  block[len] = 0x01;            // Keccak (not SHA3) padding
  block[rate - 1] |= 0x80;
  for (int i = 0; i < 17; i++) { uint64_t w = 0; for (int b = 0; b < 8; b++) w |= (uint64_t)block[i * 8 + b] << (8 * b); A[i] ^= w; }
  im_keccakf(A);
  for (int i = 0; i < 4; i++) for (int b = 0; b < 8; b++) out[i * 8 + b] = (uint8_t)(A[i] >> (8 * b));
}

// ------------------------------------------------------------------ secp256k1
static const char* IM_N_HEX = "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141";


// Ethereum address (20 bytes) for a 32-byte private key. Returns 0 on success.
static int eth_address(const uint8_t priv[32], uint8_t addr[20], int (*rng)(void*, unsigned char*, size_t), void* rng_ctx) {
  mbedtls_ecp_group grp; mbedtls_ecp_point Q; mbedtls_mpi d;
  mbedtls_ecp_group_init(&grp); mbedtls_ecp_point_init(&Q); mbedtls_mpi_init(&d);
  int ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256K1);
  if (!ret) ret = mbedtls_mpi_read_binary(&d, priv, 32);
  if (!ret) ret = mbedtls_ecp_mul(&grp, &Q, &d, &grp.G, rng, rng_ctx);
  uint8_t pub[65]; size_t olen = 0;
  if (!ret) ret = mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, pub, sizeof(pub));
  if (!ret) { uint8_t h[32]; keccak256(pub + 1, 64, h); memcpy(addr, h + 12, 20); }
  mbedtls_ecp_group_free(&grp); mbedtls_ecp_point_free(&Q); mbedtls_mpi_free(&d);
  return ret;
}

// Sign a 32-byte digest. Writes r(32) || s(32) || v(1) with v = 27/28 and low-s (EIP-2).
static int eth_sign(const uint8_t priv[32], const uint8_t digest[32], uint8_t sig[65], int (*rng)(void*, unsigned char*, size_t), void* rng_ctx) {
  mbedtls_ecp_group grp; mbedtls_ecp_point R; mbedtls_mpi d, k, z, r, s, n, halfN, kinv, t;
  mbedtls_ecp_group_init(&grp); mbedtls_ecp_point_init(&R);
  mbedtls_mpi* all[] = {&d, &k, &z, &r, &s, &n, &halfN, &kinv, &t};
  for (auto m : all) mbedtls_mpi_init(m);
  int ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256K1);
  if (!ret) ret = mbedtls_mpi_read_string(&n, 16, IM_N_HEX);
  if (!ret) ret = mbedtls_mpi_copy(&halfN, &n);
  if (!ret) ret = mbedtls_mpi_shift_r(&halfN, 1);
  if (!ret) ret = mbedtls_mpi_read_binary(&d, priv, 32);
  if (!ret) ret = mbedtls_mpi_read_binary(&z, digest, 32);
  int v = 0;
  for (int tries = 0; !ret && tries < 16; tries++) {
    // k random in [1, n-1]; R = k*G
    ret = mbedtls_ecp_gen_privkey(&grp, &k, rng, rng_ctx);
    if (!ret) ret = mbedtls_ecp_mul(&grp, &R, &k, &grp.G, rng, rng_ctx);
    if (ret) break;
    uint8_t rp[65]; size_t olen = 0;
    ret = mbedtls_ecp_point_write_binary(&grp, &R, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, rp, sizeof(rp));
    if (ret) break;
    int yOdd = rp[64] & 1;
    ret = mbedtls_mpi_read_binary(&r, rp + 1, 32);            // r = R.x
    int xOverflow = mbedtls_mpi_cmp_mpi(&r, &n) >= 0;
    if (!ret) ret = mbedtls_mpi_mod_mpi(&r, &r, &n);
    if (ret || mbedtls_mpi_cmp_int(&r, 0) == 0) continue;
    // s = k^-1 (z + r*d) mod n
    ret = mbedtls_mpi_mul_mpi(&t, &r, &d);
    if (!ret) ret = mbedtls_mpi_add_mpi(&t, &t, &z);
    if (!ret) ret = mbedtls_mpi_mod_mpi(&t, &t, &n);
    if (!ret) ret = mbedtls_mpi_inv_mod(&kinv, &k, &n);
    if (!ret) ret = mbedtls_mpi_mul_mpi(&s, &kinv, &t);
    if (!ret) ret = mbedtls_mpi_mod_mpi(&s, &s, &n);
    if (ret || mbedtls_mpi_cmp_int(&s, 0) == 0) continue;
    v = yOdd | (xOverflow << 1);
    if (mbedtls_mpi_cmp_mpi(&s, &halfN) > 0) {                // low-s: s = n - s flips parity
      ret = mbedtls_mpi_sub_mpi(&s, &n, &s);
      v ^= 1;
    }
    if (!ret) ret = mbedtls_mpi_write_binary(&r, sig, 32);
    if (!ret) ret = mbedtls_mpi_write_binary(&s, sig + 32, 32);
    sig[64] = (uint8_t)(27 + v);
    break;
  }
  mbedtls_ecp_group_free(&grp); mbedtls_ecp_point_free(&R);
  for (auto m : all) mbedtls_mpi_free(m);
  return ret;
}

// ------------------------------------------------------------------ hex helpers
static void im_hex(const uint8_t* b, size_t n, char* out) {  // out needs 2n+1
  static const char* H = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) { out[2 * i] = H[b[i] >> 4]; out[2 * i + 1] = H[b[i] & 15]; }
  out[2 * n] = 0;
}
static int im_unhex(const char* hex, uint8_t* out, size_t n) {
  if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex += 2;
  for (size_t i = 0; i < n; i++) {
    int v = 0;
    for (int j = 0; j < 2; j++) {
      char c = hex[2 * i + j]; v <<= 4;
      if (c >= '0' && c <= '9') v |= c - '0'; else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10; else return -1;
    }
    out[i] = (uint8_t)v;
  }
  return 0;
}
// 32-byte big-endian word holding a uint64
static void im_word_u64(uint64_t x, uint8_t w[32]) { memset(w, 0, 32); for (int i = 0; i < 8; i++) w[31 - i] = (uint8_t)(x >> (8 * i)); }

// EIP-712 digest for ImoleGrid Usage(bytes32 meterId,uint64 wh,uint64 nonce)
static void imole_usage_digest(const uint8_t domainSep[32], const uint8_t meterId[32], uint64_t wh, uint64_t nonce, uint8_t out[32]) {
  static const char* TYPE = "Usage(bytes32 meterId,uint64 wh,uint64 nonce)";
  uint8_t buf[128];
  keccak256((const uint8_t*)TYPE, strlen(TYPE), buf);
  memcpy(buf + 32, meterId, 32);
  im_word_u64(wh, buf + 64);
  im_word_u64(nonce, buf + 96);
  uint8_t structHash[32];
  keccak256(buf, 128, structHash);
  uint8_t msg[66];
  msg[0] = 0x19; msg[1] = 0x01;
  memcpy(msg + 2, domainSep, 32);
  memcpy(msg + 34, structHash, 32);
  keccak256(msg, 66, out);
}
