/*
 *      pocsag.c -- POCSAG protocol decoder
 *
 *      Copyright (C) 1996
 *          Thomas Sailer (sailer@ife.ee.ethz.ch, hb9jnx@hb9w.che.eu)
 *
 *      Copyright (C) 2012-2014
 *          Elias Oenal    (multimon-ng@eliasoenal.com)
 *
 *      POCSAG (Post Office Code Standard Advisory Group)
 *      Radio Paging Decoder
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* ---------------------------------------------------------------------- */

#include "multimon.h"
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <stdbool.h>
#include <sys/time.h>

/* ---------------------------------------------------------------------- */

//#define CHARSET_LATIN1
//#define CHARSET_UTF8 //ÄÖÜäöüß

#define ASCII_NUL 0x00
#define ASCII_ETX 0x03
#define ASCII_EOT 0x04
#define ASCII_ETB 0x17
#define ASCII_EM 0x19

/* ---------------------------------------------------------------------- */



/*
 * some codewords with special POCSAG meaning
 */
#define POCSAG_SYNC     0x7cd215d8
#define POCSAG_IDLE     0x7a89c197
#define POCSAG_IDLEOP   0x7a89c196
#define POCSAG_SYNCINFO 0x7cf21436 // what is this value?

#define POCSAG_SYNC_WORDS ((2000000 >> 3) << 13)

#define POCSAG_MESSAGE_DETECTION 0x80000000 // Most significant bit is a one

#define POSCAG
/* ---------------------------------------------------------------------- */

int pocsag_invert_input = 0;
int pocsag_error_correction = 2;
int pocsag_show_partial_decodes = 0;
int pocsag_prune_empty = 0;
char *pocsag_wordlog_filename=NULL;
char *pocsag_debug_filename=NULL;

/* ---------------------------------------------------------------------- */

int check_crc(const uint32_t pocsag_word);
int check_parity(const uint32_t pocsag_word);

enum states{
    NO_SYNC = 0,            //0b00000000
    SYNC = 64,              //0b10000000
    LOSING_SYNC = 65,       //0b10000001
    LOST_SYNC = 66,         //0b10000010
    ADDRESS = 67,           //0b10000011
    MESSAGE = 68,           //0b10000100
    END_OF_MESSAGE = 69,    //0b10000101
};


static inline unsigned char even_parity(uint32_t data)
{
    unsigned int temp = data ^ (data >> 16);

    temp = temp ^ (temp >> 8);
    temp = temp ^ (temp >> 4);
    temp = temp ^ (temp >> 2);
    temp = temp ^ (temp >> 1);
    return temp & 1;
}

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

void debuglog(char *format, ...) {
  static int is_startline = 1;
  if (!pocsag_debug_filename) {
    return;
  }
  FILE *debugFile = fopen(pocsag_debug_filename, "a");
  if (!debugFile) {
    perror(pocsag_debug_filename);
    exit(-99);
  }
  char time_buf[20];
  time_t t;
  struct tm* tm_info;
  
  if (is_startline) {
    t = time(NULL);
    tm_info = localtime(&t);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
    fprintf(debugFile, "%s: ", time_buf);
    is_startline = false;
  }
  if (strchr(format,'\n')) {
    is_startline = true;
  }
  va_list args;
  va_start(args, format);
  vfprintf(debugFile, format, args);
  va_end(args);
  fclose(debugFile);
}

void logword(uint32_t word, int frame, int fword) {
  if (!pocsag_wordlog_filename) {
    return;
  }
  FILE *csvFile = fopen(pocsag_wordlog_filename, "a");
  if (!csvFile) {
    perror(pocsag_wordlog_filename);
    exit(-99);
  }
  char time_buf[20];
  time_t t;
  struct tm* tm_info;
  
  t = time(NULL);
  tm_info = localtime(&t);
  strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
  fprintf(csvFile, "%s,%d,%d,%d,%d,%08" PRIx32 "\n", time_buf, frame, fword, check_crc(word), check_parity(word), word);
  fclose(csvFile);
}

/* ---------------------------------------------------------------------- */

