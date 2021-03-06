/* AVX-512 implementation of P7_FILTERMX, the one-row DP memory for MSV and
 * Viterbi filters.
 * 
 * Contents:
 *   1. The P7_FILTERMX object
 *   2. Debugging and development routines
 *   3. Copyright and license information
 */

#include "p7_config.h"

#include <stdlib.h>
#include <stdio.h>

#if p7_CPU_ARCH == intel
#include <xmmintrin.h>
#include <emmintrin.h>
#endif /* intel arch */

#include "easel.h"

#include "dp_vector/simdvec.h"
#include "dp_vector/p7_filtermx.h"
#include "hardware/hardware.h"
/*****************************************************************
 * 1. The P7_FILTERMX object.
 *****************************************************************/


/* Function:  p7_filtermx_Create()
 * Synopsis:  Create a one-row DP matrix for MSV, VF.
 *
 * Purpose:   Allocate a reusable, resizeable one-row <P7_FILTERMX>
 *            suitable for MSV and Viterbi filter calculations on 
 *            query profiles of up to <allocM> consensus positions.
 *            
 *            <allocM> must be $\leq$ 100,000. This is an H3 design
 *            limit.
 *            
 * Args:      allocM - initial allocation size, in profile positions. (>=1, <=100000)
 *
 * Returns:   ptr to new <P7_FILTERMX>
 *
 * Throws:    <NULL> on allocation failure.
 */
P7_FILTERMX *
p7_filtermx_Create_avx512(int allocM)
{
 #ifdef HAVE_AVX512 
  P7_FILTERMX *fx = NULL;
  int          status;

  /* Contract checks / argument validation */
  ESL_DASSERT1( (allocM >= 1 && allocM <= 100000) );

  ESL_ALLOC(fx, sizeof(P7_FILTERMX));
  fx->simd = AVX512;
  fx->M         = 0;
  fx->dp        = NULL;
  fx->dp_mem    = NULL;
  fx->allocM    = 0;

  fx->dp_AVX_512    = NULL;
  fx->dp_mem_AVX_512 = NULL;
  fx->allocM_AVX_512 = 0;

  fx->type      = p7F_NONE;
#ifdef p7_DEBUGGING
  fx->do_dumping= FALSE;
  fx->dfp       = NULL;
#endif 
  
  /*                              64B per vector  * (MDI)states *  ~M/4 vectors    + alignment slop */
  ESL_ALLOC(fx->dp_mem_AVX_512, (sizeof(__m512i) * p7F_NSCELLS * P7_NVW_AVX_512(allocM)) + (p7_VALIGN_AVX_512-1));
  fx->allocM_AVX_512 = allocM;

  /* Manual memory alignment incantation: */
  fx->dp_AVX_512 = (__m512i *) ( (unsigned long int) (  (char *) fx->dp_mem_AVX_512 + (p7_VALIGN_AVX_512-1) ) & p7_VALIMASK_AVX_512);
  
  return fx;

 ERROR:
  p7_filtermx_Destroy(fx);
  return NULL;
  #endif //HAVE_AVX512
  #ifndef HAVE_AVX512
  return NULL;
  #endif
}


/* Function:  p7_filtermx_GrowTo()
 * Synopsis:  Resize filter DP matrix for new profile size.
 *
 * Purpose:   Given an existing filter matrix structure <fx>,
 *            and the dimension <M> of a new profile that 
 *            we're going to use (in consensus positions),
 *            assure that <fx> is large enough for such a 
 *            profile; reallocate and reinitialize as needed.
 *
 *            <p7_filtermx_Reuse(fx); p7_filtermx_GrowTo(fx, M)>
 *            is essentially equivalent to <p7_filtermx_Create(M)>,
 *            while minimizing reallocation.
 *
 * Returns:   <eslOK> on success.
 *
 * Throws:    <eslEMEM> on allocation failure. The state of
 *            <fx> is now undefined, and it should not be used.
 */
int
p7_filtermx_GrowTo_avx512(P7_FILTERMX *fx, int allocM)
{
#ifdef HAVE_AVX512
  int status;

  /* Contract checks / argument validation */
  ESL_DASSERT1( (allocM >= 1 && allocM <= 100000) );

  /* is it already big enough? */
  if (allocM <= fx->allocM_AVX_512) return eslOK;

  /* if not, grow it */
  ESL_REALLOC(fx->dp_mem_AVX_512, (sizeof(__m512i) * (p7F_NSCELLS * P7_NVW_AVX_512(allocM))) + (p7_VALIGN_AVX_512-1));
  fx->allocM_AVX_512 = allocM;
  fx->dp_AVX_512     = (__m512i *) ( (unsigned long int) ( (char *) fx->dp_mem_AVX_512 + (p7_VALIGN_AVX_512-1)) & p7_VALIMASK_AVX_512);


  return eslOK;

 ERROR:
  return status;
#endif //HAVE_AVX512
#ifndef HAVE_AVX512
  return eslENORESULT;
#endif 
}


