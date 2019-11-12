/*
 * Copyright (c) 2017-2019 Genome Research Ltd.
 * Author(s): James Bonfield
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *    1. Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *
 *    3. Neither the names Genome Research Ltd and Wellcome Trust Sanger
 *       Institute nor the names of its contributors may be used to endorse
 *       or promote products derived from this software without specific
 *       prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY GENOME RESEARCH LTD AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL GENOME RESEARCH
 * LTD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// As per standard rANS_static but using optional RLE or bit-packing
// techniques prior to entropy encoding.  This is a significant
// reduction in some data sets.

// top bits in order byte
#define X_PACK 0x80    // Pack 2,4,8 or infinite symbols into a byte.
#define X_RLE  0x40    // Run length encoding with runs & lits encoded separately
#define X_CAT  0x20    // Nop; for tiny segments where rANS overhead is too big
#define X_NOSZ 0x10    // Don't store the original size; used by X4 mode
#define X_4    0x08    // For 4-byte integer data; rotate & encode 4 streams.

// FIXME Can we get decoder to return the compressed sized read, avoiding
// us needing to store it?  Yes we can.  See c-size comments.  If we added all these
// together we could get rans_uncompress_to_4x16 to return the number of bytes
// consumed, avoiding the calling code from needed to explicitly stored the size.
// However the effect on name tokeniser is to save 0.1 to 0.2% so not worth it.

/*-------------------------------------------------------------------------- */
/*
 * Example wrapper to use the rans_byte.h functions included above.
 *
 * This demonstrates how to use, and unroll, an order-0 and order-1 frequency
 * model.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <sys/time.h>
#include <limits.h>
#ifndef NO_THREADS
#include <pthread.h>
#endif

#include "rANS_word.h"
#include "rANS_static4x16.h"
#include "varint.h"
#include "pack.h"

#define TF_SHIFT 12
#define TOTFREQ (1<<TF_SHIFT)

// 9-11 is considerably faster in the O1sfb variant due to reduced table size.
#ifndef TF_SHIFT_O1
#define TF_SHIFT_O1 10
#endif
#define TOTFREQ_O1 (1<<TF_SHIFT_O1)


/*-----------------------------------------------------------------------------
 * Memory to memory compression functions.
 *
 * These are original versions without any manual loop unrolling. They
 * are easier to understand, but can be up to 2x slower.
 */

#define MAGIC 8

#ifdef UNUSED
static void hist1(unsigned char *in, unsigned int in_size, int F0[256]) {
    int i;
    for (i = 0; i < in_size; i++)
	F0[in[i]]++;
}

static void hist4p(unsigned char *in, unsigned int in_size, int *F0) {
    int F1[256+MAGIC] = {0}, F2[256+MAGIC] = {0}, F3[256+MAGIC] = {0};
    int i;
    unsigned char *in_end4 = in + (in_size & ~3);
    unsigned char *in_end  = in + in_size;
    while (in < in_end4) {
	F0[*in++]++;
	F1[*in++]++;
	F2[*in++]++;
	F3[*in++]++;
    }
    while (in < in_end)
	F0[*in++]++;

    for (i = 0; i < 256; i++)
	F0[i] += F1[i] + F2[i] + F3[i];
}
#endif

static void hist8(unsigned char *in, unsigned int in_size, int F0[256]) {
    int F1[256+MAGIC] = {0}, F2[256+MAGIC] = {0}, F3[256+MAGIC] = {0};
    int F4[256+MAGIC] = {0}, F5[256+MAGIC] = {0}, F6[256+MAGIC] = {0}, F7[256+MAGIC] = {0};
    int i, i8 = in_size & ~7;
    for (i = 0; i < i8; i+=8) {
	F0[in[i+0]]++;
	F1[in[i+1]]++;
	F2[in[i+2]]++;
	F3[in[i+3]]++;
	F4[in[i+4]]++;
	F5[in[i+5]]++;
	F6[in[i+6]]++;
	F7[in[i+7]]++;
    }
    while (i < in_size)
	F0[in[i++]]++;

    for (i = 0; i < 256; i++)
	F0[i] += F1[i] + F2[i] + F3[i] + F4[i] + F5[i] + F6[i] + F7[i];
}

static void present8(unsigned char *in, unsigned int in_size, int F0[256]) {
    int F1[256+MAGIC] = {0}, F2[256+MAGIC] = {0}, F3[256+MAGIC] = {0};
    int F4[256+MAGIC] = {0}, F5[256+MAGIC] = {0}, F6[256+MAGIC] = {0}, F7[256+MAGIC] = {0};
    int i, i8 = in_size & ~7;
    for (i = 0; i < i8; i+=8) {
	F0[in[i+0]]=1;
	F1[in[i+1]]=1;
	F2[in[i+2]]=1;
	F3[in[i+3]]=1;
	F4[in[i+4]]=1;
	F5[in[i+5]]=1;
	F6[in[i+6]]=1;
	F7[in[i+7]]=1;
    }
    while (i < in_size)
	F0[in[i++]]=1;

    for (i = 0; i < 256; i++)
	F0[i] += F1[i] + F2[i] + F3[i] + F4[i] + F5[i] + F6[i] + F7[i];
}