static unsigned int pocsag_syndrome(uint32_t data)
{
    uint32_t shreg = data >> 1; /* throw away parity bit */
    uint32_t mask = 1L << (BCH_N-1), coeff = BCH_POLY << (BCH_K-1);
    int n = BCH_K;

    for(; n > 0; mask >>= 1, coeff >>= 1, n--)
        if (shreg & mask)
            shreg ^= coeff;
    if (even_parity(data))
        shreg |= (1 << (BCH_N - BCH_K));
    verbprintf(9, "BCH syndrome: data: %08lx syn: %08lx\n", data, shreg);
    return shreg;
}

/* ---------------------------------------------------------------------- */
	// ISO 646 national variant: US / IRV (1991)
    char *trtab[128] = {
			"<NUL>", 	//  0x0
			"<SOH>", 	//  0x1
			"<STX>", 	//  0x2
			"<ETX>", 	//  0x3
			"<EOT>", 	//  0x4
			"<ENQ>", 	//  0x5
			"<ACK>", 	//  0x6
			"\\g", 		//  0x7
			"<BS>",  	//  0x8
			"\\t",  	//  0x9
			"\\n",  	//  0xa
			"<VT>",  	//  0xb
			"<FF>",  	//  0xc
			"\\r",  	//  0xd
			"<SO>",  	//  0xe
			"<SI>",  	//  0xf
			"<DLE>", 	// 0x10
			"<DC1>", 	// 0x11
			"<DC2>", 	// 0x12
			"<DC3>", 	// 0x13
			"<DC4>", 	// 0x14
			"<NAK>", 	// 0x15
			"<SYN>", 	// 0x16
			"<ETB>", 	// 0x17
			"<CAN>", 	// 0x18
			"<EM>",  	// 0x19
			"<SUB>", 	// 0x1a
			"<ESC>", 	// 0x1b
			"<FS>",  	// 0x1c
			"<GS>",  	// 0x1d
			"<RS>",  	// 0x1e
			"<US>",  	// 0x1f
			" ", 		// 0x20
			"!", 		// 0x21
			"\"", 		// 0x22

						// national variant
			"#", 		// 0x23
			"$", 		// 0x24

			"%", 		// 0x25
			"&", 		// 0x26
			"'", 		// 0x27
			"(", 		// 0x28
			")", 		// 0x29
			"*", 		// 0x2a
			"+", 		// 0x2b
			",", 		// 0x2c
			"-", 		// 0x2d
			".", 		// 0x2e
			"/", 		// 0x2f
			"0", 		// 0x30
			"1", 		// 0x31
			"2", 		// 0x32
			"3", 		// 0x33
			"4", 		// 0x34
			"5", 		// 0x35
			"6", 		// 0x36
			"7", 		// 0x37
			"8", 		// 0x38
			"9", 		// 0x39
			":", 		// 0x3a
			";", 		// 0x3b
			"<", 		// 0x3c
			"=", 		// 0x3d
			">", 		// 0x3e
			"?", 		// 0x3f
			"@", 		// 0x40
			"A", 		// 0x41
			"B", 		// 0x42
			"C", 		// 0x43
			"D", 		// 0x44
			"E", 		// 0x45
			"F", 		// 0x46
			"G", 		// 0x47
			"H", 		// 0x48
			"I", 		// 0x49
			"J", 		// 0x4a
			"K", 		// 0x4b
			"L", 		// 0x4c
			"M", 		// 0x4d
			"N", 		// 0x4e
			"O", 		// 0x4f
			"P", 		// 0x50
			"Q", 		// 0x51
			"R", 		// 0x52
			"S", 		// 0x53
			"T", 		// 0x54
			"U", 		// 0x55
			"V", 		// 0x56
			"W", 		// 0x57
			"X", 		// 0x58
			"Y", 		// 0x59
			"Z", 		// 0x5a
			
						// national variant
			"[", 		// 0x5b
			"\\", 		// 0x5c
			"]", 		// 0x5d
			"^", 		// 0x5e
			
			"_", 		// 0x5f

						// national variant
			"`", 		// 0x60

			"a", 		// 0x61
			"b", 		// 0x62
			"c", 		// 0x63
			"d", 		// 0x64
			"e", 		// 0x65
			"f", 		// 0x66
			"g", 		// 0x67
			"h", 		// 0x68
			"i", 		// 0x69
			"j", 		// 0x6a
			"k", 		// 0x6b
			"l", 		// 0x6c
			"m", 		// 0x6d
			"n", 		// 0x6e
			"o", 		// 0x6f
			"p", 		// 0x70
			"q", 		// 0x71
			"r", 		// 0x72
			"s", 		// 0x73
			"t", 		// 0x74
			"u", 		// 0x75
			"v", 		// 0x76
			"w", 		// 0x77
			"x", 		// 0x78
			"y", 		// 0x79
			"z", 		// 0x7a
			
						// national variant
			"{", 		// 0x7b
			"|", 		// 0x7c
			"}", 		// 0x7d
			"~", 		// 0x7e
			
			"<DEL>"		// 0x7f  
		};


