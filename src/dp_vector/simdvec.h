/* Configuration constants and initialization of our SIMD vector environment.
 * 
 * This header file may be included by non-vector code: for example,
 * P7_SPARSEMASK code that is called by the vectorized fwd-bck local
 * decoder.
 */
#ifndef p7SIMDVEC_INCLUDED
#define p7SIMDVEC_INCLUDED

/* Define our SIMD vector sizes.
 * SSE, Altivec/VMX are 128b/16B vectors, but we anticipate others.
 * Intel AVX is already roadmapped out to 1024b/128B vectors. 
 * See note [1] below on memory alignment, SIMD vectors, and
 * malloc(). We need these constants and macros not only in the
 * vector implementation, but also in the P7_SPARSEMASK code that
 * interfaces with the vector f/b decoder.
 */
#define p7_VALIGN   16		/* Vector memory must be aligned on 16-byte boundaries   */
#define p7_VNF      4		/* Number of floats per SIMD vector (Forward, Backward)  */
#define p7_VNW      8		/* Number of shorts (words) per SIMD vector (Viterbi)    */
#define p7_VNB      16		/* Number of bytes per SIMD vector (SSV, MSV)            */
#define p7_VALIMASK (~0xf)      /* Ptrs are aligned using & p7_VALIMASK                  */


// Define new constants for AVX, AVX-512 to support running multiple versions while testing
#define p7_VALIGN_AVX   32		/* AVX Vector memory must be aligned on 32-byte boundaries   */
#define p7_VNF_AVX      8		/* Number of floats per AVX SIMD vector (Forward, Backward)  */
#define p7_VNW_AVX      16		/* Number of shorts (words) per AVX SIMD vector (Viterbi)    */
#define p7_VNB_AVX      32		/* Number of bytes per AVX SIMD vector (SSV, MSV)            */
#define p7_VALIMASK_AVX (~0x1f)      /* Ptrs are aligned using & p7_VALIMASK_AVX                  */

#define p7_VALIGN_AVX_512   64		/* AVX-512 Vector memory must be aligned on 64-byte boundaries   */
#define p7_VNF_AVX_512      16		/* Number of floats per AVX-512 SIMD vector (Forward, Backward)  */
#define p7_VNW_AVX_512      32		/* Number of shorts (words) per AVX-512 SIMD vector (Viterbi)    */
#define p7_VNB_AVX_512      64		/* Number of bytes per AVX-512 SIMD vector (SSV, MSV)            */
#define p7_VALIMASK_AVX_512 (~0x3f)      /* Ptrs are aligned using & p7_VALIMASK                  */

/* In calculating Q, the number of vectors we need in a row, we have
 * to make sure there's at least 2, or a striped implementation fails.
 */
#define P7_NVB(M)   ( ESL_MAX(2, ((((M)-1) / p7_VNB) + 1)))   /* 16 uchars  */
#define P7_NVW(M)   ( ESL_MAX(2, ((((M)-1) / p7_VNW) + 1)))   /*  8 words   */
#define P7_NVF(M)   ( ESL_MAX(2, ((((M)-1) / p7_VNF) + 1)))   /*  4 floats  */

// values for 256-bit AVX vectors
#define P7_NVB_AVX(M)   ( ESL_MAX(2, ((((M)-1) / p7_VNB_AVX) + 1)))   /* 32 uchars  */
#define P7_NVW_AVX(M)   ( ESL_MAX(2, ((((M)-1) / p7_VNW_AVX) + 1)))   /*  16 words   */
#define P7_NVF_AVX(M)   ( ESL_MAX(2, ((((M)-1) / p7_VNF_AVX) + 1)))   /*  8 floats  */

// values for 512-bit AVX-512 vectors
#define P7_NVB_AVX_512(M)   ( ESL_MAX(2, ((((M)-1) / p7_VNB_AVX_512) + 1)))   /* 64 uchars  */
#define P7_NVW_AVX_512(M)   ( ESL_MAX(2, ((((M)-1) / p7_VNW_AVX_512) + 1)))   /*  32 words   */
#define P7_NVF_AVX_512(M)   ( ESL_MAX(2, ((((M)-1) / p7_VNF_AVX_512) + 1)))   /*  16 floats  */

// Files to convert striped data from one vector size to another
extern void p7_restripe_byte(char *source, char *dest, int length, int source_vector_length, int dest_vector_length);
extern void p7_restripe_short(int16_t *source, int16_t *dest, int length, int source_vector_length, int dest_vector_length);
extern void p7_restripe_float(float *source, float *dest, int length, int source_vector_length, int dest_vector_length);

extern void p7_simdvec_Init(void);

/* [1]. On memory alignment, SIMD vectors, and malloc(): 
 * 
 * Yes, the C99 standard says malloc() mustreturn a pointer "suitably
 * aligned so that it may be aligned to a pointer of any type of
 * object" (C99 7.20.3). Yes, __m128 vectors are 16 bytes. Yes, you'd
 * think malloc() ought to be required return a pointer aligned on a
 * 16-byte boundary. But, no, malloc() doesn't have to do this;
 * malloc() will return improperly aligned memory on many systems.
 * Reason: vectors are officially considered "out of scope" of the C99
 * language.
 * 
 * So vector memory has to be manually aligned. For 16-byte alignment,
 * the idiom looks like the following, where for any to-be-aligned
 * ptr, we first allocate a raw (unaligned) memory space that's 15
 * bytes larger than we need, then set the aligned ptr into that space;
 * and when we free, we free the raw memory ptr:
 * 
 *   raw_mem = malloc(n + 15);
 *   ptr     = (__m128 *) ( ((uintptr_t) raw_mem + 15) & (~0xf));
 *   
 * To allow for arbitrary byte alignment (e.g. AVX), instead of
 * hard-coding 16-byte alignment with constants 15 and 0xf, we use
 * p7_VALIGN, (p7_VALIGN-1) and p7_VALIMASK.
 * 
 * Technically, the result of casting a non-NULL pointer to an integer
 * type is undefined (C99 6.3.2.3), but this idiom for manual memory
 * alignment is in widespread use so seems generally safe.
 * 
 * See also: posix_memalign(), as an alternative.
 */
#endif  /*p7SIMDVEC_INCLUDED*/
/*****************************************************************
 * @LICENSE@
 *
 * SVN $Id$
 * SVN $URL$
 *****************************************************************/
