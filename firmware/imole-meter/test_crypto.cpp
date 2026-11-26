// PC test for imole_crypto.h. Prints address + signature for fixed inputs so they can be
// checked against ethers.js. Build: see firmware/README.md
#include <stdio.h>
#include <stdlib.h>
#include "imole_crypto.h"

static int rng(void*, unsigned char* out, size_t n) { for (size_t i = 0; i < n; i++) out[i] = (unsigned char)(rand() & 0xff); return 0; }

int main(int argc, char** argv) {
  srand(argc > 1 ? atoi(argv[1]) : 1);
  uint8_t h[32]; char hex[200];
  keccak256((const uint8_t*)"", 0, h); im_hex(h, 32, hex); printf("keccak('')=%s\n", hex);
  keccak256((const uint8_t*)"imole:ife-001", 13, h); im_hex(h, 32, hex); printf("keccak(label)=%s\n", hex);

  uint8_t priv[32]; im_unhex("59c6995e998f97a5a0044966f0945389dc9e86dae88c7a8412f4603b6b78690d", priv, 32);
  uint8_t addr[20]; eth_address(priv, addr, rng, 0); im_hex(addr, 20, hex); printf("addr=0x%s\n", hex);

  uint8_t dom[32], meter[32];
  im_unhex(argv[2], dom, 32);
  im_unhex("8585db12c1df78cc5d76e7bcdb56e3f44262e21844fe0cb640b399c4a42045e4", meter, 32);
  uint8_t dig[32]; imole_usage_digest(dom, meter, 123, 7, dig); im_hex(dig, 32, hex); printf("digest=0x%s\n", hex);
  uint8_t sig[65]; int r = eth_sign(priv, dig, sig, rng, 0); im_hex(sig, 65, hex); printf("sig=0x%s ret=%d\n", hex, r);
  return 0;
}
