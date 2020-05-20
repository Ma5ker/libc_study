/* Malloc implementation for multiple threads without lock contention.
   Copyright (C) 1996-2020 Free Software Foundation, Inc.
   This file is part of the GNU C Library.
   Contributed by Wolfram Gloger <wg@malloc.de>
   and Doug Lea <dl@cs.oswego.edu>, 2001.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public License as
   published by the Free Software Foundation; either version 2.1 of the
   License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; see the file COPYING.LIB.  If
   not, see <https://www.gnu.org/licenses/>.  */

/*
  This is a version (aka ptmalloc2) of malloc/free/realloc written by
  Doug Lea and adapted to multiple threads/arenas by Wolfram Gloger.

  There have been substantial changes made after the integration into
  glibc in all parts of the code.  Do not look for much commonality
  with the ptmalloc2 version.

* Version ptmalloc2-20011215
  based on:
  VERSION 2.7.0 Sun Mar 11 14:14:06 2001  Doug Lea  (dl at gee)

* Quickstart

  In order to compile this implementation, a Makefile is provided with
  the ptmalloc2 distribution, which has pre-defined targets for some
  popular systems (e.g. "make posix" for Posix threads).  All that is
  typically required with regard to compiler flags is the selection of
  the thread package via defining one out of USE_PTHREADS, USE_THR or
  USE_SPROC.  Check the thread-m.h file for what effects this has.
  Many/most systems will additionally require USE_TSD_DATA_HACK to be
  defined, so this is the default for "make posix".

* Why use this malloc?

  This is not the fastest, most space-conserving, most portable, or
  most tunable malloc ever written. However it is among the fastest
  while also being among the most space-conserving, portable and tunable.
  Consistent balance across these factors results in a good general-purpose
  allocator for malloc-intensive programs.

  The main properties of the algorithms are:
  * For large (>= 512 bytes) requests, it is a pure best-fit allocator,
    with ties normally decided via FIFO (i.e. least recently used).
  * For small (<= 64 bytes by default) requests, it is a caching
    allocator, that maintains pools of quickly recycled chunks.
  * In between, and for combinations of large and small requests, it does
    the best it can trying to meet both goals at once.
  * For very large requests (>= 128KB by default), it relies on system
    memory mapping facilities, if supported.

  For a longer but slightly out of date high-level description, see
     http://gee.cs.oswego.edu/dl/html/malloc.html

  You may already by default be using a C library containing a malloc
  that is  based on some version of this malloc (for example in
  linux). You might still want to use the one in this file in order to
  customize settings or to avoid overheads associated with library
  versions.

* Contents, described in more detail in "description of public routines" below.

  Standard (ANSI/SVID/...)  functions:
    malloc(size_t n);
    calloc(size_t n_elements, size_t element_size);
    free(void* p);
    realloc(void* p, size_t n);
    memalign(size_t alignment, size_t n);
    valloc(size_t n);
    mallinfo()
    mallopt(int parameter_number, int parameter_value)

  Additional functions:
    independent_calloc(size_t n_elements, size_t size, void* chunks[]);
    independent_comalloc(size_t n_elements, size_t sizes[], void* chunks[]);
    pvalloc(size_t n);
    malloc_trim(size_t pad);
    malloc_usable_size(void* p);
    malloc_stats();

* Vital statistics:

  Supported pointer representation:       4 or 8 bytes
  Supported size_t  representation:       4 or 8 bytes
       Note that size_t is allowed to be 4 bytes even if pointers are 8.
       You can adjust this by defining INTERNAL_SIZE_T

  Alignment:                              2 * sizeof(size_t) (default)
       (i.e., 8 byte alignment with 4byte size_t). This suffices for
       nearly all current machines and C compilers. However, you can
       define MALLOC_ALIGNMENT to be wider than this if necessary.

  Minimum overhead per allocated chunk:   4 or 8 bytes
       Each malloced chunk has a hidden word of overhead holding size
       and status information.

  Minimum allocated size: 4-byte ptrs:  16 bytes    (including 4 overhead)
			  8-byte ptrs:  24/32 bytes (including, 4/8 overhead)

       When a chunk is freed, 12 (for 4byte ptrs) or 20 (for 8 byte
       ptrs but 4 byte size) or 24 (for 8/8) additional bytes are
       needed; 4 (8) for a trailing size field and 8 (16) bytes for
       free list pointers. Thus, the minimum allocatable size is
       16/24/32 bytes.

       Even a request for zero bytes (i.e., malloc(0)) returns a
       pointer to something of the minimum allocatable size.

       The maximum overhead wastage (i.e., number of extra bytes
       allocated than were requested in malloc) is less than or equal
       to the minimum size, except for requests >= mmap_threshold that
       are serviced via mmap(), where the worst case wastage is 2 *
       sizeof(size_t) bytes plus the remainder from a system page (the
       minimal mmap unit); typically 4096 or 8192 bytes.

  Maximum allocated size:  4-byte size_t: 2^32 minus about two pages
			   8-byte size_t: 2^64 minus about two pages

       It is assumed that (possibly signed) size_t values suffice to
       represent chunk sizes. `Possibly signed' is due to the fact
       that `size_t' may be defined on a system as either a signed or
       an unsigned type. The ISO C standard says that it must be
       unsigned, but a few systems are known not to adhere to this.
       Additionally, even when size_t is unsigned, sbrk (which is by
       default used to obtain memory from system) accepts signed
       arguments, and may not be able to handle size_t-wide arguments
       with negative sign bit.  Generally, values that would
       appear as negative after accounting for overhead and alignment
       are supported only via mmap(), which does not have this
       limitation.

       Requests for sizes outside the allowed range will perform an optional
       failure action and then return null. (Requests may also
       also fail because a system is out of memory.)

  Thread-safety: thread-safe

  Compliance: I believe it is compliant with the 1997 Single Unix Specification
       Also SVID/XPG, ANSI C, and probably others as well.

* Synopsis of compile-time options:

    People have reported using previous versions of this malloc on all
    versions of Unix, sometimes by tweaking some of the defines
    below. It has been tested most extensively on Solaris and Linux.
    People also report using it in stand-alone embedded systems.

    The implementation is in straight, hand-tuned ANSI C.  It is not
    at all modular. (Sorry!)  It uses a lot of macros.  To be at all
    usable, this code should be compiled using an optimizing compiler
    (for example gcc -O3) that can simplify expressions and control
    paths. (FAQ: some macros import variables as arguments rather than
    declare locals because people reported that some debuggers
    otherwise get confused.)

    OPTION                     DEFAULT VALUE

    Compilation Environment options:

    HAVE_MREMAP                0

    Changing default word sizes:

    INTERNAL_SIZE_T            size_t

    Configuration and functionality options:

    USE_PUBLIC_MALLOC_WRAPPERS NOT defined
    USE_MALLOC_LOCK            NOT defined
    MALLOC_DEBUG               NOT defined
    REALLOC_ZERO_BYTES_FREES   1
    TRIM_FASTBINS              0

    Options for customizing MORECORE:

    MORECORE                   sbrk
    MORECORE_FAILURE           -1
    MORECORE_CONTIGUOUS        1
    MORECORE_CANNOT_TRIM       NOT defined
    MORECORE_CLEARS            1
    MMAP_AS_MORECORE_SIZE      (1024 * 1024)

    Tuning options that are also dynamically changeable via mallopt:

    DEFAULT_MXFAST             64 (for 32bit), 128 (for 64bit)
    DEFAULT_TRIM_THRESHOLD     128 * 1024
    DEFAULT_TOP_PAD            0
    DEFAULT_MMAP_THRESHOLD     128 * 1024
    DEFAULT_MMAP_MAX           65536

    There are several other #defined constants and macros that you
    probably don't want to touch unless you are extending or adapting malloc.  */

/*
  void* is the pointer type that malloc should say it returns
*/

#ifndef void
#define void      void
#endif /*void*/

#include <stddef.h>   /* for size_t */
#include <stdlib.h>   /* for getenv(), abort() */
#include <unistd.h>   /* for __libc_enable_secure */

#include <atomic.h>
#include <_itoa.h>
#include <bits/wordsize.h>
#include <sys/sysinfo.h>

#include <ldsodefs.h>

#include <unistd.h>
#include <stdio.h>    /* needed for malloc_stats */
#include <errno.h>
#include <assert.h>

#include <shlib-compat.h>

/* For uintptr_t.  */
#include <stdint.h>

/* For va_arg, va_start, va_end.  */
#include <stdarg.h>

/* For MIN, MAX, powerof2.  */
#include <sys/param.h>

/* For ALIGN_UP et. al.  */
#include <libc-pointer-arith.h>

/* For DIAG_PUSH/POP_NEEDS_COMMENT et al.  */
#include <libc-diag.h>

#include <malloc/malloc-internal.h>

/* For SINGLE_THREAD_P.  */
#include <sysdep-cancel.h>

/*
  Debugging:

  Because freed chunks may be overwritten with bookkeeping fields, this
  malloc will often die when freed memory is overwritten by user
  programs.  This can be very effective (albeit in an annoying way)
  in helping track down dangling pointers.

  If you compile with -DMALLOC_DEBUG, a number of assertion checks are
  enabled that will catch more memory errors. You probably won't be
  able to make much sense of the actual assertion errors, but they
  should help you locate incorrectly overwritten memory.  The checking
  is fairly extensive, and will slow down execution
  noticeably. Calling malloc_stats or mallinfo with MALLOC_DEBUG set
  will attempt to check every non-mmapped allocated and free chunk in
  the course of computing the summmaries. (By nature, mmapped regions
  cannot be checked very much automatically.)

  Setting MALLOC_DEBUG may also be helpful if you are trying to modify
  this code. The assertions in the check routines spell out in more
  detail the assumptions and invariants underlying the algorithms.

  Setting MALLOC_DEBUG does NOT provide an automated mechanism for
  checking that all accesses to malloced memory stay within their
  bounds. However, there are several add-ons and adaptations of this
  or other mallocs available that do this.
*/

#ifndef MALLOC_DEBUG
#define MALLOC_DEBUG 0
#endif

#ifndef NDEBUG
# define __assert_fail(assertion, file, line, function)			\
	 __malloc_assert(assertion, file, line, function)

extern const char *__progname;

static void
__malloc_assert (const char *assertion, const char *file, unsigned int line,
		 const char *function)
{
  (void) __fxprintf (NULL, "%s%s%s:%u: %s%sAssertion `%s' failed.\n",
		     __progname, __progname[0] ? ": " : "",
		     file, line,
		     function ? function : "", function ? ": " : "",
		     assertion);
  fflush (stderr);
  abort ();
}
#endif

#if USE_TCACHE
/* We want 64 entries.  This is an arbitrary limit, which tunables can reduce.  */
# define TCACHE_MAX_BINS		64
# define MAX_TCACHE_SIZE	tidx2usize (TCACHE_MAX_BINS-1)

/* Only used to pre-fill the tunables.  */
# define tidx2usize(idx)	(((size_t) idx) * MALLOC_ALIGNMENT + MINSIZE - SIZE_SZ)

/* When "x" is from chunksize().  */
# define csize2tidx(x) (((x) - MINSIZE + MALLOC_ALIGNMENT - 1) / MALLOC_ALIGNMENT)
/* When "x" is a user-provided size.  */
# define usize2tidx(x) csize2tidx (request2size (x))

/* With rounding and alignment, the bins are...
   idx 0   bytes 0..24 (64-bit) or 0..12 (32-bit)
   idx 1   bytes 25..40 or 13..20
   idx 2   bytes 41..56 or 21..28
   etc.  */

/* This is another arbitrary limit, which tunables can change.  Each
   tcache bin will hold at most this number of chunks.  */
/*每个tcache链上chunk的最大值*/
# define TCACHE_FILL_COUNT 7

/* Maximum chunks in tcache bins for tunables.  This value must fit the range
   of tcache->counts[] entries, else they may overflow.  */
# define MAX_TCACHE_COUNT UINT16_MAX
#endif


/*
  REALLOC_ZERO_BYTES_FREES should be set if a call to
  realloc with zero bytes should be the same as a call to free.
  This is required by the C standard. Otherwise, since this malloc
  returns a unique pointer for malloc(0), so does realloc(p, 0).
*/

#ifndef REALLOC_ZERO_BYTES_FREES
#define REALLOC_ZERO_BYTES_FREES 1
#endif

/*
  TRIM_FASTBINS controls whether free() of a very small chunk can
  immediately lead to trimming. Setting to true (1) can reduce memory
  footprint, but will almost always slow down programs that use a lot
  of small chunks.

  Define this only if you are willing to give up some speed to more
  aggressively reduce system-level memory footprint when releasing
  memory in programs that use many small chunks.  You can get
  essentially the same effect by setting MXFAST to 0, but this can
  lead to even greater slowdowns in programs using many small chunks.
  TRIM_FASTBINS is an in-between compile-time option, that disables
  only those chunks bordering topmost memory from being placed in
  fastbins.
*/

#ifndef TRIM_FASTBINS
#define TRIM_FASTBINS  0
#endif


/* Definition for getting more memory from the OS.  */
#define MORECORE         (*__morecore)
#define MORECORE_FAILURE 0
void * __default_morecore (ptrdiff_t);
void *(*__morecore)(ptrdiff_t) = __default_morecore;


#include <string.h>

/*
  MORECORE-related declarations. By default, rely on sbrk
*/


/*
  MORECORE is the name of the routine to call to obtain more memory
  from the system.  See below for general guidance on writing
  alternative MORECORE functions, as well as a version for WIN32 and a
  sample version for pre-OSX macos.
*/

#ifndef MORECORE
#define MORECORE sbrk
#endif

/*
  MORECORE_FAILURE is the value returned upon failure of MORECORE
  as well as mmap. Since it cannot be an otherwise valid memory address,
  and must reflect values of standard sys calls, you probably ought not
  try to redefine it.
*/

#ifndef MORECORE_FAILURE
#define MORECORE_FAILURE (-1)
#endif

/*
  If MORECORE_CONTIGUOUS is true, take advantage of fact that
  consecutive calls to MORECORE with positive arguments always return
  contiguous increasing addresses.  This is true of unix sbrk.  Even
  if not defined, when regions happen to be contiguous, malloc will
  permit allocations spanning regions obtained from different
  calls. But defining this when applicable enables some stronger
  consistency checks and space efficiencies.
*/

#ifndef MORECORE_CONTIGUOUS
#define MORECORE_CONTIGUOUS 1
#endif

/*
  Define MORECORE_CANNOT_TRIM if your version of MORECORE
  cannot release space back to the system when given negative
  arguments. This is generally necessary only if you are using
  a hand-crafted MORECORE function that cannot handle negative arguments.
*/

/* #define MORECORE_CANNOT_TRIM */

/*  MORECORE_CLEARS           (default 1)
     The degree to which the routine mapped to MORECORE zeroes out
     memory: never (0), only for newly allocated space (1) or always
     (2).  The distinction between (1) and (2) is necessary because on
     some systems, if the application first decrements and then
     increments the break value, the contents of the reallocated space
     are unspecified.
 */

#ifndef MORECORE_CLEARS
# define MORECORE_CLEARS 1
#endif


/*
   MMAP_AS_MORECORE_SIZE is the minimum mmap size argument to use if
   sbrk fails, and mmap is used as a backup.  The value must be a
   multiple of page size.  This backup strategy generally applies only
   when systems have "holes" in address space, so sbrk cannot perform
   contiguous expansion, but there is still space available on system.
   On systems for which this is known to be useful (i.e. most linux
   kernels), this occurs only when programs allocate huge amounts of
   memory.  Between this, and the fact that mmap regions tend to be
   limited, the size should be large, to avoid too many mmap calls and
   thus avoid running out of kernel resources.  */

#ifndef MMAP_AS_MORECORE_SIZE
#define MMAP_AS_MORECORE_SIZE (1024 * 1024)
#endif

/*
  Define HAVE_MREMAP to make realloc() use mremap() to re-allocate
  large blocks.
*/

#ifndef HAVE_MREMAP
#define HAVE_MREMAP 0
#endif

/* We may need to support __malloc_initialize_hook for backwards
   compatibility.  */

#if SHLIB_COMPAT (libc, GLIBC_2_0, GLIBC_2_24)
# define HAVE_MALLOC_INIT_HOOK 1
#else
# define HAVE_MALLOC_INIT_HOOK 0
#endif


/*
  This version of malloc supports the standard SVID/XPG mallinfo
  routine that returns a struct containing usage properties and
  statistics. It should work on any SVID/XPG compliant system that has
  a /usr/include/malloc.h defining struct mallinfo. (If you'd like to
  install such a thing yourself, cut out the preliminary declarations
  as described above and below and save them in a malloc.h file. But
  there's no compelling reason to bother to do this.)

  The main declaration needed is the mallinfo struct that is returned
  (by-copy) by mallinfo().  The SVID/XPG malloinfo struct contains a
  bunch of fields that are not even meaningful in this version of
  malloc.  These fields are are instead filled by mallinfo() with
  other numbers that might be of interest.
*/


/* ---------- description of public routines ------------ */

/*
  malloc(size_t n)
  Returns a pointer to a newly allocated chunk of at least n bytes, or null
  if no space is available. Additionally, on failure, errno is
  set to ENOMEM on ANSI C systems.

  If n is zero, malloc returns a minumum-sized chunk. (The minimum
  size is 16 bytes on most 32bit systems, and 24 or 32 bytes on 64bit
  systems.)  On most systems, size_t is an unsigned type, so calls
  with negative arguments are interpreted as requests for huge amounts
  of space, which will often fail. The maximum supported value of n
  differs across systems, but is in all cases less than the maximum
  representable value of a size_t.
*/
void*  __libc_malloc(size_t);
libc_hidden_proto (__libc_malloc)

/*
  free(void* p)
  Releases the chunk of memory pointed to by p, that had been previously
  allocated using malloc or a related routine such as realloc.
  It has no effect if p is null. It can have arbitrary (i.e., bad!)
  effects if p has already been freed.

  Unless disabled (using mallopt), freeing very large spaces will
  when possible, automatically trigger operations that give
  back unused memory to the system, thus reducing program footprint.
*/
void     __libc_free(void*);
libc_hidden_proto (__libc_free)

/*
  calloc(size_t n_elements, size_t element_size);
  Returns a pointer to n_elements * element_size bytes, with all locations
  set to zero.
*/
void*  __libc_calloc(size_t, size_t);

/*
  realloc(void* p, size_t n)
  Returns a pointer to a chunk of size n that contains the same data
  as does chunk p up to the minimum of (n, p's size) bytes, or null
  if no space is available.

  The returned pointer may or may not be the same as p. The algorithm
  prefers extending p when possible, otherwise it employs the
  equivalent of a malloc-copy-free sequence.

  If p is null, realloc is equivalent to malloc.

  If space is not available, realloc returns null, errno is set (if on
  ANSI) and p is NOT freed.

  if n is for fewer bytes than already held by p, the newly unused
  space is lopped off and freed if possible.  Unless the #define
  REALLOC_ZERO_BYTES_FREES is set, realloc with a size argument of
  zero (re)allocates a minimum-sized chunk.

  Large chunks that were internally obtained via mmap will always be
  grown using malloc-copy-free sequences unless the system supports
  MREMAP (currently only linux).

  The old unix realloc convention of allowing the last-free'd chunk
  to be used as an argument to realloc is not supported.
*/
void*  __libc_realloc(void*, size_t);
libc_hidden_proto (__libc_realloc)

/*
  memalign(size_t alignment, size_t n);
  Returns a pointer to a newly allocated chunk of n bytes, aligned
  in accord with the alignment argument.

  The alignment argument should be a power of two. If the argument is
  not a power of two, the nearest greater power is used.
  8-byte alignment is guaranteed by normal malloc calls, so don't
  bother calling memalign with an argument of 8 or less.

  Overreliance on memalign is a sure way to fragment space.
*/
void*  __libc_memalign(size_t, size_t);
libc_hidden_proto (__libc_memalign)

/*
  valloc(size_t n);
  Equivalent to memalign(pagesize, n), where pagesize is the page
  size of the system. If the pagesize is unknown, 4096 is used.
*/
void*  __libc_valloc(size_t);



/*
  mallopt(int parameter_number, int parameter_value)
  Sets tunable parameters The format is to provide a
  (parameter-number, parameter-value) pair.  mallopt then sets the
  corresponding parameter to the argument value if it can (i.e., so
  long as the value is meaningful), and returns 1 if successful else
  0.  SVID/XPG/ANSI defines four standard param numbers for mallopt,
  normally defined in malloc.h.  Only one of these (M_MXFAST) is used
  in this malloc. The others (M_NLBLKS, M_GRAIN, M_KEEP) don't apply,
  so setting them has no effect. But this malloc also supports four
  other options in mallopt. See below for details.  Briefly, supported
  parameters are as follows (listed defaults are for "typical"
  configurations).

  Symbol            param #   default    allowed param values
  M_MXFAST          1         64         0-80  (0 disables fastbins)
  M_TRIM_THRESHOLD -1         128*1024   any   (-1U disables trimming)
  M_TOP_PAD        -2         0          any
  M_MMAP_THRESHOLD -3         128*1024   any   (or 0 if no MMAP support)
  M_MMAP_MAX       -4         65536      any   (0 disables use of mmap)
*/
int      __libc_mallopt(int, int);
libc_hidden_proto (__libc_mallopt)


/*
  mallinfo()
  Returns (by copy) a struct containing various summary statistics:

  arena:     current total non-mmapped bytes allocated from system
  ordblks:   the number of free chunks
  smblks:    the number of fastbin blocks (i.e., small chunks that
	       have been freed but not use resused or consolidated)
  hblks:     current number of mmapped regions
  hblkhd:    total bytes held in mmapped regions
  usmblks:   always 0
  fsmblks:   total bytes held in fastbin blocks
  uordblks:  current total allocated space (normal or mmapped)
  fordblks:  total free space
  keepcost:  the maximum number of bytes that could ideally be released
	       back to system via malloc_trim. ("ideally" means that
	       it ignores page restrictions etc.)

  Because these fields are ints, but internal bookkeeping may
  be kept as longs, the reported values may wrap around zero and
  thus be inaccurate.
*/
struct mallinfo __libc_mallinfo(void);


/*
  pvalloc(size_t n);
  Equivalent to valloc(minimum-page-that-holds(n)), that is,
  round up n to nearest pagesize.
 */
void*  __libc_pvalloc(size_t);

/*
  malloc_trim(size_t pad);

  If possible, gives memory back to the system (via negative
  arguments to sbrk) if there is unused memory at the `high' end of
  the malloc pool. You can call this after freeing large blocks of
  memory to potentially reduce the system-level memory requirements
  of a program. However, it cannot guarantee to reduce memory. Under
  some allocation patterns, some large free blocks of memory will be
  locked between two used chunks, so they cannot be given back to
  the system.

  The `pad' argument to malloc_trim represents the amount of free
  trailing space to leave untrimmed. If this argument is zero,
  only the minimum amount of memory to maintain internal data
  structures will be left (one page or less). Non-zero arguments
  can be supplied to maintain enough trailing space to service
  future expected allocations without having to re-obtain memory
  from the system.

  Malloc_trim returns 1 if it actually released any memory, else 0.
  On systems that do not support "negative sbrks", it will always
  return 0.
*/
int      __malloc_trim(size_t);

/*
  malloc_usable_size(void* p);

  Returns the number of bytes you can actually use in
  an allocated chunk, which may be more than you requested (although
  often not) due to alignment and minimum size constraints.
  You can use this many bytes without worrying about
  overwriting other allocated objects. This is not a particularly great
  programming practice. malloc_usable_size can be more useful in
  debugging and assertions, for example:

  p = malloc(n);
  assert(malloc_usable_size(p) >= 256);

*/
size_t   __malloc_usable_size(void*);

/*
  malloc_stats();
  Prints on stderr the amount of space obtained from the system (both
  via sbrk and mmap), the maximum amount (which may be more than
  current if malloc_trim and/or munmap got called), and the current
  number of bytes allocated via malloc (or realloc, etc) but not yet
  freed. Note that this is the number of bytes allocated, not the
  number requested. It will be larger than the number requested
  because of alignment and bookkeeping overhead. Because it includes
  alignment wastage as being in use, this figure may be greater than
  zero even when no user-level chunks are allocated.

  The reported current and maximum system memory can be inaccurate if
  a program makes other calls to system memory allocation functions
  (normally sbrk) outside of malloc.

  malloc_stats prints only the most commonly interesting statistics.
  More information can be obtained by calling mallinfo.

*/
void     __malloc_stats(void);

/*
  posix_memalign(void **memptr, size_t alignment, size_t size);

  POSIX wrapper like memalign(), checking for validity of size.
*/
int      __posix_memalign(void **, size_t, size_t);

/* mallopt tuning options */

/*
  M_MXFAST is the maximum request size used for "fastbins", special bins
  that hold returned chunks without consolidating their spaces. This
  enables future requests for chunks of the same size to be handled
  very quickly, but can increase fragmentation, and thus increase the
  overall memory footprint of a program.

  This malloc manages fastbins very conservatively yet still
  efficiently, so fragmentation is rarely a problem for values less
  than or equal to the default.  The maximum supported value of MXFAST
  is 80. You wouldn't want it any higher than this anyway.  Fastbins
  are designed especially for use with many small structs, objects or
  strings -- the default handles structs/objects/arrays with sizes up
  to 8 4byte fields, or small strings representing words, tokens,
  etc. Using fastbins for larger objects normally worsens
  fragmentation without improving speed.

  M_MXFAST is set in REQUEST size units. It is internally used in
  chunksize units, which adds padding and alignment.  You can reduce
  M_MXFAST to 0 to disable all use of fastbins.  This causes the malloc
  algorithm to be a closer approximation of fifo-best-fit in all cases,
  not just for larger requests, but will generally cause it to be
  slower.
*/


/* M_MXFAST is a standard SVID/XPG tuning option, usually listed in malloc.h */
#ifndef M_MXFAST
#define M_MXFAST            1
#endif

#ifndef DEFAULT_MXFAST
#define DEFAULT_MXFAST     (64 * SIZE_SZ / 4)
#endif