/*
						// national variant
			"#", 		// 0x23
			"$", 		// 0x24

			"[", 		// 0x5b
			"\\", 		// 0x5c
			"]", 		// 0x5d
			"^", 		// 0x5e

			"`", 		// 0x60

			"{", 		// 0x7b
			"|", 		// 0x7c
			"}", 		// 0x7d
			"~", 		// 0x7e
*/

bool pocsag_init_charset(char *charset)
{
	if(strcmp(charset,"DE")==0) // German charset
	{
		#ifdef CHARSET_UTF8
			trtab[0x5b] = "Ä";
			trtab[0x5c] = "Ö";
			trtab[0x5d] = "Ü";

			trtab[0x7b] = "ä";
			trtab[0x7c] = "ö";
			trtab[0x7d] = "ü";
			trtab[0x7e] = "ß";
		#elif defined CHARSET_LATIN1
			trtab[0x5b] = "\304";
			trtab[0x5c] = "\326";
			trtab[0x5d] = "\334";

			trtab[0x7b] = "\344";
			trtab[0x7c] = "\366";
			trtab[0x7d] = "\374";
			trtab[0x7e] = "\337";
		#else
			trtab[0x5b] = "AE";
			trtab[0x5c] = "OE";
			trtab[0x5d] = "UE";

			trtab[0x7b] = "ae";
			trtab[0x7c] = "oe";
			trtab[0x7d] = "ue";
			trtab[0x7e] = "ss";
		#endif
	}
	else if (strcmp(charset,"SE")==0) // Swedish charset
	{
		#ifdef CHARSET_UTF8
			trtab[0x5b] = "Ä";
			trtab[0x5c] = "Ö";
			trtab[0x5d] = "Å";

			trtab[0x7b] = "ä";
			trtab[0x7c] = "ö";
			trtab[0x7d] = "å";
		#elif defined CHARSET_LATIN1
			trtab[0x5b] = "\304";
			trtab[0x5c] = "\326";
			trtab[0x5d] = "\305";

			trtab[0x7b] = "\344";
			trtab[0x7c] = "\366";
			trtab[0x7d] = "\345";
		#else
			trtab[0x5b] = "AE";
			trtab[0x5c] = "OE";
			trtab[0x5d] = "AO";

			trtab[0x7b] = "ae";
			trtab[0x7c] = "oe";
			trtab[0x7d] = "ao";
		#endif
	}
	else if (strcmp(charset,"FR")==0) // French charset
	{
		trtab[0x24] = "£";

		trtab[0x40] = "à";

		trtab[0x5b] = "°";
		trtab[0x5c] = "ç";
		trtab[0x5d] = "§";
		trtab[0x5e] = "^";
		trtab[0x5f] = "_";
		trtab[0x60] = "µ";

		trtab[0x7b] = "é";
		trtab[0x7c] = "ù";
		trtab[0x7d] = "è";
		trtab[0x7e] = "¨";
	}
	else if (strcmp(charset,"SI")==0) // Slovenian charset
	{
		trtab[0x40] = "Ž";
		trtab[0x5b] = "Š";
		trtab[0x5d] = "Ć";
		trtab[0x5e] = "Č";
		trtab[0x60] = "ž";
		trtab[0x7b] = "š";
		trtab[0x7d] = "ć";
		trtab[0x7e] = "č";
	}
	else if (strcmp(charset,"US")==0) // US charset
	{
		// default
	}
	else
	{
	  fprintf(stderr, "Error: invalid POCSAG charset %s\n", charset);
		fprintf(stderr, "Use: US,FR,DE,SE,SI\n");
		charset = "US";
		return false; 
	}
	return true;
}

static char *translate_alpha(unsigned char chr)
{
	return trtab[chr & 0x7f];
}

