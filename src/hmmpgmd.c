/* hmmpgmd: hmmer deamon searchs against a sequence database.
 * 
 * MSF, Thu Aug 12, 2010 [Janelia]
 * SVN $Id: hmmsearch.c 3324 2010-07-07 19:30:12Z wheelert $
 */
#include "p7_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <setjmp.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#ifndef HMMER_THREADS
#error "Program requires pthreads be enabled."
#endif /*HMMER_THREADS*/

#include "easel.h"
#include "esl_alphabet.h"
#include "esl_getopts.h"
#include "esl_sq.h"
#include "esl_sqio.h"
#include "esl_stopwatch.h"
#include "esl_threads.h"

#include "hmmer.h"
#include "hmmpgmd.h"

#define MAX_BUFFER (4*1024)

typedef struct queue_data_s {
  P7_HMM             *hmm;         /* query HMM                      */
  ESL_SQ             *seq;         /* query sequence                 */
  ESL_ALPHABET       *abc;         /* digital alphabet               */
  ESL_GETOPTS        *opts;        /* search specific options        */
  P7_BUILDER         *bld;         /* HMM construction configuration */

  int                 sock;        /* socket descriptor of client    */

  struct queue_data_s *next;
  struct queue_data_s *prev;

} QUEUE_DATA;

typedef struct {
  QUEUE_DATA      *head;
  QUEUE_DATA      *tail;
  pthread_mutex_t  mutex;
  pthread_cond_t   cond;
} SEARCH_QUEUE;

typedef struct {
  char          *pgm;
  int            sock_fd;
  SEARCH_QUEUE  *queue;
} CLIENTSIDE_ARGS;

static QUEUE_DATA *read_Queue(SEARCH_QUEUE *queue);

typedef struct {
  ESL_SQ           *sq_list;     /* list of sequences to process     */
  int               sq_cnt;      /* number of sequences              */

  pthread_mutex_t   mutex;       /* protect data                     */
  int              *sq_inx;      /* next sequence to process         */

  P7_HMM           *hmm;         /* query HMM                        */
  ESL_SQ           *seq;         /* query sequence                   */
  ESL_ALPHABET     *abc;         /* digital alphabet                 */
  ESL_GETOPTS      *opts;        /* search specific options          */
  P7_BUILDER       *bld;         /* HMM construction configuration   */

  double            elapsed;

  /* Structure created and populated by the individual threads.
   * The main thread is responsible for freeing up the memory.
   */
  P7_PIPELINE      *pli;         /* work pipeline                           */
  P7_TOPHITS       *th;          /* top hit results                         */
} WORKER_INFO;


static void setup_clientside_comm(ESL_GETOPTS *opts, SEARCH_QUEUE *queue, CLIENTSIDE_ARGS  *args);
static void send_results(ESL_STOPWATCH *w, WORKER_INFO *info, SEARCH_QUEUE *queue, CLIENTSIDE_ARGS *comm);

#define REPOPTS     "-E,-T,--cut_ga,--cut_nc,--cut_tc"
#define DOMREPOPTS  "--domE,--domT,--cut_ga,--cut_nc,--cut_tc"
#define INCOPTS     "--incE,--incT,--cut_ga,--cut_nc,--cut_tc"
#define INCDOMOPTS  "--incdomE,--incdomT,--cut_ga,--cut_nc,--cut_tc"
#define THRESHOPTS  "-E,-T,--domE,--domT,--incE,--incT,--incdomE,--incdomT,--cut_ga,--cut_nc,--cut_tc"