/*
  M_TRIM_THRESHOLD is the maximum amount of unused top-most memory
  to keep before releasing via malloc_trim in free().

  Automatic trimming is mainly useful in long-lived programs.
  Because trimming via sbrk can be slow on some systems, and can
  sometimes be wasteful (in cases where programs immediately
  afterward allocate more large chunks) the value should be high
  enough so that your overall system performance would improve by
  releasing this much memory.

  The trim threshold and the mmap control parameters (see below)
  can be traded off with one another. Trimming and mmapping are
  two different ways of releasing unused memory back to the
  system. Between these two, it is often possible to keep
  system-level demands of a long-lived program down to a bare
  minimum. For example, in one test suite of sessions measuring
  the XF86 X server on Linux, using a trim threshold of 128K and a
  mmap threshold of 192K led to near-minimal long term resource
  consumption.

  If you are using this malloc in a long-lived program, it should
  pay to experiment with these values.  As a rough guide, you
  might set to a value close to the average size of a process
  (program) running on your system.  Releasing this much memory
  would allow such a process to run in memory.  Generally, it's
  worth it to tune for trimming rather tham memory mapping when a
  program undergoes phases where several large chunks are
  allocated and released in ways that can reuse each other's
  storage, perhaps mixed with phases where there are no such
  chunks at all.  And in well-behaved long-lived programs,
  controlling release of large blocks via trimming versus mapping
  is usually faster.

  However, in most programs, these parameters serve mainly as
  protection against the system-level effects of carrying around
  massive amounts of unneeded memory. Since frequent calls to
  sbrk, mmap, and munmap otherwise degrade performance, the default
  parameters are set to relatively high values that serve only as
  safeguards.

  The trim value It must be greater than page size to have any useful
  effect.  To disable trimming completely, you can set to
  (unsigned long)(-1)

  Trim settings interact with fastbin (MXFAST) settings: Unless
  TRIM_FASTBINS is defined, automatic trimming never takes place upon
  freeing a chunk with size less than or equal to MXFAST. Trimming is
  instead delayed until subsequent freeing of larger chunks. However,
  you can still force an attempted trim by calling malloc_trim.

  Also, trimming is not generally possible in cases where
  the main arena is obtained via mmap.

  Note that the trick some people use of mallocing a huge space and
  then freeing it at program startup, in an attempt to reserve system
  memory, doesn't have the intended effect under automatic trimming,
  since that memory will immediately be returned to the system.
*/

#define M_TRIM_THRESHOLD       -1

#ifndef DEFAULT_TRIM_THRESHOLD
#define DEFAULT_TRIM_THRESHOLD (128 * 1024)
#endif

/*
  M_TOP_PAD is the amount of extra `padding' space to allocate or
  retain whenever sbrk is called. It is used in two ways internally:

  * When sbrk is called to extend the top of the arena to satisfy
  a new malloc request, this much padding is added to the sbrk
  request.

  * When malloc_trim is called automatically from free(),
  it is used as the `pad' argument.

  In both cases, the actual amount of padding is rounded
  so that the end of the arena is always a system page boundary.

  The main reason for using padding is to avoid calling sbrk so
  often. Having even a small pad greatly reduces the likelihood
  that nearly every malloc request during program start-up (or
  after trimming) will invoke sbrk, which needlessly wastes
  time.

  Automatic rounding-up to page-size units is normally sufficient
  to avoid measurable overhead, so the default is 0.  However, in
  systems where sbrk is relatively slow, it can pay to increase
  this value, at the expense of carrying around more memory than
  the program needs.
*/

#define M_TOP_PAD              -2

#ifndef DEFAULT_TOP_PAD
#define DEFAULT_TOP_PAD        (0)
#endif

/*
  MMAP_THRESHOLD_MAX and _MIN are the bounds on the dynamically
  adjusted MMAP_THRESHOLD.
*/

#ifndef DEFAULT_MMAP_THRESHOLD_MIN
#define DEFAULT_MMAP_THRESHOLD_MIN (128 * 1024)
#endif

#ifndef DEFAULT_MMAP_THRESHOLD_MAX
  /* For 32-bit platforms we cannot increase the maximum mmap
     threshold much because it is also the minimum value for the
     maximum heap size and its alignment.  Going above 512k (i.e., 1M
     for new heaps) wastes too much address space.  */
# if __WORDSIZE == 32
#  define DEFAULT_MMAP_THRESHOLD_MAX (512 * 1024)
# else
#  define DEFAULT_MMAP_THRESHOLD_MAX (4 * 1024 * 1024 * sizeof(long))
# endif
#endif

/*
  M_MMAP_THRESHOLD is the request size threshold for using mmap()
  to service a request. Requests of at least this size that cannot
  be allocated using already-existing space will be serviced via mmap.
  (If enough normal freed space already exists it is used instead.)

  Using mmap segregates relatively large chunks of memory so that
  they can be individually obtained and released from the host
  system. A request serviced through mmap is never reused by any
  other request (at least not directly; the system may just so
  happen to remap successive requests to the same locations).

  Segregating space in this way has the benefits that:

   1. Mmapped space can ALWAYS be individually released back
      to the system, which helps keep the system level memory
      demands of a long-lived program low.
   2. Mapped memory can never become `locked' between
      other chunks, as can happen with normally allocated chunks, which
      means that even trimming via malloc_trim would not release them.
   3. On some systems with "holes" in address spaces, mmap can obtain
      memory that sbrk cannot.

  However, it has the disadvantages that:

   1. The space cannot be reclaimed, consolidated, and then
      used to service later requests, as happens with normal chunks.
   2. It can lead to more wastage because of mmap page alignment
      requirements
   3. It causes malloc performance to be more dependent on host
      system memory management support routines which may vary in
      implementation quality and may impose arbitrary
      limitations. Generally, servicing a request via normal
      malloc steps is faster than going through a system's mmap.

  The advantages of mmap nearly always outweigh disadvantages for
  "large" chunks, but the value of "large" varies across systems.  The
  default is an empirically derived value that works well in most
  systems.


  Update in 2006:
  The above was written in 2001. Since then the world has changed a lot.
  Memory got bigger. Applications got bigger. The virtual address space
  layout in 32 bit linux changed.

  In the new situation, brk() and mmap space is shared and there are no
  artificial limits on brk size imposed by the kernel. What is more,
  applications have started using transient allocations larger than the
  128Kb as was imagined in 2001.

  The price for mmap is also high now; each time glibc mmaps from the
  kernel, the kernel is forced to zero out the memory it gives to the
  application. Zeroing memory is expensive and eats a lot of cache and
  memory bandwidth. This has nothing to do with the efficiency of the
  virtual memory system, by doing mmap the kernel just has no choice but
  to zero.

  In 2001, the kernel had a maximum size for brk() which was about 800
  megabytes on 32 bit x86, at that point brk() would hit the first
  mmaped shared libaries and couldn't expand anymore. With current 2.6
  kernels, the VA space layout is different and brk() and mmap
  both can span the entire heap at will.

  Rather than using a static threshold for the brk/mmap tradeoff,
  we are now using a simple dynamic one. The goal is still to avoid
  fragmentation. The old goals we kept are
  1) try to get the long lived large allocations to use mmap()
  2) really large allocations should always use mmap()
  and we're adding now:
  3) transient allocations should use brk() to avoid forcing the kernel
     having to zero memory over and over again

  The implementation works with a sliding threshold, which is by default
  limited to go between 128Kb and 32Mb (64Mb for 64 bitmachines) and starts
  out at 128Kb as per the 2001 default.

  This allows us to satisfy requirement 1) under the assumption that long
  lived allocations are made early in the process' lifespan, before it has
  started doing dynamic allocations of the same size (which will
  increase the threshold).

  The upperbound on the threshold satisfies requirement 2)

  The threshold goes up in value when the application frees memory that was
  allocated with the mmap allocator. The idea is that once the application
  starts freeing memory of a certain size, it's highly probable that this is
  a size the application uses for transient allocations. This estimator
  is there to satisfy the new third requirement.

*/

#define M_MMAP_THRESHOLD      -3
/*
#ifndef DEFAULT_MMAP_THRESHOLD_MIN
#define DEFAULT_MMAP_THRESHOLD_MIN (128 * 1024)
#endif
*/
#ifndef DEFAULT_MMAP_THRESHOLD
#define DEFAULT_MMAP_THRESHOLD DEFAULT_MMAP_THRESHOLD_MIN
#endif

/*
  M_MMAP_MAX is the maximum number of requests to simultaneously
  service using mmap. This parameter exists because
  some systems have a limited number of internal tables for
  use by mmap, and using more than a few of them may degrade
  performance.

  The default is set to a value that serves only as a safeguard.
  Setting to 0 disables use of mmap for servicing large requests.
*/

#define M_MMAP_MAX             -4

#ifndef DEFAULT_MMAP_MAX
#define DEFAULT_MMAP_MAX       (65536)
#endif

#include <malloc.h>

#ifndef RETURN_ADDRESS
#define RETURN_ADDRESS(X_) (NULL)
#endif

/* Forward declarations.  */
struct malloc_chunk;
typedef struct malloc_chunk* mchunkptr;

/* Internal routines.  */

static void*  _int_malloc(mstate, size_t);
static void     _int_free(mstate, mchunkptr, int);
static void*  _int_realloc(mstate, mchunkptr, INTERNAL_SIZE_T,
			   INTERNAL_SIZE_T);
static void*  _int_memalign(mstate, size_t, size_t);
static void*  _mid_memalign(size_t, size_t, void *);

static void malloc_printerr(const char *str) __attribute__ ((noreturn));

static void* mem2mem_check(void *p, size_t sz);
static void top_check(void);
static void munmap_chunk(mchunkptr p);
#if HAVE_MREMAP
static mchunkptr mremap_chunk(mchunkptr p, size_t new_size);
#endif

static void*   malloc_check(size_t sz, const void *caller);
static void      free_check(void* mem, const void *caller);
static void*   realloc_check(void* oldmem, size_t bytes,
			       const void *caller);
static void*   memalign_check(size_t alignment, size_t bytes,
				const void *caller);

/* ------------------ MMAP support ------------------  */


#include <fcntl.h>
#include <sys/mman.h>

#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
# define MAP_ANONYMOUS MAP_ANON
#endif

#ifndef MAP_NORESERVE
# define MAP_NORESERVE 0
#endif

#define MMAP(addr, size, prot, flags) \
 __mmap((addr), (size), (prot), (flags)|MAP_ANONYMOUS|MAP_PRIVATE, -1, 0)


/*
  -----------------------  Chunk representations -----------------------
*/


/*
  This struct declaration is misleading (but accurate and necessary).
  It declares a "view" into memory allowing access to necessary
  fields at known offsets from a given base. See explanation below.
*/

struct malloc_chunk {

  INTERNAL_SIZE_T      mchunk_prev_size;  /* Size of previous chunk (if free).  前一chunk如果是空闲，则表示它的大小，否则被其用来存储数据*/
  INTERNAL_SIZE_T      mchunk_size;       /* Size in bytes, including overhead. 当前块的大小 size的对其为0x10  后三位为AWP标志位 */

  struct malloc_chunk* fd;         /* double links -- used only if free. 前向指针*/
  struct malloc_chunk* bk;        /*后向指针*/
  /*最小的chunk大小必须包含前四个域*/
  /* Only used for large blocks: pointer to next larger size.  */
  /*这两个结构在large block中使用。一般情况下不考虑*/
  struct malloc_chunk* fd_nextsize; /* double links -- used only if free. */
  struct malloc_chunk* bk_nextsize;
};


/*
   malloc_chunk details:

    (The following includes lightly edited explanations by Colin Plumb.)

    Chunks of memory are maintained using a `boundary tag' method as
    described in e.g., Knuth or Standish.  (See the paper by Paul
    Wilson ftp://ftp.cs.utexas.edu/pub/garbage/allocsrv.ps for a
    survey of such techniques.)  Sizes of free chunks are stored both
    in the front of each chunk and at the end.  This makes
    consolidating fragmented chunks into bigger chunks very fast.  The
    size fields also hold bits representing whether chunks are free or
    in use.

    An allocated chunk looks like this:
    chunk的大概存在形式

    chunk-> +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	    |             Size of previous chunk, if unallocated (P clear)  |
	    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	    |             Size of chunk, in bytes                     |A|M|P|
      mem-> +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	    |             User data starts here...                          .
	    .                                                               .
	    .             (malloc_usable_size() bytes)                      .
	    .                                                               |
nextchunk-> +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	    |             (size of chunk, but used for application data)    |
	    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	    |             Size of next chunk, in bytes                |A|0|1|
	    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

    Where "chunk" is the front of the chunk for the purpose of most of
    the malloc code, but "mem" is the pointer that is returned to the
    user.  "Nextchunk" is the beginning of the next contiguous chunk.

    Chunks always begin on even word boundaries, so the mem portion
    (which is returned to the user) is also on an even word boundary, and
    thus at least double-word aligned.

    Free chunks are stored in circular doubly-linked lists, and look like this:

    chunk-> +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	    |             Size of previous chunk, if unallocated (P clear)  |
	    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    `head:' |             Size of chunk, in bytes                     |A|0|P|
      mem-> +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	    |             Forward pointer to next chunk in list             |
	    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	    |             Back pointer to previous chunk in list            |
	    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	    |             Unused space (may be 0 bytes long)                .
	    .                                                               .
	    .                                                               |
nextchunk-> +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    `foot:' |             Size of chunk, in bytes                           |
	    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	    |             Size of next chunk, in bytes                |A|0|0|
	    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

    The P (PREV_INUSE) bit, stored in the unused low-order bit of the
    chunk size (which is always a multiple of two words), is an in-use
    bit for the *previous* chunk.  If that bit is *clear*, then the
    word before the current chunk size contains the previous chunk
    size, and can be used to find the front of the previous chunk.
    The very first chunk allocated always has this bit set,
    preventing access to non-existent (or non-owned) memory. If
    prev_inuse is set for any given chunk, then you CANNOT determine
    the size of the previous chunk, and might even get a memory
    addressing fault when trying to do so.

    The A (NON_MAIN_ARENA) bit is cleared for chunks on the initial,
    main arena, described by the main_arena variable.  When additional
    threads are spawned, each thread receives its own arena (up to a
    configurable limit, after which arenas are reused for multiple
    threads), and the chunks in these arenas have the A bit set.  To
    find the arena for a chunk on such a non-main arena, heap_for_ptr
    performs a bit mask operation and indirection through the ar_ptr
    member of the per-heap header heap_info (see arena.c).

    Note that the `foot' of the current chunk is actually represented
    as the prev_size of the NEXT chunk. This makes it easier to
    deal with alignments etc but can be very confusing when trying
    to extend or adapt this code.

    The three exceptions to all this are:

     1. The special chunk `top' doesn't bother using the
	trailing size field since there is no next contiguous chunk
	that would have to index off it. After initialization, `top'
	is forced to always exist.  If it would become less than
	MINSIZE bytes long, it is replenished.

     2. Chunks allocated via mmap, which have the second-lowest-order
	bit M (IS_MMAPPED) set in their size fields.  Because they are
	allocated one-by-one, each must contain its own trailing size
	field.  If the M bit is set, the other bits are ignored
	(because mmapped chunks are neither in an arena, nor adjacent
	to a freed chunk).  The M bit is also used for chunks which
	originally came from a dumped heap via malloc_set_state in
	hooks.c.

     3. Chunks in fastbins are treated as allocated chunks from the
	point of view of the chunk allocator.  They are consolidated
	with their neighbors only in bulk, in malloc_consolidate.
*/

/*
  ---------- Size and alignment checks and conversions ----------
*/

/* conversion from malloc headers to user pointers, and back */
/*将chunk指针转为mem指针以及将mem指针转为chunk指针*/
#define chunk2mem(p)   ((void*)((char*)(p) + 2*SIZE_SZ))
#define mem2chunk(mem) ((mchunkptr)((char*)(mem) - 2*SIZE_SZ))

/* The smallest possible chunk */
/*定义了最小的chunk，这里可以看到最小的chunk大小为chunk到fd_nextsize偏移
 *所以最小的chunk必须有prev_size size fd bk 四个部分
 */
#define MIN_CHUNK_SIZE        (offsetof(struct malloc_chunk, fd_nextsize))

/* The smallest size we can malloc is an aligned minimal chunk */

#define MINSIZE  \
  (unsigned long)(((MIN_CHUNK_SIZE+MALLOC_ALIGN_MASK) & ~MALLOC_ALIGN_MASK))

/* Check if m has acceptable alignment */
/*检查分配内存对齐情况   默认2 * SIZE_SZ对齐(前面注释有说明，也可以自行设置) */
#define aligned_OK(m)  (((unsigned long)(m) & MALLOC_ALIGN_MASK) == 0)

#define misaligned_chunk(p) \
  ((uintptr_t)(MALLOC_ALIGNMENT == 2 * SIZE_SZ ? (p) : chunk2mem (p)) \
   & MALLOC_ALIGN_MASK)

/* pad request bytes into a usable size -- internal version */

/*处理用户请求大小  转为实际分配内存大小*/
#define request2size(req)                                         \
  (((req) + SIZE_SZ + MALLOC_ALIGN_MASK < MINSIZE)  ?             \
   MINSIZE :                                                      \
   ((req) + SIZE_SZ + MALLOC_ALIGN_MASK) & ~MALLOC_ALIGN_MASK)

/* Check if REQ overflows when padded and aligned and if the resulting value
   is less than PTRDIFF_T.  Returns TRUE and the requested size or MINSIZE in
   case the value is less than MINSIZE on SZ or false if any of the previous
   check fail.  */
static inline bool
checked_request2size (size_t req, size_t *sz) __nonnull (1)
{
  if (__glibc_unlikely (req > PTRDIFF_MAX))
    return false;
  *sz = request2size (req);
  return true;
}

/*
   --------------- Physical chunk operations ---------------
 */

/*下面的宏用来获取chunk的标志位信息*/
/* size field is or'ed with PREV_INUSE when previous adjacent chunk in use */
#define PREV_INUSE 0x1

/* extract inuse bit of previous chunk */
/*获取chunk的prev_inuse位*/
#define prev_inuse(p)       ((p)->mchunk_size & PREV_INUSE)


/* size field is or'ed with IS_MMAPPED if the chunk was obtained with mmap() */
#define IS_MMAPPED 0x2

/* check for mmap()'ed chunk */
#define chunk_is_mmapped(p) ((p)->mchunk_size & IS_MMAPPED)


/* size field is or'ed with NON_MAIN_ARENA if the chunk was obtained
   from a non-main arena.  This is only set immediately before handing
   the chunk to the user, if necessary.  */
#define NON_MAIN_ARENA 0x4

/* Check for chunk from main arena.  */
#define chunk_main_arena(p) (((p)->mchunk_size & NON_MAIN_ARENA) == 0)

/* Mark a chunk as not being on the main arena.  */
#define set_non_main_arena(p) ((p)->mchunk_size |= NON_MAIN_ARENA)


/*
   Bits to mask off when extracting size

   Note: IS_MMAPPED is intentionally not masked off from size field in
   macros for which mmapped chunks should never be seen. This should
   cause helpful core dumps to occur if it is tried by accident by
   people extending or adapting this malloc.
 */
#define SIZE_BITS (PREV_INUSE | IS_MMAPPED | NON_MAIN_ARENA)

/* Get size, ignoring use bits */
/*获取chunk的大小  去掉标志位影响*/
#define chunksize(p) (chunksize_nomask (p) & ~(SIZE_BITS))

/* Like chunksize, but do not mask SIZE_BITS.  */
#define chunksize_nomask(p)         ((p)->mchunk_size)

/* Ptr to next physical malloc_chunk. 用当前chunk指针加上size得到下一个chunk的指针*/
#define next_chunk(p) ((mchunkptr) (((char *) (p)) + chunksize (p)))
/*下面两个分别是获取prev_size与设置prev_size*/
/* Size of the chunk below P.  Only valid if !prev_inuse (P).  */
#define prev_size(p) ((p)->mchunk_prev_size)

/* Set the size of the chunk below P.  Only valid if !prev_inuse (P).  */
#define set_prev_size(p, sz) ((p)->mchunk_prev_size = (sz))
/*使用prev_size与当前chunk的指针获取上一个chunk的指针*/
/* Ptr to previous physical malloc_chunk.  Only valid if !prev_inuse (P).  */
#define prev_chunk(p) ((mchunkptr) (((char *) (p)) - prev_size (p)))

/* Treat space at ptr + offset as a chunk */
/*指定偏移的chunk*/
#define chunk_at_offset(p, s)  ((mchunkptr) (((char *) (p)) + (s)))
/*当前chunk的use情况 当前chunk指针+chunksize获得下一个的chunk，然后通过其prev_inuse得到*/
/* extract p's inuse bit */
#define inuse(p)							      \
  ((((mchunkptr) (((char *) (p)) + chunksize (p)))->mchunk_size) & PREV_INUSE)

/* set/clear chunk as being inuse without otherwise disturbing */
#define set_inuse(p)							      \
  ((mchunkptr) (((char *) (p)) + chunksize (p)))->mchunk_size |= PREV_INUSE

#define clear_inuse(p)							      \
  ((mchunkptr) (((char *) (p)) + chunksize (p)))->mchunk_size &= ~(PREV_INUSE)

/*指定偏移的chunk的prev_inuse相关操作*/
/* check/set/clear inuse bits in known places */
#define inuse_bit_at_offset(p, s)					      \
  (((mchunkptr) (((char *) (p)) + (s)))->mchunk_size & PREV_INUSE)

#define set_inuse_bit_at_offset(p, s)					      \
  (((mchunkptr) (((char *) (p)) + (s)))->mchunk_size |= PREV_INUSE)

#define clear_inuse_bit_at_offset(p, s)					      \
  (((mchunkptr) (((char *) (p)) + (s)))->mchunk_size &= ~(PREV_INUSE))

/*设置chunk的size  不改变其标志位*/
/* Set size at head, without disturbing its use bit */
#define set_head_size(p, s)  ((p)->mchunk_size = (((p)->mchunk_size & SIZE_BITS) | (s)))

/* Set size/use field */
#define set_head(p, s)       ((p)->mchunk_size = (s))

/* Set size at footer (only when chunk is not in use) */
#define set_foot(p, s)       (((mchunkptr) ((char *) (p) + (s)))->mchunk_prev_size = (s))

/*这两个很少提及 先忽略一下*/
#pragma GCC poison mchunk_size
#pragma GCC poison mchunk_prev_size

/*
   -------------------- Internal data structures --------------------

   All internal state is held in an instance of malloc_state defined
   below. There are no other static variables, except in two optional
   cases:
 * If USE_MALLOC_LOCK is defined, the mALLOC_MUTEx declared above.
 * If mmap doesn't support MAP_ANONYMOUS, a dummy file descriptor
     for mmap.

   Beware of lots of tricks that minimize the total bookkeeping space
   requirements. The result is a little over 1K bytes (for 4byte
   pointers and size_t.)
 */

/*
   Bins

    An array of bin headers for free chunks. Each bin is doubly
    linked.  The bins are approximately proportionally (log) spaced.
    There are a lot of these bins (128). This may look excessive, but
    works very well in practice.  Most bins hold sizes that are
    unusual as malloc request sizes, but are more usual for fragments
    and consolidated sets of chunks, which is what these bins hold, so
    they can be found quickly.  All procedures maintain the invariant
    that no consolidated chunk physically borders another one, so each
    chunk in a list is known to be preceeded and followed by either
    inuse chunks or the ends of memory.

    Chunks in bins are kept in size order, with ties going to the
    approximately least recently used chunk. Ordering isn't needed
    for the small bins, which all contain the same-sized chunks, but
    facilitates best-fit allocation for larger chunks. These lists
    are just sequential. Keeping them in order almost never requires
    enough traversal to warrant using fancier ordered data
    structures.
    Chunks of the same size are linked with the most
    recently freed at the front, and allocations are taken from the
    back.  This results in LRU (FIFO) allocation order, which tends
    to give each chunk an equal opportunity to be consolidated with
    adjacent freed chunks, resulting in larger free chunks and less
    fragmentation.

    To simplify use in double-linked lists, each bin header acts
    as a malloc_chunk. This avoids special-casing for headers.
    But to conserve space and improve locality, we allocate
    only the fd/bk pointers of bins, and then use repositioning tricks
    to treat these as the fields of a malloc_chunk*.
    
    这部分主要介绍Bins

    共有128个bin  每个都是双链表   也就是说malloc_state中的bin数组有256个bin指针[0-255] 
    两个一组  其中bin[0]不存在  bin[1]存放的unsorted bin 单链表

    每组small bin大小是相同的 只有每组large bin是在一定范围内从大到小排列的

    遵循FIFO   先进先出

    为了简化双链表使用，每个bin的头部都和malloc_chunk行为一致 但是节省空间只申请了fd和bk  通过技巧将其当作malloc_chunk*
 */

typedef struct malloc_chunk *mbinptr;
/*bin[0]是不存在的*/
/* addressing -- note that bin_at(0) does not exist */
/*获取某个bin的地址
 *传入malloc_state  与bin的索引
 *通过索引找到对应的bin 然后减去前面的偏移即可 将其当作malloc_chunk 
 */
#define bin_at(m, i) \
  (mbinptr) (((char *) &((m)->bins[((i) - 1) * 2]))			      \
             - offsetof (struct malloc_chunk, fd))

/* analog of ++bin */
/*下一个bin地址  当前bin地址 + 指针大小*2*/
#define next_bin(b)  ((mbinptr) ((char *) (b) + (sizeof (mchunkptr) << 1)))

/* Reminders about list directionality within bins */
/*双链表体现*/
/*这两个指针可以用来变脸整个bin的chunk链*/
#define first(b)     ((b)->fd)  /*获取头部的chunk*/
#define last(b)      ((b)->bk)  /*获取链表尾部的chunk*/