/* ---------------------------------------------------------------------- */

static void prepare_msg_numeric(struct l2_state_pocsag *rx, char* buff, unsigned int size)
{
    static const char *conv_table = "084 2.6]195-3U7[";
    unsigned char *bp = rx->buffer;
    int len = rx->numnibbles;
    char* cp = buff;

    if ( (unsigned int) len >= size)
        len = size-1;
    for (; len > 0; bp++, len -= 2) {
        *cp++ = conv_table[(*bp >> 4) & 0xf];
        if (len > 1)
            *cp++ = conv_table[*bp & 0xf];
    }
    *cp = '\0';

    cp = buff;
}

static void prepare_msg_alpha(struct l2_state_pocsag *rx, char* buff, unsigned int size)
{
  static char workbuf[8192];
  int wblen = sizeof(workbuf);
  int wbcur = 0;
    uint32_t data = 0;
    int datalen = 0;
    unsigned char *bp = rx->buffer;
    int len = rx->numnibbles;
    char* cp = buff;
    int buffree = size-1;
    unsigned char curchr;
    char *tstr;

    // Since we want to strip terminating NULs and other termination characters,
    // we save the raw ASCII in a temporary buffer first.
    while (len > 0)
    {
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

	if (wbcur < wblen-1) {
	  workbuf[wbcur++] = curchr;
	}
    }

    // Once we're done putting the string in the temporary
    // buffer, we can strip off any termination characters
    // at the end of the string. The temporary buffer doesn'
    // need to be NUL-terminated.
    while(wbcur > 0 &&
	  (workbuf[wbcur-1] == ASCII_NUL ||
	   workbuf[wbcur-1] == ASCII_ETX ||
	   workbuf[wbcur-1] == ASCII_EOT ||
	   workbuf[wbcur-1] == ASCII_ETB ||
	   workbuf[wbcur-1] == ASCII_EM)) {
      wbcur--;
    }

    // Finally, we go through the temporary buffer and
    // translate each character using the selected
    // translation table, and produce the final text.
    for (int i = 0; i < wbcur; i++) {
      tstr = translate_alpha(workbuf[i]);
      if (tstr) {
	int tlen = strlen(tstr);
	if (buffree >= tlen) {
	  memcpy(cp, tstr, tlen);
	  cp += tlen;
	  buffree -= tlen;
	}
      } else if (buffree > 0) {
	*cp++ = curchr;
	buffree--;
      }
    }
    *cp = '\0';
}

static void prepare_msg_binary(struct l2_state_pocsag *rx, char* buff, unsigned int size)
{
  char *bp = buff;
  int len;
  buff[0] = '\0';
  for (uint32_t i=0; i < rx->numnibbles && size; i += 2) {
    if (i > 0) {
      strcat(bp, ",");
      bp++;
      size--;
    }
    len = sprintf(bp, "%02x", rx->buffer[i/2]);
    size -= len;
    bp += len;
  }
}

/* ---------------------------------------------------------------------- */

