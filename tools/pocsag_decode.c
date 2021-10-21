#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>

#define BUFSIZE 8192

unsigned char buffer[BUFSIZE];
int numnibbles=0;

void handleNextWord(uint32_t word) {
  static unsigned char *bp;
  uint32_t data;

  fprintf(stderr, "Received word %08X\n", word);
  
  bp = buffer + (numnibbles >> 1);
  data = (word >> 11);
  if (numnibbles & 1) {
    bp[0] = (bp[0] & 0xf0) | ((data >> 16) & 0xf);
    bp[1] = data >> 8;
    bp[2] = data;
  } else {
    bp[0] = data >> 12;
    bp[1] = data >> 4;
    bp[2] = data << 4;
  }
  numnibbles += 5;
}

void translateMsg(char *outbuf, int buflen) {
  int len = numnibbles;
  int datalen = 0;
  uint32_t data = 0;
  unsigned char *bp = buffer;
  char *cp = outbuf;
  unsigned char curchr;
  
  while (len > 0) {
    while (datalen < 7 && len > 0) {
      if (len == 1) {
	data = (data << 4) | ((*bp >> 4) & 0xf);
	datalen += 4;
	len = 0;
      } else {
	data = (data << 8) | *bp++;
	datalen += 8;
	len -= 2;
      }
    }
    if (datalen < 7)
      continue;
    datalen -= 7;
    curchr = ((data >> datalen) & 0x7f) << 1;
    curchr = ((curchr & 0xf0) >> 4) | ((curchr & 0x0f) << 4);
    curchr = ((curchr & 0xcc) >> 2) | ((curchr & 0x33) << 2);
    curchr = ((curchr & 0xaa) >> 1) | ((curchr & 0x55) << 1);

    *cp++ = curchr;
    buflen--;
  }
  *cp = '\0';
}

int is_idle(uint32_t word) {
  return (word == 0x7a89c197);
}

int is_address(uint32_t word) {
  return !(word & 0x80000000);
}

int main(int argc, char *argv[]) {
  uint32_t word;
  static char inbuf[1024];
  int n;
  while ((fgets(inbuf,sizeof(inbuf),stdin)) > 0) {
    word = strtol(inbuf, NULL, 16);
    if (!is_address(word)) {
      handleNextWord(word);
    }
  }
  for (int i=0; i < numnibbles; i += 2) {
    fprintf(stderr, "%02x ", buffer[i / 2]);
  }
  fprintf(stderr, "\n");
  translateMsg(inbuf, sizeof(inbuf));
  printf("%s\n", inbuf);
  return 0;
}