static ESL_OPTIONS cmdlineOpts[] = {
  /* name           type      default  env  range     toggles   reqs   incomp              help                                                      docgroup*/
  { "-h",           eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  NULL,            "show brief help on version and usage",                         1 },
  /* Other options */
  { "--master",     eslARG_NONE,    NULL, NULL, NULL,    NULL,  NULL,  "--worker",      "run program as the master server",                            12 },
  { "--worker",     eslARG_STRING,  NULL, NULL, NULL,    NULL,  NULL,  "--master",      "run program as a worker with server at <s>",                  12 },
  { "--cport",      eslARG_INT,  "41139", NULL, "n>1024",NULL,  NULL,  NULL,            "port to use for client/server communication",                 12 },
  { "--wport",      eslARG_INT,  "41023", NULL, "n>1024",NULL,  NULL,  NULL,            "port to use for server/worker communication",                 12 },
  { "--ccncts",     eslARG_INT,     "16", NULL, "n>0",   NULL,  NULL,  "--worker",      "maximum number of client side connections to accept",         12 },
  { "--wcncts",     eslARG_INT,     "32", NULL, "n>0",   NULL,  NULL,  "--worker",      "maximum number of worker side connections to accept",         12 },

  { "--cpu",        eslARG_INT, NULL,"HMMER_NCPU","n>=0",NULL,  NULL,  NULL,            "number of parallel CPU workers to use for multithreads",      12 },

  {  0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};

static ESL_OPTIONS searchOpts[] = {
  /* Control of output */
  { "--acc",        eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  NULL,            "prefer accessions over names in output",                       2 },
  { "--noali",      eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  NULL,            "don't output alignments, so output is smaller",                2 },
  { "--notextw",    eslARG_NONE,    NULL, NULL, NULL,    NULL,  NULL, "--textw",        "unlimit ASCII text output line width",                         2 },
  { "--textw",      eslARG_INT,    "120", NULL, "n>=120",NULL,  NULL, "--notextw",      "set max width of ASCII text output lines",                     2 },
  /* Control of scoring system */
  { "--popen",      eslARG_REAL,  "0.02", NULL, "0<=x<0.5",NULL,  NULL,  NULL,          "gap open probability",                                         3 },
  { "--pextend",    eslARG_REAL,   "0.4", NULL, "0<=x<1",  NULL,  NULL,  NULL,          "gap extend probability",                                       3 },
  { "--mxfile",     eslARG_INFILE,  NULL, NULL, NULL,      NULL,  NULL,  NULL,          "substitution score matrix [default: BLOSUM62]",                3 },
  /* Control of reporting thresholds */
  { "-E",           eslARG_REAL,  "10.0", NULL, "x>0",   NULL,  NULL,  REPOPTS,         "report sequences <= this E-value threshold in output",         4 },
  { "-T",           eslARG_REAL,   FALSE, NULL, NULL,    NULL,  NULL,  REPOPTS,         "report sequences >= this score threshold in output",           4 },
  { "--domE",       eslARG_REAL,  "10.0", NULL, "x>0",   NULL,  NULL,  DOMREPOPTS,      "report domains <= this E-value threshold in output",           4 },
  { "--domT",       eslARG_REAL,   FALSE, NULL, NULL,    NULL,  NULL,  DOMREPOPTS,      "report domains >= this score cutoff in output",                4 },
  /* Control of inclusion (significance) thresholds */
  { "--incE",       eslARG_REAL,  "0.01", NULL, "x>0",   NULL,  NULL,  INCOPTS,         "consider sequences <= this E-value threshold as significant",  5 },
  { "--incT",       eslARG_REAL,   FALSE, NULL, NULL,    NULL,  NULL,  INCOPTS,         "consider sequences >= this score threshold as significant",    5 },
  { "--incdomE",    eslARG_REAL,  "0.01", NULL, "x>0",   NULL,  NULL,  INCDOMOPTS,      "consider domains <= this E-value threshold as significant",    5 },
  { "--incdomT",    eslARG_REAL,   FALSE, NULL, NULL,    NULL,  NULL,  INCDOMOPTS,      "consider domains >= this score threshold as significant",      5 },
  /* Model-specific thresholding for both reporting and inclusion */
  { "--cut_ga",     eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  THRESHOPTS,      "use profile's GA gathering cutoffs to set all thresholding",   6 },
  { "--cut_nc",     eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  THRESHOPTS,      "use profile's NC noise cutoffs to set all thresholding",       6 },
  { "--cut_tc",     eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL,  THRESHOPTS,      "use profile's TC trusted cutoffs to set all thresholding",     6 },
  /* Control of acceleration pipeline */
  { "--max",        eslARG_NONE,   FALSE, NULL, NULL,    NULL,  NULL, "--F1,--F2,--F3", "Turn all heuristic filters off (less speed, more power)",      7 },
  { "--F1",         eslARG_REAL,  "0.02", NULL, NULL,    NULL,  NULL, "--max",          "Stage 1 (MSV) threshold: promote hits w/ P <= F1",             7 },
  { "--F2",         eslARG_REAL,  "1e-3", NULL, NULL,    NULL,  NULL, "--max",          "Stage 2 (Vit) threshold: promote hits w/ P <= F2",             7 },
  { "--F3",         eslARG_REAL,  "1e-5", NULL, NULL,    NULL,  NULL, "--max",          "Stage 3 (Fwd) threshold: promote hits w/ P <= F3",             7 },
  { "--nobias",     eslARG_NONE,   NULL,  NULL, NULL,    NULL,  NULL, "--max",          "turn off composition bias filter",                             7 },
  /* Control of E-value calibration */
  { "--EmL",        eslARG_INT,    "200", NULL,"n>0",      NULL,  NULL,  NULL,          "length of sequences for MSV Gumbel mu fit",                   11 },   
  { "--EmN",        eslARG_INT,    "200", NULL,"n>0",      NULL,  NULL,  NULL,          "number of sequences for MSV Gumbel mu fit",                   11 },   
  { "--EvL",        eslARG_INT,    "200", NULL,"n>0",      NULL,  NULL,  NULL,          "length of sequences for Viterbi Gumbel mu fit",               11 },   
  { "--EvN",        eslARG_INT,    "200", NULL,"n>0",      NULL,  NULL,  NULL,          "number of sequences for Viterbi Gumbel mu fit",               11 },   
  { "--EfL",        eslARG_INT,    "100", NULL,"n>0",      NULL,  NULL,  NULL,          "length of sequences for Forward exp tail tau fit",            11 },   
  { "--EfN",        eslARG_INT,    "200", NULL,"n>0",      NULL,  NULL,  NULL,          "number of sequences for Forward exp tail tau fit",            11 },   
  { "--Eft",        eslARG_REAL,  "0.04", NULL,"0<x<1",    NULL,  NULL,  NULL,          "tail mass for Forward exponential tail tau fit",              11 },   
  /* Other options */
  { "--seed",       eslARG_INT,    "42",  NULL, "n>=0",  NULL,  NULL,  NULL,            "set RNG seed to <n> (if 0: one-time arbitrary seed)",         12 },
  { "--nonull2",    eslARG_NONE,   NULL,  NULL, NULL,    NULL,  NULL,  NULL,            "turn off biased composition score corrections",               12 },
  { "-Z",           eslARG_REAL,   FALSE, NULL, "x>0",   NULL,  NULL,  NULL,            "set # of comparisons done, for E-value calculation",          12 },
  { "--domZ",       eslARG_REAL,   FALSE, NULL, "x>0",   NULL,  NULL,  NULL,            "set # of significant seqs, for domain E-value calculation",   12 },
  {  0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};

static char usage[]  = "[options] <target seqfile>";

static char banner[] = "search query against a sequence database";

static int  serial_loop  (WORKER_INFO *info, ESL_SQCACHE *cache);
#define BLOCK_SIZE 1000

static int  thread_loop(ESL_THREADS *obj);
static void pipeline_thread(void *arg);


static void
print_client_error(int fd, int status, char *format, va_list ap)
{
  int   n;
  char  ebuf[512];

  HMMER_SEARCH_STATUS s;

  s.status  = status;
  s.err_len = vsnprintf(ebuf, sizeof(ebuf), format, ap);

  /* send back an unsuccessful status message */
  n = sizeof(s);
  if (writen(fd, &s, n) != n) {
    fprintf(stderr, "hmmpgmd: write (size %d) error %d - %s\n", n, errno, strerror(errno));
    exit(1);
  }

  writen(fd, ebuf, s.err_len);
}

static void
client_error(int fd, int status, char *format, ...)
{
  va_list ap;

  va_start(ap, format);
  print_client_error(fd, status, format, ap);
  va_end(ap);
}

static void
client_error_longjmp(int fd, int status, jmp_buf *env, char *format, ...)
{
  va_list ap;

  va_start(ap, format);
  print_client_error(fd, status, format, ap);
  va_end(ap);

  longjmp(*env, 1);
}

static void
process_commandline(int argc, char **argv, ESL_GETOPTS **ret_go)
{
  ESL_GETOPTS *go = NULL;

  if ((go = esl_getopts_Create(cmdlineOpts)) == NULL)     p7_Die("Internal failure creating options object");
  if (esl_opt_ProcessEnvironment(go)         != eslOK)  { printf("Failed to process environment: %s\n", go->errbuf); goto ERROR; }
  if (esl_opt_ProcessCmdline(go, argc, argv) != eslOK)  { printf("Failed to parse command line: %s\n",  go->errbuf); goto ERROR; }
  if (esl_opt_VerifyConfig(go)               != eslOK)  { printf("Failed to parse command line: %s\n",  go->errbuf); goto ERROR; }
 
  /* help format: */
  if (esl_opt_GetBoolean(go, "-h") == TRUE) {
    p7_banner(stdout, argv[0], banner);
    esl_usage(stdout, argv[0], usage);

    puts("\nBasic options:");
    esl_opt_DisplayHelp(stdout, go, 1, 2, 80); /* 1= group; 2 = indentation; 80=textwidth*/

    puts("\nOther expert options:");
    esl_opt_DisplayHelp(stdout, go, 12, 2, 80); 
    exit(0);
  }

  if (esl_opt_ArgNumber(go) < 1)                        { puts("Incorrect number of command line arguments.");       goto ERROR; }

  *ret_go = go;
  return;
  
 ERROR:  /* all errors handled here are user errors, so be polite.  */
  esl_usage(stdout, argv[0], usage);
  puts("\nwhere most common options are:");
  esl_opt_DisplayHelp(stdout, go, 1, 2, 80); /* 1= group; 2 = indentation; 80=textwidth*/
  printf("\nTo see more help on available options, do %s -h\n\n", argv[0]);
  exit(1);  
}

/* sort routines */
static int
sort_seq_size(const void *p1, const void *p2)
{
  int cmp;

  cmp  = (((ESL_SQ *)p1)->n < ((ESL_SQ *)p2)->n);
  cmp -= (((ESL_SQ *)p1)->n > ((ESL_SQ *)p2)->n);

  return cmp;
}

static int
sort_seq_inx(const void *p1, const void *p2)
{
  int cmp;

  cmp  = (((ESL_SQ *)p1)->idx > ((ESL_SQ *)p2)->idx);
  cmp -= (((ESL_SQ *)p1)->idx < ((ESL_SQ *)p2)->idx);

  return cmp;
}

int
main(int argc, char **argv)
{
  FILE            *ofp        = stdout;            /* results output file (-o)                        */
  FILE            *afp        = NULL;              /* alignment output file (-A)                      */
  FILE            *tblfp      = NULL;              /* output stream for tabular per-seq (--tblout)    */
  FILE            *domtblfp   = NULL;              /* output stream for tabular per-seq (--domtblout) */
  ESL_ALPHABET    *abc;                            /* digital alphabet                                */
  ESL_STOPWATCH   *w;                              /* timer used for profiling statistics             */
  ESL_GETOPTS     *go         = NULL;              /* command line processing                         */
  int              textw      = 0;
  int              status     = eslOK;
  int              i, j, n;

  int              ncpus      = 0;
  int              infocnt    = 0;
  WORKER_INFO     *info       = NULL;

  ESL_THREADS     *threadObj  = NULL;
  pthread_mutex_t  mutex;
  int              current_index;

  ESL_SQCACHE    **cache      = NULL;

  SEARCH_QUEUE    *queue      = NULL;
  QUEUE_DATA      *query      = NULL;

  CLIENTSIDE_ARGS  client_comm;

  /* Set processor specific flags */
  impl_Init();

  /* Initializations */
  p7_FLogsumInit();		/* we're going to use table-driven Logsum() approximations at times */

  process_commandline(argc, argv, &go);    

  w = esl_stopwatch_Create();

  abc = esl_alphabet_Create(eslAMINO);

  /* cache all the databases into memory */
  ESL_ALLOC(cache, sizeof(ESL_SQCACHE *) * esl_opt_ArgNumber(go));
  for (i = 0; i < esl_opt_ArgNumber(go); ++i) {
    ESL_RANDOMNESS *rnd  = NULL;
    ESL_SQ         *sq   = NULL;

    char *dbfile = esl_opt_GetArg(go, i+1);

    status = esl_sqfile_Cache(abc, dbfile, eslSQFILE_FASTA, p7_SEQDBENV, &cache[i]);
    if      (status == eslENOTFOUND) p7_Fail("Failed to open sequence file %s for reading\n",          dbfile);
    else if (status == eslEFORMAT)   p7_Fail("Sequence file %s is empty or misformatted\n",            dbfile);
    else if (status == eslEINVAL)    p7_Fail("Can't autodetect format of a stdin or .gz seqfile");
    else if (status != eslOK)        p7_Fail("Unexpected error %d opening sequence file %s\n", status, dbfile);

    sq  = cache[i]->sq_list;

    /* sort the cache on sequence size */
    qsort(sq, cache[i]->seq_count, sizeof(ESL_SQ), sort_seq_size);

    /* jumble up the top 2/3 of the database.  This will leave the largest sequences at
     * the beginning of the cache.  then jumble up the bottom 1/3 of the database mixing
     * up the smaller sequences.  the reason is for load balancing the threads.  as we
     * process the database, smaller and smaller blocks of sequences will be processed
     * to try eleminate the case where one thread dominates the execution time.
     */
    rnd = esl_randomness_CreateFast(cache[i]->seq_count);
    for (j = 0 ; j < cache[i]->seq_count; ++j) {
      rnd->x = rnd->x * 69069 + 1;
      sq[j].idx = rnd->x;
    }
    esl_randomness_Destroy(rnd);

    j = cache[i]->seq_count / 3 * 2;
    qsort(sq, j, sizeof(ESL_SQ), sort_seq_inx);
    qsort(sq + j, cache[i]->seq_count - j, sizeof(ESL_SQ), sort_seq_inx);
    for (j = 0 ; j < cache[i]->seq_count; ++j) sq[j].idx = j;
  }

  /* initialize the search queue */
  ESL_ALLOC(queue, sizeof(SEARCH_QUEUE));
  if ((n = pthread_mutex_init(&queue->mutex, NULL)) != 0) {
    errno = n;
    fprintf(stderr, "%s: mutex_init error %d - %s\n", argv[0], errno, strerror(errno));
    exit(1);
  }

  if ((n = pthread_cond_init(&queue->cond, NULL)) != 0) {
    errno = n;
    fprintf(stderr, "%s: mutex_init error %d - %s\n", argv[0], errno, strerror(errno));
    exit(1);
  }

  queue->head = NULL;
  queue->tail = NULL;

  /* start the communications with the web clients */
  setup_clientside_comm(go, queue, &client_comm);

  /* initialize thread data */
  if (esl_opt_IsOn(go, "--cpu")) ncpus = esl_opt_GetInteger(go, "--cpu");
  else                                   esl_threads_CPUCount(&ncpus);

  if (ncpus > 0) {
    threadObj = esl_threads_Create(&pipeline_thread);
    if (pthread_mutex_init(&mutex, NULL) != 0) p7_Fail("mutex init failed");
  }

  infocnt = (ncpus == 0) ? 1 : ncpus;
  ESL_ALLOC(info, sizeof(*info) * infocnt);

  /* read query hmm/sequence */
  while ((query = read_Queue(queue)) != NULL) {
    int dbx;

    /* figure out which cached database to use */
    dbx = 0;
    if (esl_opt_ArgNumber(query->opts) == 1) {
      char *db  = esl_opt_GetArg(query->opts, 1);
      int   len = strlen(db);
      for (i = 0; i < esl_opt_ArgNumber(go); ++i) {
        int n = strlen(cache[i]->filename);
        if (n >= len) {
          n = n - len;
          if (strcmp(cache[i]->filename + n, db) == 0) {
            dbx = i;
            break;
          }
        }
      }
      if (i >= esl_opt_ArgNumber(go)) {
        /* TODO report back an error that the db cannot be found */
      }
    }

    textw = (esl_opt_GetBoolean(query->opts, "--notextw")) ? 0 : esl_opt_GetInteger(query->opts, "--textw");

    esl_stopwatch_Start(w);

    if (query->hmm == NULL) {
      fprintf(ofp, "Query (%d):       %s  [L=%ld]\n", query->sock, query->seq->name, (long) query->seq->n);
    } else {
      fprintf(ofp, "Query (%d):       %s  [M=%d]\n", query->sock, query->hmm->name, query->hmm->M);
    }

    /* Create processing pipeline and hit list */
    for (i = 0; i < infocnt; ++i) {
      info[i].abc   = query->abc;
      info[i].hmm   = query->hmm;
      info[i].seq   = query->seq;
      info[i].opts  = query->opts;
      info[i].bld   = query->bld;

      info[i].th    = NULL;
      info[i].pli   = NULL;

      if (ncpus > 0) {
        info[i].sq_list = cache[dbx]->sq_list;
        info[i].sq_cnt  = cache[dbx]->seq_count;
        info[i].mutex   = mutex;
        info[i].sq_inx  = &current_index;

        esl_threads_AddThread(threadObj, &info[i]);
      }
    }

    if (ncpus > 0) {
      current_index = 0;
      thread_loop(threadObj);
    } else {
      serial_loop(info, cache[dbx]);
    }

    esl_stopwatch_Stop(w);
#if 0
    fprintf (ofp, "   Sequences  Residues                              Elapsed\n");
    for (i = 0; i < infocnt; ++i) {
      char buf1[16];
      int h, m, s, hs;
      P7_PIPELINE *pli = info[i].pli;
      double elapsed;

      elapsed = info[i].elapsed;
      h  = (int) (elapsed / 3600.);
      m  = (int) (elapsed / 60.) - h * 60;
      s  = (int) (elapsed) - h * 3600 - m * 60;
      hs = (int) (elapsed * 100.) - h * 360000 - m * 6000 - s * 100;
      sprintf(buf1, "%02d:%02d.%02d", m,s,hs);

      fprintf (ofp, "%2d %9" PRId64 " %9" PRId64 " %7" PRId64 " %7" PRId64 " %6" PRId64 " %5" PRId64 " %s\n",
               i, pli->nseqs, pli->nres, pli->n_past_msv, pli->n_past_bias, pli->n_past_vit, pli->n_past_fwd, buf1);
    }
#endif
    /* merge the results of the search results */
    for (i = 1; i < infocnt; ++i) {
      p7_tophits_Merge(info[0].th, info[i].th);
      p7_pipeline_Merge(info[0].pli, info[i].pli);
      p7_pipeline_Destroy(info[i].pli);
      p7_tophits_Destroy(info[i].th);
    }

    send_results(w, info, queue, &client_comm);

    /* Output the results in an MSA (-A option) */
    if (afp) {
      ESL_MSA *msa = NULL;

      if (p7_tophits_Alignment(info->th, query->abc, NULL, NULL, 0, p7_ALL_CONSENSUS_COLS, &msa) == eslOK) {
        if (textw > 0) esl_msa_Write(afp, msa, eslMSAFILE_STOCKHOLM);
        else           esl_msa_Write(afp, msa, eslMSAFILE_PFAM);
	  
        fprintf(ofp, "# Alignment of %d hits satisfying inclusion thresholds saved to: %s\n", msa->nseq, esl_opt_GetString(go, "-A"));
      } 
      else fprintf(ofp, "# No hits satisfy inclusion thresholds; no alignment saved\n");
	  
      esl_msa_Destroy(msa);
    }

    p7_pipeline_Destroy(info->pli);
    p7_tophits_Destroy(info->th);
  } /* end outer loop over query HMMs */

  if (ncpus > 0) {
    pthread_mutex_destroy(&mutex);
    esl_threads_Destroy(threadObj);
  }

  for (i = 0; i < esl_opt_ArgNumber(go); ++i) {
    esl_sqfile_Free(cache[i]);
    cache[i] = NULL;
  }
  free(cache);

  free(info);
  free(queue);

  esl_stopwatch_Destroy(w);

  if (ofp != stdout) fclose(ofp);
  if (afp)           fclose(afp);
  if (tblfp)         fclose(tblfp);
  if (domtblfp)      fclose(domtblfp);

  esl_getopts_Destroy(go);

  return eslOK;

 ERROR:
  return eslFAIL;
}

static QUEUE_DATA *
read_Queue(SEARCH_QUEUE *queue)
{
  int         n;
  QUEUE_DATA *data;

  if ((n = pthread_mutex_lock (&queue->mutex)) != 0) {
    errno = n;
    fprintf(stderr, "%08X: mutex lock error %d - %s\n", (unsigned int)pthread_self(), errno, strerror(errno));
    exit(1);
  }

  while (queue->head == NULL) {
    if ((n = pthread_cond_wait (&queue->cond, &queue->mutex)) != 0) {
      errno = n;
      fprintf(stderr, "%08X: cond wait error %d - %s\n", (unsigned int)pthread_self(), errno, strerror(errno));
      exit(1);
    }
  }

  data = queue->head;

  if ((n = pthread_mutex_unlock (&queue->mutex)) != 0) {
    errno = n;
    fprintf(stderr, "%08X: mutex unlock error %d - %s\n", (unsigned int)pthread_self(), errno, strerror(errno));
    exit(1);
  }

  return data;
}

static int
serial_loop(WORKER_INFO *info, ESL_SQCACHE *cache)
{
  int               i;

  ESL_SQ           *dbsq     = NULL;         /* one target sequence (digital)  */
  P7_BG            *bg       = NULL;	     /* null model                     */
  P7_PIPELINE      *pli      = NULL;         /* work pipeline                  */
  P7_TOPHITS       *th       = NULL;         /* top hit results                */
  P7_PROFILE       *gm       = NULL;         /* generic model                  */
  P7_OPROFILE      *om       = NULL;         /* optimized query profile        */

  /* Convert to an optimized model */
  bg = p7_bg_Create(info->abc);

  /* process a query sequence or hmm */
  if (info->seq != NULL) {
    p7_SingleBuilder(info->bld, info->seq, bg, NULL, NULL, NULL, &om); /* bypass HMM - only need model */
  } else {
    gm = p7_profile_Create (info->hmm->M, info->abc);
    om = p7_oprofile_Create(info->hmm->M, info->abc);
    p7_ProfileConfig(info->hmm, bg, gm, 100, p7_LOCAL); /* 100 is a dummy length for now; and MSVFilter requires local mode */
    p7_oprofile_Convert(gm, om);                  /* <om> is now p7_LOCAL, multihit */
  }

  /* Create processing pipeline and hit list */
  th  = p7_tophits_Create(); 
  pli = p7_pipeline_Create(info->opts, om->M, 100, FALSE, p7_SEARCH_SEQS); /* L_hint = 100 is just a dummy for now */
  p7_pli_NewModel(pli, om, bg);

  /* Main loop: */
  for (i = 0; i < cache->seq_count; ++i) {
    dbsq = cache->sq_list + i;

    p7_pli_NewSeq(pli, dbsq);
    p7_bg_SetLength(bg, dbsq->n);
    p7_oprofile_ReconfigLength(om, dbsq->n);
      
    p7_Pipeline(pli, om, bg, dbsq, th);
	  
    p7_pipeline_Reuse(pli);
  }

  /* make available the pipeline objects to the main thread */
  info->th = th;
  info->pli = pli;

  /* clean up */
  p7_bg_Destroy(bg);
  p7_oprofile_Destroy(om);

  if (gm  != NULL) p7_profile_Destroy(gm);

  return eslOK;
}

static int
thread_loop(ESL_THREADS *obj)
{
  impl_Init();

  esl_threads_WaitForStart(obj);
  esl_threads_WaitForFinish(obj);

  return eslOK;
}

static void 
pipeline_thread(void *arg)
{
  int               i;
  int               count;
  int               workeridx;
  WORKER_INFO      *info;
  ESL_THREADS      *obj;

  ESL_STOPWATCH    *w;

  P7_BG            *bg       = NULL;	     /* null model                     */
  P7_PIPELINE      *pli      = NULL;         /* work pipeline                  */
  P7_TOPHITS       *th       = NULL;         /* top hit results                */
  P7_PROFILE       *gm       = NULL;         /* generic model                  */
  P7_OPROFILE      *om       = NULL;         /* optimized query profile        */

  obj = (ESL_THREADS *) arg;
  esl_threads_Started(obj, &workeridx);

  info = (WORKER_INFO *) esl_threads_GetData(obj, workeridx);

  w = esl_stopwatch_Create();
  esl_stopwatch_Start(w);

  /* Convert to an optimized model */
  bg = p7_bg_Create(info->abc);

  /* process a query sequence or hmm */
  if (info->seq != NULL) {
    p7_SingleBuilder(info->bld, info->seq, bg, NULL, NULL, NULL, &om); /* bypass HMM - only need model */
  } else {
    gm = p7_profile_Create (info->hmm->M, info->abc);
    om = p7_oprofile_Create(info->hmm->M, info->abc);
    p7_ProfileConfig(info->hmm, bg, gm, 100, p7_LOCAL); /* 100 is a dummy length for now; and MSVFilter requires local mode */
    p7_oprofile_Convert(gm, om);                  /* <om> is now p7_LOCAL, multihit */
  }

  /* Create processing pipeline and hit list */
  th  = p7_tophits_Create(); 
  pli = p7_pipeline_Create(info->opts, om->M, 100, FALSE, p7_SEARCH_SEQS); /* L_hint = 100 is just a dummy for now */
  p7_pli_NewModel(pli, om, bg);

  /* loop until all sequences have been processed */
  count = 1;
  while (count > 0) {
    int     inx;
    ESL_SQ *dbsq;

#if 1
    /* grab the next block of sequences */
    if (pthread_mutex_lock(&info->mutex) != 0) p7_Fail("mutex lock failed");
    inx = *info->sq_inx;
    *info->sq_inx += BLOCK_SIZE;
    if (pthread_mutex_unlock(&info->mutex) != 0) p7_Fail("mutex unlock failed");

    dbsq = info->sq_list + inx;

    count = info->sq_cnt - inx;
    if (count > BLOCK_SIZE) count = BLOCK_SIZE;
    //printf("THREAD %08x: %d %d\n", workeridx, inx, count);
#endif

#if 0
    /* grab the next block of sequences */
    if (pthread_mutex_lock(&info->mutex) != 0) p7_Fail("mutex lock failed");
    inx = *info->sq_inx;
    count = info->sq_cnt - inx;
    count = count >> 7;
    if (count > 2500) count = 2500;
    if (count < 1000) count = 1000;
    *info->sq_inx += count;
    if (pthread_mutex_unlock(&info->mutex) != 0) p7_Fail("mutex unlock failed");

    dbsq = info->sq_list + inx;

    if (info->sq_cnt - inx < count) count = info->sq_cnt - inx;
    //printf("THREAD %08x: %d %d\n", workeridx, inx, count);
#endif

    /* Main loop: */
    for (i = 0; i < count; ++i, ++dbsq) {
      p7_pli_NewSeq(pli, dbsq);
      p7_bg_SetLength(bg, dbsq->n);
      p7_oprofile_ReconfigLength(om, dbsq->n);
	  
      p7_Pipeline(pli, om, bg, dbsq, th);
	  
      p7_pipeline_Reuse(pli);
    }
  }

  /* make available the pipeline objects to the main thread */
  info->th = th;
  info->pli = pli;

  /* clean up */
  p7_bg_Destroy(bg);
  p7_oprofile_Destroy(om);

  if (gm != NULL)  p7_profile_Destroy(gm);

  esl_stopwatch_Stop(w);
  info->elapsed = w->elapsed;

  esl_stopwatch_Destroy(w);

  esl_threads_Finished(obj, workeridx);

  pthread_exit(NULL);
  return;
}

static void
send_results(ESL_STOPWATCH *w, WORKER_INFO *info, SEARCH_QUEUE *queue, CLIENTSIDE_ARGS *comm)
{
  int i, j;
  int n;
  int fd;

  int total = 0;

  P7_HIT    *hit;
  P7_DOMAIN *dcl;

  HMMER_SEARCH_STATS   stats;
  HMMER_SEARCH_STATUS  status;

  QUEUE_DATA *data;

  /* add the search request to the queue */
  if ((n = pthread_mutex_lock (&queue->mutex)) != 0) {
    errno = n;
    fprintf(stderr, "%08X: mutex lock error %d - %s\n", (unsigned int)pthread_self(), errno, strerror(errno));
    exit(1);
  }

  data = queue->head;
  queue->head = data->next;
  if (data->next == NULL) queue->tail = NULL;

  if ((n = pthread_mutex_unlock (&queue->mutex)) != 0) {
    errno = n;
    fprintf(stderr, "%08X: mutex unlock error %d - %s\n", (unsigned int)pthread_self(), errno, strerror(errno));
    exit(1);
  }

  fd = data->sock;

  /* send back a successful status message */
  status.status  = eslOK;
  status.err_len = 0;
  n = sizeof(status);
  total += n;
  if (writen(fd, &status, n) != n) {
    fprintf(stderr, "hmmpgmd: write (size %d) error %d - %s\n", n, errno, strerror(errno));
    exit(1);
  }

  /* copy the search stats */
  stats.elapsed     = w->elapsed;
  stats.user        = w->user;
  stats.sys         = w->sys;

  stats.nmodels     = info->pli->nmodels;
  stats.nseqs       = info->pli->nseqs;
  stats.nres        = info->pli->nres;
  stats.nnodes      = info->pli->nnodes;
  stats.n_past_msv  = info->pli->n_past_msv;
  stats.n_past_bias = info->pli->n_past_bias;
  stats.n_past_vit  = info->pli->n_past_vit;
  stats.n_past_fwd  = info->pli->n_past_fwd;

  stats.Z           = info->pli->Z;
  stats.domZ        = info->pli->domZ;
  stats.Z_setby     = info->pli->Z_setby;
  stats.domZ_setby  = info->pli->domZ_setby;

  stats.nhits       = info->th->N;
  stats.nreported   = info->th->nreported;
  stats.nincluded   = info->th->nincluded;

  n = sizeof(stats);
  total += n;
  if (writen(fd, &stats, n) != n) {
    fprintf(stderr, "hmmpgmd: write (size %d) error %d - %s\n", n, errno, strerror(errno));
    exit(1);
  }

  /* loop through the hit list sending to dest */
  hit = info->th->unsrt;
  for (i = 0; i < info->th->N; ++i) {
    int     l;
    char   *h;
    char   *p;

    n = sizeof(P7_HIT);
    if (hit->name != NULL) n += strlen(hit->name) + 1;
    if (hit->acc  != NULL) n += strlen(hit->acc)  + 1;
    if (hit->desc != NULL) n += strlen(hit->desc) + 1;

    if ((h = malloc(n)) == NULL) {
      fprintf(stderr, "hmmpgmd: malloc error (size %d)\n", n);
      exit(1);
    }

    memcpy(h, hit, sizeof(P7_HIT));

    /* copy the strings to the memory space after the P7_HIT structure.
     * then replace the pointers with the lenght of the strings.  this
     * will allow the client to get the hit data with two receives.  the
     * first receive will be the hit structure.  then the space for the
     * strings can be calculated and one more receive for the string data.
     */
    p = h + sizeof(P7_HIT);
    if (hit->name != NULL) {
      strcpy(p, hit->name);
      l = strlen(hit->name) + 1;
      ((P7_HIT *)h)->name = ((char *) NULL) + l;
      p += l;
    }
    if (hit->acc  != NULL) {
      strcpy(p, hit->acc);
      l = strlen(hit->acc) + 1;
      ((P7_HIT *)h)->acc = ((char *) NULL) + l;
      p += l;
    }
    if (hit->desc != NULL) {
      strcpy(p, hit->desc);
      l = strlen(hit->desc) + 1;
      ((P7_HIT *)h)->desc = ((char *) NULL) + l;
      p += l;
    }

    total += n;
    if (writen(fd, h, n) != n) {
      fprintf(stderr, "hmmpgmd: write (size %d) error %d - %s\n", n, errno, strerror(errno));
      exit(1);
    }

    /* send the domains for this hit */
    dcl = hit->dcl;
    for (j = 0; j < hit->ndom; ++j) {
      n = sizeof(P7_DOMAIN) + sizeof(P7_ALIDISPLAY) + dcl->ad->memsize;

      if ((h = malloc(n)) == NULL) {
        fprintf(stderr, "hmmpgmd: malloc error (size %d)\n", n);
        exit(1);
      }

      memcpy(h, dcl, sizeof(P7_DOMAIN));
      p = h + sizeof(P7_DOMAIN);
      memcpy(p, dcl->ad, sizeof(P7_ALIDISPLAY));
      p += sizeof(P7_ALIDISPLAY);
      memcpy(p, dcl->ad->mem, dcl->ad->memsize);

      total += n;
      if (writen(fd, h, n) != n) {
        fprintf(stderr, "hmmpgmd: write (size %d) error %d - %s\n", n, errno, strerror(errno));
        exit(1);
      }

      ++dcl;
    }

    ++hit;
  }

  esl_getopts_Destroy(data->opts);

  if (data->abc != NULL) esl_alphabet_Destroy(data->abc);
  if (data->hmm != NULL) p7_hmm_Destroy(data->hmm);
  if (data->seq != NULL) esl_sq_Destroy(data->seq);
  if (data->bld != NULL) p7_builder_Destroy(data->bld);

  data->opts      = NULL;
  data->abc       = NULL;
  data->seq       = NULL;
  data->hmm       = NULL;
  data->bld       = NULL;

  printf("Bytes %d sent on socket %d\n", total, data->sock);

  free(data);
}

static void
process_searchline(int fd, char *cmdstr, ESL_GETOPTS *go)
{
  int status;

  if ((status = esl_opt_ProcessSpoof(go, cmdstr)) != eslOK) client_error(fd, status, "Failed to parse options string: %s", go->errbuf);
  if ((status = esl_opt_VerifyConfig(go))         != eslOK) client_error(fd, status, "Failed to parse options string: %s", go->errbuf);

  /* the options string can handle an optional database */
  if (esl_opt_ArgNumber(go) > 1)                 client_error(fd, eslFAIL, "Incorrect number of command line arguments.");
}

static int
clientside_loop(CLIENTSIDE_ARGS *data)
{
  int                status;

  char              *ptr;
  char              *buffer;
  char              *opt_ptr;

  int                seed;
  int                buf_size;
  int                remaining;
  int                amount;
  int                eod;
  int                n;

  P7_HMM            *hmm     = NULL;     /* query HMM                      */
  ESL_SQ            *seq     = NULL;     /* query sequence                 */
  ESL_ALPHABET      *abc     = NULL;     /* digital alphabet               */
  ESL_GETOPTS       *opts    = NULL;     /* search specific options        */
  P7_BUILDER        *bld     = NULL;     /* HMM construction configuration */

  SEARCH_QUEUE      *queue    = data->queue;

  QUEUE_DATA        *parms;

  jmp_buf            jmp_env;

  buf_size = 4096;
  buffer = malloc(buf_size);
  if (buffer == NULL) {
    fprintf(stderr, "%s: malloc error\n", data->pgm);
    exit(1);
  }

  ptr = buffer;
  remaining = buf_size;
  amount = 0;

  eod = 0;
  while (!eod) {

    /* Receive message from client */
    if ((n = read(data->sock_fd, ptr, remaining)) < 0) {
      fprintf(stderr, "%08X: recv error %d - %s\n", (unsigned int)pthread_self(), errno, strerror(errno));
      exit(1);
    }

    if (n == 0) return 1;

    ptr += n;
    amount += n;
    remaining -= n;
    eod = (amount > 1 && *(ptr - 2) == '/' && *(ptr - 2) == '/' );

    /* if the buffer is full, make it larger */
    if (!eod && remaining == 0) {
      buffer = realloc(buffer, buf_size * 2);
      if (buffer == NULL) {
        fprintf(stderr, "%s: realloc error\n", data->pgm);
        exit(1);
      }

      ptr = buffer + buf_size;
      remaining = buf_size;
      buf_size *= 2;
    }
  }

  /* zero terminate the buffer */
  if (remaining == 0) {
    buffer = realloc(buffer, buf_size + 1);
    if (buffer == NULL) {
      fprintf(stderr, "%s: realloc error\n", data->pgm);
      exit(1);
    }

    ptr = buffer + buf_size;
  }
  *ptr = 0;

  /* skip all leading white spaces */
  ptr = buffer;
  while (*ptr && isspace(*ptr)) ++ptr;

  /* process search specific options */
  opt_ptr = NULL;
  opts = esl_getopts_Create(searchOpts);
  if (opts == NULL)  {
    client_error(data->sock_fd, eslFAIL, "Internal failure creating options object");
    free(buffer);
    return 0;
  }

  if (*ptr == '@') {
    opt_ptr = ptr;

    /* skip to the end of the line */
    while (*ptr && (*ptr != '\n' && *ptr != '\r')) ++ptr;

    /* skip remaining white spaces */
    while (*ptr && isspace(*ptr)) ++ptr;
  }

  if (strncmp(ptr, "//", 2) == 0) {
    client_error(data->sock_fd, eslEFORMAT, "Missing search sequence/hmm");
    free(buffer);
    return 0;
  }

  if (!setjmp(jmp_env)) {
    
    if (opt_ptr != NULL) process_searchline(data->sock_fd, ++ptr, opts);

    abc = esl_alphabet_Create(eslAMINO);

    if (*ptr == '>') {
      /* try to parse the input buffer as a FASTA sequence */
      seq = esl_sq_CreateDigital(abc);
      status = esl_sqio_Parse(ptr, strlen(ptr), seq, eslSQFILE_DAEMON);
      if (status != eslOK) client_error_longjmp(data->sock_fd, status, &jmp_env, "Error %d parsing sequence", status);

      bld = p7_builder_Create(NULL, abc);
      if ((seed = esl_opt_GetInteger(opts, "--seed")) > 0) {
        esl_randomness_Init(bld->r, seed);
        bld->do_reseeding = TRUE;
      }
      bld->EmL = esl_opt_GetInteger(opts, "--EmL");
      bld->EmN = esl_opt_GetInteger(opts, "--EmN");
      bld->EvL = esl_opt_GetInteger(opts, "--EvL");
      bld->EvN = esl_opt_GetInteger(opts, "--EvN");
      bld->EfL = esl_opt_GetInteger(opts, "--EfL");
      bld->EfN = esl_opt_GetInteger(opts, "--EfN");
      bld->Eft = esl_opt_GetReal   (opts, "--Eft");
      status = p7_builder_SetScoreSystem(bld, esl_opt_GetString(opts, "--mxfile"), NULL, esl_opt_GetReal(opts, "--popen"), esl_opt_GetReal(opts, "--pextend"));
      if (status != eslOK) client_error_longjmp(data->sock_fd, status, &jmp_env, "Failed to set single query seq score system: %s", bld->errbuf);

    } else if (strncmp(ptr, "HMM", 3) == 0) {
      P7_HMMFILE   *hfp     = NULL;

      /* try to parse the buffer as an hmm */
      status = p7_hmmfile_OpenBuffer(ptr, strlen(ptr), &hfp);
      if (status == eslOK) client_error_longjmp(data->sock_fd, status, &jmp_env, "Error %d opening hmm", status);

      status = p7_hmmfile_Read(hfp, &abc,  &hmm);
      if (status == eslOK) client_error_longjmp(data->sock_fd, status, &jmp_env, "Error %d reading hmm", status);

      p7_hmmfile_Close(hfp);

    } else {
      /* no idea what we are trying to parse */
      client_error_longjmp(data->sock_fd, eslEFORMAT, &jmp_env, "Error unknown sequence/hmm format");
    }
  } else {
    /* an error occured some where, so try to clean up */
    if (abc != NULL) esl_getopts_Destroy(opts);
    if (abc != NULL) esl_alphabet_Destroy(abc);
    if (hmm != NULL) p7_hmm_Destroy(hmm);
    if (seq != NULL) esl_sq_Destroy(seq);
    if (bld != NULL) p7_builder_Destroy(bld);

    free(buffer);

    return 0;
  }

  parms = malloc(sizeof(QUEUE_DATA));
  if (parms == NULL) {
    fprintf(stderr, "%s: malloc error\n", data->pgm);
    exit(1);
  }

  parms->hmm  = hmm;
  parms->seq  = seq;
  parms->abc  = abc;
  parms->opts = opts;
  parms->bld  = bld;

  parms->sock = data->sock_fd;
  parms->next = NULL;
  parms->prev = NULL;

  printf("Waiting to queued %s on %d\n", parms->seq->name, parms->sock);

  /* add the search request to the queue */
  if ((n = pthread_mutex_lock (&queue->mutex)) != 0) {
    errno = n;
    fprintf(stderr, "%08X: mutex lock error %d - %s\n", (unsigned int)pthread_self(), errno, strerror(errno));
    exit(1);
  }

  if (queue->head == NULL) {
    queue->head = parms;
    queue->tail = parms;
  } else {
    queue->tail->next = parms;
    parms->prev = queue->tail;
    queue->tail = parms;
  }

  if ((n = pthread_mutex_unlock (&queue->mutex)) != 0) {
    errno = n;
    fprintf(stderr, "%08X: mutex unlock error %d - %s\n", (unsigned int)pthread_self(), errno, strerror(errno));
    exit(1);
  }

  /* if anyone is waiting, wake them up */
  if ((n = pthread_cond_broadcast (&queue->cond)) != 0) {
    errno = n;
    fprintf(stderr, "%08X: mutex unlock error %d - %s\n", (unsigned int)pthread_self(), errno, strerror(errno));
    exit(1);
  }

  printf("Queued %s on %d\n", parms->seq->name, parms->sock);

  free(buffer);
  return 0;
}

static void *
clientside_thread(void *arg)
{
  int eof;
  CLIENTSIDE_ARGS   *data     = (CLIENTSIDE_ARGS *)arg;

  /* Guarantees that thread resources are deallocated upon return */
  pthread_detach(pthread_self()); 

  eof = 0;
  while (!eof) {
    eof = clientside_loop(data);
  }

  printf("Closing socket %d\n", data->sock_fd);

  close(data->sock_fd);
  free(data);

  pthread_exit(NULL);
}

static void *
comm_thread(void *arg)
{
  int                  n;
  int                  fd;
  pthread_t            thread_id;

  struct sockaddr_in   addr;

  CLIENTSIDE_ARGS     *targs = NULL;
  CLIENTSIDE_ARGS     *data  = (CLIENTSIDE_ARGS *)arg;
  SEARCH_QUEUE        *queue = data->queue;

  for ( ;; ) {

    /* Wait for a client to connect */
    n = sizeof(addr);
    if ((fd = accept(data->sock_fd, (struct sockaddr *)&addr, (unsigned int *)&n)) < 0) {
      fprintf(stderr, "%s: accept error %d - %s\n", data->pgm, errno, strerror(errno));
      exit(1);
    }

    printf("Handling client %s (%d)\n", inet_ntoa(addr.sin_addr), fd);

    targs = malloc(sizeof(CLIENTSIDE_ARGS));
    if (targs == NULL) {
      fprintf(stderr, "%s: malloc error\n", data->pgm);
      exit(1);
    }
    targs->pgm     = data->pgm;
    targs->queue   = queue;
    targs->sock_fd = fd;

    if (pthread_create(&thread_id, NULL, clientside_thread, (void *)targs) != 0) {
      fprintf(stderr, "%s: pthread_create error %d - %s\n", data->pgm, errno, strerror(errno));
      exit(1);
    }
  }
  
  pthread_exit(NULL);
}

static void 
setup_clientside_comm(ESL_GETOPTS *opts, SEARCH_QUEUE *queue, CLIENTSIDE_ARGS *args)
{
  int                  n;
  int                  reuse;
  int                  sock_fd;
  pthread_t            thread_id;

  struct linger        linger;
  struct sockaddr_in   addr;

  /* Create socket for incoming connections */
  if ((sock_fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
    fprintf(stderr, "%s: socket error %d - %s\n", opts->argv[0], errno, strerror(errno));
    exit(1);
  }
      
  /* incase the server went down in an ungraceful way, allow the port to be
   * reused avoiding the timeout.
   */
  reuse = 1;
  if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse)) < 0) {
    fprintf(stderr, "%s: setsockopt error %d - %s\n", opts->argv[0], errno, strerror(errno));
    exit(1);
  }

  /* the sockets are never closed, so if the server exits, force the kernel to
   * close the socket and clear it so the server can be restarted immediately.
   */
  linger.l_onoff = 1;
  linger.l_linger = 0;
  if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, (char *)&linger, sizeof(linger)) < 0) {
    fprintf(stderr, "%s: setsockopt error %d - %s\n", opts->argv[0], errno, strerror(errno));
    exit(1);
  }

  /* Construct local address structure */
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(esl_opt_GetInteger(opts, "--cport"));

  /* Bind to the local address */
  if (bind(sock_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
    fprintf(stderr, "%s: bind error %d - %s\n", opts->argv[0], errno, strerror(errno));
    exit(1);
  }

  /* Mark the socket so it will listen for incoming connections */
  if (listen(sock_fd, esl_opt_GetInteger(opts, "--ccncts")) < 0) {
    fprintf(stderr, "%s: listen error %d - %s\n", opts->argv[0], errno, strerror(errno));
    exit(1);
  }

  args->pgm     = opts->argv[0];
  args->sock_fd = sock_fd;
  args->queue   = queue;

  if ((n = pthread_create(&thread_id, NULL, comm_thread, (void *)args)) != 0) {
    errno = n;
    fprintf(stderr, "%s: pthread_create error %d - %s\n", opts->argv[0], errno, strerror(errno));
    exit(1);
  }
}

/*****************************************************************
 * @LICENSE@
 *****************************************************************/

