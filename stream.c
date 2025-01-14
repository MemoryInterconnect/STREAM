/*-----------------------------------------------------------------------*/
/* Program: STREAM                                                       */
/* Revision: $Id: stream.c,v 5.10 2013/01/17 16:01:06 mccalpin Exp mccalpin $ */
/* Original code developed by John D. McCalpin                           */
/* Programmers: John D. McCalpin                                         */
/*              Joe R. Zagar                                             */
/*                                                                       */
/* This program measures memory transfer rates in MB/s for simple        */
/* computational kernels coded in C.                                     */
/*-----------------------------------------------------------------------*/
/* Copyright 1991-2013: John D. McCalpin                                 */
/*-----------------------------------------------------------------------*/
/* License:                                                              */
/*  1. You are free to use this program and/or to redistribute           */
/*     this program.                                                     */
/*  2. You are free to modify this program for your own use,             */
/*     including commercial use, subject to the publication              */
/*     restrictions in item 3.                                           */
/*  3. You are free to publish results obtained from running this        */
/*     program, or from works that you derive from this program,         */
/*     with the following limitations:                                   */
/*     3a. In order to be referred to as "STREAM benchmark results",     */
/*         published results must be in conformance to the STREAM        */
/*         Run Rules, (briefly reviewed below) published at              */
/*         http://www.cs.virginia.edu/stream/ref.html                    */
/*         and incorporated herein by reference.                         */
/*         As the copyright holder, John McCalpin retains the            */
/*         right to determine conformity with the Run Rules.             */
/*     3b. Results based on modified source code or on runs not in       */
/*         accordance with the STREAM Run Rules must be clearly          */
/*         labelled whenever they are published.  Examples of            */
/*         proper labelling include:                                     */
/*           "tuned STREAM benchmark results"                            */
/*           "based on a variant of the STREAM benchmark code"           */
/*         Other comparable, clear, and reasonable labelling is          */
/*         acceptable.                                                   */
/*     3c. Submission of results to the STREAM benchmark web site        */
/*         is encouraged, but not required.                              */
/*  4. Use of this program or creation of derived works based on this    */
/*     program constitutes acceptance of these licensing restrictions.   */
/*  5. Absolutely no warranty is expressed or implied.                   */
/*-----------------------------------------------------------------------*/
# include <stdio.h>
# include <unistd.h>
# include <math.h>
# include <float.h>
# include <limits.h>
# include <sys/time.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdlib.h>

//swsok, definition of uintptr_t
#include <stdint.h>

/*-----------------------------------------------------------------------
 * INSTRUCTIONS:
 *
 *	1) STREAM requires different amounts of memory to run on different
 *           systems, depending on both the system cache size(s) and the
 *           granularity of the system timer.
 *     You should adjust the value of 'STREAM_ARRAY_SIZE' (below)
 *           to meet *both* of the following criteria:
 *       (a) Each array must be at least 4 times the size of the
 *           available cache memory. I don't worry about the difference
 *           between 10^6 and 2^20, so in practice the minimum array size
 *           is about 3.8 times the cache size.
 *           Example 1: One Xeon E3 with 8 MB L3 cache
 *               STREAM_ARRAY_SIZE should be >= 4 million, giving
 *               an array size of 30.5 MB and a total memory requirement
 *               of 91.5 MB.  
 *           Example 2: Two Xeon E5's with 20 MB L3 cache each (using OpenMP)
 *               STREAM_ARRAY_SIZE should be >= 20 million, giving
 *               an array size of 153 MB and a total memory requirement
 *               of 458 MB.  
 *       (b) The size should be large enough so that the 'timing calibration'
 *           output by the program is at least 20 clock-ticks.  
 *           Example: most versions of Windows have a 10 millisecond timer
 *               granularity.  20 "ticks" at 10 ms/tic is 200 milliseconds.
 *               If the chip is capable of 10 GB/s, it moves 2 GB in 200 msec.
 *               This means the each array must be at least 1 GB, or 128M elements.
 *
 *      Version 5.10 increases the default array size from 2 million
 *          elements to 10 million elements in response to the increasing
 *          size of L3 caches.  The new default size is large enough for caches
 *          up to 20 MB. 
 *      Version 5.10 changes the loop index variables from "register int"
 *          to "ssize_t", which allows array indices >2^32 (4 billion)
 *          on properly configured 64-bit systems.  Additional compiler options
 *          (such as "-mcmodel=medium") may be required for large memory runs.
 *
 *      Array size can be set at compile time without modifying the source
 *          code for the (many) compilers that support preprocessor definitions
 *          on the compile line.  E.g.,
 *                gcc -O -DSTREAM_ARRAY_SIZE=100000000 stream.c -o stream.100M
 *          will override the default size of 10M with a new size of 100M elements
 *          per array.
 */