// Rounds to next power of 2.
// credit to http://graphics.stanford.edu/~seander/bithacks.html
static uint32_t round2(uint32_t v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

static int normalise_freq(int *F, int size, int tot) {
    int m = 0, M = 0, fsum = 0, j;
    if (!size)
	return 0;

    uint64_t tr = ((uint64_t)tot<<31)/size + (1<<30)/size;

    for (m = M = j = 0; j < 256; j++) {
	if (!F[j])
	    continue;

	if (m < F[j])
	    m = F[j], M = j;

	if ((F[j] = (F[j]*tr)>>31) == 0)
	    F[j] = 1;
	fsum += F[j];
    }

    int adjust = tot - fsum;
    if (adjust > 0) {
	F[M] += adjust;
    } else if (adjust < 0) {
	if (F[M] > -adjust) {
	    F[M] += adjust;
	} else {
	    adjust += F[M]-1;
	    F[M] = 1;
	    for (j = 0; adjust && j < 256; j++) {
		if (F[j] < 2) continue;

		int d = F[j] > -adjust;
		int m = d ? adjust : 1-F[j];
		F[j]   += m;
		adjust -= m;
	    }
	}
    }

    //printf("F[%d]=%d\n", M, F[M]);
    return F[M]>0 ? 0 : -1;
}

// A specialised version of normalise_freq_shift where the input size
// is already normalised to a power of 2, meaning we can just perform
// shifts instead of hard to define multiplications and adjustments.
static void normalise_freq_shift(int *F, int size, int max_tot) {
    if (size == 0 || size == max_tot)
	return;

    int shift = 0, i;
    while (size < max_tot)
	size*=2, shift++;

    for (i = 0; i < 256; i++)
	F[i] <<= shift;
}

// symbols only
static int encode_alphabet(uint8_t *cp, int *F) {
    uint8_t *op = cp;
    int rle, j;

    for (rle = j = 0; j < 256; j++) {
	if (F[j]) {
	    // j
	    if (rle) {
		rle--;
	    } else {
		*cp++ = j;
		if (!rle && j && F[j-1])  {
		    for(rle=j+1; rle<256 && F[rle]; rle++)
			;
		    rle -= j+1;
		    *cp++ = rle;
		}
		//fprintf(stderr, "%d: %d %d\n", j, rle, N[j]);
	    }
	}
    }
    *cp++ = 0;
    
    return cp - op;
}

static int decode_alphabet(uint8_t *cp, uint8_t *cp_end, int *F) {
    if (cp == cp_end)
	return 0;

    uint8_t *op = cp;
    int rle = 0;
    int j = *cp++;
    if (cp+2 >= cp_end)
	goto carefully;

    do {
	F[j] = 1;
	if (!rle && j+1 == *cp) {
	    j = *cp++;
	    rle = *cp++;
	} else if (rle) {
	    rle--;
	    j++;
	    if (j > 255)
		return 0;
	} else {
	    j = *cp++;
	}
    } while(j && cp+2 < cp_end);

 carefully:
    if (j) {
	do {
	    F[j] = 1;
	    if(cp >= cp_end) return 0;
	    if (!rle && j+1 == *cp) {
		if (cp+1 >= cp_end) return 0;
		j = *cp++;
		rle = *cp++;
	    } else if (rle) {
		rle--;
		j++;
		if (j > 255)
		    return 0;
	    } else {
		if (cp >= cp_end) return 0;
		j = *cp++;
	    }
	} while(j && cp < cp_end);
    }

    return cp - op;
}

static int encode_freq(uint8_t *cp, int *F) {
    uint8_t *op = cp;
    int j;

    cp += encode_alphabet(cp, F);

    for (j = 0; j < 256; j++) {
	if (F[j])
	    cp += var_put_u32(cp, NULL, F[j]);
    }

    return cp - op;
}

static int decode_freq(uint8_t *cp, uint8_t *cp_end, int *F, int *fsum) {
    if (cp == cp_end)
	return 0;

    uint8_t *op = cp;
    cp += decode_alphabet(cp, cp_end, F);

    int j, tot = 0;
    for (j = 0; j < 256; j++) {
	if (F[j]) {
	    cp += var_get_u32(cp, cp_end, (unsigned int *)&F[j]);
	    tot += F[j];
	}
    }

    *fsum = tot;
    return cp - op;
}


// Use the order-0 freqs in F0 to encode the order-1 stats in F.
// All symbols present in F are present in F0, but some in F0 will
// be empty in F.  Thus we run-length encode the 0 frequencies.
static int encode_freq_d(uint8_t *cp, int *F0, int *F) {
    uint8_t *op = cp;
    int j, dz;

    for (dz = j = 0; j < 256; j++) {
	if (F0[j]) {
	    if (F[j] != 0) {
		if (dz) {
		    // Replace dz zeros with zero + dz-1 run length
		    cp -= dz-1;
		    *cp++ = dz-1;
		}
		dz = 0;
		cp += var_put_u32(cp, NULL, F[j]);
	    } else {
		//fprintf(stderr, "2: j=%d F0[j]=%d, F[j]=%d, dz=%d\n", j, F0[j], F[j], dz);
		dz++;
		*cp++ = 0;
	    }
	} else {
	    assert(F[j] == 0);
	}
    }
    
    if (dz) {
	cp -= dz-1;
	*cp++ = dz-1;
    }

    return cp - op;
}

static int decode_freq_d(uint8_t *cp, uint8_t *cp_end, int *F0, int *F, int *total) {
    if (cp == cp_end)
	return 0;

    uint8_t *op = cp;
    int j, dz, T = 0;

    for (j = dz = 0; j < 256 && cp < cp_end; j++) {
	//if (F0[j]) fprintf(stderr, "F0[%d]=%d\n", j, F0[j]);
	if (!F0[j])
	    continue;

	uint32_t f;
	if (dz) {
	    f = 0;
	    dz--;
	} else {
	    if (cp >= cp_end) return 0;
	    cp += var_get_u32(cp, cp_end, &f);
	    if (f == 0) {
		if (cp >= cp_end) return 0;
		dz = *cp++;
	    }
	}
	F[j] = f;
	T += f;
    }

    if (total) *total = T;
    return cp - op;
}

unsigned int rans_compress_bound_4x16(unsigned int size, int order) {
    return (order == 0
	? 1.05*size + 257*3 + 4
	: 1.05*size + 257*257*3 + 4 + 257*3+4) +
	((order & X_PACK) ? 1 : 0) +
	((order & X_RLE) ? 1 + 257*3+4: 0) + 20;
}

// Compresses in_size bytes from 'in' to *out_size bytes in 'out'.
//
// NB: The output buffer does not hold the original size, so it is up to
// the caller to store this.
static
unsigned char *rans_compress_O0_4x16(unsigned char *in, unsigned int in_size,
				     unsigned char *out, unsigned int *out_size) {
    unsigned char *cp, *out_end;
    RansEncSymbol syms[256];
    RansState rans0;
    RansState rans2;
    RansState rans1;
    RansState rans3;
    uint8_t* ptr;
    int F[256+MAGIC] = {0}, i, j, tab_size = 0, rle, x;
    int bound = rans_compress_bound_4x16(in_size,0)-20; // -20 for order/size/meta

    if (!out) {
	*out_size = bound;
	out = malloc(*out_size);
    }
    if (!out || bound > *out_size)
	return NULL;

    ptr = out_end = out + bound;

    if (in_size == 0)
	goto empty;

    // Compute statistics
    hist8(in, in_size, F);

    // Normalise so frequences sum to power of 2
    int fsum = in_size;
    int max_val = round2(fsum);
    if (max_val > TOTFREQ)
	max_val = TOTFREQ;

    if (normalise_freq(F, fsum, max_val) < 0)
	return NULL;
    fsum=max_val;

    cp = out;
    cp += encode_freq(cp, F);
    tab_size = cp-out;
    //write(2, out+4, cp-(out+4));

    if (normalise_freq(F, fsum, TOTFREQ) < 0)
	return NULL;

    // Encode statistics.
    for (x = rle = j = 0; j < 256; j++) {
	if (F[j]) {
	    RansEncSymbolInit(&syms[j], x, F[j], TF_SHIFT);
	    x += F[j];
	}
    }

    RansEncInit(&rans0);
    RansEncInit(&rans1);
    RansEncInit(&rans2);
    RansEncInit(&rans3);

    switch (i=(in_size&3)) {
    case 3: RansEncPutSymbol(&rans2, &ptr, &syms[in[in_size-(i-2)]]);
    case 2: RansEncPutSymbol(&rans1, &ptr, &syms[in[in_size-(i-1)]]);
    case 1: RansEncPutSymbol(&rans0, &ptr, &syms[in[in_size-(i-0)]]);
    case 0:
	break;
    }
    for (i=(in_size &~3); i>0; i-=4) {
	RansEncSymbol *s3 = &syms[in[i-1]];
	RansEncSymbol *s2 = &syms[in[i-2]];
	RansEncSymbol *s1 = &syms[in[i-3]];
	RansEncSymbol *s0 = &syms[in[i-4]];

#if 1
	RansEncPutSymbol(&rans3, &ptr, s3);
	RansEncPutSymbol(&rans2, &ptr, s2);
	RansEncPutSymbol(&rans1, &ptr, s1);
	RansEncPutSymbol(&rans0, &ptr, s0);
#else
	// Slightly beter on gcc, much better on clang
	uint16_t *ptr16 = (uint16_t *)ptr;

	if (rans3 >= s3->x_max) *--ptr16 = (uint16_t)rans3, rans3 >>= 16;
	if (rans2 >= s2->x_max) *--ptr16 = (uint16_t)rans2, rans2 >>= 16;
	uint32_t q3 = (uint32_t) (((uint64_t)rans3 * s3->rcp_freq) >> s3->rcp_shift);
	uint32_t q2 = (uint32_t) (((uint64_t)rans2 * s2->rcp_freq) >> s2->rcp_shift);
	rans3 += s3->bias + q3 * s3->cmpl_freq;
	rans2 += s2->bias + q2 * s2->cmpl_freq;

	if (rans1 >= s1->x_max) *--ptr16 = (uint16_t)rans1, rans1 >>= 16;
	if (rans0 >= s0->x_max) *--ptr16 = (uint16_t)rans0, rans0 >>= 16;
	uint32_t q1 = (uint32_t) (((uint64_t)rans1 * s1->rcp_freq) >> s1->rcp_shift);
	uint32_t q0 = (uint32_t) (((uint64_t)rans0 * s0->rcp_freq) >> s0->rcp_shift);
	rans1 += s1->bias + q1 * s1->cmpl_freq;
	rans0 += s0->bias + q0 * s0->cmpl_freq;

	ptr = (uint8_t *)ptr16;
#endif
    }

    RansEncFlush(&rans3, &ptr);
    RansEncFlush(&rans2, &ptr);
    RansEncFlush(&rans1, &ptr);
    RansEncFlush(&rans0, &ptr);

 empty:
    // Finalise block size and return it
    *out_size = (out_end - ptr) + tab_size;

    memmove(out + tab_size, ptr, out_end-ptr);

    return out;
}

typedef struct {
    unsigned char R[TOTFREQ];
} ari_decoder;

static
unsigned char *rans_uncompress_O0_4x16(unsigned char *in, unsigned int in_size,
				       unsigned char *out, unsigned int out_sz) {
    if (in_size < 16) // 4-states at least
	return NULL;

    if (out_sz >= INT_MAX)
	return NULL; // protect against some overflow cases

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    if (out_sz > 100000)
	return NULL;
#endif

    /* Load in the static tables */
    unsigned char *cp = in, *out_free = NULL;
    unsigned char *cp_end = in + in_size - 8; // within 8 => be extra safe
    int i, j;
    unsigned int x, y;
    uint16_t sfreq[TOTFREQ+32];
    uint16_t sbase[TOTFREQ+32]; // faster to use 32-bit on clang
    uint8_t  ssym [TOTFREQ+64]; // faster to use 16-bit on clang

    if (!out)
	out_free = out = malloc(out_sz);
    if (!out)
	return NULL;

    // Precompute reverse lookup of frequency.
    int F[256] = {0}, fsum;
    int fsz = decode_freq(cp, cp_end, F, &fsum);
    if (!fsz)
	goto err;
    cp += fsz;

    normalise_freq_shift(F, fsum, TOTFREQ);

    // Build symbols; fixme, do as part of decode, see the _d variant
    for (j = x = 0; j < 256; j++) {
	if (F[j]) {
	    if (x + F[j] > TOTFREQ || F[j] < 0)
		goto err;
	    for (y = 0; y < F[j]; y++) {
		ssym [y + x] = j;
		sfreq[y + x] = F[j];
		sbase[y + x] = y;
	    }
	    x += F[j];
	}
    }

    if (x != TOTFREQ)
	goto err;

    if (cp+16 > cp_end+8)
	goto err;

    RansState R[4];
    RansDecInit(&R[0], &cp); if (R[0] < RANS_BYTE_L) goto err;
    RansDecInit(&R[1], &cp); if (R[1] < RANS_BYTE_L) goto err;
    RansDecInit(&R[2], &cp); if (R[2] < RANS_BYTE_L) goto err;
    RansDecInit(&R[3], &cp); if (R[3] < RANS_BYTE_L) goto err;

// Simple version is comparable to below, but only with -O3
//
//    for (i = 0; cp < cp_end-8 && i < (out_sz&~7); i+=8) {
//        for(j=0; j<8;j++) {
//	    RansState m = RansDecGet(&R[j%4], TF_SHIFT);
//	    R[j%4] = sfreq[m] * (R[j%4] >> TF_SHIFT) + sbase[m];
//	    out[i+j] = ssym[m];
//	    RansDecRenorm(&R[j%4], &cp);
//        }
//    }

    for (i = 0; cp < cp_end-8 && i < (out_sz&~7); i+=8) {
	for (j = 0; j < 8; j+=4) {
	    RansState m0 = RansDecGet(&R[0], TF_SHIFT);
	    RansState m1 = RansDecGet(&R[1], TF_SHIFT);
	    R[0] = sfreq[m0] * (R[0] >> TF_SHIFT) + sbase[m0];
	    R[1] = sfreq[m1] * (R[1] >> TF_SHIFT) + sbase[m1];

	    RansDecRenorm(&R[0], &cp);
	    RansDecRenorm(&R[1], &cp);

	    out[i+j+0] = ssym[m0];
	    out[i+j+1] = ssym[m1];

	    RansState m3 = RansDecGet(&R[2], TF_SHIFT);
	    RansState m4 = RansDecGet(&R[3], TF_SHIFT);

	    R[2] = sfreq[m3] * (R[2] >> TF_SHIFT) + sbase[m3];
	    R[3] = sfreq[m4] * (R[3] >> TF_SHIFT) + sbase[m4];

	    out[i+j+2] = ssym[m3];
	    out[i+j+3] = ssym[m4];

	    RansDecRenorm(&R[2], &cp);
	    RansDecRenorm(&R[3], &cp);
	}
    }

    // remainder
    for (; i < out_sz; i++) {
        RansState m = RansDecGet(&R[i%4], TF_SHIFT);
	R[i%4] = sfreq[m] * (R[i%4] >> TF_SHIFT) + sbase[m];
	out[i] = ssym[m];
	RansDecRenormSafe(&R[i%4], &cp, cp_end+8);
    }

    //fprintf(stderr, "    0 Decoded %d bytes\n", (int)(cp-in)); //c-size

    return out;

 err:
    free(out_free);
    return NULL;
}

#ifdef UNUSED
static void hist1_1(unsigned char *in, unsigned int in_size,
		    int F0[256][256], int T0[256]) {
    unsigned int last_i, i;
    unsigned char c;

    for (last_i=i=0; i<in_size; i++) {
	F0[last_i][c = in[i]]++;
	T0[last_i]++;
	last_i = c;
    }
}
#endif

static void hist1_4(unsigned char *in, unsigned int in_size,
		    int F0[256][256], int *T0) {
    int T1[256+MAGIC] = {0}, T2[256+MAGIC] = {0}, T3[256+MAGIC] = {0};
    unsigned int idiv4 = in_size/4;
    int i;
    unsigned char c0=0, c1=0, c2=0, c3=0;

    unsigned char *in0 = in + 0;
    unsigned char *in1 = in + idiv4;
    unsigned char *in2 = in + idiv4*2;
    unsigned char *in3 = in + idiv4*3;

    unsigned char last_0 = 0, last_1 = in1[-1], last_2 = in2[-1], last_3 = in3[-1];
    //unsigned char last_0 = 0, last_1 = 0, last_2 = 0, last_3 = 0;

    unsigned char *in0_end = in1;

    while (in0 < in0_end) {
	F0[last_0][c0 = *in0++]++;
	T0[last_0]++;
	last_0 = c0;

	F0[last_1][c1 = *in1++]++;
	T1[last_1]++;
	last_1 = c1;

	F0[last_2][c2 = *in2++]++;
	T2[last_2]++;
	last_2 = c2;

	F0[last_3][c3 = *in3++]++;
	T3[last_3]++;
	last_3 = c3;
    }

    while (in3 < in + in_size) {
	F0[last_3][c3 = *in3++]++;
	T3[last_3]++;
	last_3 = c3;
    }

    for (i = 0; i < 256; i++) {
	T0[i]+=T1[i]+T2[i]+T3[i];
    }
}

//-----------------------------------------------------------------------------
static uint8_t *rle_encode(uint8_t *data, int64_t len,
			   uint8_t *out_meta, unsigned int *out_meta_len, uint64_t *out_len) {
    uint64_t i, j, k;
    int last = -1;
    unsigned char *out = malloc(len*2);
    if (!out)
	return NULL;

    // Two pass:  Firstly compute which symbols are worth using RLE on.
    int64_t saved[256+MAGIC] = {0};

    if (len > 256) {
	// 186/450
	// Interleaved buffers to avoid cache collisions
	int64_t saved2[256+MAGIC] = {0};
	int64_t saved3[256+MAGIC] = {0};
	int64_t saved4[256+MAGIC] = {0};
	int64_t len4 = len&~3;
	for (i = 0; i < len4; i+=4) {
	    int d1 = (data[i+0] == last)     <<1;
	    int d2 = (data[i+1] == data[i+0])<<1;
	    int d3 = (data[i+2] == data[i+1])<<1;
	    int d4 = (data[i+3] == data[i+2])<<1;
	    last = data[i+3];
	    saved [data[i+0]] += d1-1;
	    saved2[data[i+1]] += d2-1;
	    saved3[data[i+2]] += d3-1;
	    saved4[data[i+3]] += d4-1;
	}
	while (i < len) {
	    int d = (data[i] == last)<<1;
	    saved[data[i]] += d - 1;
	    last = data[i];
	    i++;
	}
	for (i = 0; i < 256; i++)
	    saved[i] += saved2[i] + saved3[i] + saved4[i];
    } else {
	// 163/391
	for (i = 0; i < len; i++) {
	    if (data[i] == last) {
		saved[data[i]]++;
	    } else {
		saved[data[i]]--;
		last = data[i];
	    }
	}
    }

    // fixme: pack into bits instead of bytes if many symbols?
    for (j = 1, i = 0; i < 256; i++) {
	if (saved[i] > 0) {
	    out_meta[j++] = i;
	}
    }
    if (j == 0) {
	// not worth it.
	free(out);
	return NULL;
    }
    out_meta[0] = j-1; // NB: all symbols being worth it is 0 not 256.

    // 2nd pass: perform RLE itself to out[] and out_meta[] arrays.
    for (i = k = 0; i < len; i++) {
	out[k++] = data[i];
	if (saved[data[i]] > 0) {
	    int run_len = i;
	    int last = data[i];
	    while (i < len && data[i] == last)
		i++;
	    i--;
	    run_len = i-run_len;

	    j += var_put_u32(&out_meta[j], NULL, run_len);
	}
    }
    
    *out_meta_len = j;
    *out_len = k;
    return out;
}

// On input *out_len holds the allocated size of out[].
// On output it holds the used size of out[].
static uint8_t *rle_decode(uint8_t *in, int64_t in_len, uint8_t *meta, uint32_t meta_sz,
			   unsigned char *out, uint64_t *out_len) {
    uint64_t j, m = 0;
    uint8_t *meta_end = meta + meta_sz;

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    if (*out_len > 100000)
	return NULL;
#endif

    if (meta_sz == 0 || meta[0] >= meta_sz)
	return NULL;

    int saved[256] = {0};
    j = meta[m++];
    if (j == 0)
	j = 256;
    for (; j && m < meta_sz; j--)
	saved[meta[m++]]=1;

    j = 0;
    meta += m;
    uint8_t *in_end = in + in_len;
    uint8_t *out_end = out + *out_len;
    uint8_t *outp = out;
    while (in < in_end) {
	uint8_t b = *in++;
	if (saved[b]) {
	    uint32_t run_len, v;
	    if ((v = var_get_u32(meta, meta_end, &run_len)) == 0)
		return NULL;
	    meta += v;
	    run_len++;
	    if (outp + run_len > out_end)
		run_len = out_end - outp;
	    memset(outp, b, run_len);
	    outp += run_len;
	} else {
	    if (outp >= out_end)
		break;
	    *outp++ = b;
	}
    }

    *out_len = outp-out;
    return out;
}

//-----------------------------------------------------------------------------

static
unsigned char *rans_compress_O1_4x16(unsigned char *in, unsigned int in_size,
				     unsigned char *out, unsigned int *out_size) {
    unsigned char *cp, *out_end, *op;
    unsigned int tab_size;
    RansEncSymbol syms[256][256];
    int bound = rans_compress_bound_4x16(in_size,1)-20; // -20 for order/size/meta

    if (!out) {
	*out_size = bound;
	out = malloc(*out_size);
    }
    if (!out || bound > *out_size)
	return NULL;

    out_end = out + bound;

    int F[256][256] = {{0}}, T[256+MAGIC] = {0}, i, j;

    //memset(F, 0, 256*256*sizeof(int));
    //memset(T, 0, 256*sizeof(int));

    hist1_4(in, in_size, F, T);
    F[0][in[1*(in_size>>2)]]++;
    F[0][in[2*(in_size>>2)]]++;
    F[0][in[3*(in_size>>2)]]++;
    T[0]+=3;

    op = cp = out;
    *cp++ = 0; // uncompressed header marker

    // Encode the order-0 symbols for use in the order-1 frequency tables
    int F0[256+MAGIC] = {0};
    present8(in, in_size, F0);
    F0[0]=1;
    cp += encode_alphabet(cp, F0);

    // Normalise so T[i] == TOTFREQ_O1
    for (i = 0; i < 256; i++) {
	unsigned int x;

	if (F0[i] == 0)
	    continue;

	// Order-1 frequencies often end up totalling under TOTFREQ.
	// In this case it's smaller to output the real frequencies
	// prior to normalisation and normalise after (with an extra
	// normalisation step needed in the decoder too).
	//
	// Thus we normalise to a power of 2 only, store those,
	// and renormalise later here (and in decoder) by bit-shift
	// to get to the fixed size.
	int max_val = round2(T[i]);

	// Small optimisation; trade frequency table size with
	// accuracy of stats for smaller data.
	// It's a reasonable trade off for small datasets.
	int ns = 0;
	for (j = 0; j < 256; j++)
	    if (F[i][j])
		ns++;
	if (ns < 64 && max_val > 128) max_val /= 2;
	if (max_val > 1024)           max_val /= 2;

	if (max_val > TOTFREQ_O1)
	    max_val = TOTFREQ_O1;

	if (normalise_freq(F[i], T[i], max_val) < 0)
	    return NULL;
	T[i]=max_val;

	cp += encode_freq_d(cp, F0, F[i]);

	normalise_freq_shift(F[i], T[i], TOTFREQ_O1); T[i]=TOTFREQ_O1;

	int *F_i_ = F[i];
	for (x = j = 0; j < 256; j++) {
	    RansEncSymbolInit(&syms[i][j], x, F_i_[j], TF_SHIFT_O1);
	    x += F_i_[j];
	}

    }

    if (cp - op > 1000) {
	// try rans0 compression of header
	unsigned int u_freq_sz = cp-(op+1);
	unsigned int c_freq_sz;
	unsigned char *c_freq = rans_compress_O0_4x16(op+1, u_freq_sz, NULL, &c_freq_sz);
	if (c_freq && c_freq_sz + 6 < cp-op) {
	    *op++ = 1; // compressed
	    op += var_put_u32(op, NULL, u_freq_sz);
	    op += var_put_u32(op, NULL, c_freq_sz);
	    memcpy(op, c_freq, c_freq_sz);
	    cp = op+c_freq_sz;
	}
	free(c_freq);
    }

    //write(2, out+4, cp-(out+4));
    tab_size = cp - out;
    assert(tab_size < 257*257*3);
    
    RansState rans0, rans1, rans2, rans3;
    RansEncInit(&rans0);
    RansEncInit(&rans1);
    RansEncInit(&rans2);
    RansEncInit(&rans3);

    uint8_t* ptr = out_end;

    int isz4 = in_size>>2;
    int i0 = 1*isz4-2;
    int i1 = 2*isz4-2;
    int i2 = 3*isz4-2;
    int i3 = 4*isz4-2;

    unsigned char l0 = in[i0+1];
    unsigned char l1 = in[i1+1];
    unsigned char l2 = in[i2+1];
    unsigned char l3 = in[i3+1];

    // Deal with the remainder
    l3 = in[in_size-1];
    for (i3 = in_size-2; i3 > 4*isz4-2; i3--) {
	unsigned char c3 = in[i3];
	RansEncPutSymbol(&rans3, &ptr, &syms[c3][l3]);
	l3 = c3;
    }

    for (; i0 >= 0; i0--, i1--, i2--, i3--) {
	unsigned char c0, c1, c2, c3;
	RansEncSymbol *s3 = &syms[c3 = in[i3]][l3];
	RansEncSymbol *s2 = &syms[c2 = in[i2]][l2];
	RansEncSymbol *s1 = &syms[c1 = in[i1]][l1];
	RansEncSymbol *s0 = &syms[c0 = in[i0]][l0];

	RansEncPutSymbol(&rans3, &ptr, s3);
	RansEncPutSymbol(&rans2, &ptr, s2);
	RansEncPutSymbol(&rans1, &ptr, s1);
	RansEncPutSymbol(&rans0, &ptr, s0);

	l0 = c0;
	l1 = c1;
	l2 = c2;
	l3 = c3;
    }

    RansEncPutSymbol(&rans3, &ptr, &syms[0][l3]);
    RansEncPutSymbol(&rans2, &ptr, &syms[0][l2]);
    RansEncPutSymbol(&rans1, &ptr, &syms[0][l1]);
    RansEncPutSymbol(&rans0, &ptr, &syms[0][l0]);

    RansEncFlush(&rans3, &ptr);
    RansEncFlush(&rans2, &ptr);
    RansEncFlush(&rans1, &ptr);
    RansEncFlush(&rans0, &ptr);

    *out_size = (out_end - ptr) + tab_size;

    cp = out;
    memmove(out + tab_size, ptr, out_end-ptr);

    return out;
}

typedef struct {
    uint16_t c;
    uint16_t f;
    uint16_t b;
} sb_t;

#ifndef NO_THREADS
/*
 * Thread local storage per thread in the pool.
 */
pthread_once_t rans_once = PTHREAD_ONCE_INIT;
pthread_key_t rans_key;

static void rans_tls_init(void) {
    pthread_key_create(&rans_key, free);
}
#endif

static
unsigned char *rans_uncompress_O1sfb_4x16(unsigned char *in, unsigned int in_size,
					  unsigned char *out, unsigned int out_sz) {
    if (in_size < 16) // 4-states at least
	return NULL;

    if (out_sz >= INT_MAX)
	return NULL; // protect against some overflow cases

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    if (out_sz > 100000)
	return NULL;
#endif

    /* Load in the static tables */
    unsigned char *cp = in, *cp_end = in+in_size, *out_free = NULL;
    unsigned char *c_freq = NULL;
    int i, j = -999;
    unsigned int x, y;

#ifndef NO_THREADS
    /*
     * The calloc below is expensive as it's a large structure.  We
     * could use malloc, but we're only initialising parts of the structure
     * that we need to, as dictated by the frequency table.  This is far
     * faster than initialising everything (ie malloc+memset => calloc).
     * Not initialising the data means malformed input with mismatching
     * frequency tables to actual data can lead to accessing of the
     * uninitialised sfb table and in turn potential leakage of the
     * uninitialised memory returned by malloc.  That could be anything at
     * all, including important encryption keys used within a server (for
     * example).
     *
     * However (I hope!) we don't care about leaking about the sfb symbol
     * frequencies previously computed by an earlier execution of *this*
     * code.  So calloc once and reuse is the fastest alternative.
     *
     * We do this through pthread local storage as we don't know if this
     * code is being executed in many threads simultaneously.
     */
    pthread_once(&rans_once, rans_tls_init);

    sb_t *sfb_ = pthread_getspecific(rans_key);
    if (!sfb_) {
	sfb_ = calloc(256*TOTFREQ_O1, sizeof(*sfb_));
	pthread_setspecific(rans_key, sfb_);
    }
#else
    sb_t *sfb_ = calloc(256*TOTFREQ_O1, sizeof(*sfb_));
#endif

    if (!sfb_)
	return NULL;
    sb_t *sfb[256];
    for (i = 0; i < 256; i++)
	sfb[i]=  sfb_ + i*TOTFREQ_O1;

    //memset(D, 0, 256*sizeof(*D));

    if (!out)
	out_free = out = malloc(out_sz);

    if (!out)
	goto err;

    //fprintf(stderr, "out_sz=%d\n", out_sz);

    // compressed header? If so uncompress it
    unsigned char *tab_end = NULL;
    unsigned char *c_freq_end = cp_end;
    if (*cp++ == 1) {
	uint32_t u_freq_sz, c_freq_sz;
	cp += var_get_u32(cp, cp_end, &u_freq_sz);
	cp += var_get_u32(cp, cp_end, &c_freq_sz);
	if (c_freq_sz >= cp_end - cp - 16)
	    goto err;
	tab_end = cp + c_freq_sz;
	if (!(c_freq = rans_uncompress_O0_4x16(cp, c_freq_sz, NULL, u_freq_sz)))
	    goto err;
	cp = c_freq;
	c_freq_end = c_freq + u_freq_sz;
    }

    // Decode order-0 symbol list; avoids needing in order-1 tables
    int F0[256] = {0};
    int fsz = decode_alphabet(cp, c_freq_end, F0);
    if (!fsz)
	goto err;
    cp += fsz;

    if (cp >= c_freq_end)
	goto err;

    for (i = 0; i < 256; i++) {
	if (F0[i] == 0)
	    continue;

	int F[256] = {0}, T = 0;
	fsz = decode_freq_d(cp, c_freq_end, F0, F, &T);
	if (!fsz)
	    goto err;
	cp += fsz;

	if (!T) {
	    //fprintf(stderr, "No freq for F_%d\n", i);
	    continue;
	}

	normalise_freq_shift(F, T, TOTFREQ_O1);

	// Build symbols; fixme, do as part of decode, see the _d variant
	for (j = x = 0; j < 256; j++) {
	    if (F[j]) {
		if (x + F[j] > TOTFREQ_O1 || F[j] < 0)
		    goto err;
		for (y = 0; y < F[j]; y++) {
		    sfb[i][y + x].c = j;
		    sfb[i][y + x].f = F[j];
		    sfb[i][y + x].b = y;
		}
		x += F[j];
	    }
	}
	if (x != TOTFREQ_O1)
	    goto err;
    }

    if (tab_end)
	cp = tab_end;
    free(c_freq);
    c_freq = NULL;

    if (cp+16 > cp_end)
	goto err;

    RansState rans0, rans1, rans2, rans3;
    uint8_t *ptr = cp, *ptr_end = in + in_size - 8;
    RansDecInit(&rans0, &ptr); if (rans0 < RANS_BYTE_L) goto err;
    RansDecInit(&rans1, &ptr); if (rans1 < RANS_BYTE_L) goto err;
    RansDecInit(&rans2, &ptr); if (rans2 < RANS_BYTE_L) goto err;
    RansDecInit(&rans3, &ptr); if (rans3 < RANS_BYTE_L) goto err;

    unsigned int isz4 = out_sz>>2;
    int l0 = 0, l1 = 0, l2 = 0, l3 = 0;
    unsigned int i4[] = {0*isz4, 1*isz4, 2*isz4, 3*isz4};

    RansState R[4];
    R[0] = rans0;
    R[1] = rans1;
    R[2] = rans2;
    R[3] = rans3;

    const uint32_t mask = ((1u << TF_SHIFT_O1)-1);
    for (; i4[0] < isz4; i4[0]++, i4[1]++, i4[2]++, i4[3]++) {
	uint32_t m[4];
	uint32_t c[4];

	m[0] = R[0] & mask;
	R[0] = sfb[l0][m[0]].f * (R[0]>>TF_SHIFT_O1) + sfb[l0][m[0]].b;
	c[0] = sfb[l0][m[0]].c;

	m[1] = R[1] & mask;
	R[1] = sfb[l1][m[1]].f * (R[1]>>TF_SHIFT_O1) + sfb[l1][m[1]].b;
	c[1] = sfb[l1][m[1]].c;

	m[2] = R[2] & mask;
	R[2] = sfb[l2][m[2]].f * (R[2]>>TF_SHIFT_O1) + sfb[l2][m[2]].b;
	c[2] = sfb[l2][m[2]].c;

	m[3] = R[3] & mask;
	R[3] = sfb[l3][m[3]].f * (R[3]>>TF_SHIFT_O1) + sfb[l3][m[3]].b;
	c[3] = sfb[l3][m[3]].c;

	// TODO: inline expansion of 4-way packing here is about 4% faster.
	out[i4[0]] = c[0];
	out[i4[1]] = c[1];
	out[i4[2]] = c[2];
	out[i4[3]] = c[3];

	if (ptr < ptr_end) {
	    RansDecRenorm(&R[0], &ptr);
	    RansDecRenorm(&R[1], &ptr);
	    RansDecRenorm(&R[2], &ptr);
	    RansDecRenorm(&R[3], &ptr);
	} else {
	    RansDecRenormSafe(&R[0], &ptr, ptr_end+8);
	    RansDecRenormSafe(&R[1], &ptr, ptr_end+8);
	    RansDecRenormSafe(&R[2], &ptr, ptr_end+8);
	    RansDecRenormSafe(&R[3], &ptr, ptr_end+8);
	}

	l0 = c[0];
	l1 = c[1];
	l2 = c[2];
	l3 = c[3];
    }

    // Remainder
    for (; i4[3] < out_sz; i4[3]++) {
	uint32_t m3 = R[3] & ((1u<<TF_SHIFT_O1)-1);
	unsigned char c3 = sfb[l3][m3].c;
	out[i4[3]] = c3;
	R[3] = sfb[l3][m3].f * (R[3]>>TF_SHIFT_O1) + sfb[l3][m3].b;
	RansDecRenormSafe(&R[3], &ptr, ptr_end + 8);
	l3 = c3;
    }
    
    //fprintf(stderr, "    1 Decoded %d bytes\n", (int)(ptr-in)); //c-size

#ifdef NO_THREADS
    free(sfb_);
#endif
    return out;

 err:
#ifdef NO_THREADS
    free(sfb_);
#endif
    free(out_free);
    free(c_freq);

    return NULL;
}

/*-----------------------------------------------------------------------------
 * Simple interface to the order-0 vs order-1 encoders and decoders.
 *
 * Smallest is method, <in_size> <input>, so worst case 2 bytes longer.
 */
unsigned char *rans_compress_to_4x16(unsigned char *in,  unsigned int in_size,
				     unsigned char *out, unsigned int *out_size,
				     int order) {
    unsigned int c_meta_len;
    uint8_t *meta = NULL, *rle = NULL, *packed = NULL;

    if (!out) {
	*out_size = rans_compress_bound_4x16(in_size, order);
	if (!(out = malloc(*out_size)))
	    return NULL;
    }
    unsigned char *out_end = out + *out_size;

    if (in_size%4 != 0 || in_size <= 20)
	order &= ~X_4;

    if (order & X_4) {
	unsigned char *in4 = malloc(in_size);
	if (!in4)
	    return NULL;
	unsigned int len4 = in_size/4, i4[4];
	int i;

	for (i = 0; i < 4; i++)
	    i4[i] = i*len4;
	for (i = 0; i4[0] < len4; i4[0]++, i4[1]++, i4[2]++, i4[3]++, i+=4) {
	    in4[i4[0]] = in[i+0];
	    in4[i4[1]] = in[i+1];
	    in4[i4[2]] = in[i+2];
	    in4[i4[3]] = in[i+3];
	}

	unsigned int olen2;
	unsigned char *out2;
	c_meta_len = 1;
	*out = order;
	c_meta_len += var_put_u32(out+c_meta_len, out_end, in_size);
	out2 = out+26;
        for (i = 0; i < 4; i++) {
            // Brute force try all methods.
            int j, m[] = {1,64,128,0}, best_j = 0, best_sz = in_size+10;
            for (j = 0; j < 4; j++) {
                if ((order & m[j]) != m[j])
                    continue;
                olen2 = *out_size - (out2 - out);
                rans_compress_to_4x16(in4+i*len4, len4, out2, &olen2, m[j] | X_NOSZ);
                if (best_sz > olen2) {
                    best_sz = olen2;
                    best_j = j;
                }
            }
	    if (best_j != j-1) {
		olen2 = *out_size - (out2 - out);
		rans_compress_to_4x16(in4+i*len4, len4, out2, &olen2, m[best_j] | X_NOSZ);
	    }
            //rans_compress_to_4x16(in4+i*len4, len4, out2, &olen2, (order & ~X_4) | X_NOSZ);
            out2 += olen2;
            c_meta_len += var_put_u32(out+c_meta_len, out_end, olen2);
        }
	memmove(out+c_meta_len, out+26, out2-(out+26));
	free(in4);
	*out_size = c_meta_len + out2-(out+26);
	return out;
    }

    if (order & X_CAT) {
	out[0] = X_CAT;
	c_meta_len = 1;
	c_meta_len += var_put_u32(&out[1], out_end, in_size);
	memcpy(out+c_meta_len, in, in_size);
	*out_size = c_meta_len + in_size;
	return out;
    }

    int do_pack = order & X_PACK;
    int do_rle  = order & X_RLE;
    int no_size = order & X_NOSZ;

    out[0] = order;
    c_meta_len = 1;

    if (!no_size)
	c_meta_len += var_put_u32(&out[1], out_end, in_size);

    order &= 0xf;

    // Format is compressed meta-data, compressed data.
    // Meta-data can be empty, pack, rle lengths, or pack + rle lengths.
    // Data is either the original data, bit-packed packed, rle literals or
    // packed + rle literals.

    if (do_pack && in_size) {
	// PACK 2, 4 or 8 symbols into one byte.
	int pmeta_len;
	uint64_t packed_len;
	packed = hts_pack(in, in_size, out+c_meta_len, &pmeta_len, &packed_len);
	if (!packed || (pmeta_len == 1 && out[c_meta_len] > 16)) {
	    out[0] &= ~X_PACK;
	    do_pack = 0;
	    free(packed);
	    packed = NULL;
	} else {
	    in = packed;
	    in_size = packed_len;
	    c_meta_len += pmeta_len;

	    // Could derive this rather than storing verbatim.
	    // Orig size * 8/nbits (+1 if not multiple of 8/n)
	    int sz = var_put_u32(out+c_meta_len, out_end, in_size);
	    c_meta_len += sz;
	    *out_size -= sz;
	}
    } else if (do_pack) {
	out[0] &= ~X_PACK;
    }

    if (do_rle && in_size) {
	// RLE 'in' -> rle_length + rle_literals arrays
	unsigned int rmeta_len, c_rmeta_len;
	uint64_t rle_len;
	c_rmeta_len = in_size;
	if (!(meta = malloc(c_rmeta_len)))
	    return NULL;

	rle = rle_encode(in, in_size, meta, &rmeta_len, &rle_len);
	if (!rle || rle_len + rmeta_len >= .99*in_size) {
	    // Not worth the speed hit.
	    out[0] &= ~X_RLE;
	    do_rle = 0;
	    free(rle);
	    rle = NULL;
	} else {
	    // Compress lengths with O0 and literals with O0/O1 ("order" param)
	    int sz = var_put_u32(out+c_meta_len, out_end, rmeta_len*2), sz2;
	    sz += var_put_u32(out+c_meta_len+sz, out_end, rle_len);
	    c_rmeta_len = *out_size - (c_meta_len+sz+5);
	    rans_compress_O0_4x16(meta, rmeta_len, out+c_meta_len+sz+5, &c_rmeta_len);
	    if (c_rmeta_len < rmeta_len) {
		sz2 = var_put_u32(out+c_meta_len+sz, out_end, c_rmeta_len);
		memmove(out+c_meta_len+sz+sz2, out+c_meta_len+sz+5, c_rmeta_len);
	    } else {
		// Uncompressed RLE meta-data as too small
		sz = var_put_u32(out+c_meta_len, out_end, rmeta_len*2+1);
		sz2 = var_put_u32(out+c_meta_len+sz, out_end, rle_len);
		memcpy(out+c_meta_len+sz+sz2, meta, rmeta_len);
		c_rmeta_len = rmeta_len;
	    }

	    c_meta_len += sz + sz2 + c_rmeta_len;

	    in = rle;
	    in_size = rle_len;
	}

	free(meta);
    } else if (do_rle) {
	out[0] &= ~X_RLE;
    }

    *out_size -= c_meta_len;
    if (order && in_size < 8) {
	out[0] &= ~1;
	order  &= ~1;
    }

    if (order == 1)
	rans_compress_O1_4x16(in, in_size, out+c_meta_len, out_size);
    else
	rans_compress_O0_4x16(in, in_size, out+c_meta_len, out_size);

    if (*out_size >= in_size) {
	out[0] &= ~3;
	out[0] |= X_CAT | no_size;
	memcpy(out+c_meta_len, in, in_size);
	*out_size = in_size;
    }

    free(rle);
    free(packed);

    *out_size += c_meta_len;

    return out;
}

unsigned char *rans_compress_4x16(unsigned char *in, unsigned int in_size,
				  unsigned int *out_size, int order) {
    return rans_compress_to_4x16(in, in_size, NULL, out_size, order);
}

unsigned char *rans_uncompress_to_4x16(unsigned char *in,  unsigned int in_size,
				       unsigned char *out, unsigned int *out_size) {
    unsigned char *in_end = in + in_size;
    unsigned char *out_free = NULL, *tmp_free = NULL, *meta_free = NULL;

    if (in_size == 0)
	return NULL;

    if (*in & X_4) {
	unsigned int ulen, olen, clen4[4], c_meta_len = 1;
	int i, j;

	// Decode lengths
	c_meta_len += var_get_u32(in+c_meta_len, in_end, &ulen);
	if (!out) {
	    if (ulen >= INT_MAX)
		return NULL;
	    if (!(out_free = out = malloc(ulen)))
		return NULL;
	    *out_size = ulen;
	}
	if (ulen != *out_size || (ulen%4 != 0)) {
	    free(out_free);
	    return NULL;
	}

	for (i = 0; i < 4; i++) {
	    c_meta_len += var_get_u32(in+c_meta_len, in_end, &clen4[i]);
	    if (c_meta_len > in_size || clen4[i] > in_size || clen4[i] < 1) {
		free(out_free);
		return NULL;
	    }
	}

	// We can call this with a larger buffer, but once we've determined
	// how much we really use we limit it so the recursion becomes easier
	// to limit.
	if (c_meta_len + clen4[0] + clen4[1] + clen4[2] + clen4[3] > in_size) {
	    free(out_free);
	    return NULL;
	}
	in_size = c_meta_len + clen4[0] + clen4[1] + clen4[2] + clen4[3];

	//fprintf(stderr, "    x4 meta %d\n", c_meta_len); //c-size

	// Uncompress the 4 streams
	unsigned char *out4 = malloc(ulen);
	if (!out4) {
	    free(out_free);
	    return NULL;
	}
	for (i = 0; i < 4; i++) {
	    olen = ulen/4;
	    if (in_size < c_meta_len) {
		free(out_free);
		free(out4);
		return NULL;
	    }
	    if (!rans_uncompress_to_4x16(in+c_meta_len, in_size-c_meta_len, out4 + i*(ulen/4), &olen)
		|| olen != ulen/4) {
		free(out_free);
		free(out4);
		return NULL;
	    }
	    c_meta_len += clen4[i];
	}

	unsigned int i4[4] = {0*(ulen/4), 1*(ulen/4), 2*(ulen/4), 3*(ulen/4)};
	j = 0;
	while (j < ulen) {
	    out[j++] = out4[i4[0]++];
	    out[j++] = out4[i4[1]++];
	    out[j++] = out4[i4[2]++];
	    out[j++] = out4[i4[3]++];
	}
	free(out4);
	*out_size = ulen;
	return out;
    }

    int order = *in++;  in_size--;
    int do_pack = order & X_PACK;
    int do_rle  = order & X_RLE;
    int do_cat  = order & X_CAT;
    int no_size = order & X_NOSZ;
    order &= 1;

    int sz = 0;
    unsigned int osz;
    if (!no_size)
	sz = var_get_u32(in, in_end, &osz);
    else
	sz = 0, osz = *out_size;
    in += sz;
    in_size -= sz;

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    if (osz > 100000)
	return NULL;
#endif

    if (no_size && !out)
	goto err; // Need one or the other

    if (!out) {
	*out_size = osz;
	if (!(out = out_free = malloc(*out_size)))
	    return NULL;
    } else {
	if (*out_size < osz)
	goto err;
	*out_size = osz;
    }

//    if (do_pack || do_rle) {
//	in += sz; // size field not needed when pure rANS
//	in_size -= sz;
//    }

    uint32_t c_meta_size = 0;
    unsigned int tmp1_size = *out_size;
    unsigned int tmp2_size = *out_size;
    unsigned int tmp3_size = *out_size;
    unsigned char *tmp1 = NULL, *tmp2 = NULL, *tmp3 = NULL, *tmp = NULL;

    // Need In, Out and Tmp buffers with temporary buffer of the same size
    // as output.  All use rANS, but with optional transforms (none, RLE,
    // Pack, or both).
    //
    //                    rans   unrle  unpack
    // If none:     in -> out
    // If RLE:      in -> tmp -> out
    // If Pack:     in -> tmp        -> out
    // If RLE+Pack: in -> out -> tmp -> out
    //                    tmp1   tmp2   tmp3
    //
    // So rans is in   -> tmp1
    // RLE     is tmp1 -> tmp2
    // Unpack  is tmp2 -> tmp3

    // Format is meta data (Pack and RLE in that order if present),
    // followed by rANS compressed data.

    if (do_pack || do_rle) {
	if (!(tmp = tmp_free = malloc(*out_size)))
	    goto err;
	if (do_pack && do_rle) {
	    tmp1 = out;
	    tmp2 = tmp;
	    tmp3 = out;
	} else if (do_pack) {
	    tmp1 = tmp;
	    tmp2 = tmp1;
	    tmp3 = out;
	} else if (do_rle) {
	    tmp1 = tmp;
	    tmp2 = out;
	    tmp3 = out;
	}
    } else {
	// neither
	tmp  = NULL;
	tmp1 = out;
	tmp2 = out;
	tmp3 = out;
    }

    
    // Decode the bit-packing map.
    uint8_t map[16] = {0};
    int npacked_sym = 0;
    uint64_t unpacked_sz = 0; // FIXME: rename to packed_per_byte
    if (do_pack) {
	c_meta_size = hts_unpack_meta(in, in_size, *out_size, map, &npacked_sym);
	if (c_meta_size == 0)
	    goto err;

	unpacked_sz = osz;
	in      += c_meta_size;
	in_size -= c_meta_size;

	// New unpacked size.  We could derive this bit from *out_size
	// and npacked_sym.
	unsigned int osz;
	sz = var_get_u32(in, in_end, &osz);
	in += sz;
	in_size -= sz;
	if (osz > tmp1_size)
	    goto err;
	tmp1_size = osz;
    }

    uint8_t *meta = NULL;
    uint32_t u_meta_size = 0;
    if (do_rle) {
	// Uncompress meta data
	uint32_t c_meta_size, rle_len, sz;
	sz  = var_get_u32(in,    in_end, &u_meta_size);
	sz += var_get_u32(in+sz, in_end, &rle_len);
	if (rle_len > tmp1_size) // should never grow
	    goto err;
	if (u_meta_size & 1) {
	    meta = in + sz;
	    u_meta_size = u_meta_size/2 > (in_end-meta) ? (in_end-meta) : u_meta_size/2;
	    c_meta_size = u_meta_size;
	} else {
	    sz += var_get_u32(in+sz, in_end, &c_meta_size);
	    u_meta_size /= 2;
	    meta_free = meta = rans_uncompress_O0_4x16(in+sz, in_size-sz, NULL, u_meta_size);
	    if (!meta)
		goto err;
	}
	if (c_meta_size+sz > in_size)
	    goto err;
	in      += c_meta_size+sz;
	in_size -= c_meta_size+sz;
	tmp1_size = rle_len;
    }
   
    //fprintf(stderr, "    meta_size %d bytes\n", (int)(in - orig_in)); //c-size

    // uncompress RLE data.  in -> tmp1
    if (in_size) {
	if (do_cat) {
	    //fprintf(stderr, "    CAT %d\n", tmp1_size); //c-size
	    if (tmp1_size > in_size)
		goto err;
	    if (tmp1_size > *out_size)
		goto err;
	    memcpy(tmp1, in, tmp1_size);
	} else {
	    tmp1 = order
		? rans_uncompress_O1sfb_4x16(in, in_size, tmp1, tmp1_size)
		: rans_uncompress_O0_4x16(in, in_size, tmp1, tmp1_size);
	    if (!tmp1)
		goto err;
	}
    } else {
	tmp1 = NULL;
	tmp1_size = 0;
    }
    tmp2_size = tmp3_size = tmp1_size;

    if (do_rle) {
	// Unpack RLE.  tmp1 -> tmp2.
	uint64_t unrle_size = *out_size;
	if (!rle_decode(tmp1, tmp1_size, meta, u_meta_size, tmp2, &unrle_size))
	    goto err;
	tmp3_size = tmp2_size = unrle_size;
	free(meta_free);
	meta_free = NULL;
    }
    if (do_pack) {
	// Unpack bits via pack-map.  tmp2 -> tmp3
	if (npacked_sym == 1)
	    unpacked_sz = tmp2_size;
	//uint8_t *porig = unpack(tmp2, tmp2_size, unpacked_sz, npacked_sym, map);
	//memcpy(tmp3, porig, unpacked_sz);
	if (!hts_unpack(tmp2, tmp2_size, tmp3, unpacked_sz, npacked_sym, map))
	    goto err;
	tmp3_size = unpacked_sz;
    }

    if (tmp)
	free(tmp);

    *out_size = tmp3_size;
    return tmp3;

 err:
    free(meta_free);
    free(out_free);
    free(tmp_free);
    return NULL;
}

unsigned char *rans_uncompress_4x16(unsigned char *in, unsigned int in_size,
				    unsigned int *out_size) {
    return rans_uncompress_to_4x16(in, in_size, NULL, out_size);
}