/* Function:  p7_filtermx_Sizeof()
 * Synopsis:  Calculate and return the current size, in bytes.
 *
 * Purpose:   Calculate and return the current allocated size
 *            of the filter matrix <fx>, in bytes.
 *            
 *            Used in diagnostics, benchmarking of memory usage.
 *
 * Returns:   the allocation size.
 */
size_t 
p7_filtermx_Sizeof_avx512(const P7_FILTERMX *fx)
{
  size_t n = sizeof(P7_FILTERMX);

#ifdef HAVE_AVX512
  n += (sizeof(__m512i) * p7F_NSCELLS * P7_NVW_AVX_512(fx->allocM_AVX_512)) + (p7_VALIGN_AVX_512-1);
#endif
  return n;

}


/* Function:  p7_filtermx_MinSizeof()
 * Synopsis:  Calculate minimum size of a filter matrix, in bytes.
 *
 * Purpose:   Calculate and return the minimum allocation size
 *            required for a one-row filter matrix for a query 
 *            profile of <M> consensus positions.
 */
size_t
p7_filtermx_MinSizeof_avx512(int M)
{
  size_t n = sizeof(P7_FILTERMX);
#ifdef HAVE_AVX512  
  n += (sizeof(__m512i) * p7F_NSCELLS * P7_NVW_AVX_512(M)) + (p7_VALIGN-1);
 #endif 
  return n;
}

/* Function:  p7_filtermx_Destroy()
 * Synopsis:  Frees a one-row MSV/VF filter DP matrix.
 *
 * Purpose:   Frees the one-row MSV/VF filter DP matrix <fx>.
 *
 * Returns:   (void)
 */
void
p7_filtermx_Destroy_avx512(P7_FILTERMX *fx)
{
  if (fx) {
#ifdef HAVE_AVX512
    if (fx->dp_mem_AVX_512) free(fx->dp_mem_AVX_512);
#endif    
    free(fx);
  }
  return;
}
  

/*****************************************************************
 * 2. Debugging and development routines
 *****************************************************************/

#ifdef p7_DEBUGGING
/* Function:  p7_filtermx_DumpMFRow()
 * Synopsis:  Dump one row from MSV version of a DP matrix.
 *
 * Purpose:   Dump current row of MSV calculations from DP matrix <fx>
 *            for diagnostics, and include the values of specials
 *            <xE>, etc. The index <rowi> for the current row is used
 *            as a row label. This routine has to be specialized for
 *            the layout of the MSVFilter() row, because it's all
 *            match scores dp[0..q..Q-1], rather than triplets of
 *            M,D,I.
 * 
 *            If <rowi> is 0, print a header first too.
 * 
 *            The output format is coordinated with <p7_refmx_Dump()> to
 *            facilitate comparison to a known answer.
 *            
 *            This also works for an SSV filter row, for SSV implementations
 *            that use a single row of DP memory (like <_longtarget>). 
 *            The Knudsen assembly code SSV does not use any RAM.
 *
 * Returns:   <eslOK> on success.
 *
 * Throws:    <eslEMEM> on allocation failure. 
 */
int
p7_filtermx_DumpMFRow_avx512(const P7_FILTERMX *fx, int rowi, uint8_t xE, uint8_t xN, uint8_t xJ, uint8_t xB, uint8_t xC)
{
#ifdef HAVE_AVX512  
  int      Q  = P7_NVB_AVX_512(fx->M);	/* number of vectors in the MSV row */
  uint8_t *v  = NULL;		/* array of scores after unstriping them */
  int      q,z,k;
  union { __m512i v; uint8_t i[64]; } tmp;
  int      status;

  ESL_DASSERT1( (fx->type == p7F_MSVFILTER || fx->type == p7F_SSVFILTER) );

  /* We'll unstripe the whole row; then print it in its normal order. */
  ESL_ALLOC(v, sizeof(unsigned char) * ((Q*64)+1));
  v[0] = 0;

  /* Header (if we're on the 0th row)  */
  if (rowi == 0)
    {
      fprintf(fx->dfp, "       ");
      for (k = 0; k <= fx->M;  k++) fprintf(fx->dfp, "%3d ", k);
      fprintf(fx->dfp, "%3s %3s %3s %3s %3s\n", "E", "N", "J", "B", "C");
      fprintf(fx->dfp, "       ");
      for (k = 0; k <= fx->M+5;  k++) fprintf(fx->dfp, "%3s ", "---");
      fprintf(fx->dfp, "\n");
    }

  /* Unpack and unstripe, then print M's. */
  for (q = 0; q < Q; q++) {
    tmp.v = fx->dp[q];
    for (z = 0; z < 64; z++) v[q+Q*z+1] = tmp.i[z]; 
  }
  fprintf(fx->dfp, "%4d M ", rowi);
  for (k = 0; k <= fx->M; k++) fprintf(fx->dfp, "%3d ", v[k]);

  /* The specials */
  fprintf(fx->dfp, "%3d %3d %3d %3d %3d\n", xE, xN, xJ, xB, xC);

  /* I's are all 0's; print just to facilitate comparison to refmx. */
  fprintf(fx->dfp, "%4d I ", rowi);
  for (k = 0; k <= fx->M; k++) fprintf(fx->dfp, "%3d ", 0);
  fprintf(fx->dfp, "\n");

  /* D's are all 0's too */
  fprintf(fx->dfp, "%4d D ", rowi);
  for (k = 0; k <= fx->M; k++) fprintf(fx->dfp, "%3d ", 0);
  fprintf(fx->dfp, "\n\n");

  free(v);
  return eslOK;

ERROR:
  free(v);
  return status;
#endif //HAVE_AVX512
#ifndef HAVE_AVX512
  return eslENORESULT;
#endif 
}