#ifndef STREAM_ARRAY_SIZE
#   define STREAM_ARRAY_SIZE	10
#endif

/*  2) STREAM runs each kernel "NTIMES" times and reports the *best* result
 *         for any iteration after the first, therefore the minimum value
 *         for NTIMES is 2.
 *      There are no rules on maximum allowable values for NTIMES, but
 *         values larger than the default are unlikely to noticeably
 *         increase the reported performance.
 *      NTIMES can also be set on the compile line without changing the source
 *         code using, for example, "-DNTIMES=7".
 */
#ifdef NTIMES
#if NTIMES<=1
#   define NTIMES	10
#endif
#endif
#ifndef NTIMES
#   define NTIMES	10
#endif

/*  Users are allowed to modify the "OFFSET" variable, which *may* change the
 *         relative alignment of the arrays (though compilers may change the 
 *         effective offset by making the arrays non-contiguous on some systems). 
 *      Use of non-zero values for OFFSET can be especially helpful if the
 *         STREAM_ARRAY_SIZE is set to a value close to a large power of 2.
 *      OFFSET can also be set on the compile line without changing the source
 *         code using, for example, "-DOFFSET=56".
 */
#ifndef OFFSET
#   define OFFSET	0
#endif

/*
 *	3) Compile the code with optimization.  Many compilers generate
 *       unreasonably bad code before the optimizer tightens things up.  
 *     If the results are unreasonably good, on the other hand, the
 *       optimizer might be too smart for me!
 *
 *     For a simple single-core version, try compiling with:
 *            cc -O stream.c -o stream
 *     This is known to work on many, many systems....
 *
 *     To use multiple cores, you need to tell the compiler to obey the OpenMP
 *       directives in the code.  This varies by compiler, but a common example is
 *            gcc -O -fopenmp stream.c -o stream_omp
 *       The environment variable OMP_NUM_THREADS allows runtime control of the 
 *         number of threads/cores used when the resulting "stream_omp" program
 *         is executed.
 *
 *     To run with single-precision variables and arithmetic, simply add
 *         -DSTREAM_TYPE=float
 *     to the compile line.
 *     Note that this changes the minimum array sizes required --- see (1) above.
 *
 *     The preprocessor directive "TUNED" does not do much -- it simply causes the 
 *       code to call separate functions to execute each kernel.  Trivial versions
 *       of these functions are provided, but they are *not* tuned -- they just 
 *       provide predefined interfaces to be replaced with tuned code.
 *
 *
 *	4) Optional: Mail the results to mccalpin@cs.virginia.edu
 *	   Be sure to include info that will help me understand:
 *		a) the computer hardware configuration (e.g., processor model, memory type)
 *		b) the compiler name/version and compilation flags
 *      c) any run-time information (such as OMP_NUM_THREADS)
 *		d) all of the output from the test case.
 *
 * Thanks!
 *
 *-----------------------------------------------------------------------*/

# define HLINE "\r-------------------------------------------------------------\n"

# ifndef MIN
# define MIN(x,y) ((x)<(y)?(x):(y))
# endif
# ifndef MAX
# define MAX(x,y) ((x)>(y)?(x):(y))
# endif

#ifndef STREAM_TYPE
#define STREAM_TYPE double
#endif