/*
   Indexing
    
    Bins for sizes < 512 bytes contain chunks of all the same size, spaced
    8 bytes apart. Larger bins are approximately logarithmically spaced:

    64 bins of size       8  small bin的组间距（32位下为8bit 64位下为6bit）
    32 bins of size      64  large bin每个bin内存放的是一定范围的chunk  这里数值表示的是这个范围  也就是组内间距0x40
    16 bins of size     512    0x200
     8 bins of size    4096    0x1000
     4 bins of size   32768    0x8000
     2 bins of size  262144    0x40000
     1 bin  of size what's left  剩下的？？

    There is actually a little bit of slop in the numbers in bin_index
    for the sake of speed. This makes no difference elsewhere.

    The bins top out around 1MB because we expect to service large
    requests via mmap.

    Bin 0 does not exist.  Bin 1 is the unordered list; if that would be
    a valid chunk size the small bins are bumped up one.
    unsorted bin  1个  bin[1]
    small bin 有62个 bin[2]bin[3]...bin[124]bin[125]组内chunk大小相同，组间距8字节
    large bin 有63个 bin[126]bin[127]  ... bin[250]bin[251]   

    这段咋总感觉不太对  少一个bin啊  第127个bin是干啥的 bin[252]bin[253]好像没有作用?
 */

#define NBINS             128 /*bin的总数  unsorted bin(1)+smallbin(62)+largebin(63)+1 */
#define NSMALLBINS         64/*small bin  本来是只有62个small bin，64方便计算？*/
#define SMALLBIN_WIDTH    MALLOC_ALIGNMENT
#define SMALLBIN_CORRECTION (MALLOC_ALIGNMENT > 2 * SIZE_SZ)/*针对small bin的矫正*/
#define MIN_LARGE_SIZE    ((NSMALLBINS - SMALLBIN_CORRECTION) * SMALLBIN_WIDTH)/*因为每个small bin以SMALLBIN_WIDTH等差递增 所以这样得到最小的large bin?*/
/*判断是否在small bin大小中*/
#define in_smallbin_range(sz)  \
  ((unsigned long) (sz) < (unsigned long) MIN_LARGE_SIZE)
/*判断当前的size在哪一组small bin*/
#define smallbin_index(sz) \
  ((SMALLBIN_WIDTH == 16 ? (((unsigned) (sz)) >> 4) : (((unsigned) (sz)) >> 3))\
   + SMALLBIN_CORRECTION)
/*计算size所在large bin组数的宏 没啥好研究的 */
#define largebin_index_32(sz)                                                \
  (((((unsigned long) (sz)) >> 6) <= 38) ?  56 + (((unsigned long) (sz)) >> 6) :\
   ((((unsigned long) (sz)) >> 9) <= 20) ?  91 + (((unsigned long) (sz)) >> 9) :\
   ((((unsigned long) (sz)) >> 12) <= 10) ? 110 + (((unsigned long) (sz)) >> 12) :\
   ((((unsigned long) (sz)) >> 15) <= 4) ? 119 + (((unsigned long) (sz)) >> 15) :\
   ((((unsigned long) (sz)) >> 18) <= 2) ? 124 + (((unsigned long) (sz)) >> 18) :\
   126)

#define largebin_index_32_big(sz)                                            \
  (((((unsigned long) (sz)) >> 6) <= 45) ?  49 + (((unsigned long) (sz)) >> 6) :\
   ((((unsigned long) (sz)) >> 9) <= 20) ?  91 + (((unsigned long) (sz)) >> 9) :\
   ((((unsigned long) (sz)) >> 12) <= 10) ? 110 + (((unsigned long) (sz)) >> 12) :\
   ((((unsigned long) (sz)) >> 15) <= 4) ? 119 + (((unsigned long) (sz)) >> 15) :\
   ((((unsigned long) (sz)) >> 18) <= 2) ? 124 + (((unsigned long) (sz)) >> 18) :\
   126)

// XXX It remains to be seen whether it is good to keep the widths of
// XXX the buckets the same or whether it should be scaled by a factor
// XXX of two as well.
#define largebin_index_64(sz)                                                \
  (((((unsigned long) (sz)) >> 6) <= 48) ?  48 + (((unsigned long) (sz)) >> 6) :\
   ((((unsigned long) (sz)) >> 9) <= 20) ?  91 + (((unsigned long) (sz)) >> 9) :\
   ((((unsigned long) (sz)) >> 12) <= 10) ? 110 + (((unsigned long) (sz)) >> 12) :\
   ((((unsigned long) (sz)) >> 15) <= 4) ? 119 + (((unsigned long) (sz)) >> 15) :\
   ((((unsigned long) (sz)) >> 18) <= 2) ? 124 + (((unsigned long) (sz)) >> 18) :\
   126)

#define largebin_index(sz) \
  (SIZE_SZ == 8 ? largebin_index_64 (sz)                                     \
   : MALLOC_ALIGNMENT == 16 ? largebin_index_32_big (sz)                     \
   : largebin_index_32 (sz))

#define bin_index(sz) \
  ((in_smallbin_range (sz)) ? smallbin_index (sz) : largebin_index (sz))

/* Take a chunk off a bin list.  */
/*将chunk p 从bin的链表拆下*/
/*需要注意的
 *
 *(1)其实它并没改变p本身的fd和bk  可用来泄露堆地址或libc地址
 *(2)使用情况
 * 根据ctf-wiki所述，使用场景有4
 *  malloc             从恰好大小合适的 large bin 中获取 chunk/从比请求的 chunk 所在的 bin 大的 bin 中取 chunk
 *  free               后向合并，合并物理相邻低地址空闲 chunk/前向合并，合并物理相邻高地址空闲 chunk(除了 top chunk)
 *  malloc_consolidate 后向合并，合并物理相邻低地址空闲 chunk/前向合并，合并物理相邻高地址空闲 chunk（除了 top chunk）
 *  realloc            前向扩展，合并物理相邻高地址空闲 chunk（除了 top chunk）
 **/
static void
unlink_chunk (mstate av, mchunkptr p)
{
  /*检查了当前的size和下一个chunk中储存的prev_size是否对的上*/
  if (chunksize (p) != prev_size (next_chunk (p)))
    /*glibc malloc 时检测到错误的时候，会调用 malloc_printerr */
    malloc_printerr ("corrupted size vs. prev_size");

  mchunkptr fd = p->fd;
  mchunkptr bk = p->bk;
  /*检查双链表是不是正确的， 防止简单篡改空闲的 chunk 的 fd 与 bk 来实现任意写的效果*/
  if (__builtin_expect (fd->bk != p || bk->fd != p, 0))
    malloc_printerr ("corrupted double-linked list");

  fd->bk = bk;
  bk->fd = fd;
  /*不在small bin中 就是说碰到前面提到的large block的情况 需要判断chunk的后两个字段  就是fd_nextsize 和bk_nextsize*/
  /*如果P->fd_nextsize为NULL，表明P未插入到nextsize链表中。*/

  if (!in_smallbin_range (chunksize_nomask (p)) && p->fd_nextsize != NULL)
    {
      /*这个检查和前面的chunk检查类似*/
      if (p->fd_nextsize->bk_nextsize != p
	  || p->bk_nextsize->fd_nextsize != p)
	malloc_printerr ("corrupted double-linked list (not small)");
      /*这说明fd不在nextsize链表*/
      if (fd->fd_nextsize == NULL)
	{
    /*nextsize双链表是否只有P本身*/
	  if (p->fd_nextsize == p)
      /*就将p取走*/
	    fd->fd_nextsize = fd->bk_nextsize = fd;
	  else
	    {
        /*不止p一个 就将FD插入到 nextsize双链表*/
	      fd->fd_nextsize = p->fd_nextsize;
	      fd->bk_nextsize = p->bk_nextsize;
	      p->fd_nextsize->bk_nextsize = fd;
	      p->bk_nextsize->fd_nextsize = fd;
	    }
	}
      else/*fd在nextsize链表，直接取p  也就是拆双链表*/
	{
	  p->fd_nextsize->bk_nextsize = p->bk_nextsize;
	  p->bk_nextsize->fd_nextsize = p->fd_nextsize;
	}
    }
}

/*
   Unsorted chunks

    All remainders from chunk splits, as well as all returned chunks,
    are first placed in the "unsorted" bin. They are then placed
    in regular bins after malloc gives them ONE chance to be used before
    binning. So, basically, the unsorted_chunks list acts as a queue,
    with chunks being placed on it in free (and malloc_consolidate),
    and taken off (to be either used or placed in bins) in malloc.

    The NON_MAIN_ARENA flag is never set for unsorted chunks, so it
    does not have to be taken into account in size comparisons.

    unsorted bin就类似一个垃圾箱，位于bin[1]  是一个单链表,使用时采用先进先出FIFO
    所有分配后剩余的或者回收的都会到unsorted bin称为unsorted chunk，主要是两类：
    (1)当一个较大的 chunk 被分割成两半后，如果剩下的部分大于 MINSIZE，就会被放到 unsorted bin 中。
    (2)释放一个不属于 fastbin 的 chunk，并且该 chunk 不和 top chunk 紧邻时，该 chunk 会被首先放到 unsorted bin 中。

    malloc的时候会有重新归类的机会
 */

/* The otherwise unindexable 1-bin is used to hold unsorted chunks. */
/*获取当前malloc_state的unsorted bin头部  就是bin[1]*/
#define unsorted_chunks(M)          (bin_at (M, 1))

/*
   Top

    The top-most available chunk (i.e., the one bordering the end of
    available memory) is treated specially. It is never included in
    any bin, is used only if no other chunk is available, and is
    released back to the system if it is very large (see
    M_TRIM_THRESHOLD).  Because top initially
    points to its own bin with initial zero size, thus forcing
    extension on the first malloc request, we avoid having any special
    code in malloc to check whether it even exists yet. But we still
    need to do so when getting memory from system, so we make
    initial_top treat the bin as a legal but unusable chunk during the
    interval between initialization and the first call to
    sysmalloc. (This is somewhat delicate, since it relies on
    the 2 preceding words to be zero during this interval as well.)

    TOP chunk不包含在任何bin中，只有在没有任何chunk可用是才会用到；
    如果很大的话会被释放回系统
    因为top最初指向初始大小为零的自己的bin，从而在第一个malloc请求时强制扩展，所以避免在malloc中使用任何特殊代码来检查它是否还存在(这段啥意思?)

    在初始化和首次调用sysmalloc之间的时间间隔内，使用initial_top将bin视为合法但无法使用的块

    top chunk 的 prev_inuse 比特位始终为 1，否则其前面的 chunk 就会被合并到 top chunk 中。
 */

/* Conveniently, the unsorted bin can be used as dummy top on first call */
/*初始时，使用unsorted chunk作为top_chunk*/
#define initial_top(M)              (unsorted_chunks (M))

/*
   Binmap

    To help compensate for the large number of bins, a one-level index
    structure is used for bin-by-bin searching.  `binmap' is a
    bitvector recording whether bins are definitely empty so they can
    be skipped over during during traversals.  The bits are NOT always
    cleared as soon as bins are empty, but instead only
    when they are noticed to be empty during traversal in malloc.

    binmap是一个位向量，用于记录bin是否绝对为空，以便在遍历期间可以将其跳过。 
    bin不为空时，并不总是立即清除这些位，而是仅当在malloc中遍历时发现它们为空时才清除

 */

/* Conservatively use 32 bits per map word, even if on 64bit system */
/*每个map设置为32位 可以记录32个bin*/
#define BINMAPSHIFT      5
#define BITSPERMAP       (1U << BINMAPSHIFT)/*0x100000  -> 32*/
#define BINMAPSIZE       (NBINS / BITSPERMAP)/*总共需要  bin总数/32  这么多个map*/
/*下面是从binmap中获取某个bin是否为空的相关操作*/
#define idx2block(i)     ((i) >> BINMAPSHIFT)
#define idx2bit(i)       ((1U << ((i) & ((1U << BINMAPSHIFT) - 1))))

#define mark_bin(m, i)    ((m)->binmap[idx2block (i)] |= idx2bit (i))
#define unmark_bin(m, i)  ((m)->binmap[idx2block (i)] &= ~(idx2bit (i)))
#define get_binmap(m, i)  ((m)->binmap[idx2block (i)] & idx2bit (i))

/*
   Fastbins

    An array of lists holding recently freed small chunks.  Fastbins
    are not doubly linked.  It is faster to single-link them, and
    since chunks are never removed from the middles of these lists,
    double linking is not necessary. Also, unlike regular bins, they
    are not even processed in FIFO order (they use faster LIFO) since
    ordering doesn't much matter in the transient contexts in which
    fastbins are normally used.

    Chunks in fastbins keep their inuse bit set, so they cannot
    be consolidated with other free chunks. malloc_consolidate
    releases all chunks in fastbins and consolidates them with
    other free chunks.

    关于fastbins
    fastbin是保存最近被free的small chunk的单链表 (因为不会从链表中间移除chunk  以及速度)
    相比前面的small bin与large bin  ，fastbins采用的是LIFO(后进先出  使得更加适合于局部性，考虑速度就完事了)
    fastbin中的chunk inuse位永远处于set状态，防止他们跟其他chunk合并

    malloc_consolidate会释放掉fastbin中所有的chunks 并且将他们与其他free的chunk合并  这个在malloc工作流程中会看到
    fastbin也是有多组的，每组是chunk大小一样的单链表 头部都存放在malloc_state的fastbinsY中
 */

typedef struct malloc_chunk *mfastbinptr;
/*取malloc_state中对应索引的fastbin头部*/
#define fastbin(ar_ptr, idx) ((ar_ptr)->fastbinsY[idx])

/* offset 2 to use otherwise unindexable first 2 bins */
/*这是对fastbin index的计算 malloc会进行fastbin的size检查，进行fastbin attack时需要保证chunk的size合法  得到的index在正确范围*/
#define fastbin_index(sz) \
  ((((unsigned int) (sz)) >> (SIZE_SZ == 8 ? 4 : 3)) - 2)


/* The maximum fastbin request size we support */
/*支持的最大fastbin大小*/
#define MAX_FAST_SIZE     (80 * SIZE_SZ / 4)

/*fastbin数量*/
#define NFASTBINS  (fastbin_index (request2size (MAX_FAST_SIZE)) + 1)

/*
   FASTBIN_CONSOLIDATION_THRESHOLD is the size of a chunk in free()
   that triggers automatic consolidation of possibly-surrounding
   fastbin chunks. This is a heuristic, so the exact value should not
   matter too much. It is defined at half the default trim threshold as a
   compromise heuristic to only attempt consolidation if it is likely
   to lead to trimming. However, it is not dynamically tunable, since
   consolidation reduces fragmentation surrounding large chunks even
   if trimming is not used.

   当释放的 chunk 与该 chunk 相邻的空闲 chunk 合并后的大小大于 FASTBIN_CONSOLIDATION_THRESHOLD 时，内存碎片可能比较多了，
   就需要把 fast bins 中的 chunk 都进行合并，以减少内存碎片对系统的影响
 */

#define FASTBIN_CONSOLIDATION_THRESHOLD  (65536UL)

/*
   NONCONTIGUOUS_BIT indicates that MORECORE does not return contiguous
   regions.  Otherwise, contiguity is exploited in merging together,
   when possible, results from consecutive MORECORE calls.

   The initial value comes from MORECORE_CONTIGUOUS, but is
   changed dynamically if mmap is ever used as an sbrk substitute.
 */

#define NONCONTIGUOUS_BIT     (2U)

#define contiguous(M)          (((M)->flags & NONCONTIGUOUS_BIT) == 0)
#define noncontiguous(M)       (((M)->flags & NONCONTIGUOUS_BIT) != 0)
#define set_noncontiguous(M)   ((M)->flags |= NONCONTIGUOUS_BIT)
#define set_contiguous(M)      ((M)->flags &= ~NONCONTIGUOUS_BIT)

/* Maximum size of memory handled in fastbins.  */
/*Fastbin中处理的最大内存大小的相关操作  这部分对于堆溢出好像没啥影响 先不细看了*/
static INTERNAL_SIZE_T global_max_fast;

/*
   Set value of max_fast.
   Use impossibly small value if 0.
   Precondition: there are no existing fastbin chunks in the main arena.
   Since do_check_malloc_state () checks this, we call malloc_consolidate ()
   before changing max_fast.  Note other arenas will leak their fast bin
   entries if max_fast is reduced.
 */

#define set_max_fast(s) \
  global_max_fast = (((s) == 0)						      \
                     ? MIN_CHUNK_SIZE / 2 : ((s + SIZE_SZ) & ~MALLOC_ALIGN_MASK))

static inline INTERNAL_SIZE_T
get_max_fast (void)
{
  /* Tell the GCC optimizers that global_max_fast is never larger
     than MAX_FAST_SIZE.  This avoids out-of-bounds array accesses in
     _int_malloc after constant propagation of the size parameter.
     (The code never executes because malloc preserves the
     global_max_fast invariant, but the optimizers may not recognize
     this.)  */
  if (global_max_fast > MAX_FAST_SIZE)
    __builtin_unreachable ();
  return global_max_fast;
}

/*
   ----------- Internal state representation and initialization -----------
 */

/*
   have_fastchunks indicates that there are probably some fastbin chunks.
   It is set true on entering a chunk into any fastbin, and cleared early in
   malloc_consolidate.  The value is approximate since it may be set when there
   are no fastbin chunks, or it may be clear even if there are fastbin chunks
   available.  Given it's sole purpose is to reduce number of redundant calls to
   malloc_consolidate, it does not affect correctness.  As a result we can safely
   use relaxed atomic accesses.
 */

/*保存堆状态的结构体malloc_state*/
struct malloc_state
{
  /* Serialize access.  */
  /*该变量用于控制程序串行访问同一个分配区，当一个线程获取了分配区之后，其它线程要想访问该分配区，就必须等待该线程分配完成后才能够使用。*/
  __libc_lock_define (, mutex);

  /* Flags (formerly in max_fast).  */
  /*flags 记录了分配区的一些标志*/
  int flags;

  /* Set if the fastbin chunks contain recently inserted free blocks.  */
  /* Note this is a bool but not all targets support atomics on booleans.  */
  int have_fastchunks;

  /* Fastbins */
  mfastbinptr fastbinsY[NFASTBINS];/*记录fastbin*/

  /* Base of the topmost chunk -- not otherwise kept in a bin */
  mchunkptr top;/*记录top chunk*/

  /* The remainder from the most recent split of a small request */
  mchunkptr last_remainder;/*分割后剩余部分*/

  /* Normal bins packed as described above */
  mchunkptr bins[NBINS * 2 - 2];/* unsorted bin small bin large bin */

  /* Bitmap of bins */
  unsigned int binmap[BINMAPSIZE];/*标识某个bin是否空的map   具体定义在前面*/

  /* Linked list */
  struct malloc_state *next;/*与下一个malloc_state形成双链表*/

  /* Linked list for free arenas.  Access to this field is serialized
     by free_list_lock in arena.c.  */
  struct malloc_state *next_free;

  /* Number of threads attached to this arena.  0 if the arena is on
     the free list.  Access to this field is serialized by
     free_list_lock in arena.c.  */
  INTERNAL_SIZE_T attached_threads;

  /* Memory allocated from the system in this arena.  */
  INTERNAL_SIZE_T system_mem;
  INTERNAL_SIZE_T max_system_mem;
};

struct malloc_par
{
  /* Tunable parameters */
  unsigned long trim_threshold;
  INTERNAL_SIZE_T top_pad;
  INTERNAL_SIZE_T mmap_threshold;
  INTERNAL_SIZE_T arena_test;
  INTERNAL_SIZE_T arena_max;

  /* Memory map support */
  int n_mmaps;
  int n_mmaps_max;
  int max_n_mmaps;
  /* the mmap_threshold is dynamic, until the user sets
     it manually, at which point we need to disable any
     dynamic behavior. */
  int no_dyn_threshold;

  /* Statistics */
  INTERNAL_SIZE_T mmapped_mem;
  INTERNAL_SIZE_T max_mmapped_mem;

  /* First address handed out by MORECORE/sbrk.  */
  char *sbrk_base;

#if USE_TCACHE
  /* Maximum number of buckets to use.  */
  size_t tcache_bins;
  size_t tcache_max_bytes;
  /* Maximum number of chunks in each bucket.  */
  size_t tcache_count;
  /* Maximum number of chunks to remove from the unsorted list, which
     aren't used to prefill the cache.  */
  size_t tcache_unsorted_limit;
#endif
};

/* There are several instances of this struct ("arenas") in this
   malloc.  If you are adapting this malloc in a way that does NOT use
   a static or mmapped malloc_state, you MUST explicitly zero-fill it
   before using. This malloc relies on the property that malloc_state
   is initialized to all zeroes (as is true of C statics).  */

static struct malloc_state main_arena =
{
  .mutex = _LIBC_LOCK_INITIALIZER,
  .next = &main_arena,
  .attached_threads = 1
};

/* These variables are used for undumping support.  Chunked are marked
   as using mmap, but we leave them alone if they fall into this
   range.  NB: The chunk size for these chunks only includes the
   initial size field (of SIZE_SZ bytes), there is no trailing size
   field (unlike with regular mmapped chunks).  */
static mchunkptr dumped_main_arena_start; /* Inclusive.  */
static mchunkptr dumped_main_arena_end;   /* Exclusive.  */

/* True if the pointer falls into the dumped arena.  Use this after
   chunk_is_mmapped indicates a chunk is mmapped.  */
#define DUMPED_MAIN_ARENA_CHUNK(p) \
  ((p) >= dumped_main_arena_start && (p) < dumped_main_arena_end)

/* There is only one instance of the malloc parameters.  */
/*默认情况下的值*/
/*
#ifndef DEFAULT_MMAP_THRESHOLD_MIN
#define DEFAULT_MMAP_THRESHOLD_MIN (128 * 1024)
#endif
#ifndef DEFAULT_MMAP_THRESHOLD
#define DEFAULT_MMAP_THRESHOLD DEFAULT_MMAP_THRESHOLD_MIN
#endif
#ifndef DEFAULT_TOP_PAD
# define DEFAULT_TOP_PAD 131072
#endif
*/
static struct malloc_par mp_ =
{
  .top_pad = DEFAULT_TOP_PAD,
  .n_mmaps_max = DEFAULT_MMAP_MAX,
  .mmap_threshold = DEFAULT_MMAP_THRESHOLD,//128*1024   就是128k
  .trim_threshold = DEFAULT_TRIM_THRESHOLD,
#define NARENAS_FROM_NCORES(n) ((n) * (sizeof (long) == 4 ? 2 : 8))
  .arena_test = NARENAS_FROM_NCORES (1)
#if USE_TCACHE
  ,
  .tcache_count = TCACHE_FILL_COUNT,
  .tcache_bins = TCACHE_MAX_BINS,
  .tcache_max_bytes = tidx2usize (TCACHE_MAX_BINS-1),
  .tcache_unsorted_limit = 0 /* No limit.  */
#endif
};

/*
   Initialize a malloc_state struct.

   This is called from ptmalloc_init () or from _int_new_arena ()
   when creating a new arena.
 */
/*堆初始化过程*/
static void
malloc_init_state (mstate av)
{
  int i;
  mbinptr bin;

  /* Establish circular links for normal bins */
  for (i = 1; i < NBINS; ++i)
    {
      bin = bin_at (av, i);
      bin->fd = bin->bk = bin;
    }

#if MORECORE_CONTIGUOUS
  if (av != &main_arena)
#endif
  set_noncontiguous (av);
  if (av == &main_arena)
    set_max_fast (DEFAULT_MXFAST);
  atomic_store_relaxed (&av->have_fastchunks, false);

  av->top = initial_top (av);
}

/*
   Other internal utilities operating on mstates
 */

static void *sysmalloc (INTERNAL_SIZE_T, mstate);
static int      systrim (size_t, mstate);
static void     malloc_consolidate (mstate);


/* -------------- Early definitions for debugging hooks ---------------- */

/* Define and initialize the hook variables.  These weak definitions must
   appear before any use of the variables in a function (arena.c uses one).  */
#ifndef weak_variable
/* In GNU libc we want the hook variables to be weak definitions to
   avoid a problem with Emacs.  */
# define weak_variable weak_function
#endif

/* Forward declarations.  */
static void *malloc_hook_ini (size_t sz,
                              const void *caller) __THROW;
static void *realloc_hook_ini (void *ptr, size_t sz,
                               const void *caller) __THROW;
static void *memalign_hook_ini (size_t alignment, size_t sz,
                                const void *caller) __THROW;

#if HAVE_MALLOC_INIT_HOOK
void weak_variable (*__malloc_initialize_hook) (void) = NULL;
compat_symbol (libc, __malloc_initialize_hook,
	       __malloc_initialize_hook, GLIBC_2_0);
#endif

void weak_variable (*__free_hook) (void *__ptr,
                                   const void *) = NULL;
void *weak_variable (*__malloc_hook)
  (size_t __size, const void *) = malloc_hook_ini;
void *weak_variable (*__realloc_hook)
  (void *__ptr, size_t __size, const void *)
  = realloc_hook_ini;
void *weak_variable (*__memalign_hook)
  (size_t __alignment, size_t __size, const void *)
  = memalign_hook_ini;
void weak_variable (*__after_morecore_hook) (void) = NULL;

/* This function is called from the arena shutdown hook, to free the
   thread cache (if it exists).  */
static void tcache_thread_shutdown (void);

/* ------------------ Testing support ----------------------------------*/

static int perturb_byte;

static void
alloc_perturb (char *p, size_t n)
{
  if (__glibc_unlikely (perturb_byte))
    memset (p, perturb_byte ^ 0xff, n);
}

static void
free_perturb (char *p, size_t n)
{
  if (__glibc_unlikely (perturb_byte))
    memset (p, perturb_byte, n);
}



#include <stap-probe.h>

/* ------------------- Support for multiple arenas -------------------- */
#include "arena.c"

/*
   Debugging support

   These routines make a number of assertions about the states
   of data structures that should be true at all times. If any
   are not true, it's very likely that a user program has somehow
   trashed memory. (It's also possible that there is a coding error
   in malloc. In which case, please report it!)
 */

#if !MALLOC_DEBUG

# define check_chunk(A, P)
# define check_free_chunk(A, P)
# define check_inuse_chunk(A, P)
# define check_remalloced_chunk(A, P, N)
# define check_malloced_chunk(A, P, N)
# define check_malloc_state(A)

#else