static void pocsag_printmessage(struct demod_state *s, bool sync)
{
    if(!pocsag_show_partial_decodes && ((s->l2.pocsag.address == -2) || (s->l2.pocsag.function == -2) || !sync))
        return; // Hide partial decodes
    if(pocsag_prune_empty && (s->l2.pocsag.numnibbles == 0))
        return;

    if((s->l2.pocsag.address != -1) || (s->l2.pocsag.function != -1))
    {
        if(s->l2.pocsag.numnibbles == 0)
        {
            verbprintf(0, "%s: Address: %7lu  Function: %1hhi ",s->dem_par->name,
                       s->l2.pocsag.address, s->l2.pocsag.function);
            debuglog("%s: Address: %7lu  Function: %1hhi ",s->dem_par->name,
                       s->l2.pocsag.address, s->l2.pocsag.function);
            if(!sync) {
	      verbprintf(2,"<LOST SYNC>");
	    }
            verbprintf(0,"\n");
        }
        else
        {
            char string[1024];
            int func = 0;
            func = s->l2.pocsag.function;

	    if((s->l2.pocsag.address != -2) || (s->l2.pocsag.function != -2)) {
	      verbprintf(0, "%s: Address: %7lu  Function: %1hhi  ",s->dem_par->name,
			 s->l2.pocsag.address, s->l2.pocsag.function);
	      debuglog("%s: Address: %7lu  Function: %1hhi  ",s->dem_par->name,
		       s->l2.pocsag.address, s->l2.pocsag.function);
	    } else {
	      verbprintf(0, "%s: Address:       -  Function: -  ",s->dem_par->name);
	      debuglog("%s: Address:       -  Function: -  ",s->dem_par->name);
	    }
	    switch (func) {
	    case 0:
	      prepare_msg_numeric(&s->l2.pocsag, string, sizeof(string));
	      verbprintf(0, "Numeric: %s", string);
	      debuglog("Numeric: %s", string);
	      break;
	    case 1:
	    case 2:
	      prepare_msg_binary(&s->l2.pocsag, string, sizeof(string));
	      verbprintf(0, "Binary:  %s  ", string);
	      debuglog("Binary:  %s  ", string);
	    case 3:
	      prepare_msg_alpha(&s->l2.pocsag, string, sizeof(string));	 
	      verbprintf(0, "Alpha:   %s", string);
	      debuglog("Alpha:   %s", string);
	      break;
	    default:
	      prepare_msg_binary(&s->l2.pocsag, string, sizeof(string));
	      verbprintf(0, "Binary:  %s  ", string);
	      debuglog("Binary:  %s  ", string);
	      break;
	    }

	    if(!sync) {
	      verbprintf(2,"<LOST SYNC>");
	    }
	    verbprintf(0,"\n");
	    debuglog("\n");
        }
    }
}

/* ---------------------------------------------------------------------- */

void pocsag_init(struct demod_state *s)
{
    memset(&s->l2.pocsag, 0, sizeof(s->l2.pocsag));
    s->l2.pocsag.address = -1;
    s->l2.pocsag.function = -1;
}

void pocsag_deinit(struct demod_state *s)
{
    if(s->l2.pocsag.pocsag_total_error_count)
        verbprintf(1, "\n===%s stats===\n"
                   "Words BCH checked: %u\n"
                   "Corrected errors: %u\n"
                   "Corrected 1bit errors: %u\n"
                   "Corrected 2bit errors: %u\n"
                   "Invalid word or >2 bits errors: %u\n\n"
                   "Total bits processed: %u\n"
                   "Bits processed while in sync: %u\n"
                   "Bits processed while out of sync: %u\n"
                   "Successfully decoded: %f%%\n",
                   s->dem_par->name,
                   s->l2.pocsag.pocsag_total_error_count,
                   s->l2.pocsag.pocsag_corrected_error_count,
                   s->l2.pocsag.pocsag_corrected_1bit_error_count,
                   s->l2.pocsag.pocsag_corrected_2bit_error_count,
                   s->l2.pocsag.pocsag_uncorrected_error_count,
                   s->l2.pocsag.pocsag_total_bits_received,
                   s->l2.pocsag.pocsag_bits_processed_while_synced,
                   s->l2.pocsag.pocsag_bits_processed_while_not_synced,
                   (100./s->l2.pocsag.pocsag_total_bits_received)*s->l2.pocsag.pocsag_bits_processed_while_synced);
}

static uint32_t
transpose_n(int n, uint32_t *matrix)
{
    uint32_t out = 0;
    int j;

    for (j = 0; j < 32; ++j) {
        if (matrix[j] & (1<<n))
            out |= (1<<j);
    }

    return out;
}

#define ONE 0xffffffff

static uint32_t *
transpose_clone(uint32_t src, uint32_t *out)
{
    int i;
    if (!out)
        out = malloc(sizeof(uint32_t)*32);

    for (i = 0; i < 32; ++i) {
        if (src & (1<<i))
            out[i] = ONE;
        else
            out[i] = 0;
    }

    return out;
}