/*
static STREAM_TYPE	aa[STREAM_ARRAY_SIZE+OFFSET],
			bb[STREAM_ARRAY_SIZE+OFFSET],
			cc[STREAM_ARRAY_SIZE+OFFSET];

static STREAM_TYPE	*a = aa,
			*b = bb,
			*c = cc;
*/
static STREAM_TYPE      *a = NULL,
			*b = NULL,
			*c = NULL;

static double	avgtime[4] = {0}, maxtime[4] = {0},
		mintime[4] = {FLT_MAX,FLT_MAX,FLT_MAX,FLT_MAX};

static char	*label[4] = {"Copy:      ", "Scale:     ",
    "Add:       ", "Triad:     "};

static double	bytes[4] = {
    2 * sizeof(STREAM_TYPE) * STREAM_ARRAY_SIZE,
    2 * sizeof(STREAM_TYPE) * STREAM_ARRAY_SIZE,
    3 * sizeof(STREAM_TYPE) * STREAM_ARRAY_SIZE,
    3 * sizeof(STREAM_TYPE) * STREAM_ARRAY_SIZE
    };

extern double mysecond();
extern void checkSTREAMresults(ssize_t stream_array_size);
#ifdef TUNED
extern void tuned_STREAM_Copy(ssize_t stream_array_size);
extern void tuned_STREAM_Scale(STREAM_TYPE scalar, ssize_t stream_array_size);
extern void tuned_STREAM_Add(ssize_t stream_array_size);
extern void tuned_STREAM_Triad(STREAM_TYPE scalar, ssize_t stream_array_size);
#endif
#ifdef _OPENMP
extern int omp_get_num_threads();
#endif
//#define PRINT
#ifdef PRINT
#define PRINT_LOG(x) printf("%d - %ld\r", __LINE__, x)
#define PRINT_PROGRESS(name, x) do {\
	if ( x % 10240 == 0 ) printf("\r%s - %ld/%ld                        ", name, x, stream_array_size);\
	} while(0)