# define check_chunk(A, P)              do_check_chunk (A, P)
# define check_free_chunk(A, P)         do_check_free_chunk (A, P)
# define check_inuse_chunk(A, P)        do_check_inuse_chunk (A, P)
# define check_remalloced_chunk(A, P, N) do_check_remalloced_chunk (A, P, N)
# define check_malloced_chunk(A, P, N)   do_check_malloced_chunk (A, P, N)
# define check_malloc_state(A)         do_check_malloc_state (A)

/*
   Properties of all chunks
 */

static void
do_check_chunk (mstate av, mchunkptr p)
{
  unsigned long sz = chunksize (p);
  /* min and max possible addresses assuming contiguous allocation */
  char *max_address = (char *) (av->top) + chunksize (av->top);
  char *min_address = max_address - av->system_mem;

  if (!chunk_is_mmapped (p))
    {
      /* Has legal address ... */
      if (p != av->top)
        {
          if (contiguous (av))
            {
              assert (((char *) p) >= min_address);
              assert (((char *) p + sz) <= ((char *) (av->top)));
            }
        }
      else
        {
          /* top size is always at least MINSIZE */
          assert ((unsigned long) (sz) >= MINSIZE);
          /* top predecessor always marked inuse */
          assert (prev_inuse (p));
        }
    }
  else if (!DUMPED_MAIN_ARENA_CHUNK (p))
    {
      /* address is outside main heap  */
      if (contiguous (av) && av->top != initial_top (av))
        {
          assert (((char *) p) < min_address || ((char *) p) >= max_address);
        }
      /* chunk is page-aligned */
      assert (((prev_size (p) + sz) & (GLRO (dl_pagesize) - 1)) == 0);
      /* mem is aligned */
      assert (aligned_OK (chunk2mem (p)));
    }
}

/*
   Properties of free chunks
 */

static void
do_check_free_chunk (mstate av, mchunkptr p)
{
  INTERNAL_SIZE_T sz = chunksize_nomask (p) & ~(PREV_INUSE | NON_MAIN_ARENA);
  mchunkptr next = chunk_at_offset (p, sz);

  do_check_chunk (av, p);

  /* Chunk must claim to be free ... */
  assert (!inuse (p));
  assert (!chunk_is_mmapped (p));

  /* Unless a special marker, must have OK fields */
  if ((unsigned long) (sz) >= MINSIZE)
    {
      assert ((sz & MALLOC_ALIGN_MASK) == 0);
      assert (aligned_OK (chunk2mem (p)));
      /* ... matching footer field */
      assert (prev_size (next_chunk (p)) == sz);
      /* ... and is fully consolidated */
      assert (prev_inuse (p));
      assert (next == av->top || inuse (next));

      /* ... and has minimally sane links */
      assert (p->fd->bk == p);
      assert (p->bk->fd == p);
    }
  else /* markers are always of size SIZE_SZ */
    assert (sz == SIZE_SZ);
}

/*
   Properties of inuse chunks
 */

static void
do_check_inuse_chunk (mstate av, mchunkptr p)
{
  mchunkptr next;

  do_check_chunk (av, p);

  if (chunk_is_mmapped (p))
    return; /* mmapped chunks have no next/prev */

  /* Check whether it claims to be in use ... */
  assert (inuse (p));

  next = next_chunk (p);

  /* ... and is surrounded by OK chunks.
     Since more things can be checked with free chunks than inuse ones,
     if an inuse chunk borders them and debug is on, it's worth doing them.
   */
  if (!prev_inuse (p))
    {
      /* Note that we cannot even look at prev unless it is not inuse */
      mchunkptr prv = prev_chunk (p);
      assert (next_chunk (prv) == p);
      do_check_free_chunk (av, prv);
    }

  if (next == av->top)
    {
      assert (prev_inuse (next));
      assert (chunksize (next) >= MINSIZE);
    }
  else if (!inuse (next))
    do_check_free_chunk (av, next);
}

/*
   Properties of chunks recycled from fastbins
 */

static void
do_check_remalloced_chunk (mstate av, mchunkptr p, INTERNAL_SIZE_T s)
{
  INTERNAL_SIZE_T sz = chunksize_nomask (p) & ~(PREV_INUSE | NON_MAIN_ARENA);

  if (!chunk_is_mmapped (p))
    {
      assert (av == arena_for_chunk (p));
      if (chunk_main_arena (p))
        assert (av == &main_arena);
      else
        assert (av != &main_arena);
    }

  do_check_inuse_chunk (av, p);

  /* Legal size ... */
  assert ((sz & MALLOC_ALIGN_MASK) == 0);
  assert ((unsigned long) (sz) >= MINSIZE);
  /* ... and alignment */
  assert (aligned_OK (chunk2mem (p)));
  /* chunk is less than MINSIZE more than request */
  assert ((long) (sz) - (long) (s) >= 0);
  assert ((long) (sz) - (long) (s + MINSIZE) < 0);
}

/*
   Properties of nonrecycled chunks at the point they are malloced
 */

static void
do_check_malloced_chunk (mstate av, mchunkptr p, INTERNAL_SIZE_T s)
{
  /* same as recycled case ... */
  do_check_remalloced_chunk (av, p, s);

  /*
     ... plus,  must obey implementation invariant that prev_inuse is
     always true of any allocated chunk; i.e., that each allocated
     chunk borders either a previously allocated and still in-use
     chunk, or the base of its memory arena. This is ensured
     by making all allocations from the `lowest' part of any found
     chunk.  This does not necessarily hold however for chunks
     recycled via fastbins.
   */

  assert (prev_inuse (p));
}


/*
   Properties of malloc_state.

   This may be useful for debugging malloc, as well as detecting user
   programmer errors that somehow write into malloc_state.

   If you are extending or experimenting with this malloc, you can
   probably figure out how to hack this routine to print out or
   display chunk addresses, sizes, bins, and other instrumentation.
 */

static void
do_check_malloc_state (mstate av)
{
  int i;
  mchunkptr p;
  mchunkptr q;
  mbinptr b;
  unsigned int idx;
  INTERNAL_SIZE_T size;
  unsigned long total = 0;
  int max_fast_bin;

  /* internal size_t must be no wider than pointer type */
  assert (sizeof (INTERNAL_SIZE_T) <= sizeof (char *));

  /* alignment is a power of 2 */
  assert ((MALLOC_ALIGNMENT & (MALLOC_ALIGNMENT - 1)) == 0);

  /* Check the arena is initialized. */
  assert (av->top != 0);

  /* No memory has been allocated yet, so doing more tests is not possible.  */
  if (av->top == initial_top (av))
    return;

  /* pagesize is a power of 2 */
  assert (powerof2(GLRO (dl_pagesize)));

  /* A contiguous main_arena is consistent with sbrk_base.  */
  if (av == &main_arena && contiguous (av))
    assert ((char *) mp_.sbrk_base + av->system_mem ==
            (char *) av->top + chunksize (av->top));

  /* properties of fastbins */

  /* max_fast is in allowed range */
  assert ((get_max_fast () & ~1) <= request2size (MAX_FAST_SIZE));

  max_fast_bin = fastbin_index (get_max_fast ());

  for (i = 0; i < NFASTBINS; ++i)
    {
      p = fastbin (av, i);

      /* The following test can only be performed for the main arena.
         While mallopt calls malloc_consolidate to get rid of all fast
         bins (especially those larger than the new maximum) this does
         only happen for the main arena.  Trying to do this for any
         other arena would mean those arenas have to be locked and
         malloc_consolidate be called for them.  This is excessive.  And
         even if this is acceptable to somebody it still cannot solve
         the problem completely since if the arena is locked a
         concurrent malloc call might create a new arena which then
         could use the newly invalid fast bins.  */

      /* all bins past max_fast are empty */
      if (av == &main_arena && i > max_fast_bin)
        assert (p == 0);

      while (p != 0)
        {
          /* each chunk claims to be inuse */
          do_check_inuse_chunk (av, p);
          total += chunksize (p);
          /* chunk belongs in this bin */
          assert (fastbin_index (chunksize (p)) == i);
          p = p->fd;
        }
    }

  /* check normal bins */
  for (i = 1; i < NBINS; ++i)
    {
      b = bin_at (av, i);

      /* binmap is accurate (except for bin 1 == unsorted_chunks) */
      if (i >= 2)
        {
          unsigned int binbit = get_binmap (av, i);
          int empty = last (b) == b;
          if (!binbit)
            assert (empty);
          else if (!empty)
            assert (binbit);
        }

      for (p = last (b); p != b; p = p->bk)
        {
          /* each chunk claims to be free */
          do_check_free_chunk (av, p);
          size = chunksize (p);
          total += size;
          if (i >= 2)
            {
              /* chunk belongs in bin */
              idx = bin_index (size);
              assert (idx == i);
              /* lists are sorted */
              assert (p->bk == b ||
                      (unsigned long) chunksize (p->bk) >= (unsigned long) chunksize (p));

              if (!in_smallbin_range (size))
                {
                  if (p->fd_nextsize != NULL)
                    {
                      if (p->fd_nextsize == p)
                        assert (p->bk_nextsize == p);
                      else
                        {
                          if (p->fd_nextsize == first (b))
                            assert (chunksize (p) < chunksize (p->fd_nextsize));
                          else
                            assert (chunksize (p) > chunksize (p->fd_nextsize));

                          if (p == first (b))
                            assert (chunksize (p) > chunksize (p->bk_nextsize));
                          else
                            assert (chunksize (p) < chunksize (p->bk_nextsize));
                        }
                    }
                  else
                    assert (p->bk_nextsize == NULL);
                }
            }
          else if (!in_smallbin_range (size))
            assert (p->fd_nextsize == NULL && p->bk_nextsize == NULL);
          /* chunk is followed by a legal chain of inuse chunks */
          for (q = next_chunk (p);
               (q != av->top && inuse (q) &&
                (unsigned long) (chunksize (q)) >= MINSIZE);
               q = next_chunk (q))
            do_check_inuse_chunk (av, q);
        }
    }

  /* top chunk is OK */
  check_chunk (av, av->top);
}
#endif


/* ----------------- Support for debugging hooks -------------------- */
#include "hooks.c"


/* ----------- Routines dealing with system allocation -------------- */

/*
   sysmalloc handles malloc cases requiring more memory from the system.
   On entry, it is assumed that av->top does not have enough
   space to service request for nb bytes, thus requiring that av->top
   be extended or replaced.
 */
/*在top chunk不够的时候，向系统申请更多的空间*/
static void *
sysmalloc (INTERNAL_SIZE_T nb, mstate av)
{
  mchunkptr old_top;              /* incoming value of av->top */
  INTERNAL_SIZE_T old_size;       /* its size */
  char *old_end;                  /* its end address */

  long size;                      /* arg to first MORECORE or mmap call */
  char *brk;                      /* return value from MORECORE */

  long correction;                /* arg to 2nd MORECORE call */
  char *snd_brk;                  /* 2nd return val */

  INTERNAL_SIZE_T front_misalign; /* unusable bytes at front of new space */
  INTERNAL_SIZE_T end_misalign;   /* partial page left at end of new space */
  char *aligned_brk;              /* aligned offset into brk */

  mchunkptr p;                    /* the allocated/returned chunk */
  mchunkptr remainder;            /* remainder from allocation */
  unsigned long remainder_size;   /* its size */

  /* 关于pagesize 即页大小
   * #ifndef EXEC_PAGESIZE
   * #define EXEC_PAGESIZE	4096
   * #endif
   * size_t _dl_pagesize = EXEC_PAGESIZE;
   * # define GLRO(name) _##name
   **/
  size_t pagesize = GLRO (dl_pagesize);//0x1000
  bool tried_mmap = false;


  /*
     If have mmap, and the request size meets the mmap threshold, and
     the system supports mmap, and there are few enough currently
     allocated mmapped regions, try to directly map this request
     rather than expanding top.
   */
  /*如果具有mmap，并且请求大小满足mmap阈值，并且系统支持mmap，并且当前分配的mmapped区域很少，尝试直接映射此请求，而不是扩展top*/
  /*具体来说就是申请的内存大于mmap_threshold 而mmap 的数量n_mmaps小于最大值n_mmaps_max；(这里默认临界值存放在mp_中)*/
  if (av == NULL
      || ((unsigned long) (nb) >= (unsigned long) (mp_.mmap_threshold)
	  && (mp_.n_mmaps < mp_.n_mmaps_max)))
    {
      char *mm;           /* return value from mmap call*/

    try_mmap:
      /*
         Round up size to nearest page.  For mmapped chunks, the overhead
         is one SIZE_SZ unit larger than for normal chunks, because there
         is no following chunk whose prev_size field could be used.

         See the front_misalign handling below, for glibc there is no
         need for further alignments unless we have have high alignment.
       */
      if (MALLOC_ALIGNMENT == 2 * SIZE_SZ)
        size = ALIGN_UP (nb + SIZE_SZ, pagesize);
      else
        size = ALIGN_UP (nb + SIZE_SZ + MALLOC_ALIGN_MASK, pagesize);
      tried_mmap = true;

      /* Don't try if size wraps around 0 */
      if ((unsigned long) (size) > (unsigned long) (nb))
        {
          mm = (char *) (MMAP (0, size, PROT_READ | PROT_WRITE, 0));

          if (mm != MAP_FAILED)
            {
              /*
                 The offset to the start of the mmapped region is stored
                 in the prev_size field of the chunk. This allows us to adjust
                 returned start address to meet alignment requirements here
                 and in memalign(), and still be able to compute proper
                 address argument for later munmap in free() and realloc().
               */

              if (MALLOC_ALIGNMENT == 2 * SIZE_SZ)
                {
                  /* For glibc, chunk2mem increases the address by 2*SIZE_SZ and
                     MALLOC_ALIGN_MASK is 2*SIZE_SZ-1.  Each mmap'ed area is page
                     aligned and therefore definitely MALLOC_ALIGN_MASK-aligned.  */
                  assert (((INTERNAL_SIZE_T) chunk2mem (mm) & MALLOC_ALIGN_MASK) == 0);
                  front_misalign = 0;
                }
              else
                front_misalign = (INTERNAL_SIZE_T) chunk2mem (mm) & MALLOC_ALIGN_MASK;
              if (front_misalign > 0)
                {
                  correction = MALLOC_ALIGNMENT - front_misalign;
                  p = (mchunkptr) (mm + correction);
		  set_prev_size (p, correction);
                  set_head (p, (size - correction) | IS_MMAPPED);
                }
              else
                {
                  p = (mchunkptr) mm;
		  set_prev_size (p, 0);
                  set_head (p, size | IS_MMAPPED);
                }

              /* update statistics */

              int new = atomic_exchange_and_add (&mp_.n_mmaps, 1) + 1;
              atomic_max (&mp_.max_n_mmaps, new);

              unsigned long sum;
              sum = atomic_exchange_and_add (&mp_.mmapped_mem, size) + size;
              atomic_max (&mp_.max_mmapped_mem, sum);

              check_chunk (av, p);

              return chunk2mem (p);
            }
        }
    }

  /* There are no usable arenas and mmap also failed.  */
  /*mmap失败了  或者说没有可用的arena  就退出*/
  if (av == NULL)
    return 0;

  /* Record incoming configuration of top */
  /*成功了至少一个 记录旧堆的top chunk信息  */
  old_top = av->top;
  old_size = chunksize (old_top);
  old_end = (char *) (chunk_at_offset (old_top, old_size));

  brk = snd_brk = (char *) (MORECORE_FAILURE);

  /*
     If not the first time through, we require old_size to be
     at least MINSIZE and to have prev_inuse set.
   */
  /*检查旧堆的信息*/
  /*如果是第一次  那top chunk为0*/
  /*新的堆 堆的大小应该不小于 MINSIZE，并且前一个堆块应该处于使用中*/
  /*堆的结束地址应该是页对齐的，大小默认是 0x1000，低12位为0*/
  assert ((old_top == initial_top (av) && old_size == 0) ||
          ((unsigned long) (old_size) >= MINSIZE &&
           prev_inuse (old_top) &&
           ((unsigned long) old_end & (pagesize - 1)) == 0));

  /* Precondition: not enough current space to satisfy nb request */
  /*继续检查旧堆信息*/
  assert ((unsigned long) (old_size) < (unsigned long) (nb + MINSIZE));

  /*av不是main arena*/
  if (av != &main_arena)
    {
      heap_info *old_heap, *heap;
      size_t old_heap_size;

      /* First try to extend the current heap. */
      old_heap = heap_for_ptr (old_top);
      old_heap_size = old_heap->size;
      if ((long) (MINSIZE + nb - old_size) > 0
          && grow_heap (old_heap, MINSIZE + nb - old_size) == 0)
        {
          av->system_mem += old_heap->size - old_heap_size;
          set_head (old_top, (((char *) old_heap + old_heap->size) - (char *) old_top)
                    | PREV_INUSE);
        }
      else if ((heap = new_heap (nb + (MINSIZE + sizeof (*heap)), mp_.top_pad)))
        {
          /* Use a newly allocated heap.  */
          heap->ar_ptr = av;
          heap->prev = old_heap;
          av->system_mem += heap->size;
          /* Set up the new top.  */
          top (av) = chunk_at_offset (heap, sizeof (*heap));
          set_head (top (av), (heap->size - sizeof (*heap)) | PREV_INUSE);

          /* Setup fencepost and free the old top chunk with a multiple of
             MALLOC_ALIGNMENT in size. */
          /* The fencepost takes at least MINSIZE bytes, because it might
             become the top chunk again later.  Note that a footer is set
             up, too, although the chunk is marked in use. */
          old_size = (old_size - MINSIZE) & ~MALLOC_ALIGN_MASK;
          set_head (chunk_at_offset (old_top, old_size + 2 * SIZE_SZ), 0 | PREV_INUSE);
          if (old_size >= MINSIZE)
            {
              set_head (chunk_at_offset (old_top, old_size), (2 * SIZE_SZ) | PREV_INUSE);
              set_foot (chunk_at_offset (old_top, old_size), (2 * SIZE_SZ));
              set_head (old_top, old_size | PREV_INUSE | NON_MAIN_ARENA);
              _int_free (av, old_top, 1);
            }
          else
            {
              set_head (old_top, (old_size + 2 * SIZE_SZ) | PREV_INUSE);
              set_foot (old_top, (old_size + 2 * SIZE_SZ));
            }
        }
      else if (!tried_mmap)
        /* We can at least try to use to mmap memory.  */
        goto try_mmap;
    }
  else     /* av == main_arena的情况下 */

    /*计算请求需要的大小   这个mp_.top_pad默认是131072  就是0x20000*/
    { /* Request enough space for nb + pad + overhead */
      size = nb + mp_.top_pad + MINSIZE;

      /*
         If contiguous, we can subtract out existing space that we hope to
         combine with new space. We add it back later only if
         we don't actually get contiguous space.
       */
      /*是否希望连续  复用之前的内存  所以剪掉原来的大小*/
      if (contiguous (av))
        size -= old_size;

      /*
         Round to a multiple of page size.
         If MORECORE is not contiguous, this ensures that we only call it
         with whole-page arguments.  And if MORECORE is contiguous and
         this is not first time through, this preserves page-alignment of
         previous calls. Otherwise, we correct to page-align below.
       */
      /*对齐*/
      size = ALIGN_UP (size, pagesize);

      /*
         Don't try to call MORECORE if argument is so big as to appear
         negative. Note that since mmap takes size_t arg, it may succeed
         below even if we cannot call MORECORE.
       */
      /*申请内存*/
      if (size > 0)
        {
          /*移动brk*/
          brk = (char *) (MORECORE (size));
          LIBC_PROBE (memory_sbrk_more, 2, brk, size);
        }

      if (brk != (char *) (MORECORE_FAILURE))
        {
          /*c成功后调用了个hook？*/
          /* Call the `morecore' hook if necessary.  */
          void (*hook) (void) = atomic_forced_read (__after_morecore_hook);
          if (__builtin_expect (hook != NULL, 0))
            (*hook)();
        }
      /*申请失败  使用mmap*/
      else
        {
          /*
             If have mmap, try using it as a backup when MORECORE fails or
             cannot be used. This is worth doing on systems that have "holes" in
             address space, so sbrk cannot extend to give contiguous space, but
             space is available elsewhere.  Note that we ignore mmap max count
             and threshold limits, since the space will not be used as a
             segregated mmap region.
           */

          /* Cannot merge with old top, so add its size back in */
          if (contiguous (av))
            size = ALIGN_UP (size + old_size, pagesize);

          /* If we are relying on mmap as backup, then use larger units */
          if ((unsigned long) (size) < (unsigned long) (MMAP_AS_MORECORE_SIZE))
            size = MMAP_AS_MORECORE_SIZE;

          /* Don't try if size wraps around 0 */
          if ((unsigned long) (size) > (unsigned long) (nb))
            {
              char *mbrk = (char *) (MMAP (0, size, PROT_READ | PROT_WRITE, 0));

              if (mbrk != MAP_FAILED)
                {
                  /* We do not need, and cannot use, another sbrk call to find end */
                  brk = mbrk;
                  snd_brk = brk + size;

                  /*
                     Record that we no longer have a contiguous sbrk region.
                     After the first time mmap is used as backup, we do not
                     ever rely on contiguous space since this could incorrectly
                     bridge regions.
                   */
                  set_noncontiguous (av);
                }
            }
        }

      if (brk != (char *) (MORECORE_FAILURE))
        {
          /*成功后*/
          if (mp_.sbrk_base == 0)
            mp_.sbrk_base = brk;
          av->system_mem += size;

          /*
             If MORECORE extends previous space, we can likewise extend top size.
           */
          /*扩展top chunk的size  设置头部*/
          if (brk == old_end && snd_brk == (char *) (MORECORE_FAILURE))
            set_head (old_top, (size + old_size) | PREV_INUSE);
          /*内存耗尽？*/
          else if (contiguous (av) && old_size && brk < old_end)
	    /* Oops!  Someone else killed our space..  Can't touch anything.  */
	    malloc_printerr ("break adjusted to free malloc space");

          /*
             Otherwise, make adjustments:

           * If the first time through or noncontiguous, we need to call sbrk
              just to find out where the end of memory lies.

           * We need to ensure that all returned chunks from malloc will meet
              MALLOC_ALIGNMENT

           * If there was an intervening foreign sbrk, we need to adjust sbrk
              request size to account for fact that we will not be able to
              combine new space with existing space in old_top.

           * Almost all systems internally allocate whole pages at a time, in
              which case we might as well use the whole last page of request.
              So we allocate enough more memory to hit a page boundary now,
              which in turn causes future contiguous calls to page-align.
           */
          /*其他情况*/
          else
            {
              front_misalign = 0;
              end_misalign = 0;
              correction = 0;
              aligned_brk = brk;

              /* handle contiguous cases */
              /*连续内存情况*/
              if (contiguous (av))
                {
                  /* Count foreign sbrk as system_mem.  */
                  if (old_size)
                    av->system_mem += brk - old_end;

                  /* Guarantee alignment of first new chunk made from this space */

                  front_misalign = (INTERNAL_SIZE_T) chunk2mem (brk) & MALLOC_ALIGN_MASK;
                  if (front_misalign > 0)
                    {
                      /*
                         Skip over some bytes to arrive at an aligned position.
                         We don't need to specially mark these wasted front bytes.
                         They will never be accessed anyway because
                         prev_inuse of av->top (and any chunk created from its start)
                         is always true after initialization.
                       */

                      correction = MALLOC_ALIGNMENT - front_misalign;
                      aligned_brk += correction;
                    }

                  /*
                     If this isn't adjacent to existing space, then we will not
                     be able to merge with old_top space, so must add to 2nd request.
                   */

                  correction += old_size;

                  /* Extend the end address to hit a page boundary */
                  end_misalign = (INTERNAL_SIZE_T) (brk + size + correction);
                  correction += (ALIGN_UP (end_misalign, pagesize)) - end_misalign;

                  assert (correction >= 0);
                  snd_brk = (char *) (MORECORE (correction));

                  /*
                     If can't allocate correction, try to at least find out current
                     brk.  It might be enough to proceed without failing.

                     Note that if second sbrk did NOT fail, we assume that space
                     is contiguous with first sbrk. This is a safe assumption unless
                     program is multithreaded but doesn't use locks and a foreign sbrk
                     occurred between our first and second calls.
                   */

                  if (snd_brk == (char *) (MORECORE_FAILURE))
                    {
                      correction = 0;
                      snd_brk = (char *) (MORECORE (0));
                    }
                  else
                    {
                      /* Call the `morecore' hook if necessary.  */
                      void (*hook) (void) = atomic_forced_read (__after_morecore_hook);
                      if (__builtin_expect (hook != NULL, 0))
                        (*hook)();
                    }
                }

              /* handle non-contiguous cases */
              /*不连续内存情况*/
              else
                {
                  if (MALLOC_ALIGNMENT == 2 * SIZE_SZ)
                    /* MORECORE/mmap must correctly align */
                    assert (((unsigned long) chunk2mem (brk) & MALLOC_ALIGN_MASK) == 0);
                  else
                    {
                      front_misalign = (INTERNAL_SIZE_T) chunk2mem (brk) & MALLOC_ALIGN_MASK;
                      if (front_misalign > 0)
                        {
                          /*
                             Skip over some bytes to arrive at an aligned position.
                             We don't need to specially mark these wasted front bytes.
                             They will never be accessed anyway because
                             prev_inuse of av->top (and any chunk created from its start)
                             is always true after initialization.
                           */

                          aligned_brk += MALLOC_ALIGNMENT - front_misalign;
                        }
                    }

                  /* Find out current end of memory */
                  if (snd_brk == (char *) (MORECORE_FAILURE))
                    {
                      snd_brk = (char *) (MORECORE (0));
                    }
                }

              /* Adjust top based on results of second sbrk */
              /*根据sbrk结果调整top chunk*/
              if (snd_brk != (char *) (MORECORE_FAILURE))
                {
                  av->top = (mchunkptr) aligned_brk;
                  set_head (av->top, (snd_brk - aligned_brk + correction) | PREV_INUSE);
                  av->system_mem += correction;

                  /*
                     If not the first time through, we either have a
                     gap due to foreign sbrk or a non-contiguous region.  Insert a
                     double fencepost at old_top to prevent consolidation with space
                     we don't own. These fenceposts are artificial chunks that are
                     marked as inuse and are in any case too small to use.  We need
                     two to make sizes and alignments work out.
                   */

                  if (old_size != 0)
                    {
                      /*
                         Shrink old_top to insert fenceposts, keeping size a
                         multiple of MALLOC_ALIGNMENT. We know there is at least
                         enough space in old_top to do this.
                       */
                      old_size = (old_size - 4 * SIZE_SZ) & ~MALLOC_ALIGN_MASK;
                      set_head (old_top, old_size | PREV_INUSE);

                      /*
                         Note that the following assignments completely overwrite
                         old_top when old_size was previously MINSIZE.  This is
                         intentional. We need the fencepost, even if old_top otherwise gets
                         lost.
                       */
		      set_head (chunk_at_offset (old_top, old_size),
				(2 * SIZE_SZ) | PREV_INUSE);
		      set_head (chunk_at_offset (old_top, old_size + 2 * SIZE_SZ),
				(2 * SIZE_SZ) | PREV_INUSE);

                      /* If possible, release the rest. */
                      if (old_size >= MINSIZE)
                        {
                          _int_free (av, old_top, 1);
                        }
                    }
                }
            }
        }
    } /* if (av !=  &main_arena) */
  /*更新最大内存*/
  if ((unsigned long) av->system_mem > (unsigned long) (av->max_system_mem))
    av->max_system_mem = av->system_mem;
  check_malloc_state (av);

  /* finally, do the allocation */
  /*分配内存块*/
  /*获取大小*/
  p = av->top;
  size = chunksize (p);
  /*对top进行切分*/
  /* check that one of the above allocation paths succeeded */
  if ((unsigned long) (size) >= (unsigned long) (nb + MINSIZE))
    {
      remainder_size = size - nb;
      remainder = chunk_at_offset (p, nb);
      av->top = remainder;
      set_head (p, nb | PREV_INUSE | (av != &main_arena ? NON_MAIN_ARENA : 0));
      set_head (remainder, remainder_size | PREV_INUSE);
      check_malloced_chunk (av, p, nb);
      return chunk2mem (p);
    }

  /* catch all failure paths */
  /*catch 出现的错误*/
  __set_errno (ENOMEM);
  return 0;
}