/* Function:  p7_filtermx_DumpVFRow()
 * Synopsis:  Dump current row of ViterbiFilter (int16) filter matrix.
 *
 * Purpose:   Dump current row of ViterbiFilter (int16) filter DP
 *            matrix <fx> for diagnostics, and include the values of
 *            specials <xE>, etc. The index <rowi> for the current row
 *            is used as a row label.
 *
 *            If <rowi> is 0, print a header first too.
 * 
 *            The output format is coordinated with <p7_refmx_Dump()> to
 *            facilitate comparison to a known answer.
 *
 * Returns:   <eslOK> on success.
 *
 * Throws:    <eslEMEM> on allocation failure.
 */
int
p7_filtermx_DumpVFRow_avx512(const P7_FILTERMX *fx, int rowi, int16_t xE, int16_t xN, int16_t xJ, int16_t xB, int16_t xC)
{
#ifdef HAVE_AVX512  
  __m512i *dp = fx->dp;		/* enable MMXf(q), DMXf(q), IMXf(q) macros */
  int      Q  = P7_NVW_AVX_512(fx->M);	/* number of vectors in the VF row */
  int16_t *v  = NULL;		/* array of unstriped, uninterleaved scores  */
  int      q,z,k;
  union { __m512i v; int16_t i[32]; } tmp;
  int      status;

  ESL_ALLOC(v, sizeof(int16_t) * ((Q*32)+1));
  v[0] = 0;

  /* Header (if we're on the 0th row)
   */
  if (rowi == 0)
    {
      fprintf(fx->dfp, "       ");
      for (k = 0; k <= fx->M;  k++) fprintf(fx->dfp, "%6d ", k);
      fprintf(fx->dfp, "%6s %6s %6s %6s %6s\n", "E", "N", "J", "B", "C");
      fprintf(fx->dfp, "       ");
      for (k = 0; k <= fx->M+5;  k++) fprintf(fx->dfp, "%6s ", "------");
      fprintf(fx->dfp, "\n");
    }

  /* Unpack and unstripe, then print M's. */
  for (q = 0; q < Q; q++) {
    tmp.v = MMX_AVX_512f(q);
    for (z = 0; z < 32; z++) v[q+Q*z+1] = tmp.i[z];
  }
  fprintf(fx->dfp, "%4d M ", rowi);
  for (k = 0; k <= fx->M; k++) fprintf(fx->dfp, "%6d ", v[k]);

  /* The specials */
  fprintf(fx->dfp, "%6d %6d %6d %6d %6d\n", xE, xN, xJ, xB, xC);

  /* Unpack and unstripe, then print I's. */
  for (q = 0; q < Q; q++) {
    tmp.v = IMX_AVX_512f(q);
    for (z = 0; z < 32; z++) v[q+Q*z+1] = tmp.i[z];
  }
  fprintf(fx->dfp, "%4d I ", rowi);
  for (k = 0; k <= fx->M; k++) fprintf(fx->dfp, "%6d ", v[k]);
  fprintf(fx->dfp, "\n");

  /* Unpack, unstripe, then print D's. */
  for (q = 0; q < Q; q++) {
    tmp.v = DMX_AVX_512f(q);
    for (z = 0; z < 32; z++) v[q+Q*z+1] = tmp.i[z];
  }
  fprintf(fx->dfp, "%4d D ", rowi);
  for (k = 0; k <= fx->M; k++) fprintf(fx->dfp, "%6d ", v[k]);
  fprintf(fx->dfp, "\n\n");

  free(v);
  return eslOK;

ERROR:
  free(v);
  return status;
#endif //HAVE_AVX512
#ifndef HAVE_AVX512
  return eslENORESULT;
#endif 
}
#endif /*p7_DEBUGGING*/



/*****************************************************************
 * @LICENSE@
 * 
 * SVN $Id$
 * SVN $URL$
 *****************************************************************/