#else
#define PRINT_LOG(x)
#define PRINT_PROGRESS(name, x)
#endif
int
main(int argc, char **argv)
    {
    int			quantum, checktick();
    int			BytesPerWord;
    int			k;
    ssize_t		j;
    STREAM_TYPE		scalar;
    double		t, times[4][NTIMES];
    ssize_t		buffer_size=(STREAM_ARRAY_SIZE+OFFSET)*sizeof(STREAM_TYPE), stream_array_size;
    ssize_t		offset=0;

    /* --- SETUP --- determine precision and check timing --- */
    //swsok, for char dev mmap
    int fid;

    buffer_size = (STREAM_ARRAY_SIZE+OFFSET)*sizeof(STREAM_TYPE);

    printf("\nUsage: \t%s \t\t\t\t\t- Local RAM test with %ld bytes\n", argv[0], buffer_size);
    printf("\t%s [size]\t\t\t\t- Local RAM test with [size] bytes\n", argv[0]);
    printf("\t%s [size] [/dev/mem]\t\t- /dev/mem test with [size] and offset=0x100000000\n", argv[0]);
    printf("\t%s [size] [/dev/mem] [offset]\t- /dev/mem test with [size] and [offset]\n\n", argv[0]);

    if ( argc > 1 ) {
	    buffer_size = atoll(argv[1]);
	    if ( buffer_size <= 0 ) buffer_size = (STREAM_ARRAY_SIZE+OFFSET)*sizeof(STREAM_TYPE);
    }

    //buffer_size must be a multiple of page size(4kB)
    buffer_size = (buffer_size+4095)&(~0xFFF);

    stream_array_size = buffer_size/sizeof(STREAM_TYPE);
	bytes[0] = 2 * buffer_size;
	bytes[1] = 2 * buffer_size;
	bytes[2] = 3 * buffer_size;
	bytes[3] = 3 * buffer_size;

    if ( argc > 2 ) {
	    if ( argc > 3 ) offset = strtoll(argv[3], NULL, 0);
	    if ( offset <= 0 ) offset = 0x100000000;
	    //offset must be page-aligned
	    offset = offset & (~0xFFF);

	fid = open(argv[2], O_RDWR);
	if (fid < 0) {
		printf("%s is not opened\n", argv[1]);
		return 0;
		}

	a = mmap(NULL, buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, fid, offset);
	b = mmap(NULL, buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, fid, offset + buffer_size);
	c = mmap(NULL, buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, fid, offset + 2*buffer_size);
/*	a = mmap(NULL, (STREAM_ARRAY_SIZE+OFFSET)*sizeof(STREAM_TYPE), PROT_READ | PROT_WRITE, MAP_SHARED, fid, 0);
	b = mmap(NULL, (STREAM_ARRAY_SIZE+OFFSET)*sizeof(STREAM_TYPE), PROT_READ | PROT_WRITE, MAP_SHARED, fid, ((STREAM_ARRAY_SIZE+OFFSET)*sizeof(STREAM_TYPE)+0x1000)&(~0xfff));
	c = mmap(NULL, (STREAM_ARRAY_SIZE+OFFSET)*sizeof(STREAM_TYPE), PROT_READ | PROT_WRITE, MAP_SHARED, fid, 2*(((STREAM_ARRAY_SIZE+OFFSET)*sizeof(STREAM_TYPE)+0x1000)&(~0xfff)));*/
    }

    if ( a == NULL ) a = aligned_alloc (4096, buffer_size);
    if ( b == NULL ) b = aligned_alloc (4096, buffer_size);
    if ( c == NULL ) c = aligned_alloc (4096, buffer_size);

    printf(HLINE);
    printf("STREAM version $Revision: 5.10 $\n");
    printf(HLINE);
    BytesPerWord = sizeof(STREAM_TYPE);
    printf("This system uses %d bytes per array element.\n",
	BytesPerWord);

    printf(HLINE);
#ifdef N
    printf("*****  WARNING: ******\n");
    printf("      It appears that you set the preprocessor variable N when compiling this code.\n");
    printf("      This version of the code uses the preprocesor variable STREAM_ARRAY_SIZE to control the array size\n");
    printf("      Reverting to default value of STREAM_ARRAY_SIZE=%llu\n",(unsigned long long) STREAM_ARRAY_SIZE);
    printf("*****  WARNING: ******\n");
#endif

//    printf("Array size = %llu (elements), Offset = %d (elements)\n" , (unsigned long long) STREAM_ARRAY_SIZE, OFFSET);
    printf("Array size = %llu (elements), Offset = %d (elements)\n" , (unsigned long long) stream_array_size, OFFSET);
    printf("Memory per array = %.1f MiB (= %.1f GiB).\n", 
	 ( (double) buffer_size / 1024.0/1024.0),
	 ( (double) buffer_size / 1024.0/1024.0/1024.0));
    printf("Total memory required = %.1f MiB (= %.1f GiB).\n",
	(3.0) * ( (double) buffer_size / 1024.0/1024.),
	(3.0) * ( (double) buffer_size / 1024.0/1024./1024.));
    printf("Each kernel will be executed %d times.\n", NTIMES);
    printf(" The *best* time for each kernel (excluding the first iteration)\n"); 
    printf(" will be used to compute the reported bandwidth.\n");

#ifdef _OPENMP
    printf(HLINE);
#pragma omp parallel 
    {
#pragma omp master
	{
	    k = omp_get_num_threads();
	    printf ("Number of Threads requested = %i\n",k);
        }
    }
#endif

#ifdef _OPENMP
	k = 0;
#pragma omp parallel
#pragma omp atomic 
		k++;
    printf ("Number of Threads counted = %i\n",k);
#endif

    /* Get initial value for system clock. */
#pragma omp parallel for
    for (j=0; j<stream_array_size; j++) {
	    PRINT_LOG(j);
	    PRINT_PROGRESS("Setup initial values", j);
	    a[j] = 1.0;
	    b[j] = 2.0;
	    c[j] = 0.0;
	}

    printf(HLINE);

    if  ( (quantum = checktick()) >= 1) 
	printf("\rYour clock granularity/precision appears to be "
	    "%d microseconds.\n", quantum);
    else {
	printf("\rYour clock granularity appears to be "
	    "less than one microsecond.\n");
	quantum = 1;
    }
    if(0){
	double temp;
	for (j = 0; j < stream_array_size; j++)
	    temp =  a[j];
    }

    t = mysecond();
#pragma omp parallel for
    for (j = 0; j < stream_array_size; j++) {
	        PRINT_LOG(j);
	        PRINT_PROGRESS("a = a * 2.0e0", j);
		a[j] = 2.0E0 * a[j];
    }
    t = 1.0E6 * (mysecond() - t);

    printf("\rEach test below will take on the order"
	" of %d microseconds.\n", (int) t  );
    printf("   (= %d clock ticks)\n", (int) (t/quantum) );
    printf("Increase the size of the arrays if this shows that\n");
    printf("you are not getting at least 20 clock ticks per test.\n");

    printf(HLINE);

    printf("WARNING -- The above is only a rough guideline.\n");
    printf("For best results, please be sure you know the\n");
    printf("precision of your system timer.\n");
    printf(HLINE);
    
    /*	--- MAIN LOOP --- repeat test cases NTIMES times --- */

    scalar = 3.0;
    for (k=0; k<NTIMES; k++)
	{
	times[0][k] = mysecond();
#ifdef TUNED
        tuned_STREAM_Copy(stream_array_size);
#else
#pragma omp parallel for
	for (j=0; j<stream_array_size; j++) {
	    PRINT_LOG(j);
	    PRINT_PROGRESS("COPY (c = a)", j);
	    c[j] = a[j];
	}
#endif
	times[0][k] = mysecond() - times[0][k];
	
	times[1][k] = mysecond();
#ifdef TUNED
        tuned_STREAM_Scale(scalar, stream_array_size);
#else
#pragma omp parallel for
	for (j=0; j<stream_array_size; j++) {
	    PRINT_LOG(j);
	    PRINT_PROGRESS("SCALE (b = 3.0*c)", j);
	    b[j] = scalar*c[j];
	}
#endif
	times[1][k] = mysecond() - times[1][k];
	
	times[2][k] = mysecond();
#ifdef TUNED
        tuned_STREAM_Add(stream_array_size);
#else
#pragma omp parallel for
	for (j=0; j<stream_array_size; j++) {
	    PRINT_LOG(j);
	    PRINT_PROGRESS("ADD (c = a + b)", j);
	    c[j] = a[j]+b[j];
	}
#endif
	times[2][k] = mysecond() - times[2][k];
	
	times[3][k] = mysecond();
#ifdef TUNED
        tuned_STREAM_Triad(scalar, stream_array_size);
#else
#pragma omp parallel for
	for (j=0; j<stream_array_size; j++) {
	    PRINT_LOG(j);
	    PRINT_PROGRESS("TRIAD (a = b + 3.0*c)", j);
	    a[j] = b[j]+scalar*c[j];
	}
#endif
	times[3][k] = mysecond() - times[3][k];
	}

    /*	--- SUMMARY --- */

    for (k=1; k<NTIMES; k++) /* note -- skip first iteration */
	{
	for (j=0; j<4; j++)
	    {
	    avgtime[j] = avgtime[j] + times[j][k];
	    mintime[j] = MIN(mintime[j], times[j][k]);
	    maxtime[j] = MAX(maxtime[j], times[j][k]);
	    }
	}
    
    printf("\rFunction    Best Rate MB/s  Avg time     Min time     Max time\n");
    for (j=0; j<4; j++) {
		avgtime[j] = avgtime[j]/(double)(NTIMES-1);

		printf("%s%12.1f  %11.6f  %11.6f  %11.6f\n", label[j],
	       1.0E-06 * bytes[j]/mintime[j],
	       avgtime[j],
	       mintime[j],
	       maxtime[j]);
    }
    printf(HLINE);

    /* --- Check Results --- */
    checkSTREAMresults(stream_array_size);
    printf(HLINE);

    //swsok
    if ( argc > 1 ) {
	munmap(a, buffer_size);
	munmap(b, buffer_size);
	munmap(c, buffer_size);
	if ( fid ) close(fid);
    }

    return 0;
}