static void
bitslice_syndrome(uint32_t *slices)
{
    const int firstBit = BCH_N - 1;
    int i, n;
    uint32_t paritymask = slices[0];

    // do the parity and shift together
    for (i = 1; i < 32; ++i) {
        paritymask ^= slices[i];
        slices[i-1] = slices[i];
    }
    slices[31] = 0;

    // BCH_POLY << (BCH_K - 1) is
    //                                                              20   21 22 23
    //  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ONE, 0, 0, ONE,
    //  24 25   26  27  28   29   30   31
    //  0, ONE, ONE, 0, ONE, ONE, ONE, 0

    for (n = 0; n < BCH_K; ++n) {
        // one line here for every '1' bit in coeff (above)
        const int bit = firstBit - n;
        slices[20 - n] ^= slices[bit];
        slices[23 - n] ^= slices[bit];
        slices[25 - n] ^= slices[bit];
        slices[26 - n] ^= slices[bit];
        slices[28 - n] ^= slices[bit];
        slices[29 - n] ^= slices[bit];
        slices[30 - n] ^= slices[bit];
        slices[31 - n] ^= slices[bit];
    }

    // apply the parity mask we built up
    slices[BCH_N - BCH_K] |= paritymask;
}

/* ---------------------------------------------------------------------- */



// This might not be elegant, yet effective!
// Error correction via bruteforce ;)
//
// It's a pragmatic solution since this was much faster to implement
// than understanding the math to solve it while being as effective.
// Besides that the overhead is neglectable.
int pocsag_brute_repair(struct l2_state_pocsag *rx, uint32_t* data)
{
    if (pocsag_syndrome(*data)) {
        rx->pocsag_total_error_count++;
        verbprintf(6, "Error in syndrome detected!\n");
    } else {
        return 0;
    }

    if(pocsag_error_correction == 0)
    {
        rx->pocsag_uncorrected_error_count++;
        verbprintf(6, "Couldn't correct error!\n");
        return 1;
    }

    // check for single bit errors
    {
        int i, n, b1, b2;
        uint32_t res;
        uint32_t *xpose = 0, *in = 0;

        xpose = malloc(sizeof(uint32_t)*32);
        in = malloc(sizeof(uint32_t)*32);

        transpose_clone(*data, xpose);
        for (i = 0; i < 32; ++i)
            xpose[i] ^= (1<<i);

        bitslice_syndrome(xpose);

        res = 0;
        for (i = 0; i < 32; ++i)
            res |= xpose[i];
        res = ~res;

        if (res) {
            int n = 0;
            while (res) {
                ++n;
                res >>= 1;
            }
            --n;

            *data ^= (1<<n);
            rx->pocsag_corrected_error_count++;
            rx->pocsag_corrected_1bit_error_count++;
            goto returnfree;
        }

        if(pocsag_error_correction == 1)
        {
            rx->pocsag_uncorrected_error_count++;
            verbprintf(6, "Couldn't correct error!\n");
            if (xpose)
                free(xpose);
            if (in)
                free(in);
            return 1;
        }

        //check for two bit errors
        n = 0;
        transpose_clone(*data, xpose);

        for (b1 = 0; b1 < 32; ++b1) {
            for (b2 = b1; b2 < 32; ++b2) {
                xpose[b1] ^= (1<<n);
                xpose[b2] ^= (1<<n);

                if (++n == 32) {
                    memcpy(in, xpose, sizeof(uint32_t)*32);

                    bitslice_syndrome(xpose);

                    res = 0;
                    for (i = 0; i < 32; ++i)
                        res |= xpose[i];
                    res = ~res;

                    if (res) {
                        int n = 0;
                        while (res) {
                            ++n;
                            res >>= 1;
                        }
                        --n;

                        *data = transpose_n(n, in);
                        rx->pocsag_corrected_error_count++;
                        rx->pocsag_corrected_2bit_error_count++;
                        goto returnfree;
                    }

                    transpose_clone(*data, xpose);
                    n = 0;
                }
            }
        }

        if (n > 0) {
            memcpy(in, xpose, sizeof(uint32_t)*32);

            bitslice_syndrome(xpose);

            res = 0;
            for (i = 0; i < 32; ++i)
                res |= xpose[i];
            res = ~res;

            if (res) {
                int n = 0;
                while (res) {
                    ++n;
                    res >>= 1;
                }
                --n;

                *data = transpose_n(n, in);
                rx->pocsag_corrected_error_count++;
                rx->pocsag_corrected_2bit_error_count++;
                goto returnfree;
            }
        }

        rx->pocsag_uncorrected_error_count++;
        verbprintf(6, "Couldn't correct error!\n");
        if (xpose)
            free(xpose);
        if (in)
            free(in);
        return 1;

returnfree:
        if (xpose)
            free(xpose);
        if (in)
            free(in);
        return 0;
    }
}

