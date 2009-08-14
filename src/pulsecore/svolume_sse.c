/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2009 Wim Taymans <wim.taymans@collabora.co.uk>

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/timeval.h>
#include <pulsecore/random.h>
#include <pulsecore/macro.h>
#include <pulsecore/g711.h>
#include <pulsecore/core-util.h>

#include "cpu-x86.h"

#include "sample-util.h"
#include "endianmacros.h"

#define VOLUME_32x16(s,v)                  /* .. |   vh  |   vl  | */                   \
      " pxor %%xmm4, %%xmm4          \n\t" /* .. |    0  |    0  | */                   \
      " punpcklwd %%xmm4, "#s"       \n\t" /* .. |    0  |   p0  | */                   \
      " pcmpgtw "#s", %%xmm4         \n\t" /* .. |    0  | s(p0) | */                   \
      " pand "#v", %%xmm4            \n\t" /* .. |    0  |  (vl) | */                   \
      " movdqa "#s", %%xmm5          \n\t"                                              \
      " pmulhuw "#v", "#s"           \n\t" /* .. |    0  | vl*p0 | */                   \
      " psubd %%xmm4, "#s"           \n\t" /* .. |    0  | vl*p0 | + sign correct */    \
      " psrld $16, "#v"              \n\t" /* .. |   p0  |    0  | */                   \
      " pmaddwd %%xmm5, "#v"         \n\t" /* .. |    p0 * vh    | */                   \
      " paddd "#s", "#v"             \n\t" /* .. |    p0 * v0    | */                   \
      " packssdw "#v", "#v"          \n\t" /* .. | p1*v1 | p0*v0 | */         

#define MOD_ADD(a,b) \
      " add "#a", %3                 \n\t" /* channel += inc           */ \
      " mov %3, %4                   \n\t"                                \
      " sub "#b", %4                 \n\t" /* tmp = channel - channels */ \
      " cmp "#b", %3                 \n\t" /* if (channel >= channels) */ \
      " cmovae %4, %3                \n\t" /*   channel = tmp          */

/* swap 16 bits */
#define SWAP_16(s) \
      " movdqa "#s", %%xmm4          \n\t" /* .. |  h  l |  */ \
      " psrlw $8, %%xmm4             \n\t" /* .. |  0  h |  */ \
      " psllw $8, "#s"               \n\t" /* .. |  l  0 |  */ \
      " por %%xmm4, "#s"             \n\t" /* .. |  l  h |  */

/* swap 2 registers 16 bits for better pairing */
#define SWAP_16_2(s1,s2) \
      " movdqa "#s1", %%xmm4         \n\t" /* .. |  h  l |  */ \
      " movdqa "#s2", %%xmm5         \n\t"                     \
      " psrlw $8, %%xmm4             \n\t" /* .. |  0  h |  */ \
      " psrlw $8, %%xmm5             \n\t"                     \
      " psllw $8, "#s1"              \n\t" /* .. |  l  0 |  */ \
      " psllw $8, "#s2"              \n\t"                     \
      " por %%xmm4, "#s1"            \n\t" /* .. |  l  h |  */ \
      " por %%xmm5, "#s2"            \n\t"