/*
   systrim is an inverse of sorts to sysmalloc.  It gives memory back
   to the system (via negative arguments to sbrk) if there is unused
   memory at the `high' end of the malloc pool. It is called
   automatically by free() when top space exceeds the trim
   threshold. It is also called by the public malloc_trim routine.  It
   returns 1 if it actually released any memory, else 0.
 */

static int
systrim (size_t pad, mstate av)
{
  long top_size;         /* Amount of top-most memory */
  long extra;            /* Amount to release */
  long released;         /* Amount actually released */
  char *current_brk;     /* address returned by pre-check sbrk call */
  char *new_brk;         /* address returned by post-check sbrk call */
  size_t pagesize;
  long top_area;

  pagesize = GLRO (dl_pagesize);
  top_size = chunksize (av->top);

  top_area = top_size - MINSIZE - 1;
  if (top_area <= pad)
    return 0;

  /* Release in pagesize units and round down to the nearest page.  */
  extra = ALIGN_DOWN(top_area - pad, pagesize);

  if (extra == 0)
    return 0;

  /*
     Only proceed if end of memory is where we last set it.
     This avoids problems if there were foreign sbrk calls.
   */
  current_brk = (char *) (MORECORE (0));
  if (current_brk == (char *) (av->top) + top_size)
    {
      /*
         Attempt to release memory. We ignore MORECORE return value,
         and instead call again to find out where new end of memory is.
         This avoids problems if first call releases less than we asked,
         of if failure somehow altered brk value. (We could still
         encounter problems if it altered brk in some very bad way,
         but the only thing we can do is adjust anyway, which will cause
         some downstream failure.)
       */

      MORECORE (-extra);
      /* Call the `morecore' hook if necessary.  */
      void (*hook) (void) = atomic_forced_read (__after_morecore_hook);
      if (__builtin_expect (hook != NULL, 0))
        (*hook)();
      new_brk = (char *) (MORECORE (0));

      LIBC_PROBE (memory_sbrk_less, 2, new_brk, extra);

      if (new_brk != (char *) MORECORE_FAILURE)
        {
          released = (long) (current_brk - new_brk);

          if (released != 0)
            {
              /* Success. Adjust top. */
              av->system_mem -= released;
              set_head (av->top, (top_size - released) | PREV_INUSE);
              check_malloc_state (av);
              return 1;
            }
        }
    }
  return 0;
}

static void
munmap_chunk (mchunkptr p)
{
  size_t pagesize = GLRO (dl_pagesize);
  INTERNAL_SIZE_T size = chunksize (p);

  assert (chunk_is_mmapped (p));

  /* Do nothing if the chunk is a faked mmapped chunk in the dumped
     main arena.  We never free this memory.  */
  if (DUMPED_MAIN_ARENA_CHUNK (p))
    return;

  uintptr_t mem = (uintptr_t) chunk2mem (p);
  uintptr_t block = (uintptr_t) p - prev_size (p);
  size_t total_size = prev_size (p) + size;
  /* Unfortunately we have to do the compilers job by hand here.  Normally
     we would test BLOCK and TOTAL-SIZE separately for compliance with the
     page size.  But gcc does not recognize the optimization possibility
     (in the moment at least) so we combine the two values into one before
     the bit test.  */
  if (__glibc_unlikely ((block | total_size) & (pagesize - 1)) != 0
      || __glibc_unlikely (!powerof2 (mem & (pagesize - 1))))
    malloc_printerr ("munmap_chunk(): invalid pointer");

  atomic_decrement (&mp_.n_mmaps);
  atomic_add (&mp_.mmapped_mem, -total_size);

  /* If munmap failed the process virtual memory address space is in a
     bad shape.  Just leave the block hanging around, the process will
     terminate shortly anyway since not much can be done.  */
  __munmap ((char *) block, total_size);
}

#if HAVE_MREMAP

static mchunkptr
mremap_chunk (mchunkptr p, size_t new_size)
{
  size_t pagesize = GLRO (dl_pagesize);
  INTERNAL_SIZE_T offset = prev_size (p);
  INTERNAL_SIZE_T size = chunksize (p);
  char *cp;

  assert (chunk_is_mmapped (p));

  uintptr_t block = (uintptr_t) p - offset;
  uintptr_t mem = (uintptr_t) chunk2mem(p);
  size_t total_size = offset + size;
  if (__glibc_unlikely ((block | total_size) & (pagesize - 1)) != 0
      || __glibc_unlikely (!powerof2 (mem & (pagesize - 1))))
    malloc_printerr("mremap_chunk(): invalid pointer");

  /* Note the extra SIZE_SZ overhead as in mmap_chunk(). */
  new_size = ALIGN_UP (new_size + offset + SIZE_SZ, pagesize);

  /* No need to remap if the number of pages does not change.  */
  if (total_size == new_size)
    return p;

  cp = (char *) __mremap ((char *) block, total_size, new_size,
                          MREMAP_MAYMOVE);

  if (cp == MAP_FAILED)
    return 0;

  p = (mchunkptr) (cp + offset);

  assert (aligned_OK (chunk2mem (p)));

  assert (prev_size (p) == offset);
  set_head (p, (new_size - offset) | IS_MMAPPED);

  INTERNAL_SIZE_T new;
  new = atomic_exchange_and_add (&mp_.mmapped_mem, new_size - size - offset)
        + new_size - size - offset;
  atomic_max (&mp_.max_mmapped_mem, new);
  return p;
}
#endif /* HAVE_MREMAP */

/*------------------------ Public wrappers. --------------------------------*/

#if USE_TCACHE

/* We overlay this structure on the user-data portion of a chunk when
   the chunk is stored in the per-thread cache.  */
/*tcache entry结构体 用于链接空闲的chunk结构体，其中的next指针指向下一个相同大小的chunk*/
typedef struct tcache_entry
{
  struct tcache_entry *next;/* next指向的是chunk的user data的开始，而非chunk的开始*/
  /* This field exists to detect double frees.  */
  /*这个域用来检测double free？*/
  struct tcache_perthread_struct *key;
} tcache_entry;

/* There is one of these for each thread, which contains the
   per-thread cache (hence "tcache_perthread_struct").  Keeping
   overall size low is mildly important.  Note that COUNTS and ENTRIES
   are redundant (we could have just counted the linked list each
   time), this is for performance reasons.  */
/*每个thread维护一个tcache_prethread_struct*/
typedef struct tcache_perthread_struct
{
  uint16_t counts[TCACHE_MAX_BINS];//计数器 记录tcache_entry链上空闲chunk的数目，每条链上最多有7个chunk
  tcache_entry *entries[TCACHE_MAX_BINS];//tcache_entry数组，每个都是以单向链表的方式链接了大小相同的的处于空闲状态或者说free 后的chunk
} tcache_perthread_struct;

static __thread bool tcache_shutting_down = false;
static __thread tcache_perthread_struct *tcache = NULL;

/* Caller must ensure that we know tc_idx is valid and there's room
   for more chunks.  */
/*将一个chunk放回tcache中*/
static __always_inline void
tcache_put (mchunkptr chunk, size_t tc_idx)
{
  /*chunk转为mem指针*/
  tcache_entry *e = (tcache_entry *) chunk2mem (chunk);

  /* Mark this chunk as "in the tcache" so the test in _int_free will
     detect a double free.  */
  //设置放回头部
  e->key = tcache;

  e->next = tcache->entries[tc_idx];
  tcache->entries[tc_idx] = e;
  ++(tcache->counts[tc_idx]);//计数器自增
}

/* Caller must ensure that we know tc_idx is valid and there's
   available chunks to remove.  */
/*传入index  获取对应tcache链的chunk*/
static __always_inline void *
tcache_get (size_t tc_idx)
{
  tcache_entry *e = tcache->entries[tc_idx];//得到对应的链表头
  tcache->entries[tc_idx] = e->next;//将第一个tcache拿掉
  --(tcache->counts[tc_idx]);//count减一
  e->key = NULL;//拿出的tcache chunk 其key指针从指向tcache_perthreadStruct结构体置为NULL
  return (void *) e;
}

static void
tcache_thread_shutdown (void)
{
  int i;
  tcache_perthread_struct *tcache_tmp = tcache;

  if (!tcache)
    return;

  /* Disable the tcache and prevent it from being reinitialized.  */
  tcache = NULL;
  tcache_shutting_down = true;

  /* Free all of the entries and the tcache itself back to the arena
     heap for coalescing.  */
  for (i = 0; i < TCACHE_MAX_BINS; ++i)
    {
      while (tcache_tmp->entries[i])
	{
	  tcache_entry *e = tcache_tmp->entries[i];
	  tcache_tmp->entries[i] = e->next;
	  __libc_free (e);
	}
    }

  __libc_free (tcache_tmp);
}
/*初始化tcache*/
static void
tcache_init(void)
{
  mstate ar_ptr;
  void *victim = 0;
  const size_t bytes = sizeof (tcache_perthread_struct);

  if (tcache_shutting_down)
    return;

  arena_get (ar_ptr, bytes);//找到可用arena
  victim = _int_malloc (ar_ptr, bytes);//申请一个chunk存放 tcache_prethread_struct
  if (!victim && ar_ptr != NULL)
    {
      ar_ptr = arena_get_retry (ar_ptr, bytes);
      victim = _int_malloc (ar_ptr, bytes);
    }


  if (ar_ptr != NULL)
    __libc_lock_unlock (ar_ptr->mutex);

  /* In a low memory situation, we may not be able to allocate memory
     - in which case, we just keep trying later.  However, we
     typically do this very early, so either there is sufficient
     memory, or there isn't enough memory to do non-trivial
     allocations anyway.  */
     /*初始化tcache*/
  if (victim)
    {
      tcache = (tcache_perthread_struct *) victim;
      memset (tcache, 0, sizeof (tcache_perthread_struct));
    }

}
/*MAYBE_INIT_TCACHE调用了tcache_init()初始化tcache*/
# define MAYBE_INIT_TCACHE() \
  if (__glibc_unlikely (tcache == NULL)) \
    tcache_init();

#else  /* !USE_TCACHE */
# define MAYBE_INIT_TCACHE()

static void
tcache_thread_shutdown (void)
{
  /* Nothing to do if there is no thread cache.  */
}

#endif /* !USE_TCACHE  */
/*malloc 在glibc的实现  其更底层是_int_malloc*/
void *
__libc_malloc (size_t bytes)
{
  mstate ar_ptr;/*malloc_state指针*/
  void *victim;

  _Static_assert (PTRDIFF_MAX <= SIZE_MAX / 2,
                  "PTRDIFF_MAX is not more than half of SIZE_MAX");
  /*检查是否有内存分配钩子*/
  void *(*hook) (size_t, const void *)
    = atomic_forced_read (__malloc_hook);
  if (__builtin_expect (hook != NULL, 0))
    return (*hook)(bytes, RETURN_ADDRESS (0));
#if USE_TCACHE
  /* int_free also calls request2size, be careful to not pad twice.  */

  size_t tbytes;
  /*根据传入的参数计算chunk大小，得到tcache对应的下标*/
  if (!checked_request2size (bytes, &tbytes))
    {
      __set_errno (ENOMEM);
      return NULL;
    }
  size_t tc_idx = csize2tidx (tbytes);/*转为tcache 的index*/
  /*初始化 tcache*/
  MAYBE_INIT_TCACHE ();

  DIAG_PUSH_NEEDS_COMMENT;
  if (tc_idx < mp_.tcache_bins//根据size得到的idx在合法的范围内
      && tcache
      && tcache->counts[tc_idx] > 0)//对应的tcache 链不为空
    {
      return tcache_get (tc_idx);//转入tcache获取
    }
  DIAG_POP_NEEDS_COMMENT;
#endif

  if (SINGLE_THREAD_P)
    {
      victim = _int_malloc (&main_arena, bytes);
      assert (!victim || chunk_is_mmapped (mem2chunk (victim)) ||
	      &main_arena == arena_for_chunk (mem2chunk (victim)));
      return victim;
    }
  /*寻找一个 arena 来试图分配内存，并上锁，宏位于arena.c */
  arena_get (ar_ptr, bytes);

  /*调用 _int_malloc 函数申请对应大小的内存*/
  victim = _int_malloc (ar_ptr, bytes);
  /* Retry with another arena only if we were able to find a usable arena
     before.  */
  if (!victim && ar_ptr != NULL)
  /*如果分配失败的话，ptmalloc 会再次尝试*/
    {
      LIBC_PROBE (memory_malloc_retry, 1, bytes);
      ar_ptr = arena_get_retry (ar_ptr, bytes);
      victim = _int_malloc (ar_ptr, bytes);
    }
  /*如果申请到了 arena，退出前解锁*/
  if (ar_ptr != NULL)
    __libc_lock_unlock (ar_ptr->mutex);
  /*断言目前状态满足以下条件之一*/
  /* 没有申请到内存
   * mmap的内存
   * 申请到的内存必须在其所分配的arena中
   */
  assert (!victim || chunk_is_mmapped (mem2chunk (victim)) ||
          ar_ptr == arena_for_chunk (mem2chunk (victim)));
  /*返回内存*/
  return victim;
}
libc_hidden_def (__libc_malloc)

void
__libc_free (void *mem)
{
  mstate ar_ptr;
  mchunkptr p;                          /* chunk corresponding to mem */
  /*是否有钩子函数 __free_hook*/
  void (*hook) (void *, const void *)
    = atomic_forced_read (__free_hook);
  if (__builtin_expect (hook != NULL, 0))
    {
      (*hook)(mem, RETURN_ADDRESS (0));
      return;
    }
  /*free一个NULL不起作用*/
  if (mem == 0)                              /* free(0) has no effect */
    return;
  /*mem指针转换为chunk指针*/
  p = mem2chunk (mem);
  /*内存是mmap得到的*/
  if (chunk_is_mmapped (p))                       /* release mmapped memory. */
    {
      /* See if the dynamic brk/mmap threshold needs adjusting.
	 Dumped fake mmapped chunks do not affect the threshold.  */
      if (!mp_.no_dyn_threshold
          && chunksize_nomask (p) > mp_.mmap_threshold
          && chunksize_nomask (p) <= DEFAULT_MMAP_THRESHOLD_MAX
	  && !DUMPED_MAIN_ARENA_CHUNK (p))
        {
          mp_.mmap_threshold = chunksize (p);
          mp_.trim_threshold = 2 * mp_.mmap_threshold;
          LIBC_PROBE (memory_mallopt_free_dyn_thresholds, 2,
                      mp_.mmap_threshold, mp_.trim_threshold);
        }
      munmap_chunk (p);
      return;
    }
  /*初始化tcache MAYBE_INIT_TCACHE()在tcache不为空时无作用*/
  MAYBE_INIT_TCACHE ();
  /*根据chunk获得分配区的指针*/
  ar_ptr = arena_for_chunk (p);
  /*调用_int_free进行释放*/
  _int_free (ar_ptr, p, 0);
}
libc_hidden_def (__libc_free)

void *
__libc_realloc (void *oldmem, size_t bytes)
{
  mstate ar_ptr;
  INTERNAL_SIZE_T nb;         /* padded request size */

  void *newp;             /* chunk to return */

  void *(*hook) (void *, size_t, const void *) =
    atomic_forced_read (__realloc_hook);
  if (__builtin_expect (hook != NULL, 0))
    return (*hook)(oldmem, bytes, RETURN_ADDRESS (0));

#if REALLOC_ZERO_BYTES_FREES
  if (bytes == 0 && oldmem != NULL)
    {
      __libc_free (oldmem); return 0;
    }
#endif

  /* realloc of null is supposed to be same as malloc */
  if (oldmem == 0)
    return __libc_malloc (bytes);

  /* chunk corresponding to oldmem */
  const mchunkptr oldp = mem2chunk (oldmem);
  /* its size */
  const INTERNAL_SIZE_T oldsize = chunksize (oldp);

  if (chunk_is_mmapped (oldp))
    ar_ptr = NULL;
  else
    {
      MAYBE_INIT_TCACHE ();
      ar_ptr = arena_for_chunk (oldp);
    }

  /* Little security check which won't hurt performance: the allocator
     never wrapps around at the end of the address space.  Therefore
     we can exclude some size values which might appear here by
     accident or by "design" from some intruder.  We need to bypass
     this check for dumped fake mmap chunks from the old main arena
     because the new malloc may provide additional alignment.  */
  if ((__builtin_expect ((uintptr_t) oldp > (uintptr_t) -oldsize, 0)
       || __builtin_expect (misaligned_chunk (oldp), 0))
      && !DUMPED_MAIN_ARENA_CHUNK (oldp))
      malloc_printerr ("realloc(): invalid pointer");

  if (!checked_request2size (bytes, &nb))
    {
      __set_errno (ENOMEM);
      return NULL;
    }

  if (chunk_is_mmapped (oldp))
    {
      /* If this is a faked mmapped chunk from the dumped main arena,
	 always make a copy (and do not free the old chunk).  */
      if (DUMPED_MAIN_ARENA_CHUNK (oldp))
	{
	  /* Must alloc, copy, free. */
	  void *newmem = __libc_malloc (bytes);
	  if (newmem == 0)
	    return NULL;
	  /* Copy as many bytes as are available from the old chunk
	     and fit into the new size.  NB: The overhead for faked
	     mmapped chunks is only SIZE_SZ, not 2 * SIZE_SZ as for
	     regular mmapped chunks.  */
	  if (bytes > oldsize - SIZE_SZ)
	    bytes = oldsize - SIZE_SZ;
	  memcpy (newmem, oldmem, bytes);
	  return newmem;
	}

      void *newmem;

#if HAVE_MREMAP
      newp = mremap_chunk (oldp, nb);
      if (newp)
        return chunk2mem (newp);
#endif
      /* Note the extra SIZE_SZ overhead. */
      if (oldsize - SIZE_SZ >= nb)
        return oldmem;                         /* do nothing */

      /* Must alloc, copy, free. */
      newmem = __libc_malloc (bytes);
      if (newmem == 0)
        return 0;              /* propagate failure */

      memcpy (newmem, oldmem, oldsize - 2 * SIZE_SZ);
      munmap_chunk (oldp);
      return newmem;
    }

  if (SINGLE_THREAD_P)
    {
      newp = _int_realloc (ar_ptr, oldp, oldsize, nb);
      assert (!newp || chunk_is_mmapped (mem2chunk (newp)) ||
	      ar_ptr == arena_for_chunk (mem2chunk (newp)));

      return newp;
    }

  __libc_lock_lock (ar_ptr->mutex);

  newp = _int_realloc (ar_ptr, oldp, oldsize, nb);

  __libc_lock_unlock (ar_ptr->mutex);
  assert (!newp || chunk_is_mmapped (mem2chunk (newp)) ||
          ar_ptr == arena_for_chunk (mem2chunk (newp)));

  if (newp == NULL)
    {
      /* Try harder to allocate memory in other arenas.  */
      LIBC_PROBE (memory_realloc_retry, 2, bytes, oldmem);
      newp = __libc_malloc (bytes);
      if (newp != NULL)
        {
          memcpy (newp, oldmem, oldsize - SIZE_SZ);
          _int_free (ar_ptr, oldp, 0);
        }
    }

  return newp;
}
libc_hidden_def (__libc_realloc)

void *
__libc_memalign (size_t alignment, size_t bytes)
{
  void *address = RETURN_ADDRESS (0);
  return _mid_memalign (alignment, bytes, address);
}

static void *
_mid_memalign (size_t alignment, size_t bytes, void *address)
{
  mstate ar_ptr;
  void *p;

  void *(*hook) (size_t, size_t, const void *) =
    atomic_forced_read (__memalign_hook);
  if (__builtin_expect (hook != NULL, 0))
    return (*hook)(alignment, bytes, address);

  /* If we need less alignment than we give anyway, just relay to malloc.  */
  if (alignment <= MALLOC_ALIGNMENT)
    return __libc_malloc (bytes);

  /* Otherwise, ensure that it is at least a minimum chunk size */
  if (alignment < MINSIZE)
    alignment = MINSIZE;

  /* If the alignment is greater than SIZE_MAX / 2 + 1 it cannot be a
     power of 2 and will cause overflow in the check below.  */
  if (alignment > SIZE_MAX / 2 + 1)
    {
      __set_errno (EINVAL);
      return 0;
    }


  /* Make sure alignment is power of 2.  */
  if (!powerof2 (alignment))
    {
      size_t a = MALLOC_ALIGNMENT * 2;
      while (a < alignment)
        a <<= 1;
      alignment = a;
    }

  if (SINGLE_THREAD_P)
    {
      p = _int_memalign (&main_arena, alignment, bytes);
      assert (!p || chunk_is_mmapped (mem2chunk (p)) ||
	      &main_arena == arena_for_chunk (mem2chunk (p)));

      return p;
    }

  arena_get (ar_ptr, bytes + alignment + MINSIZE);

  p = _int_memalign (ar_ptr, alignment, bytes);
  if (!p && ar_ptr != NULL)
    {
      LIBC_PROBE (memory_memalign_retry, 2, bytes, alignment);
      ar_ptr = arena_get_retry (ar_ptr, bytes);
      p = _int_memalign (ar_ptr, alignment, bytes);
    }

  if (ar_ptr != NULL)
    __libc_lock_unlock (ar_ptr->mutex);

  assert (!p || chunk_is_mmapped (mem2chunk (p)) ||
          ar_ptr == arena_for_chunk (mem2chunk (p)));
  return p;
}
/* For ISO C11.  */
weak_alias (__libc_memalign, aligned_alloc)
libc_hidden_def (__libc_memalign)

void *
__libc_valloc (size_t bytes)
{
  if (__malloc_initialized < 0)
    ptmalloc_init ();

  void *address = RETURN_ADDRESS (0);
  size_t pagesize = GLRO (dl_pagesize);
  return _mid_memalign (pagesize, bytes, address);
}

void *
__libc_pvalloc (size_t bytes)
{
  if (__malloc_initialized < 0)
    ptmalloc_init ();

  void *address = RETURN_ADDRESS (0);
  size_t pagesize = GLRO (dl_pagesize);
  size_t rounded_bytes;
  /* ALIGN_UP with overflow check.  */
  if (__glibc_unlikely (__builtin_add_overflow (bytes,
						pagesize - 1,
						&rounded_bytes)))
    {
      __set_errno (ENOMEM);
      return 0;
    }
  rounded_bytes = rounded_bytes & -(pagesize - 1);

  return _mid_memalign (pagesize, rounded_bytes, address);
}