# define	M	20

int
checktick()
    {
    int		i, minDelta, Delta;
    double	t1, t2, timesfound[M];

/*  Collect a sequence of M unique time values from the system. */

    for (i = 0; i < M; i++) {
	t1 = mysecond();
	while( ((t2=mysecond()) - t1) < 1.0E-6 )
	    ;
	timesfound[i] = t1 = t2;
	}

/*
 * Determine the minimum difference between these M values.
 * This result will be our estimate (in microseconds) for the
 * clock granularity.
 */

    minDelta = 1000000;
    for (i = 1; i < M; i++) {
	Delta = (int)( 1.0E6 * (timesfound[i]-timesfound[i-1]));
	minDelta = MIN(minDelta, MAX(Delta,0));
	}

   return(minDelta);
    }



/* A gettimeofday routine to give access to the wall
   clock timer on most UNIX-like systems.  */

#include <sys/time.h>

#if 0
double mysecond()
{
        struct timeval tp;
        struct timezone tzp;
        int i;

        i = gettimeofday(&tp,&tzp);
        return ( (double) tp.tv_sec + (double) tp.tv_usec * 1.e-6 );
}
#else
static uintptr_t rdcycle()
{
  uintptr_t out;
  __asm__ __volatile__ ("rdcycle %0" : "=r"(out));
  return out;
}