static void
pa_volume_s16ne_sse (int16_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
  pa_reg_x86 channel, temp;

  /* the max number of samples we process at a time, this is also the max amount
   * we overread the volume array, which should have enough padding. */
  channels = MAX (8, channels);

  __asm__ __volatile__ (
    " xor %3, %3                    \n\t"
    " sar $1, %2                    \n\t" /* length /= sizeof (int16_t) */

    " test $1, %2                   \n\t" /* check for odd samples */
    " je 2f                         \n\t" 

    " movd (%1, %3, 4), %%xmm0      \n\t" /* |  v0h  |  v0l  | */
    " movw (%0), %4                 \n\t" /*     ..  |   p0  | */
    " movd %4, %%xmm1               \n\t" 
    VOLUME_32x16 (%%xmm1, %%xmm0)
    " movd %%xmm0, %4               \n\t" /*     ..  | p0*v0 | */
    " movw %4, (%0)                 \n\t" 
    " add $2, %0                    \n\t"
    MOD_ADD ($1, %5)

    "2:                             \n\t"
    " sar $1, %2                    \n\t" /* prepare for processing 2 samples at a time */
    " test $1, %2                   \n\t" 
    " je 4f                         \n\t" 

    "3:                             \n\t" /* do samples in groups of 2 */
    " movq (%1, %3, 4), %%xmm0      \n\t" /* |  v1h  |  v1l  |  v0h  |  v0l  | */
    " movd (%0), %%xmm1             \n\t" /*              .. |   p1  |  p0   | */
    VOLUME_32x16 (%%xmm1, %%xmm0)
    " movd %%xmm0, (%0)             \n\t" /*              .. | p1*v1 | p0*v0 | */
    " add $4, %0                    \n\t"
    MOD_ADD ($2, %5)

    "4:                             \n\t"
    " sar $1, %2                    \n\t" /* prepare for processing 4 samples at a time */
    " test $1, %2                   \n\t" 
    " je 6f                         \n\t" 

    "5:                             \n\t" /* do samples in groups of 4 */
    " movdqu (%1, %3, 4), %%xmm0    \n\t" /* |  v3h  |  v3l  ..  v0h  |  v0l  | */
    " movq (%0), %%xmm1             \n\t" /*              .. |   p3  ..  p0   | */
    VOLUME_32x16 (%%xmm1, %%xmm0)
    " movq %%xmm0, (%0)             \n\t" /*              .. | p3*v3 .. p0*v0 | */
    " add $8, %0                    \n\t"
    MOD_ADD ($4, %5)

    "6:                             \n\t"
    " sar $1, %2                    \n\t" /* prepare for processing 8 samples at a time */
    " cmp $0, %2                    \n\t"
    " je 8f                         \n\t"

    "7:                             \n\t" /* do samples in groups of 8 */
    " movdqu (%1, %3, 4), %%xmm0    \n\t" /* |  v3h  |  v3l  ..  v0h  |  v0l  | */
    " movdqu 16(%1, %3, 4), %%xmm2  \n\t" /* |  v7h  |  v7l  ..  v4h  |  v4l  | */
    " movq (%0), %%xmm1             \n\t" /*              .. |   p3  ..  p0   | */
    " movq 8(%0), %%xmm3            \n\t" /*              .. |   p7  ..  p4   | */
    VOLUME_32x16 (%%xmm1, %%xmm0)
    VOLUME_32x16 (%%xmm3, %%xmm2)
    " movq %%xmm0, (%0)             \n\t" /*              .. | p3*v3 .. p0*v0 | */
    " movq %%xmm2, 8(%0)            \n\t" /*              .. | p7*v7 .. p4*v4 | */
    " add $16, %0                   \n\t"
    MOD_ADD ($8, %5)
    " dec %2                        \n\t"
    " jne 7b                        \n\t"
    "8:                             \n\t"

    : "+r" (samples), "+r" (volumes), "+r" (length), "=D" (channel), "=&r" (temp)
    : "r" ((pa_reg_x86)channels)
    : "cc"
  );
}

static void
pa_volume_s16re_sse (int16_t *samples, int32_t *volumes, unsigned channels, unsigned length)
{
  pa_reg_x86 channel, temp;

  /* the max number of samples we process at a time, this is also the max amount
   * we overread the volume array, which should have enough padding. */
  channels = MAX (8, channels);

  __asm__ __volatile__ (
    " xor %3, %3                    \n\t"
    " sar $1, %2                    \n\t" /* length /= sizeof (int16_t) */

    " test $1, %2                   \n\t" /* check for odd samples */
    " je 2f                         \n\t" 

    " movd (%1, %3, 4), %%xmm0      \n\t" /* |  v0h  |  v0l  | */
    " movw (%0), %4                 \n\t" /*     ..  |   p0  | */
    " rorw $8, %4                   \n\t" 
    " movd %4, %%xmm1               \n\t" 
    VOLUME_32x16 (%%xmm1, %%xmm0)
    " movd %%xmm0, %4               \n\t" /*     ..  | p0*v0 | */
    " rorw $8, %4                   \n\t" 
    " movw %4, (%0)                 \n\t" 
    " add $2, %0                    \n\t"
    MOD_ADD ($1, %5)

    "2:                             \n\t"
    " sar $1, %2                    \n\t" /* prepare for processing 2 samples at a time */
    " test $1, %2                   \n\t"
    " je 4f                         \n\t" 

    "3:                             \n\t" /* do samples in groups of 2 */
    " movq (%1, %3, 4), %%xmm0      \n\t" /* |  v1h  |  v1l  |  v0h  |  v0l  | */
    " movd (%0), %%xmm1             \n\t" /*              .. |   p1  |  p0   | */
    SWAP_16 (%%xmm1)
    VOLUME_32x16 (%%xmm1, %%xmm0)
    SWAP_16 (%%xmm0)
    " movd %%xmm0, (%0)             \n\t" /*              .. | p1*v1 | p0*v0 | */
    " add $4, %0                    \n\t"
    MOD_ADD ($2, %5)

    "4:                             \n\t"
    " sar $1, %2                    \n\t" /* prepare for processing 4 samples at a time */
    " test $1, %2                   \n\t"
    " je 6f                         \n\t" 

    /* FIXME, we can do aligned access of the volume values if we can guarantee
     * that the array is 16 bytes aligned, we probably have to do the odd values
     * after this then. */
    "5:                             \n\t" /* do samples in groups of 4 */
    " movdqu (%1, %3, 4), %%xmm0    \n\t" /* |  v3h  |  v3l  ..  v0h  |  v0l  | */
    " movq (%0), %%xmm1             \n\t" /*              .. |   p3  ..  p0   | */
    SWAP_16 (%%xmm1)
    VOLUME_32x16 (%%xmm1, %%xmm0)
    SWAP_16 (%%xmm0)
    " movq %%xmm0, (%0)             \n\t" /*              .. | p3*v3 .. p0*v0 | */
    " add $8, %0                    \n\t"
    MOD_ADD ($4, %5)

    "6:                             \n\t"
    " sar $1, %2                    \n\t" /* prepare for processing 8 samples at a time */
    " cmp $0, %2                    \n\t"
    " je 8f                         \n\t"

    "7:                             \n\t" /* do samples in groups of 8 */
    " movdqu (%1, %3, 4), %%xmm0    \n\t" /* |  v3h  |  v3l  ..  v0h  |  v0l  | */
    " movdqu 16(%1, %3, 4), %%xmm2  \n\t" /* |  v7h  |  v7l  ..  v4h  |  v4l  | */
    " movq (%0), %%xmm1             \n\t" /*              .. |   p3  ..  p0   | */
    " movq 8(%0), %%xmm3            \n\t" /*              .. |   p7  ..  p4   | */
    SWAP_16_2 (%%xmm1, %%xmm3)
    VOLUME_32x16 (%%xmm1, %%xmm0)
    VOLUME_32x16 (%%xmm3, %%xmm2)
    SWAP_16_2 (%%xmm0, %%xmm2)
    " movq %%xmm0, (%0)             \n\t" /*              .. | p3*v3 .. p0*v0 | */
    " movq %%xmm2, 8(%0)            \n\t" /*              .. | p7*v7 .. p4*v4 | */
    " add $16, %0                   \n\t"
    MOD_ADD ($8, %5)
    " dec %2                        \n\t"
    " jne 7b                        \n\t"
    "8:                             \n\t"

    : "+r" (samples), "+r" (volumes), "+r" (length), "=D" (channel), "=&r" (temp)
    : "r" ((pa_reg_x86)channels)
    : "cc"
  );
}