void *
__libc_calloc (size_t n, size_t elem_size)
{
  mstate av;
  mchunkptr oldtop, p;
  INTERNAL_SIZE_T sz, csz, oldtopsize;
  void *mem;
  unsigned long clearsize;
  unsigned long nclears;
  INTERNAL_SIZE_T *d;
  ptrdiff_t bytes;

  if (__glibc_unlikely (__builtin_mul_overflow (n, elem_size, &bytes)))
    {
       __set_errno (ENOMEM);
       return NULL;
    }
  sz = bytes;

  void *(*hook) (size_t, const void *) =
    atomic_forced_read (__malloc_hook);
  if (__builtin_expect (hook != NULL, 0))
    {
      mem = (*hook)(sz, RETURN_ADDRESS (0));
      if (mem == 0)
        return 0;

      return memset (mem, 0, sz);
    }

  MAYBE_INIT_TCACHE ();

  if (SINGLE_THREAD_P)
    av = &main_arena;
  else
    arena_get (av, sz);

  if (av)
    {
      /* Check if we hand out the top chunk, in which case there may be no
	 need to clear. */
#if MORECORE_CLEARS
      oldtop = top (av);
      oldtopsize = chunksize (top (av));
# if MORECORE_CLEARS < 2
      /* Only newly allocated memory is guaranteed to be cleared.  */
      if (av == &main_arena &&
	  oldtopsize < mp_.sbrk_base + av->max_system_mem - (char *) oldtop)
	oldtopsize = (mp_.sbrk_base + av->max_system_mem - (char *) oldtop);
# endif
      if (av != &main_arena)
	{
	  heap_info *heap = heap_for_ptr (oldtop);
	  if (oldtopsize < (char *) heap + heap->mprotect_size - (char *) oldtop)
	    oldtopsize = (char *) heap + heap->mprotect_size - (char *) oldtop;
	}
#endif
    }
  else
    {
      /* No usable arenas.  */
      oldtop = 0;
      oldtopsize = 0;
    }
  mem = _int_malloc (av, sz);

  assert (!mem || chunk_is_mmapped (mem2chunk (mem)) ||
          av == arena_for_chunk (mem2chunk (mem)));

  if (!SINGLE_THREAD_P)
    {
      if (mem == 0 && av != NULL)
	{
	  LIBC_PROBE (memory_calloc_retry, 1, sz);
	  av = arena_get_retry (av, sz);
	  mem = _int_malloc (av, sz);
	}

      if (av != NULL)
	__libc_lock_unlock (av->mutex);
    }

  /* Allocation failed even after a retry.  */
  if (mem == 0)
    return 0;

  p = mem2chunk (mem);

  /* Two optional cases in which clearing not necessary */
  if (chunk_is_mmapped (p))
    {
      if (__builtin_expect (perturb_byte, 0))
        return memset (mem, 0, sz);

      return mem;
    }

  csz = chunksize (p);

#if MORECORE_CLEARS
  if (perturb_byte == 0 && (p == oldtop && csz > oldtopsize))
    {
      /* clear only the bytes from non-freshly-sbrked memory */
      csz = oldtopsize;
    }
#endif

  /* Unroll clear of <= 36 bytes (72 if 8byte sizes).  We know that
     contents have an odd number of INTERNAL_SIZE_T-sized words;
     minimally 3.  */
  d = (INTERNAL_SIZE_T *) mem;
  clearsize = csz - SIZE_SZ;
  nclears = clearsize / sizeof (INTERNAL_SIZE_T);
  assert (nclears >= 3);

  if (nclears > 9)
    return memset (d, 0, clearsize);

  else
    {
      *(d + 0) = 0;
      *(d + 1) = 0;
      *(d + 2) = 0;
      if (nclears > 4)
        {
          *(d + 3) = 0;
          *(d + 4) = 0;
          if (nclears > 6)
            {
              *(d + 5) = 0;
              *(d + 6) = 0;
              if (nclears > 8)
                {
                  *(d + 7) = 0;
                  *(d + 8) = 0;
                }
            }
        }
    }

  return mem;
}

/*
   ------------------------------ malloc ------------------------------
 */
/*真实的malloc存在*/
static void *
_int_malloc (mstate av, size_t bytes)
{
  INTERNAL_SIZE_T nb;               /* normalized request size */
  unsigned int idx;                 /* associated bin index */
  mbinptr bin;                      /* associated bin */

  mchunkptr victim;                 /* inspected/selected chunk */
  INTERNAL_SIZE_T size;             /* its size */
  int victim_index;                 /* its bin index */

  mchunkptr remainder;              /* remainder from a split */
  unsigned long remainder_size;     /* its size */

  unsigned int block;               /* bit map traverser */
  unsigned int bit;                 /* bit map traverser */
  unsigned int map;                 /* current word of binmap */

  mchunkptr fwd;                    /* misc temp for linking */
  mchunkptr bck;                    /* misc temp for linking */

#if USE_TCACHE
  size_t tcache_unsorted_count;	    /* count of unsorted chunks processed */
#endif

  /*
     Convert request size to internal form by adding SIZE_SZ bytes
     overhead plus possibly more to obtain necessary alignment and/or
     to obtain a size of at least MINSIZE, the smallest allocatable
     size. Also, checked_request2size returns false for request sizes
     that are so large that they wrap around zero when padded and
     aligned.
   */
  /*检查请求  将请求大小转为chunk对齐后的大小*/
  if (!checked_request2size (bytes, &nb))
    {
      __set_errno (ENOMEM);
      return NULL;
    }

  /* There are no usable arenas.  Fall back to sysmalloc to get a chunk from
     mmap.  */
  /*无可用arena*/
  if (__glibc_unlikely (av == NULL))
    {
      /*调用sysmalloc申请*/
      void *p = sysmalloc (nb, av);
      if (p != NULL)
	alloc_perturb (p, bytes);
      return p;
    }

  /*
     If the size qualifies as a fastbin, first check corresponding bin.
     This code is safe to execute even if av is not yet initialized, so we
     can try it without checking, which saves some time on this fast path.
   */

#define REMOVE_FB(fb, victim, pp)			\
  do							\
    {							\
      victim = pp;					\
      if (victim == NULL)				\
	break;						\
    }							\
  while ((pp = catomic_compare_and_exchange_val_acq (fb, victim->fd, victim)) \
	 != victim);					\
  /*如果大小小于fastbin，从fastbin中获取合适大小*/
  if ((unsigned long) (nb) <= (unsigned long) (get_max_fast ()))
    {
      /*计算大小对应的fastbin 索引*/
      idx = fastbin_index (nb);
      /*对应的fastbin链的头指针*/
      mfastbinptr *fb = &fastbin (av, idx);
      mchunkptr pp;
      victim = *fb;
      /*判断此fastbin链是不是空的*/
      if (victim != NULL)
	{
	  if (SINGLE_THREAD_P)
	    *fb = victim->fd;
	  else
    /*利用fd遍历对应的bin内是否有空闲的chunk块*/
	    REMOVE_FB (fb, pp, victim);
    /*存在一个可用的chunk*/
	  if (__glibc_likely (victim != NULL))
	    {
        /*判断这个chunk的size与其fastbin链所在的索引index是否是相对应的*/
        /*就是这里导致了在fastbin伪造chunk时由write-anything-anywhere转为了受限地址的任意写入*/
	      size_t victim_idx = fastbin_index (chunksize (victim));
	      if (__builtin_expect (victim_idx != idx, 0))
		malloc_printerr ("malloc(): memory corruption (fast)");
        /*调试状态下进行的细致的检查*/
	      check_remalloced_chunk (av, victim, nb);
        /*这部分是使用tcache的逻辑*/
#if USE_TCACHE
	      /* While we're here, if we see other chunks of the same size,
		 stash them in the tcache.  */
     /*找对应的tcache索引*/
	      size_t tc_idx = csize2tidx (nb);
	      if (tcache && tc_idx < mp_.tcache_bins)
		{
		  mchunkptr tc_victim;

		  /* While bin not empty and tcache not full, copy chunks.  */
      /*tcache没满 放进去填满*/
		  while (tcache->counts[tc_idx] < mp_.tcache_count
			 && (tc_victim = *fb) != NULL)
		    {
		      if (SINGLE_THREAD_P)
			*fb = tc_victim->fd;
		      else
			{
			  REMOVE_FB (fb, pp, tc_victim);
			  if (__glibc_unlikely (tc_victim == NULL))
			    break;
			}
		      tcache_put (tc_victim, tc_idx);
		    }
		}
#endif
        /*找到可用chunk  转为mem指针输出*/
	      void *p = chunk2mem (victim);
        /* 如果设置了perturb_type, 则将获取到的chunk初始化为 perturb_type ^ 0xff*/
	      alloc_perturb (p, bytes);
	      return p;
	    }
	}
    }

  /*
     If a small request, check regular bin.  Since these "smallbins"
     hold one size each, no searching within bins is necessary.
     (For a large request, we need to wait until unsorted chunks are
     processed to find best fit. But for small ones, fits are exact
     anyway, so we can check now, which is faster.)
   */
  /*如果大小位于small bin中的话  得到位于的smallbin索引 */
  if (in_smallbin_range (nb))
    {
      /*获取small bin对应的索引*/
      idx = smallbin_index (nb);
      /*找到对应的bin*/
      bin = bin_at (av, idx);
      /*获取其最后一个chunk 如果为bin 则说明bin是空的 否则有下面两个情况*/
      if ((victim = last (bin)) != bin)
        {
          bck = victim->bk;
        /*bin不为空  检查这个chunk的尾部chunk的前一个chunk 防止出现伪造*/
	  if (__glibc_unlikely (bck->fd != victim))
	    malloc_printerr ("malloc(): smallbin double linked list corrupted");
          /*设置inuse位*/
          set_inuse_bit_at_offset (victim, nb);
          /* 修改对应的bin链表将 small bin 的最后一个 chunk 取出来*/
          bin->bk = bck;
          bck->fd = bin;

          if (av != &main_arena)
	    set_non_main_arena (victim);
      /*调试状态下进行的细致的检查*/
          check_malloced_chunk (av, victim, nb);
#if USE_TCACHE
	  /* While we're here, if we see other chunks of the same size,
	     stash them in the tcache.  */
	  size_t tc_idx = csize2tidx (nb);
	  if (tcache && tc_idx < mp_.tcache_bins)
	    {
	      mchunkptr tc_victim;

	      /* While bin not empty and tcache not full, copy chunks over.  */
	      while (tcache->counts[tc_idx] < mp_.tcache_count
		     && (tc_victim = last (bin)) != bin)
		{
		  if (tc_victim != 0)
		    {
		      bck = tc_victim->bk;
		      set_inuse_bit_at_offset (tc_victim, nb);
		      if (av != &main_arena)
			set_non_main_arena (tc_victim);
		      bin->bk = bck;
		      bck->fd = bin;

		      tcache_put (tc_victim, tc_idx);
	            }
		}
	    }
#endif
          /*找到可用chunk  转为mem指针输出*/
          void *p = chunk2mem (victim);
          /* 如果设置了perturb_type, 则将获取到的chunk初始化为 perturb_type ^ 0xff*/
          alloc_perturb (p, bytes);
          return p;
        }
    }

  /*
     If this is a large request, consolidate fastbins before continuing.
     While it might look excessive to kill all fastbins before
     even seeing if there is space available, this avoids
     fragmentation problems normally associated with fastbins.
     Also, in practice, programs tend to have runs of either small or
     large requests, but less often mixtures, so consolidation is not
     invoked all that often in most programs. And the programs that
     it is called frequently in otherwise tend to fragment.
   */

  else
    {
      /*获取size对应的large bin中的索引*/
      idx = largebin_index (nb);
      /*如果存在fastbin的话，会处理(合并所有的)fastbin */
      if (atomic_load_relaxed (&av->have_fastchunks))
        malloc_consolidate (av);
    }

  /*
     Process recently freed or remaindered chunks, taking one only if
     it is exact fit, or, if this a small request, the chunk is remainder from
     the most recent non-exact fit.  Place other traversed chunks in
     bins.  Note that this step is the only place in any routine where
     chunks are placed in bins.

     The outer loop here is needed because we might not realize until
     near the end of malloc that we should have consolidated, so must
     do so and retry. This happens at most once, and only when we would
     otherwise need to expand memory to service a "small" request.
   */

#if USE_TCACHE
  INTERNAL_SIZE_T tcache_nb = 0;
  size_t tc_idx = csize2tidx (nb);
  if (tcache && tc_idx < mp_.tcache_bins)
    tcache_nb = nb;
  int return_cached = 0;

  tcache_unsorted_count = 0;
#endif
  /*大循环遍历unsorted bin  大致逻辑如下*/
  /*
   * 按照 FIFO 的方式逐个将 unsorted bin 中的 chunk 取出来
   * 如果是 small request，则考虑是不是恰好满足，是的话，直接返回
   * 如果不是的话，放到对应的 bin 中
   * 尝试从 large bin 中分配用户所需的内存
   *  -- from ctf-wiki  不一定正确
   */
  for (;; )
    {
      int iters = 0;
      /*unsorted bin不为空时*/
      while ((victim = unsorted_chunks (av)->bk) != unsorted_chunks (av))
        {
          /*victim为unsorted bin的最后一个chunk*/
          /*bck设置为unsorted bin的倒数第二一个chunk*/
          bck = victim->bk;
          /*获取chunk的size*/
          size = chunksize (victim);
          /*获取下一个chunk*/
          mchunkptr next = chunk_at_offset (victim, size);
          /*判断当前chunk以及下一个chunk各项信息是否合法*/
          if (__glibc_unlikely (size <= 2 * SIZE_SZ)
              || __glibc_unlikely (size > av->system_mem))
            malloc_printerr ("malloc(): invalid size (unsorted)");
          if (__glibc_unlikely (chunksize_nomask (next) < 2 * SIZE_SZ)
              || __glibc_unlikely (chunksize_nomask (next) > av->system_mem))
            malloc_printerr ("malloc(): invalid next size (unsorted)");
          if (__glibc_unlikely ((prev_size (next) & ~(SIZE_BITS)) != size))
            malloc_printerr ("malloc(): mismatching next->prev_size (unsorted)");
          if (__glibc_unlikely (bck->fd != victim)
              || __glibc_unlikely (victim->fd != unsorted_chunks (av)))
            malloc_printerr ("malloc(): unsorted double linked list corrupted");
          if (__glibc_unlikely (prev_inuse (next)))
            malloc_printerr ("malloc(): invalid next->prev_inuse (unsorted)");

          /*
             If a small request, try to use last remainder if it is the
             only chunk in unsorted bin.  This helps promote locality for
             runs of consecutive small requests. This is the only
             exception to best-fit, and applies only when there is
             no exact fit for a small chunk.
           */
          /*用户的请求为 small bin chunk  先考虑是否unsorted bin中只有last remainder*/
          if (in_smallbin_range (nb) &&
              bck == unsorted_chunks (av) &&
              victim == av->last_remainder &&
              (unsigned long) (size) > (unsigned long) (nb + MINSIZE))
            {
              /* split and reattach remainder */
              /*新remainder的大小*/
              remainder_size = size - nb;
              /*获取新的 remainder 的位置*/
              remainder = chunk_at_offset (victim, nb);
              /*更新unsorted bin*/
              unsorted_chunks (av)->bk = unsorted_chunks (av)->fd = remainder;
              /*更新malloc_state中记录的last remainder*/
              av->last_remainder = remainder;
              /*更新remainder的指针*/
              remainder->bk = remainder->fd = unsorted_chunks (av);
              if (!in_smallbin_range (remainder_size))
                {
                  remainder->fd_nextsize = NULL;
                  remainder->bk_nextsize = NULL;
                }
              /*设置victim chunk的头部*/
              set_head (victim, nb | PREV_INUSE |
                        (av != &main_arena ? NON_MAIN_ARENA : 0));
              /*设置 remainder 的头部*/
              set_head (remainder, remainder_size | PREV_INUSE);
              /*设置记录remainder大小的prev_size字段，因为处于空闲状态*/
              set_foot (remainder, remainder_size);

              check_malloced_chunk (av, victim, nb);
              /*chunk转为mem指针*/
              void *p = chunk2mem (victim);
              /*chunk初始化*/
              alloc_perturb (p, bytes);
              return p;
            }

          /* remove from unsorted list */
          if (__glibc_unlikely (bck->fd != victim))
            malloc_printerr ("malloc(): corrupted unsorted chunks 3");
          /*取出chunk*/
          unsorted_chunks (av)->bk = bck;
          bck->fd = unsorted_chunks (av);

          /* Take now instead of binning if exact fit */
          /*如果大小合适  直接使用*/
          if (size == nb)
            {
              /*设置*/
              set_inuse_bit_at_offset (victim, size);
              if (av != &main_arena)
		set_non_main_arena (victim);
#if USE_TCACHE
	      /* Fill cache first, return to user only if cache fills.
		 We may return one of these chunks later.  */
	      if (tcache_nb
		  && tcache->counts[tc_idx] < mp_.tcache_count)
		{
		  tcache_put (victim, tc_idx);
		  return_cached = 1;
		  continue;
		}
	      else
		{
#endif
              /* 调试时细致检查*/
              check_malloced_chunk (av, victim, nb);
              /*chunk转为mem指针返回*/
              void *p = chunk2mem (victim);
              alloc_perturb (p, bytes);
              return p;
#if USE_TCACHE
		}
#endif
            }
          /*当前chunk不符合要求时  整理chunk*/
          /* place chunk in bin */
          /*如果chunk在small bin大小范围  整理后放回small bin*/
          if (in_smallbin_range (size))
            {
              victim_index = smallbin_index (size);
              bck = bin_at (av, victim_index);
              fwd = bck->fd;
            }
          else/*否则 取出来放入large bin  下面是对应的放入large bin逻辑*/
            {
              /*对应的large bin的索引*/
              victim_index = largebin_index (size);
              bck = bin_at (av, victim_index);// 当前的large bin 的头部
              fwd = bck->fd;

              /* maintain large bins in sorted order */
              /*下面部分暗示了每个  large bin是从大到小排列的*/
              /*large bin不是空的*/
              if (fwd != bck)
                {
                  /* Or with inuse bit to speed comparisons */
                  size |= PREV_INUSE;
                  /* if smaller than smallest, bypass loop below */
                  /*bck->bk 存储着相应 large bin 中最小的chunk  如果比这个还小，只需要插入到链表尾部*/
                  assert (chunk_main_arena (bck->bk));
                  if ((unsigned long) (size)
		      < (unsigned long) chunksize_nomask (bck->bk))
                    {
                      fwd = bck;//fwd 指向 large bin 头部
                      bck = bck->bk;//bck 指向 largin bin 尾部 chunk

                      victim->fd_nextsize = fwd->fd;//victim 的 fd_nextsize 指向 largin bin 的第一个 chunk
                      victim->bk_nextsize = fwd->fd->bk_nextsize;//victim 的 bk_nextsize 指向原来链表的第一个 chunk 指向的 bk_nextsize
                      /*原来链表的第一个 chunk 的 bk_nextsize 指向 victim*/
                      /*原来指向链表第一个 chunk 的 fd_nextsize 指向 victim*/ 
                      fwd->fd->bk_nextsize = victim->bk_nextsize->fd_nextsize = victim;
                    }
                  else//当前要插入的 victim 的大小大于最小的 chunk
                    {
                      assert (chunk_main_arena (fwd));
                      //从链表头部开始找比victim小于或等于的chunk
                      while ((unsigned long) size < chunksize_nomask (fwd))
                        {
                          fwd = fwd->fd_nextsize;
			  assert (chunk_main_arena (fwd));
                        }
                      /*如果找到个相等的，就直接将 chunk 插入到该chunk的后面，不改变nextsize指针*/
                      if ((unsigned long) size
			  == (unsigned long) chunksize_nomask (fwd))
                        /* Always insert in the second position.  */
                        fwd = fwd->fd;
                      else//如果找到的chunk和当前victim大小不一样，需要构造双向链表将当前的chunk链进去
                        {
                          victim->fd_nextsize = fwd;
                          victim->bk_nextsize = fwd->bk_nextsize;
                          if (__glibc_unlikely (fwd->bk_nextsize->fd_nextsize != fwd))
                            malloc_printerr ("malloc(): largebin double linked list corrupted (nextsize)");
                          fwd->bk_nextsize = victim;
                          victim->bk_nextsize->fd_nextsize = victim;
                        }
                      bck = fwd->bk;
                      if (bck->fd != fwd)
                        malloc_printerr ("malloc(): largebin double linked list corrupted (bk)");
                    }
                }
              else//对应的large bin是空的话，直接让fd_nextsize与bk_nextsize构成双向链表
                victim->fd_nextsize = victim->bk_nextsize = victim;
            }
          /*对应的 bin的binmap进行标记，并修改这个bin的链表*/
          mark_bin (av, victim_index);
          victim->bk = bck;
          victim->fd = fwd;
          fwd->bk = victim;
          bck->fd = victim;

#if USE_TCACHE
      /* If we've processed as many chunks as we're allowed while
	 filling the cache, return one of the cached ones.  */
      ++tcache_unsorted_count;
      if (return_cached
	  && mp_.tcache_unsorted_limit > 0
	  && tcache_unsorted_count > mp_.tcache_unsorted_limit)
	{
	  return tcache_get (tc_idx);
	}
#endif
        /*这个清理过程最多迭代10000次  超过后退出*/
#define MAX_ITERS       10000
          if (++iters >= MAX_ITERS)
            break;
        }

#if USE_TCACHE
      /* If all the small chunks we found ended up cached, return one now.  */
      if (return_cached)
	{
	  return tcache_get (tc_idx);
	}
#endif

      /*
         If a large request, scan through the chunks of current bin in
         sorted order to find smallest that fits.  Use the skip list for this.
       */
      /*明确一下  进行到这里 前面从unsorted bin中对chunk进行了回收 仍然没发现可用的chunk   这意味着small bin中的chunk都不符合*/
      /*在large chunk中开始搜索*/
      if (!in_smallbin_range (nb))
        {
          /*获得对应的bin*/
          bin = bin_at (av, idx);

          /* skip scan if empty or largest chunk is too small */
          //如果对应的 bin为空或者其中的chunk最大的也很小，那就跳过
          if ((victim = first (bin)) != bin
	      && (unsigned long) chunksize_nomask (victim)
	        >= (unsigned long) (nb))
            {
              /*反向遍历链表 直到找到一个不小于需要申请大小的chunk*/
              victim = victim->bk_nextsize;
              while (((unsigned long) (size = chunksize (victim)) <
                      (unsigned long) (nb)))
                victim = victim->bk_nextsize;

              /* Avoid removing the first entry for a size so that the skip
                 list does not have to be rerouted.  */
              /* 这边是一个简化操作的技巧  如果找到的chunk并不是最后一个 而且其前面还有大小与它相同的，那就
               * 取它前面的那个chunk  这样就可以避免调整bk_nextsize
               * 因为一个bin中同样大小的chunk 只有一个会被链在bk_nextsize上
               */
              if (victim != last (bin)
		  && chunksize_nomask (victim)
		    == chunksize_nomask (victim->fd))
                victim = victim->fd;
              /*计算分配和剩余的大小*/
              remainder_size = size - nb;
              /*将chunk unlink出来*/
              unlink_chunk (av, victim);

              /* Exhaust */
              /*剩下的大小不足以当做一个chunk*/
              if (remainder_size < MINSIZE)
                {
                  /*好像并没进行其他操作*/
                  set_inuse_bit_at_offset (victim, size);
                  if (av != &main_arena)
		    set_non_main_arena (victim);
                }
              /* Split */
              else
                {
                  /*如果剩下的还能构成一个chunk  就分裂它  获取剩下的部分的指针 */
                  remainder = chunk_at_offset (victim, nb);
                  /* We cannot assume the unsorted list is empty and therefore
                     have to perform a complete insert here.  */
                  /*获取unsorted bin的头部与第一个chunk，准备链入unsorted bin*/
                  bck = unsorted_chunks (av);
                  fwd = bck->fd;
                  /*先判断unsorted bin是不是被破坏了*/
		  if (__glibc_unlikely (fwd->bk != bck))
		    malloc_printerr ("malloc(): corrupted unsorted chunks");
                  /*链入unsorted bin*/
                  remainder->bk = bck;
                  remainder->fd = fwd;
                  bck->fd = remainder;
                  fwd->bk = remainder;
                  /*这个剩余的chunk如果不在small bin范围的话 设置对应的字段*/
                  if (!in_smallbin_range (remainder_size))
                    {
                      remainder->fd_nextsize = NULL;
                      remainder->bk_nextsize = NULL;
                    }
                  /*设置分裂出来的两个chunk的头部*/
                  set_head (victim, nb | PREV_INUSE |
                            (av != &main_arena ? NON_MAIN_ARENA : 0));
                  set_head (remainder, remainder_size | PREV_INUSE);
                  /*设置remainder的大小*/
                  set_foot (remainder, remainder_size);
                }
              /*老三样*/
              /*调试时会进行的细致检查*/
              check_malloced_chunk (av, victim, nb);
              /*chunk指针转为mem指针*/
              void *p = chunk2mem (victim);
              /*chunk初始填充*/
              alloc_perturb (p, bytes);
              return p;
            }
        }

      /*
         Search for a chunk by scanning bins, starting with next largest
         bin. This search is strictly by best-fit; i.e., the smallest
         (with ties going to approximately the least recently used) chunk
         that fits is selected.

         The bitmap avoids needing to check that most blocks are nonempty.
         The particular case of skipping all bins during warm-up phases
         when no chunks have been returned yet is faster than it might look.
       */
      /*到这里  如果还没找到可用的chunk  意味着large bin也无能为力*/
      /*尝试增加idx来寻找*/
      ++idx;
      /*获取对应的bin*/
      bin = bin_at (av, idx);
      /*这一段有点没懂  binmap是标记bin是否为空的 这里是可以通过这样对所有的bin做统一处理吗？*/
      /*当前索引在binmap中的block索引  从而找到对应的map #define idx2block(i) ((i) >> BINMAPSHIFT)  ,BINMAPSHIFT=5
       *前面说过一个map是32位  所以需要右移5
       */
      block = idx2block (idx);
      map = av->binmap[block];
      bit = idx2bit (idx);//idx对应的位设置为1，其它位为0
      /*上述过程就是计算了idx应该对应的map 以及自己的bit  以进行比较*/

      for (;; )
        {
          /* Skip rest of block if there are no more set bits in this block.  */
          /*如果bit>map，则表示该 map 中没有比当前所需要chunk大的空闲块*/
          /*而如果bit等于0 那表示上面idx2bit的参数为0*/
          if (bit > map || bit == 0)
            {
              do/*寻找下一个block，直到其对应的map不为0*/
                {
                  /*如果bllock超出了binmap的大小  说明还是找不到 那就需要去top chunk找了*/
                  if (++block >= BINMAPSIZE) /* out of bins */
                    goto use_top;
                }
              while ((map = av->binmap[block]) == 0);
              /* 找到了map，获取对应的bin，因为该map中的chunk大小都比所需的chunk大(idx先自增了)，而且
               * map本身不为0，所以这个map所在的所有bin中必然存在合适的一个chunk。*/
              bin = bin_at (av, (block << BINMAPSHIFT));/*这个bin是map的最小bin  因为index最小*/
              bit = 1;
            }

          /* Advance to bin with set bit. There must be one. */
          /*从最小bin找，直到找到合适的bin  前面说过 这个过程必然能找到*/
          while ((bit & map) == 0)
            {
              bin = next_bin (bin);
              bit <<= 1;
              assert (bit != 0);
            }

          /* Inspect the bin. It is likely to be non-empty */
          /*获取对应的bin*/
          victim = last (bin);

          /*  If a false alarm (empty bin), clear the bit. */
          /*如果victim=bin，那么我们就将map对应的位清0，然后获取下一个bin*/
          /*victim等于bin说明这个bin是空的 不可能啊？*/
          if (victim == bin)
            {
              av->binmap[block] = map &= ~bit; /* Write through */
              bin = next_bin (bin);
              bit <<= 1;
            }

          else
            {
              /*获取chunk的size*/
              size = chunksize (victim);

              /*  We know the first chunk in this bin is big enough to use. */
              assert ((unsigned long) (size) >= (unsigned long) (nb));
              /*计算分割后剩余的大小*/
              remainder_size = size - nb;

              /* unlink */
              unlink_chunk (av, victim);

              /* Exhaust */
              /*跟上面逻辑一样 分割后不够一个chunk的  这也没写怎么处理的呀？*/
              if (remainder_size < MINSIZE)
                {
                  set_inuse_bit_at_offset (victim, size);
                  if (av != &main_arena)
		    set_non_main_arena (victim);
                }

              /* Split */
              else/*分裂后还能够一个chunk  那就分裂它*/
                {/*计算剩余的chunk的偏移*/
                  remainder = chunk_at_offset (victim, nb);

                  /* We cannot assume the unsorted list is empty and therefore
                     have to perform a complete insert here.  */
                  /*将剩余的chunk插入到unsorted bin*/
                  bck = unsorted_chunks (av);
                  fwd = bck->fd;
		  if (__glibc_unlikely (fwd->bk != bck))
		    malloc_printerr ("malloc(): corrupted unsorted chunks 2");
                  remainder->bk = bck;
                  remainder->fd = fwd;
                  bck->fd = remainder;
                  fwd->bk = remainder;

                  /* advertise as last remainder */
                  /* 如果在small bin范围内，就将其标记为remainder*/
                  if (in_smallbin_range (nb))
                    av->last_remainder = remainder;
                  if (!in_smallbin_range (remainder_size))
                    {
                      remainder->fd_nextsize = NULL;
                      remainder->bk_nextsize = NULL;
                    }
                  /*设置老三样*/
                  set_head (victim, nb | PREV_INUSE |
                            (av != &main_arena ? NON_MAIN_ARENA : 0));
                  set_head (remainder, remainder_size | PREV_INUSE);
                  set_foot (remainder, remainder_size);
                }
              check_malloced_chunk (av, victim, nb);
              /*继续老三样*/
              void *p = chunk2mem (victim);
              alloc_perturb (p, bytes);
              return p;
            }
        }

    use_top:/*这部分开始使用top chunk*/
      /*
         If large enough, split off the chunk bordering the end of memory
         (held in av->top). Note that this is in accord with the best-fit
         search rule.  In effect, av->top is treated as larger (and thus
         less well fitting) than any other available chunk since it can
         be extended to be as large as necessary (up to system
         limitations).

         We require that av->top always exists (i.e., has size >=
         MINSIZE) after initialization, so if it would otherwise be
         exhausted by current request, it is replenished. (The main
         reason for ensuring it exists is that we may need MINSIZE space
         to put in fenceposts in sysmalloc.)
       */
      /*获取top chunk*/
      victim = av->top;
      /*top chunk大小*/
      size = chunksize (victim);

      if (__glibc_unlikely (size > av->system_mem))
        malloc_printerr ("malloc(): corrupted top size");
      /*top chunk很大  分裂完还是满足  需要大小+最小chunk大小  那就分裂它*/
      if ((unsigned long) (size) >= (unsigned long) (nb + MINSIZE))
        {
          remainder_size = size - nb;
          remainder = chunk_at_offset (victim, nb);
          av->top = remainder;
          set_head (victim, nb | PREV_INUSE |
                    (av != &main_arena ? NON_MAIN_ARENA : 0));
          set_head (remainder, remainder_size | PREV_INUSE);

          check_malloced_chunk (av, victim, nb);
          void *p = chunk2mem (victim);
          alloc_perturb (p, bytes);
          return p;
        }

      /* When we are using atomic ops to free fast chunks we can get
         here for all block sizes.  */
      /*否则就判断是否有 fast chunk*/
      else if (atomic_load_relaxed (&av->have_fastchunks))
        {
          /*先进行fastbin合并*/
          malloc_consolidate (av);
          /* restore original bin index */
          /*
           * 判断需要的chunk是在small bin范围还是large bin范围  然后计算对应的索引
           * 
           **/
          if (in_smallbin_range (nb))
            idx = smallbin_index (nb);
          else
            idx = largebin_index (nb);
        }

      /*
         Otherwise, relay to handle system-dependent cases
       */
      else/*进行到这  就说明堆内存不够了 需要调用sysmalloc来申请内存*/
        {
          void *p = sysmalloc (nb, av);
          if (p != NULL)
            alloc_perturb (p, bytes);
          return p;
        }
    }
}

