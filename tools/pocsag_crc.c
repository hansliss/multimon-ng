#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* ---------------------------------------------------------------------- */

/*
 * the code used by POCSAG is a (n=31,k=21) BCH Code with dmin=5,
 * thus it could correct two bit errors in a 31-Bit codeword.
 * It is a systematic code.
 * The generator polynomial is:
 *   g(x) = x^10+x^9+x^8+x^6+x^5+x^3+1
 * The parity check polynomial is:
 *   h(x) = x^21+x^20+x^18+x^16+x^14+x^13+x^12+x^11+x^8+x^5+x^3+1
 * g(x) * h(x) = x^n+1
 */
#define BCH_POLY 03551 /* octal */
#define BCH_N    31
#define BCH_K    21



/* ---------------------------------------------------------------------- */


//0011 1111 1111 1111 1111 11xx xxxx xxxx

static unsigned int pocsag_syndrome(uint32_t data)
{
  uint32_t shreg = (data >> 1) & 0xfffffc00; /* throw away parity bit */
    uint32_t mask = 1L << (BCH_N-1), coeff = BCH_POLY << (BCH_K-1);
    int n = BCH_K;

    for(; n > 0; mask >>= 1, coeff >>= 1, n--) {
      if (shreg & mask) {
	shreg ^= coeff;
      }
      //fprintf(stderr, "shreg=%08x coeff=%08x mask=%08x\n", shreg, coeff, mask);
    }
    return shreg;
}


int check_crc(const uint32_t pocsag_word) {
  const uint32_t generator = 0x0769;
  const uint32_t crc_bits = 10;
  uint32_t denominator = generator << 20;
  uint32_t msg = (pocsag_word & 0xfffff800) >> (11 - crc_bits);
  uint32_t mask = 1L << 30;
  for (int i=0; i<21; i++) {
    if ((msg & mask) != 0) {
      msg ^= denominator;
    }
    //    fprintf(stderr, "msg=%08x denominator=%08x mask=%08x\n", msg, denominator, mask);
    mask >>= 1;
    denominator >>= 1;
  }
  //  fprintf(stderr, "CRC = %04x, should be %04x or %04x\n", (pocsag_word >> 1) & 0x3FF, msg & 0x3FF, pocsag_syndrome(pocsag_word));
  return (((pocsag_word >> 1) & 0x3FF) == (msg & 0x3FF));
}

int check_parity(const uint32_t pocsag_word) {
  uint32_t p = pocsag_word ^ (pocsag_word >> 16);
  p ^= (p >> 8);
  p ^= (p >> 4);
  p &= 0x0f;
  return ((0x6996 >> p) & 1) ^ 1;
}

int main(int argc, char *argv[]) {
  uint32_t pword;
  for (int i = 1; i < argc; i++) {
    pword = strtol(argv[i], NULL, 16);
    printf("%08x CRC: %s, parity: %s\n", pword, check_crc(pword)?"OK":"FAIL", check_parity(pword)?"OK":"FAIL");
  }
}