//100MHz
#define CLOCK_PER_SEC 100000000
double mysecond()
{
	double t = 0;
	uintptr_t clockcounter = rdcycle();
	t = (double)clockcounter / CLOCK_PER_SEC;
	return t;
}
#endif


#ifndef abs
#define abs(a) ((a) >= 0 ? (a) : -(a))
#endif
void checkSTREAMresults (ssize_t stream_array_size)
{
	STREAM_TYPE aj,bj,cj,scalar;
	STREAM_TYPE aSumErr,bSumErr,cSumErr;
	STREAM_TYPE aAvgErr,bAvgErr,cAvgErr;
	double epsilon;
	ssize_t	j;
	int	k,ierr,err;

    /* reproduce initialization */
	aj = 1.0;
	bj = 2.0;
	cj = 0.0;
    /* a[] is modified during timing check */
	aj = 2.0E0 * aj;
    /* now execute timing loop */
	scalar = 3.0;
	for (k=0; k<NTIMES; k++)
        {
            cj = aj;
            bj = scalar*cj;
            cj = aj+bj;
            aj = bj+scalar*cj;
        }

    /* accumulate deltas between observed and expected results */
	aSumErr = 0.0;
	bSumErr = 0.0;
	cSumErr = 0.0;
	for (j=0; j<stream_array_size; j++) {
		aSumErr += abs(a[j] - aj);
		bSumErr += abs(b[j] - bj);
		cSumErr += abs(c[j] - cj);
		// if (j == 417) printf("Index 417: c[j]: %f, cj: %f\n",c[j],cj);	// MCCALPIN
	}
	aAvgErr = aSumErr / (STREAM_TYPE) stream_array_size;
	bAvgErr = bSumErr / (STREAM_TYPE) stream_array_size;
	cAvgErr = cSumErr / (STREAM_TYPE) stream_array_size;

	if (sizeof(STREAM_TYPE) == 4) {
		epsilon = 1.e-6;
	}
	else if (sizeof(STREAM_TYPE) == 8) {
		epsilon = 1.e-13;
	}
	else {
		printf("WEIRD: sizeof(STREAM_TYPE) = %lu\n",sizeof(STREAM_TYPE));
		epsilon = 1.e-6;
	}

	err = 0;
	if (abs(aAvgErr/aj) > epsilon) {
		err++;
		printf ("Failed Validation on array a[], AvgRelAbsErr > epsilon (%e)\n",epsilon);
		printf ("     Expected Value: %e, AvgAbsErr: %e, AvgRelAbsErr: %e\n",aj,aAvgErr,abs(aAvgErr)/aj);
		ierr = 0;
		for (j=0; j<stream_array_size; j++) {
			if (abs(a[j]/aj-1.0) > epsilon) {
				ierr++;
#ifdef VERBOSE
				if (ierr < 10) {
					printf("         array a: index: %ld, expected: %e, observed: %e, relative error: %e\n",
						j,aj,a[j],abs((aj-a[j])/aAvgErr));
				}
#endif
			}
		}
		printf("     For array a[], %d errors were found.\n",ierr);
	}
	if (abs(bAvgErr/bj) > epsilon) {
		err++;
		printf ("Failed Validation on array b[], AvgRelAbsErr > epsilon (%e)\n",epsilon);
		printf ("     Expected Value: %e, AvgAbsErr: %e, AvgRelAbsErr: %e\n",bj,bAvgErr,abs(bAvgErr)/bj);
		printf ("     AvgRelAbsErr > Epsilon (%e)\n",epsilon);
		ierr = 0;
		for (j=0; j<stream_array_size; j++) {
			if (abs(b[j]/bj-1.0) > epsilon) {
				ierr++;
#ifdef VERBOSE
				if (ierr < 10) {
					printf("         array b: index: %ld, expected: %e, observed: %e, relative error: %e\n",
						j,bj,b[j],abs((bj-b[j])/bAvgErr));
				}
#endif
			}
		}
		printf("     For array b[], %d errors were found.\n",ierr);
	}
	if (abs(cAvgErr/cj) > epsilon) {
		err++;
		printf ("Failed Validation on array c[], AvgRelAbsErr > epsilon (%e)\n",epsilon);
		printf ("     Expected Value: %e, AvgAbsErr: %e, AvgRelAbsErr: %e\n",cj,cAvgErr,abs(cAvgErr)/cj);
		printf ("     AvgRelAbsErr > Epsilon (%e)\n",epsilon);
		ierr = 0;
		for (j=0; j<stream_array_size; j++) {
			if (abs(c[j]/cj-1.0) > epsilon) {
				ierr++;
#ifdef VERBOSE
				if (ierr < 10) {
					printf("         array c: index: %ld, expected: %e, observed: %e, relative error: %e\n",
						j,cj,c[j],abs((cj-c[j])/cAvgErr));
				}
#endif
			}
		}
		printf("     For array c[], %d errors were found.\n",ierr);
	}
	if (err == 0) {
		printf ("Solution Validates: avg error less than %e on all three arrays\n",epsilon);
	}
#ifdef VERBOSE
	printf ("Results Validation Verbose Results: \n");
	printf ("    Expected a(1), b(1), c(1): %f %f %f \n",aj,bj,cj);
	printf ("    Observed a(1), b(1), c(1): %f %f %f \n",a[1],b[1],c[1]);
	printf ("    Rel Errors on a, b, c:     %e %e %e \n",abs(aAvgErr/aj),abs(bAvgErr/bj),abs(cAvgErr/cj));
#endif
}

#ifdef TUNED
/* stubs for "tuned" versions of the kernels */
void tuned_STREAM_Copy(ssize_t stream_array_size)
{
	ssize_t j;
#pragma omp parallel for
        for (j=0; j<stream_array_size; j++)
            c[j] = a[j];
}

void tuned_STREAM_Scale(STREAM_TYPE scalar, ssize_t stream_array_size)
{
	ssize_t j;
#pragma omp parallel for
	for (j=0; j<stream_array_size; j++)
	    b[j] = scalar*c[j];
}

void tuned_STREAM_Add(ssize_t stream_array_size)
{
	ssize_t j;
#pragma omp parallel for
	for (j=0; j<stream_array_size; j++)
	    c[j] = a[j]+b[j];
}

void tuned_STREAM_Triad(STREAM_TYPE scalar, ssize_t stream_array_size)
{
	ssize_t j;
#pragma omp parallel for
	for (j=0; j<stream_array_size; j++)
	    a[j] = b[j]+scalar*c[j];
}
/* end of stubs for the "tuned" versions of the kernels */
#endif