/*
   ------------------------------ free ------------------------------
 */
/*这部分是真正的free*/
static void
_int_free (mstate av, mchunkptr p, int have_lock)
{
  INTERNAL_SIZE_T size;        /* its size */
  mfastbinptr *fb;             /* associated fastbin */
  mchunkptr nextchunk;         /* next contiguous chunk */
  INTERNAL_SIZE_T nextsize;    /* its size */
  int nextinuse;               /* true if nextchunk is used */
  INTERNAL_SIZE_T prevsize;    /* size of previous contiguous chunk */
  mchunkptr bck;               /* misc temp for linking */
  mchunkptr fwd;               /* misc temp for linking */
  /*获取size大小*/
  size = chunksize (p);

  /* Little security check which won't hurt performance: the
     allocator never wrapps around at the end of the address space.
     Therefore we can exclude some size values which might appear
     here by accident or by "design" from some intruder.  */
  /*对指针进行检查  第一个指针大于-size是啥意思？  第二个是判断对齐*/
  if (__builtin_expect ((uintptr_t) p > (uintptr_t) -size, 0)
      || __builtin_expect (misaligned_chunk (p), 0))
    malloc_printerr ("free(): invalid pointer");
  /* We know that each chunk is at least MINSIZE bytes in size or a
     multiple of MALLOC_ALIGNMENT.  */
  /*大小没有最小的chunk大或大小不是MALLOC_ALIGNMENT的整数倍*/
  if (__glibc_unlikely (size < MINSIZE || !aligned_OK (size)))
    malloc_printerr ("free(): invalid size");
  /*检查chunk是否处于使用状态*/
  check_inuse_chunk(av, p);

#if USE_TCACHE
  {
    /*找对应到tcache的index*/
    size_t tc_idx = csize2tidx (size);
    if (tcache != NULL && tc_idx < mp_.tcache_bins)
      {
	/* Check to see if it's already in the tcache.  */
	tcache_entry *e = (tcache_entry *) chunk2mem (p);

	/* This test succeeds on double free.  However, we don't 100%
	   trust it (it also matches random payload data at a 1 in
	   2^<size_t> chance), so verify it's not an unlikely
	   coincidence before aborting.  */
  /*遍历链表看是不是有相同的tcache chunk 防止double free*/
	if (__glibc_unlikely (e->key == tcache))
	  {
	    tcache_entry *tmp;
	    LIBC_PROBE (memory_tcache_double_free, 2, e, tc_idx);
	    for (tmp = tcache->entries[tc_idx];
		 tmp;
		 tmp = tmp->next)
	      if (tmp == e)
		malloc_printerr ("free(): double free detected in tcache 2");
	    /* If we get here, it was a coincidence.  We've wasted a
	       few cycles, but don't abort.  */
	  }
  /*如果对应的tcache链没满  就是小于7个  那就调用tcache_put放进去*/
	if (tcache->counts[tc_idx] < mp_.tcache_count)
	  {
	    tcache_put (p, tc_idx);/*直接放回对应tcache链的头部，也没有对放入的chunk的inuse置位*/
	    return;
	  }
      }
  }
#endif

  /*
    If eligible, place chunk on a fastbin so it can be found
    and used quickly in malloc.
  */
  /*判断是不是在 fast bin 范围*/
  if ((unsigned long)(size) <= (unsigned long)(get_max_fast ())

#if TRIM_FASTBINS
      /*
	If TRIM_FASTBINS set, don't place chunks
	bordering top into fastbins
      */
      /*此chunk是fast chunk，并且下一个chunk是top chunk，则不能插入*/
      && (chunk_at_offset(p, size) != av->top)
#endif
      ) {
    /*下一个chunk的大小不能小于两倍的SIZE_SZ,并且下一个chunk的大小不能大于system_mem*/
    if (__builtin_expect (chunksize_nomask (chunk_at_offset (p, size))
			  <= 2 * SIZE_SZ, 0)
	|| __builtin_expect (chunksize (chunk_at_offset (p, size))
			     >= av->system_mem, 0))
      {
	bool fail = true;
	/* We might not have a lock at this point and concurrent modifications
	   of system_mem might result in a false positive.  Redo the test after
	   getting the lock.  */
	if (!have_lock)
	  {
	    __libc_lock_lock (av->mutex);
	    fail = (chunksize_nomask (chunk_at_offset (p, size)) <= 2 * SIZE_SZ
		    || chunksize (chunk_at_offset (p, size)) >= av->system_mem);
	    __libc_lock_unlock (av->mutex);
	  }

	if (fail)
	  malloc_printerr ("free(): invalid next size (fast)");
      }
    /*将chunk的mem设置为perturb_byte*/
    free_perturb (chunk2mem(p), size - 2 * SIZE_SZ);
    
    atomic_store_relaxed (&av->have_fastchunks, true);
    /*由size获取fastbin的索引*/
    unsigned int idx = fastbin_index(size);
    /*获取对应fastbin的头指针*/
    fb = &fastbin (av, idx);

    /* Atomically link P to its fastbin: P->FD = *FB; *FB = P;  */
    /*使用原子操作将P插入到fastbin链表中*/
    mchunkptr old = *fb, old2;

    if (SINGLE_THREAD_P)
      {
	/* Check that the top of the bin is not the record we are going to
	   add (i.e., double free).  */
	if (__builtin_expect (old == p, 0))
	  malloc_printerr ("double free or corruption (fasttop)");
	p->fd = old;
	*fb = p;
      }
    else
      do
	{
	  /* Check that the top of the bin is not the record we are going to
	     add (i.e., double free).  */
	  if (__builtin_expect (old == p, 0))
	    malloc_printerr ("double free or corruption (fasttop)");
	  p->fd = old2 = old;
	}
      while ((old = catomic_compare_and_exchange_val_rel (fb, p, old2))
	     != old2);

    /* Check that size of fastbin chunk at the top is the same as
       size of the chunk that we are adding.  We can dereference OLD
       only if we have the lock, otherwise it might have already been
       allocated again.  */
    if (have_lock && old != NULL
	&& __builtin_expect (fastbin_index (chunksize (old)) != idx, 0))
      malloc_printerr ("invalid fastbin entry (free)");
  }

  /*
    Consolidate other non-mmapped chunks as they arrive.
  */
  /*内存合并部分*/
  else if (!chunk_is_mmapped(p)) {

    /* If we're single-threaded, don't lock the arena.  */
    if (SINGLE_THREAD_P)
      have_lock = true;
    /*先请求锁*/
    if (!have_lock)
      __libc_lock_lock (av->mutex);

    nextchunk = chunk_at_offset(p, size);

    /* Lightweight tests: check whether the block is already the
       top block.  */
    /*轻量级检测*/
    /*free的chunk不能是top chunk*/
    if (__glibc_unlikely (p == av->top))
      malloc_printerr ("double free or corruption (top)");
    /* Or whether the next chunk is beyond the boundaries of the arena.  */
    /*下一个chunk不能超过arena的边界*/
    if (__builtin_expect (contiguous (av)
			  && (char *) nextchunk
			  >= ((char *) av->top + chunksize(av->top)), 0))
	malloc_printerr ("double free or corruption (out)");
    /* Or whether the block is actually not marked used.  */
    /*chunk的使用标记没有被标记 防止double free？*/
    if (__glibc_unlikely (!prev_inuse(nextchunk)))
      malloc_printerr ("double free or corruption (!prev)");
    /*下一个chunk的大小*/
    nextsize = chunksize(nextchunk);
    /*下一个chunk的大小是否不大于2*SIZE_SZ，或者nextsize是否大于系统可提供内存*/
    if (__builtin_expect (chunksize_nomask (nextchunk) <= 2 * SIZE_SZ, 0)
	|| __builtin_expect (nextsize >= av->system_mem, 0))
      malloc_printerr ("free(): invalid next size (normal)");
    /*释放 并填充chunk的mem部分*/
    free_perturb (chunk2mem(p), size - 2 * SIZE_SZ);

    /* consolidate backward */
    /*后向合并  合并的是比他地址低的chunk*/
    if (!prev_inuse(p)) {
      prevsize = prev_size (p);
      size += prevsize;
      p = chunk_at_offset(p, -((long) prevsize));
      if (__glibc_unlikely (chunksize(p) != prevsize))
        malloc_printerr ("corrupted size vs. prev_size while consolidating");
      unlink_chunk (av, p);
    }
    /*下一个chunk不是top chunk*/
    if (nextchunk != av->top) {
      /* get and clear inuse bit */
      nextinuse = inuse_bit_at_offset(nextchunk, nextsize);
    /*而且下一个并不inuse  合并掉下一个chunk*/
      /* consolidate forward */
      if (!nextinuse) {
	unlink_chunk (av, nextchunk);
	size += nextsize;
      } else
	clear_inuse_bit_at_offset(nextchunk, 0);

      /*
	Place the chunk in unsorted chunk list. Chunks are
	not placed into regular bins until after they have
	been given one chance to be used in malloc.
      */
      /*合并完放入unsorted bin*/
      bck = unsorted_chunks(av);
      fwd = bck->fd;
      if (__glibc_unlikely (fwd->bk != bck))
	malloc_printerr ("free(): corrupted unsorted chunks");
      p->fd = fwd;
      p->bk = bck;
      /* 如果是large chunk，就设置nextsize指针字段为NULL*/
      if (!in_smallbin_range(size))
	{
	  p->fd_nextsize = NULL;
	  p->bk_nextsize = NULL;
	}
      bck->fd = p;
      fwd->bk = p;

      set_head(p, size | PREV_INUSE);
      set_foot(p, size);

      check_free_chunk(av, p);
    }

    /*
      If the chunk borders the current high end of memory,
      consolidate into top
    */
    /*释放的chunk的下一个chunk如果是top chunk，合并到 top chunk*/
    else {
      size += nextsize;
      set_head(p, size | PREV_INUSE);
      av->top = p;
      check_chunk(av, p);
    }

    /*
      If freeing a large space, consolidate possibly-surrounding
      chunks. Then, if the total unused topmost memory exceeds trim
      threshold, ask malloc_trim to reduce top.

      Unless max_fast is 0, we don't know if there are fastbins
      bordering top, so we cannot tell for sure whether threshold
      has been reached unless fastbins are consolidated.  But we
      don't want to consolidate on each free.  As a compromise,
      consolidation is performed if FASTBIN_CONSOLIDATION_THRESHOLD
      is reached.
    */
    /*如果合并后的 chunk 的大小大于FASTBIN_CONSOLIDATION_THRESHOLD，向系统返还内存*/
    if ((unsigned long)(size) >= FASTBIN_CONSOLIDATION_THRESHOLD) {
      if (atomic_load_relaxed (&av->have_fastchunks))
	malloc_consolidate(av);

      if (av == &main_arena) {
#ifndef MORECORE_CANNOT_TRIM
	if ((unsigned long)(chunksize(av->top)) >=
	    (unsigned long)(mp_.trim_threshold))
	  systrim(mp_.top_pad, av);
#endif
      } else {
	/* Always try heap_trim(), even if the top chunk is not
	   large, because the corresponding heap might go away.  */
	heap_info *heap = heap_for_ptr(top(av));

	assert(heap->ar_ptr == av);
	heap_trim(heap, mp_.top_pad);
      }
    }

    if (!have_lock)
      __libc_lock_unlock (av->mutex);
  }
  /*
    If the chunk was allocated via mmap, release via munmap().
  */
  /*释放 mmap 的 chunk*/
  else {
    munmap_chunk (p);
  }
}

/*
  ------------------------- malloc_consolidate -------------------------

  malloc_consolidate is a specialized version of free() that tears
  down chunks held in fastbins.  Free itself cannot be used for this
  purpose since, among other things, it might place chunks back onto
  fastbins.  So, instead, we need to use a minor variant of the same
  code.
*/

static void malloc_consolidate(mstate av)
{
  mfastbinptr*    fb;                 /* current fastbin being consolidated */
  mfastbinptr*    maxfb;              /* last fastbin (for loop control) */
  mchunkptr       p;                  /* current chunk being consolidated */
  mchunkptr       nextp;              /* next chunk to consolidate */
  mchunkptr       unsorted_bin;       /* bin header */
  mchunkptr       first_unsorted;     /* chunk to link to */

  /* These have same use as in free() */
  mchunkptr       nextchunk;
  INTERNAL_SIZE_T size;
  INTERNAL_SIZE_T nextsize;
  INTERNAL_SIZE_T prevsize;
  int             nextinuse;

  atomic_store_relaxed (&av->have_fastchunks, false);

  unsorted_bin = unsorted_chunks(av);

  /*
    Remove each chunk from fast bin and consolidate it, placing it
    then in unsorted bin. Among other reasons for doing this,
    placing in unsorted bin avoids needing to calculate actual bins
    until malloc is sure that chunks aren't immediately going to be
    reused anyway.
  */
  /*按照fd顺序遍历fastbins的每一个bin，将bin中的每一个chunk都合并掉*/
  maxfb = &fastbin (av, NFASTBINS - 1);
  fb = &fastbin (av, 0);
  do {
    p = atomic_exchange_acq (fb, NULL);
    if (p != 0) {
      do {
	{
	  unsigned int idx = fastbin_index (chunksize (p));
	  if ((&fastbin (av, idx)) != fb)
	    malloc_printerr ("malloc_consolidate(): invalid chunk size");
	}

	check_inuse_chunk(av, p);
	nextp = p->fd;

	/* Slightly streamlined version of consolidation code in free() */
	size = chunksize (p);
	nextchunk = chunk_at_offset(p, size);
	nextsize = chunksize(nextchunk);
  /*找当前chunk前面的chunk  尝试合并*/
	if (!prev_inuse(p)) {
	  prevsize = prev_size (p);
	  size += prevsize;
	  p = chunk_at_offset(p, -((long) prevsize));
	  if (__glibc_unlikely (chunksize(p) != prevsize))
	    malloc_printerr ("corrupted size vs. prev_size in fastbins");
	  unlink_chunk (av, p);
	}
  /*当前chunk后面的chunk  尝试合并*/
	if (nextchunk != av->top) {
    //判断 nextchunk inuse位
	  nextinuse = inuse_bit_at_offset(nextchunk, nextsize);
	  if (!nextinuse) {
	    size += nextsize;
	    unlink_chunk (av, nextchunk);
	  } else
      //设置 nextchunk 的 prev inuse 为0，表示可以合并当前fast chunk
	    clear_inuse_bit_at_offset(nextchunk, 0);

	  first_unsorted = unsorted_bin->fd;
	  unsorted_bin->fd = p;
	  first_unsorted->bk = p;

	  if (!in_smallbin_range (size)) {
	    p->fd_nextsize = NULL;
	    p->bk_nextsize = NULL;
	  }

	  set_head(p, size | PREV_INUSE);
	  p->bk = unsorted_bin;
	  p->fd = first_unsorted;
	  set_foot(p, size);
	}

	else {
	  size += nextsize;
	  set_head(p, size | PREV_INUSE);
	  av->top = p;
	}

      } while ( (p = nextp) != 0);

    }
  } while (fb++ != maxfb);
}

/*
  ------------------------------ realloc ------------------------------
*/

void*
_int_realloc(mstate av, mchunkptr oldp, INTERNAL_SIZE_T oldsize,
	     INTERNAL_SIZE_T nb)
{
  mchunkptr        newp;            /* chunk to return */
  INTERNAL_SIZE_T  newsize;         /* its size */
  void*          newmem;          /* corresponding user mem */

  mchunkptr        next;            /* next contiguous chunk after oldp */

  mchunkptr        remainder;       /* extra space at end of newp */
  unsigned long    remainder_size;  /* its size */

  /* oldmem size */
  if (__builtin_expect (chunksize_nomask (oldp) <= 2 * SIZE_SZ, 0)
      || __builtin_expect (oldsize >= av->system_mem, 0))
    malloc_printerr ("realloc(): invalid old size");

  check_inuse_chunk (av, oldp);

  /* All callers already filter out mmap'ed chunks.  */
  assert (!chunk_is_mmapped (oldp));

  next = chunk_at_offset (oldp, oldsize);
  INTERNAL_SIZE_T nextsize = chunksize (next);
  if (__builtin_expect (chunksize_nomask (next) <= 2 * SIZE_SZ, 0)
      || __builtin_expect (nextsize >= av->system_mem, 0))
    malloc_printerr ("realloc(): invalid next size");

  if ((unsigned long) (oldsize) >= (unsigned long) (nb))
    {
      /* already big enough; split below */
      newp = oldp;
      newsize = oldsize;
    }

  else
    {
      /* Try to expand forward into top */
      if (next == av->top &&
          (unsigned long) (newsize = oldsize + nextsize) >=
          (unsigned long) (nb + MINSIZE))
        {
          set_head_size (oldp, nb | (av != &main_arena ? NON_MAIN_ARENA : 0));
          av->top = chunk_at_offset (oldp, nb);
          set_head (av->top, (newsize - nb) | PREV_INUSE);
          check_inuse_chunk (av, oldp);
          return chunk2mem (oldp);
        }

      /* Try to expand forward into next chunk;  split off remainder below */
      else if (next != av->top &&
               !inuse (next) &&
               (unsigned long) (newsize = oldsize + nextsize) >=
               (unsigned long) (nb))
        {
          newp = oldp;
          unlink_chunk (av, next);
        }

      /* allocate, copy, free */
      else
        {
          newmem = _int_malloc (av, nb - MALLOC_ALIGN_MASK);
          if (newmem == 0)
            return 0; /* propagate failure */

          newp = mem2chunk (newmem);
          newsize = chunksize (newp);

          /*
             Avoid copy if newp is next chunk after oldp.
           */
          if (newp == next)
            {
              newsize += oldsize;
              newp = oldp;
            }
          else
            {
	      memcpy (newmem, chunk2mem (oldp), oldsize - SIZE_SZ);
              _int_free (av, oldp, 1);
              check_inuse_chunk (av, newp);
              return chunk2mem (newp);
            }
        }
    }

  /* If possible, free extra space in old or extended chunk */

  assert ((unsigned long) (newsize) >= (unsigned long) (nb));

  remainder_size = newsize - nb;

  if (remainder_size < MINSIZE)   /* not enough extra to split off */
    {
      set_head_size (newp, newsize | (av != &main_arena ? NON_MAIN_ARENA : 0));
      set_inuse_bit_at_offset (newp, newsize);
    }
  else   /* split remainder */
    {
      remainder = chunk_at_offset (newp, nb);
      set_head_size (newp, nb | (av != &main_arena ? NON_MAIN_ARENA : 0));
      set_head (remainder, remainder_size | PREV_INUSE |
                (av != &main_arena ? NON_MAIN_ARENA : 0));
      /* Mark remainder as inuse so free() won't complain */
      set_inuse_bit_at_offset (remainder, remainder_size);
      _int_free (av, remainder, 1);
    }

  check_inuse_chunk (av, newp);
  return chunk2mem (newp);
}

/*
   ------------------------------ memalign ------------------------------
 */

static void *
_int_memalign (mstate av, size_t alignment, size_t bytes)
{
  INTERNAL_SIZE_T nb;             /* padded  request size */
  char *m;                        /* memory returned by malloc call */
  mchunkptr p;                    /* corresponding chunk */
  char *brk;                      /* alignment point within p */
  mchunkptr newp;                 /* chunk to return */
  INTERNAL_SIZE_T newsize;        /* its size */
  INTERNAL_SIZE_T leadsize;       /* leading space before alignment point */
  mchunkptr remainder;            /* spare room at end to split off */
  unsigned long remainder_size;   /* its size */
  INTERNAL_SIZE_T size;



  if (!checked_request2size (bytes, &nb))
    {
      __set_errno (ENOMEM);
      return NULL;
    }

  /*
     Strategy: find a spot within that chunk that meets the alignment
     request, and then possibly free the leading and trailing space.
   */

  /* Call malloc with worst case padding to hit alignment. */

  m = (char *) (_int_malloc (av, nb + alignment + MINSIZE));

  if (m == 0)
    return 0;           /* propagate failure */

  p = mem2chunk (m);

  if ((((unsigned long) (m)) % alignment) != 0)   /* misaligned */

    { /*
                Find an aligned spot inside chunk.  Since we need to give back
                leading space in a chunk of at least MINSIZE, if the first
                calculation places us at a spot with less than MINSIZE leader,
                we can move to the next aligned spot -- we've allocated enough
                total room so that this is always possible.
                 */
      brk = (char *) mem2chunk (((unsigned long) (m + alignment - 1)) &
                                - ((signed long) alignment));
      if ((unsigned long) (brk - (char *) (p)) < MINSIZE)
        brk += alignment;

      newp = (mchunkptr) brk;
      leadsize = brk - (char *) (p);
      newsize = chunksize (p) - leadsize;

      /* For mmapped chunks, just adjust offset */
      if (chunk_is_mmapped (p))
        {
          set_prev_size (newp, prev_size (p) + leadsize);
          set_head (newp, newsize | IS_MMAPPED);
          return chunk2mem (newp);
        }

      /* Otherwise, give back leader, use the rest */
      set_head (newp, newsize | PREV_INUSE |
                (av != &main_arena ? NON_MAIN_ARENA : 0));
      set_inuse_bit_at_offset (newp, newsize);
      set_head_size (p, leadsize | (av != &main_arena ? NON_MAIN_ARENA : 0));
      _int_free (av, p, 1);
      p = newp;

      assert (newsize >= nb &&
              (((unsigned long) (chunk2mem (p))) % alignment) == 0);
    }

  /* Also give back spare room at the end */
  if (!chunk_is_mmapped (p))
    {
      size = chunksize (p);
      if ((unsigned long) (size) > (unsigned long) (nb + MINSIZE))
        {
          remainder_size = size - nb;
          remainder = chunk_at_offset (p, nb);
          set_head (remainder, remainder_size | PREV_INUSE |
                    (av != &main_arena ? NON_MAIN_ARENA : 0));
          set_head_size (p, nb);
          _int_free (av, remainder, 1);
        }
    }

  check_inuse_chunk (av, p);
  return chunk2mem (p);
}