// t0000000 00000000 00000ccc cccccccp

int check_crc(const uint32_t pocsag_word) {
  if (pocsag_word == POCSAG_IDLE) {
    return 1;
  }
  const uint32_t generator = 0x0769;
  const uint32_t crc_bits = 10;
  uint32_t denominator = generator << 20;
  uint32_t msg = (pocsag_word & 0xfffff800) >> (11 - crc_bits);
  uint32_t mask = 1 << 30;
  for (int i=0; i<21; i++) {
    if ((msg & mask) != 0) {
      msg ^= denominator;
    }
    mask >>= 1;
    denominator >>= 1;
  }
  //  fprintf(stderr, "CRC = %04x, should be %04x\n", (pocsag_word >> 1) & 0x3FF, msg & 0x3FF);
  /*  if (((pocsag_word >> 1) & 0x3FF) != (msg & 0x3FF)) {
    fprintf(stderr, "CRC error!\n");
    }*/
  return (((pocsag_word >> 1) & 0x3FF) == (msg & 0x3FF));
}

int check_parity(const uint32_t pocsag_word) {
  uint32_t p = pocsag_word ^ (pocsag_word >> 16);
  p ^= (p >> 8);
  p ^= (p >> 4);
  p &= 0x0f;
  /*  if (!(((0x6996 >> p) & 1) ^ 1)) {
    fprintf(stderr, "Parity error!\n");
    }*/
  return ((0x6996 >> p) & 1) ^ 1;
}

static inline bool word_complete(struct demod_state *s)
{    
    // Do nothing for 31 bits
    // When the word is complete let the program counter pass
    s->l2.pocsag.rx_bit = (s->l2.pocsag.rx_bit + 1) % 32;
    return (s->l2.pocsag.rx_bit == 0/* && check_crc(s->l2.pocsag.rx_data) && check_parity(s->l2.pocsag.rx_data)*/);
}

static inline bool is_sync(const uint32_t rx_data)
{
    if(rx_data == POCSAG_SYNC)
        return true; // Sync found!
    return false;
}

static inline bool is_idle(const uint32_t rx_data)
{
    if(rx_data == POCSAG_IDLE || rx_data == POCSAG_IDLEOP)
        return true; // Idle found!
    return false;
}

unsigned int pocsag_getAddress(uint32_t word, int frameno) {
  return ((word >> 10) & 0x1ffff8) | (frameno & 7);
}

unsigned int pocsag_getFunction(uint32_t word) {
  return (word >> 11) & 3;
}