#undef RUN_TEST

#ifdef RUN_TEST
#define CHANNELS 2
#define SAMPLES 1021
#define TIMES 1000
#define PADDING 16

static void run_test (void) {
  int16_t samples[SAMPLES];
  int16_t samples_ref[SAMPLES];
  int16_t samples_orig[SAMPLES];
  int32_t volumes[CHANNELS + PADDING];
  int i, j, padding;
  pa_do_volume_func_t func;
  struct timeval start, stop;

  func = pa_get_volume_func (PA_SAMPLE_S16NE);

  printf ("checking SSE %zd\n", sizeof (samples));

  pa_random (samples, sizeof (samples));
  memcpy (samples_ref, samples, sizeof (samples));
  memcpy (samples_orig, samples, sizeof (samples));

  for (i = 0; i < CHANNELS; i++)
    volumes[i] = rand() >> 1;
  for (padding = 0; padding < PADDING; padding++, i++)
    volumes[i] = volumes[padding];

  func (samples_ref, volumes, CHANNELS, sizeof (samples));
  pa_volume_s16ne_sse (samples, volumes, CHANNELS, sizeof (samples));
  for (i = 0; i < SAMPLES; i++) {
    if (samples[i] != samples_ref[i]) {
      printf ("%d: %04x != %04x (%04x * %04x)\n", i, samples[i], samples_ref[i],
              samples_orig[i], volumes[i % CHANNELS]);
    }
  }

  pa_gettimeofday(&start);
  for (j = 0; j < TIMES; j++) {
    memcpy (samples, samples_orig, sizeof (samples));
    pa_volume_s16ne_sse (samples, volumes, CHANNELS, sizeof (samples));
  }
  pa_gettimeofday(&stop);
  pa_log_info("SSE: %llu usec.", (long long unsigned int)pa_timeval_diff (&stop, &start));

  pa_gettimeofday(&start);
  for (j = 0; j < TIMES; j++) {
    memcpy (samples_ref, samples_orig, sizeof (samples));
    func (samples_ref, volumes, CHANNELS, sizeof (samples));
  }
  pa_gettimeofday(&stop);
  pa_log_info("ref: %llu usec.", (long long unsigned int)pa_timeval_diff (&stop, &start));
}
#endif

void pa_volume_func_init_sse (pa_cpu_x86_flag_t flags) {
  pa_log_info("Initialising SSE optimized functions.");

#ifdef RUN_TEST
  run_test ();
#endif

  pa_set_volume_func (PA_SAMPLE_S16NE,     (pa_do_volume_func_t) pa_volume_s16ne_sse);
  pa_set_volume_func (PA_SAMPLE_S16RE,     (pa_do_volume_func_t) pa_volume_s16re_sse);
}