/*
   ------------------------------ malloc_trim ------------------------------
 */

static int
mtrim (mstate av, size_t pad)
{
  /* Ensure all blocks are consolidated.  */
  malloc_consolidate (av);

  const size_t ps = GLRO (dl_pagesize);
  int psindex = bin_index (ps);
  const size_t psm1 = ps - 1;

  int result = 0;
  for (int i = 1; i < NBINS; ++i)
    if (i == 1 || i >= psindex)
      {
        mbinptr bin = bin_at (av, i);

        for (mchunkptr p = last (bin); p != bin; p = p->bk)
          {
            INTERNAL_SIZE_T size = chunksize (p);

            if (size > psm1 + sizeof (struct malloc_chunk))
              {
                /* See whether the chunk contains at least one unused page.  */
                char *paligned_mem = (char *) (((uintptr_t) p
                                                + sizeof (struct malloc_chunk)
                                                + psm1) & ~psm1);

                assert ((char *) chunk2mem (p) + 4 * SIZE_SZ <= paligned_mem);
                assert ((char *) p + size > paligned_mem);

                /* This is the size we could potentially free.  */
                size -= paligned_mem - (char *) p;

                if (size > psm1)
                  {
#if MALLOC_DEBUG
                    /* When debugging we simulate destroying the memory
                       content.  */
                    memset (paligned_mem, 0x89, size & ~psm1);
#endif
                    __madvise (paligned_mem, size & ~psm1, MADV_DONTNEED);

                    result = 1;
                  }
              }
          }
      }

#ifndef MORECORE_CANNOT_TRIM
  return result | (av == &main_arena ? systrim (pad, av) : 0);

#else
  return result;
#endif
}


int
__malloc_trim (size_t s)
{
  int result = 0;

  if (__malloc_initialized < 0)
    ptmalloc_init ();

  mstate ar_ptr = &main_arena;
  do
    {
      __libc_lock_lock (ar_ptr->mutex);
      result |= mtrim (ar_ptr, s);
      __libc_lock_unlock (ar_ptr->mutex);

      ar_ptr = ar_ptr->next;
    }
  while (ar_ptr != &main_arena);

  return result;
}


/*
   ------------------------- malloc_usable_size -------------------------
 */

static size_t
musable (void *mem)
{
  mchunkptr p;
  if (mem != 0)
    {
      p = mem2chunk (mem);

      if (__builtin_expect (using_malloc_checking == 1, 0))
        return malloc_check_get_size (p);

      if (chunk_is_mmapped (p))
	{
	  if (DUMPED_MAIN_ARENA_CHUNK (p))
	    return chunksize (p) - SIZE_SZ;
	  else
	    return chunksize (p) - 2 * SIZE_SZ;
	}
      else if (inuse (p))
        return chunksize (p) - SIZE_SZ;
    }
  return 0;
}


size_t
__malloc_usable_size (void *m)
{
  size_t result;

  result = musable (m);
  return result;
}

/*
   ------------------------------ mallinfo ------------------------------
   Accumulate malloc statistics for arena AV into M.
 */

static void
int_mallinfo (mstate av, struct mallinfo *m)
{
  size_t i;
  mbinptr b;
  mchunkptr p;
  INTERNAL_SIZE_T avail;
  INTERNAL_SIZE_T fastavail;
  int nblocks;
  int nfastblocks;

  check_malloc_state (av);

  /* Account for top */
  avail = chunksize (av->top);
  nblocks = 1;  /* top always exists */

  /* traverse fastbins */
  nfastblocks = 0;
  fastavail = 0;

  for (i = 0; i < NFASTBINS; ++i)
    {
      for (p = fastbin (av, i); p != 0; p = p->fd)
        {
          ++nfastblocks;
          fastavail += chunksize (p);
        }
    }

  avail += fastavail;

  /* traverse regular bins */
  for (i = 1; i < NBINS; ++i)
    {
      b = bin_at (av, i);
      for (p = last (b); p != b; p = p->bk)
        {
          ++nblocks;
          avail += chunksize (p);
        }
    }

  m->smblks += nfastblocks;
  m->ordblks += nblocks;
  m->fordblks += avail;
  m->uordblks += av->system_mem - avail;
  m->arena += av->system_mem;
  m->fsmblks += fastavail;
  if (av == &main_arena)
    {
      m->hblks = mp_.n_mmaps;
      m->hblkhd = mp_.mmapped_mem;
      m->usmblks = 0;
      m->keepcost = chunksize (av->top);
    }
}


struct mallinfo
__libc_mallinfo (void)
{
  struct mallinfo m;
  mstate ar_ptr;

  if (__malloc_initialized < 0)
    ptmalloc_init ();

  memset (&m, 0, sizeof (m));
  ar_ptr = &main_arena;
  do
    {
      __libc_lock_lock (ar_ptr->mutex);
      int_mallinfo (ar_ptr, &m);
      __libc_lock_unlock (ar_ptr->mutex);

      ar_ptr = ar_ptr->next;
    }
  while (ar_ptr != &main_arena);

  return m;
}

/*
   ------------------------------ malloc_stats ------------------------------
 */

void
__malloc_stats (void)
{
  int i;
  mstate ar_ptr;
  unsigned int in_use_b = mp_.mmapped_mem, system_b = in_use_b;

  if (__malloc_initialized < 0)
    ptmalloc_init ();
  _IO_flockfile (stderr);
  int old_flags2 = stderr->_flags2;
  stderr->_flags2 |= _IO_FLAGS2_NOTCANCEL;
  for (i = 0, ar_ptr = &main_arena;; i++)
    {
      struct mallinfo mi;

      memset (&mi, 0, sizeof (mi));
      __libc_lock_lock (ar_ptr->mutex);
      int_mallinfo (ar_ptr, &mi);
      fprintf (stderr, "Arena %d:\n", i);
      fprintf (stderr, "system bytes     = %10u\n", (unsigned int) mi.arena);
      fprintf (stderr, "in use bytes     = %10u\n", (unsigned int) mi.uordblks);
#if MALLOC_DEBUG > 1
      if (i > 0)
        dump_heap (heap_for_ptr (top (ar_ptr)));
#endif
      system_b += mi.arena;
      in_use_b += mi.uordblks;
      __libc_lock_unlock (ar_ptr->mutex);
      ar_ptr = ar_ptr->next;
      if (ar_ptr == &main_arena)
        break;
    }
  fprintf (stderr, "Total (incl. mmap):\n");
  fprintf (stderr, "system bytes     = %10u\n", system_b);
  fprintf (stderr, "in use bytes     = %10u\n", in_use_b);
  fprintf (stderr, "max mmap regions = %10u\n", (unsigned int) mp_.max_n_mmaps);
  fprintf (stderr, "max mmap bytes   = %10lu\n",
           (unsigned long) mp_.max_mmapped_mem);
  stderr->_flags2 = old_flags2;
  _IO_funlockfile (stderr);
}


/*
   ------------------------------ mallopt ------------------------------
 */
static __always_inline int
do_set_trim_threshold (size_t value)
{
  LIBC_PROBE (memory_mallopt_trim_threshold, 3, value, mp_.trim_threshold,
	      mp_.no_dyn_threshold);
  mp_.trim_threshold = value;
  mp_.no_dyn_threshold = 1;
  return 1;
}

static __always_inline int
do_set_top_pad (size_t value)
{
  LIBC_PROBE (memory_mallopt_top_pad, 3, value, mp_.top_pad,
	      mp_.no_dyn_threshold);
  mp_.top_pad = value;
  mp_.no_dyn_threshold = 1;
  return 1;
}

static __always_inline int
do_set_mmap_threshold (size_t value)
{
  /* Forbid setting the threshold too high.  */
  if (value <= HEAP_MAX_SIZE / 2)
    {
      LIBC_PROBE (memory_mallopt_mmap_threshold, 3, value, mp_.mmap_threshold,
		  mp_.no_dyn_threshold);
      mp_.mmap_threshold = value;
      mp_.no_dyn_threshold = 1;
      return 1;
    }
  return 0;
}

static __always_inline int
do_set_mmaps_max (int32_t value)
{
  LIBC_PROBE (memory_mallopt_mmap_max, 3, value, mp_.n_mmaps_max,
	      mp_.no_dyn_threshold);
  mp_.n_mmaps_max = value;
  mp_.no_dyn_threshold = 1;
  return 1;
}

static __always_inline int
do_set_mallopt_check (int32_t value)
{
  return 1;
}

static __always_inline int
do_set_perturb_byte (int32_t value)
{
  LIBC_PROBE (memory_mallopt_perturb, 2, value, perturb_byte);
  perturb_byte = value;
  return 1;
}

static __always_inline int
do_set_arena_test (size_t value)
{
  LIBC_PROBE (memory_mallopt_arena_test, 2, value, mp_.arena_test);
  mp_.arena_test = value;
  return 1;
}

static __always_inline int
do_set_arena_max (size_t value)
{
  LIBC_PROBE (memory_mallopt_arena_max, 2, value, mp_.arena_max);
  mp_.arena_max = value;
  return 1;
}

#if USE_TCACHE
static __always_inline int
do_set_tcache_max (size_t value)
{
  if (value <= MAX_TCACHE_SIZE)
    {
      LIBC_PROBE (memory_tunable_tcache_max_bytes, 2, value, mp_.tcache_max_bytes);
      mp_.tcache_max_bytes = value;
      mp_.tcache_bins = csize2tidx (request2size(value)) + 1;
      return 1;
    }
  return 0;
}

static __always_inline int
do_set_tcache_count (size_t value)
{
  if (value <= MAX_TCACHE_COUNT)
    {
      LIBC_PROBE (memory_tunable_tcache_count, 2, value, mp_.tcache_count);
      mp_.tcache_count = value;
      return 1;
    }
  return 0;
}

static __always_inline int
do_set_tcache_unsorted_limit (size_t value)
{
  LIBC_PROBE (memory_tunable_tcache_unsorted_limit, 2, value, mp_.tcache_unsorted_limit);
  mp_.tcache_unsorted_limit = value;
  return 1;
}
#endif

static inline int
__always_inline
do_set_mxfast (size_t value)
{
  if (value <= MAX_FAST_SIZE)
    {
      LIBC_PROBE (memory_mallopt_mxfast, 2, value, get_max_fast ());
      set_max_fast (value);
      return 1;
    }
  return 0;
}

int
__libc_mallopt (int param_number, int value)
{
  mstate av = &main_arena;
  int res = 1;

  if (__malloc_initialized < 0)
    ptmalloc_init ();
  __libc_lock_lock (av->mutex);

  LIBC_PROBE (memory_mallopt, 2, param_number, value);

  /* We must consolidate main arena before changing max_fast
     (see definition of set_max_fast).  */
  malloc_consolidate (av);

  /* Many of these helper functions take a size_t.  We do not worry
     about overflow here, because negative int values will wrap to
     very large size_t values and the helpers have sufficient range
     checking for such conversions.  Many of these helpers are also
     used by the tunables macros in arena.c.  */

  switch (param_number)
    {
    case M_MXFAST:
      res = do_set_mxfast (value);
      break;

    case M_TRIM_THRESHOLD:
      res = do_set_trim_threshold (value);
      break;

    case M_TOP_PAD:
      res = do_set_top_pad (value);
      break;

    case M_MMAP_THRESHOLD:
      res = do_set_mmap_threshold (value);
      break;

    case M_MMAP_MAX:
      res = do_set_mmaps_max (value);
      break;

    case M_CHECK_ACTION:
      res = do_set_mallopt_check (value);
      break;

    case M_PERTURB:
      res = do_set_perturb_byte (value);
      break;

    case M_ARENA_TEST:
      if (value > 0)
	res = do_set_arena_test (value);
      break;

    case M_ARENA_MAX:
      if (value > 0)
	res = do_set_arena_max (value);
      break;
    }
  __libc_lock_unlock (av->mutex);
  return res;
}
libc_hidden_def (__libc_mallopt)


/*
   -------------------- Alternative MORECORE functions --------------------
 */


/*
   General Requirements for MORECORE.

   The MORECORE function must have the following properties:

   If MORECORE_CONTIGUOUS is false:

 * MORECORE must allocate in multiples of pagesize. It will
      only be called with arguments that are multiples of pagesize.

 * MORECORE(0) must return an address that is at least
      MALLOC_ALIGNMENT aligned. (Page-aligning always suffices.)

   else (i.e. If MORECORE_CONTIGUOUS is true):

 * Consecutive calls to MORECORE with positive arguments
      return increasing addresses, indicating that space has been
      contiguously extended.

 * MORECORE need not allocate in multiples of pagesize.
      Calls to MORECORE need not have args of multiples of pagesize.

 * MORECORE need not page-align.

   In either case:

 * MORECORE may allocate more memory than requested. (Or even less,
      but this will generally result in a malloc failure.)

 * MORECORE must not allocate memory when given argument zero, but
      instead return one past the end address of memory from previous
      nonzero call. This malloc does NOT call MORECORE(0)
      until at least one call with positive arguments is made, so
      the initial value returned is not important.

 * Even though consecutive calls to MORECORE need not return contiguous
      addresses, it must be OK for malloc'ed chunks to span multiple
      regions in those cases where they do happen to be contiguous.

 * MORECORE need not handle negative arguments -- it may instead
      just return MORECORE_FAILURE when given negative arguments.
      Negative arguments are always multiples of pagesize. MORECORE
      must not misinterpret negative args as large positive unsigned
      args. You can suppress all such calls from even occurring by defining
      MORECORE_CANNOT_TRIM,

   There is some variation across systems about the type of the
   argument to sbrk/MORECORE. If size_t is unsigned, then it cannot
   actually be size_t, because sbrk supports negative args, so it is
   normally the signed type of the same width as size_t (sometimes
   declared as "intptr_t", and sometimes "ptrdiff_t").  It doesn't much
   matter though. Internally, we use "long" as arguments, which should
   work across all reasonable possibilities.

   Additionally, if MORECORE ever returns failure for a positive
   request, then mmap is used as a noncontiguous system allocator. This
   is a useful backup strategy for systems with holes in address spaces
   -- in this case sbrk cannot contiguously expand the heap, but mmap
   may be able to map noncontiguous space.

   If you'd like mmap to ALWAYS be used, you can define MORECORE to be
   a function that always returns MORECORE_FAILURE.

   If you are using this malloc with something other than sbrk (or its
   emulation) to supply memory regions, you probably want to set
   MORECORE_CONTIGUOUS as false.  As an example, here is a custom
   allocator kindly contributed for pre-OSX macOS.  It uses virtually
   but not necessarily physically contiguous non-paged memory (locked
   in, present and won't get swapped out).  You can use it by
   uncommenting this section, adding some #includes, and setting up the
   appropriate defines above:

 *#define MORECORE osMoreCore
 *#define MORECORE_CONTIGUOUS 0

   There is also a shutdown routine that should somehow be called for
   cleanup upon program exit.

 *#define MAX_POOL_ENTRIES 100
 *#define MINIMUM_MORECORE_SIZE  (64 * 1024)
   static int next_os_pool;
   void *our_os_pools[MAX_POOL_ENTRIES];

   void *osMoreCore(int size)
   {
    void *ptr = 0;
    static void *sbrk_top = 0;

    if (size > 0)
    {
      if (size < MINIMUM_MORECORE_SIZE)
         size = MINIMUM_MORECORE_SIZE;
      if (CurrentExecutionLevel() == kTaskLevel)
         ptr = PoolAllocateResident(size + RM_PAGE_SIZE, 0);
      if (ptr == 0)
      {
        return (void *) MORECORE_FAILURE;
      }
      // save ptrs so they can be freed during cleanup
      our_os_pools[next_os_pool] = ptr;
      next_os_pool++;
      ptr = (void *) ((((unsigned long) ptr) + RM_PAGE_MASK) & ~RM_PAGE_MASK);
      sbrk_top = (char *) ptr + size;
      return ptr;
    }
    else if (size < 0)
    {
      // we don't currently support shrink behavior
      return (void *) MORECORE_FAILURE;
    }
    else
    {
      return sbrk_top;
    }
   }

   // cleanup any allocated memory pools
   // called as last thing before shutting down driver

   void osCleanupMem(void)
   {
    void **ptr;

    for (ptr = our_os_pools; ptr < &our_os_pools[MAX_POOL_ENTRIES]; ptr++)
      if (*ptr)
      {
         PoolDeallocate(*ptr);
 * ptr = 0;
      }
   }

 */


/* Helper code.  */

extern char **__libc_argv attribute_hidden;

static void
malloc_printerr (const char *str)
{
  __libc_message (do_abort, "%s\n", str);
  __builtin_unreachable ();
}

/* We need a wrapper function for one of the additions of POSIX.  */
int
__posix_memalign (void **memptr, size_t alignment, size_t size)
{
  void *mem;

  /* Test whether the SIZE argument is valid.  It must be a power of
     two multiple of sizeof (void *).  */
  if (alignment % sizeof (void *) != 0
      || !powerof2 (alignment / sizeof (void *))
      || alignment == 0)
    return EINVAL;


  void *address = RETURN_ADDRESS (0);
  mem = _mid_memalign (alignment, size, address);

  if (mem != NULL)
    {
      *memptr = mem;
      return 0;
    }

  return ENOMEM;
}
weak_alias (__posix_memalign, posix_memalign)


int
__malloc_info (int options, FILE *fp)
{
  /* For now, at least.  */
  if (options != 0)
    return EINVAL;

  int n = 0;
  size_t total_nblocks = 0;
  size_t total_nfastblocks = 0;
  size_t total_avail = 0;
  size_t total_fastavail = 0;
  size_t total_system = 0;
  size_t total_max_system = 0;
  size_t total_aspace = 0;
  size_t total_aspace_mprotect = 0;



  if (__malloc_initialized < 0)
    ptmalloc_init ();

  fputs ("<malloc version=\"1\">\n", fp);

  /* Iterate over all arenas currently in use.  */
  mstate ar_ptr = &main_arena;
  do
    {
      fprintf (fp, "<heap nr=\"%d\">\n<sizes>\n", n++);

      size_t nblocks = 0;
      size_t nfastblocks = 0;
      size_t avail = 0;
      size_t fastavail = 0;
      struct
      {
	size_t from;
	size_t to;
	size_t total;
	size_t count;
      } sizes[NFASTBINS + NBINS - 1];
#define nsizes (sizeof (sizes) / sizeof (sizes[0]))

      __libc_lock_lock (ar_ptr->mutex);

      /* Account for top chunk.  The top-most available chunk is
	 treated specially and is never in any bin. See "initial_top"
	 comments.  */
      avail = chunksize (ar_ptr->top);
      nblocks = 1;  /* Top always exists.  */

      for (size_t i = 0; i < NFASTBINS; ++i)
	{
	  mchunkptr p = fastbin (ar_ptr, i);
	  if (p != NULL)
	    {
	      size_t nthissize = 0;
	      size_t thissize = chunksize (p);

	      while (p != NULL)
		{
		  ++nthissize;
		  p = p->fd;
		}

	      fastavail += nthissize * thissize;
	      nfastblocks += nthissize;
	      sizes[i].from = thissize - (MALLOC_ALIGNMENT - 1);
	      sizes[i].to = thissize;
	      sizes[i].count = nthissize;
	    }
	  else
	    sizes[i].from = sizes[i].to = sizes[i].count = 0;

	  sizes[i].total = sizes[i].count * sizes[i].to;
	}


      mbinptr bin;
      struct malloc_chunk *r;

      for (size_t i = 1; i < NBINS; ++i)
	{
	  bin = bin_at (ar_ptr, i);
	  r = bin->fd;
	  sizes[NFASTBINS - 1 + i].from = ~((size_t) 0);
	  sizes[NFASTBINS - 1 + i].to = sizes[NFASTBINS - 1 + i].total
					  = sizes[NFASTBINS - 1 + i].count = 0;

	  if (r != NULL)
	    while (r != bin)
	      {
		size_t r_size = chunksize_nomask (r);
		++sizes[NFASTBINS - 1 + i].count;
		sizes[NFASTBINS - 1 + i].total += r_size;
		sizes[NFASTBINS - 1 + i].from
		  = MIN (sizes[NFASTBINS - 1 + i].from, r_size);
		sizes[NFASTBINS - 1 + i].to = MAX (sizes[NFASTBINS - 1 + i].to,
						   r_size);

		r = r->fd;
	      }

	  if (sizes[NFASTBINS - 1 + i].count == 0)
	    sizes[NFASTBINS - 1 + i].from = 0;
	  nblocks += sizes[NFASTBINS - 1 + i].count;
	  avail += sizes[NFASTBINS - 1 + i].total;
	}

      size_t heap_size = 0;
      size_t heap_mprotect_size = 0;
      size_t heap_count = 0;
      if (ar_ptr != &main_arena)
	{
	  /* Iterate over the arena heaps from back to front.  */
	  heap_info *heap = heap_for_ptr (top (ar_ptr));
	  do
	    {
	      heap_size += heap->size;
	      heap_mprotect_size += heap->mprotect_size;
	      heap = heap->prev;
	      ++heap_count;
	    }
	  while (heap != NULL);
	}

      __libc_lock_unlock (ar_ptr->mutex);

      total_nfastblocks += nfastblocks;
      total_fastavail += fastavail;

      total_nblocks += nblocks;
      total_avail += avail;

      for (size_t i = 0; i < nsizes; ++i)
	if (sizes[i].count != 0 && i != NFASTBINS)
	  fprintf (fp, "\
  <size from=\"%zu\" to=\"%zu\" total=\"%zu\" count=\"%zu\"/>\n",
		   sizes[i].from, sizes[i].to, sizes[i].total, sizes[i].count);

      if (sizes[NFASTBINS].count != 0)
	fprintf (fp, "\
  <unsorted from=\"%zu\" to=\"%zu\" total=\"%zu\" count=\"%zu\"/>\n",
		 sizes[NFASTBINS].from, sizes[NFASTBINS].to,
		 sizes[NFASTBINS].total, sizes[NFASTBINS].count);

      total_system += ar_ptr->system_mem;
      total_max_system += ar_ptr->max_system_mem;

      fprintf (fp,
	       "</sizes>\n<total type=\"fast\" count=\"%zu\" size=\"%zu\"/>\n"
	       "<total type=\"rest\" count=\"%zu\" size=\"%zu\"/>\n"
	       "<system type=\"current\" size=\"%zu\"/>\n"
	       "<system type=\"max\" size=\"%zu\"/>\n",
	       nfastblocks, fastavail, nblocks, avail,
	       ar_ptr->system_mem, ar_ptr->max_system_mem);

      if (ar_ptr != &main_arena)
	{
	  fprintf (fp,
		   "<aspace type=\"total\" size=\"%zu\"/>\n"
		   "<aspace type=\"mprotect\" size=\"%zu\"/>\n"
		   "<aspace type=\"subheaps\" size=\"%zu\"/>\n",
		   heap_size, heap_mprotect_size, heap_count);
	  total_aspace += heap_size;
	  total_aspace_mprotect += heap_mprotect_size;
	}
      else
	{
	  fprintf (fp,
		   "<aspace type=\"total\" size=\"%zu\"/>\n"
		   "<aspace type=\"mprotect\" size=\"%zu\"/>\n",
		   ar_ptr->system_mem, ar_ptr->system_mem);
	  total_aspace += ar_ptr->system_mem;
	  total_aspace_mprotect += ar_ptr->system_mem;
	}

      fputs ("</heap>\n", fp);
      ar_ptr = ar_ptr->next;
    }
  while (ar_ptr != &main_arena);

  fprintf (fp,
	   "<total type=\"fast\" count=\"%zu\" size=\"%zu\"/>\n"
	   "<total type=\"rest\" count=\"%zu\" size=\"%zu\"/>\n"
	   "<total type=\"mmap\" count=\"%d\" size=\"%zu\"/>\n"
	   "<system type=\"current\" size=\"%zu\"/>\n"
	   "<system type=\"max\" size=\"%zu\"/>\n"
	   "<aspace type=\"total\" size=\"%zu\"/>\n"
	   "<aspace type=\"mprotect\" size=\"%zu\"/>\n"
	   "</malloc>\n",
	   total_nfastblocks, total_fastavail, total_nblocks, total_avail,
	   mp_.n_mmaps, mp_.mmapped_mem,
	   total_system, total_max_system,
	   total_aspace, total_aspace_mprotect);

  return 0;
}
weak_alias (__malloc_info, malloc_info)


strong_alias (__libc_calloc, __calloc) weak_alias (__libc_calloc, calloc)
strong_alias (__libc_free, __free) strong_alias (__libc_free, free)
strong_alias (__libc_malloc, __malloc) strong_alias (__libc_malloc, malloc)
strong_alias (__libc_memalign, __memalign)
weak_alias (__libc_memalign, memalign)
strong_alias (__libc_realloc, __realloc) strong_alias (__libc_realloc, realloc)
strong_alias (__libc_valloc, __valloc) weak_alias (__libc_valloc, valloc)
strong_alias (__libc_pvalloc, __pvalloc) weak_alias (__libc_pvalloc, pvalloc)
strong_alias (__libc_mallinfo, __mallinfo)
weak_alias (__libc_mallinfo, mallinfo)
strong_alias (__libc_mallopt, __mallopt) weak_alias (__libc_mallopt, mallopt)

weak_alias (__malloc_stats, malloc_stats)
weak_alias (__malloc_usable_size, malloc_usable_size)
weak_alias (__malloc_trim, malloc_trim)

#if SHLIB_COMPAT (libc, GLIBC_2_0, GLIBC_2_26)
compat_symbol (libc, __libc_free, cfree, GLIBC_2_0);
#endif

/* ------------------------------------------------------------
   History:

   [see ftp://g.oswego.edu/pub/misc/malloc.c for the history of dlmalloc]

 */
/*
 * Local variables:
 * c-basic-offset: 2
 * End:
 */