static void do_one_bit(struct demod_state *s, uint32_t rx_data) {
  // We need to keep track of the number of words we've received, since
  // we want to terminate a batch after 16 words.
  static int received_words=0;
  int frame=0;

  //  printf("%08x\n", rx_data);
  //  fflush(stdout);
  
  s->l2.pocsag.pocsag_total_bits_received++;
  
  // If we're not in sync, just check if we have received the
  // sync word yet. pocsag_rxbit() will keep shifting in new bits
  // into rx_data, so we can just keep checking it until it matches
  // the sync word.
  if (s->l2.pocsag.state == NO_SYNC) {
    s->l2.pocsag.pocsag_bits_processed_while_not_synced++;
    if(is_sync(rx_data)) {
      logword(rx_data, -1, -1);
      verbprintf(4, "Aquired sync!\n");
      debuglog( "Acquired sync\n");
      s->l2.pocsag.state = SYNC;
      // Now reset the bit counter so the next word starts from the
      // beginning.
      s->l2.pocsag.rx_bit = 0;
      received_words=0;
    }
  } else /* is in sync */ {
    // If we receive a new sync word, we start a new batch
    if (is_sync(rx_data)) {
      logword(rx_data, -1, -1);
      debuglog("Received sync. Resetting.\n");
      s->l2.pocsag.rx_bit = 0;
      received_words=0;
      return;
    } 
    s->l2.pocsag.pocsag_bits_processed_while_synced++;

    // Check if we have received 32 bits
    if (!word_complete(s)) {
      return; // Wait for more bits to arrive.
    }

    // We need to keep track of the frame#, since that is
    // used as part of the address calculation
    frame = received_words / 2;
    logword(rx_data, frame, received_words % 2);

    received_words++;


    // If we receive an IDLE word, any active message is terminated.
    if (is_idle(rx_data)) {
      debuglog("f%dw%d: Received IDLE\n",
	     (received_words - 1) / 2,
	     (received_words - 1) % 2);
      if (s->l2.pocsag.numnibbles > 0) {
	pocsag_printmessage(s, true);
	s->l2.pocsag.numnibbles = 0;
	s->l2.pocsag.address = -1;
	s->l2.pocsag.function = -1;
      }
    } else /* not IDLE */ {
      debuglog( "f%dw%d: Received a complete word: %08" PRIx32 " CRC: %s, parity: %s\n",
	      (received_words - 1) / 2,
	      (received_words - 1) % 2,
	      rx_data, check_crc(rx_data)?"OK":"FAIL", check_parity(rx_data)?"OK":"FAIL");
      if(pocsag_brute_repair(&s->l2.pocsag, &rx_data))
        {
	  // Arbitration lost
#if 0
	  pocsag_printmessage(s, false);
	  s->l2.pocsag.numnibbles = 0;
	  s->l2.pocsag.address = -1;
	  s->l2.pocsag.function = -1;
	  s->l2.pocsag.state = NO_SYNC;
	  return;
#endif
	}

      // If we receive an address word, any active message is terminated.
      // Then we calculate the address and function
      if(!(rx_data & POCSAG_MESSAGE_DETECTION)) {
	if (s->l2.pocsag.numnibbles > 0) {
	  debuglog("Detected non-message word. Saved nibbles: %d\n", s->l2.pocsag.numnibbles);
	  pocsag_printmessage(s, true);
	  s->l2.pocsag.numnibbles = 0;
	}
	s->l2.pocsag.address = pocsag_getAddress(rx_data, frame);
	s->l2.pocsag.function = pocsag_getFunction(rx_data);
	debuglog("Address: %u Function: %1hhi\n", s->l2.pocsag.address, s->l2.pocsag.function);
	s->l2.pocsag.state = MESSAGE;
      } else /* Message word */ {
	// If we receive a message word, we just collect the contents, regardless
	// of whether we've received an address
	s->l2.pocsag.state = MESSAGE;
	if (s->l2.pocsag.numnibbles > sizeof(s->l2.pocsag.buffer)*2 - 5) {
	  verbprintf(0, "%s: Warning: Message too long\n",
		     s->dem_par->name);
	  
	  debuglog( "Message too long. Saved nibbles: %d\n", s->l2.pocsag.numnibbles);
	  pocsag_printmessage(s, true);
	  s->l2.pocsag.numnibbles = 0;
	  s->l2.pocsag.address = -1;
	  s->l2.pocsag.function = -1;
	} else /* Message is not too long */ {
	  uint32_t data;
	  unsigned char *bp;
	  bp = s->l2.pocsag.buffer + (s->l2.pocsag.numnibbles >> 1);
	  data = (rx_data >> 11);
	  if (s->l2.pocsag.numnibbles & 1) {
	    bp[0] = (bp[0] & 0xf0) | ((data >> 16) & 0xf);
	    bp[1] = data >> 8;
	    bp[2] = data;
	  } else {
	    bp[0] = data >> 12;
	    bp[1] = data >> 4;
	    bp[2] = data << 4;
	  }
	  s->l2.pocsag.numnibbles += 5;
	}
      }
    }
    // Once we've received 16 words, a batch is finished and we
    // go out of sync. We don't HAVE to, since the code above will
    // handle an in-line sync word just fine. But we do anyway.
    if (received_words == 16) {
      debuglog( "End of batch.\n");
      s->l2.pocsag.state = NO_SYNC;
      received_words = 0;
    }
  }
}

/* ---------------------------------------------------------------------- */

void pocsag_rxbit(struct demod_state *s, int32_t bit)
{
  s->l2.pocsag.rx_data <<= 1;
  s->l2.pocsag.rx_data |= !bit;
  verbprintf(9, " %c ", '1'-(s->l2.pocsag.rx_data & 1));
  if(pocsag_invert_input)
    do_one_bit(s, ~(s->l2.pocsag.rx_data)); // this tries the inverted signal
  else
    do_one_bit(s, s->l2.pocsag.rx_data);
}

/* ---------------------------------------------------------------------- */
