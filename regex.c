/* Extended regular expression matching and search library.
   Copyright (C) 1985, 1989-90 Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 1, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.  */
/* Multi-byte extension added May, 1993 by t^2 (Takahiro Tanimoto)
   Last change: May 21, 1993 by t^2  */


/* To test, compile with -Dtest.  This Dtestable feature turns this into
   a self-contained program which reads a pattern, describes how it
   compiles, then reads a string and searches for it.

   On the other hand, if you compile with both -Dtest and -Dcanned you
   can run some tests we've already thought of.  */

/* We write fatal error messages on standard error.  */
#include <stdio.h>

/* isalpha(3) etc. are used for the character classes.  */
#include <ctype.h>
#include <sys/types.h>

#ifdef __STDC__
#define P(s)    s
#define MALLOC_ARG_T size_t
#else
#define P(s)    ()
#define MALLOC_ARG_T unsigned
#define volatile
#define const
#endif

#include "config.h"

void *xmalloc P((unsigned long));
void *xcalloc P((unsigned long,unsigned long));
void *xrealloc P((void*,unsigned long));
void free P((void*));

/* #define	NO_ALLOCA	/* try it out for now */
#ifndef NO_ALLOCA
/* Make alloca work the best possible way.  */
#ifdef __GNUC__
# ifndef atarist
#  ifndef alloca
#   define alloca __builtin_alloca
#  endif
# endif /* atarist */
#else
# if defined(HAVE_ALLOCA_H)
#  include <alloca.h>
# else
char *alloca();
# endif
#endif /* __GNUC__ */

#ifdef _AIX
#pragma alloca
#endif

#ifdef HAVE_STRING_H
# include <string.h>
#else
# include <strings.h>
#endif

#define RE_ALLOCATE alloca
#ifdef C_ALLOCA
#define FREE_VARIABLES() alloca(0)
#else
#define FREE_VARIABLES()
#endif

#define FREE_AND_RETURN_VOID(stackb)	return
#define FREE_AND_RETURN(stackb,val)	return(val)
#define DOUBLE_STACK(stackx,stackb,len,type) \
        (stackx = (type*)alloca(2 * len * sizeof(type)),		\
	/* Only copy what is in use.  */				\
        (type*)memcpy(stackx, stackb, len * sizeof (type)))
#else  /* NO_ALLOCA defined */

#define RE_ALLOCATE xmalloc

#define FREE_VAR(var) if (var) free(var); var = NULL
#define FREE_VARIABLES()						\
  do {									\
    FREE_VAR(regstart);							\
    FREE_VAR(regend);							\
    FREE_VAR(old_regstart)						\
    FREE_VAR(old_regend);						\
    FREE_VAR(best_regstart);						\
    FREE_VAR(best_regend);						\
    FREE_VAR(reg_info);							\
  } while (0)

#define FREE_AND_RETURN_VOID(stackb)   free(stackb);return
#define FREE_AND_RETURN(stackb,val)    free(stackb);return(val)
#define DOUBLE_STACK(stackx,stackb,len,type) \
        (type*)xrealloc(stackb, 2 * len * sizeof(type))
#endif /* NO_ALLOCA */

#define RE_TALLOC(n,t)  ((t*)RE_ALLOCATE((n)*sizeof(t)))
#define TMALLOC(n,t)    ((t*)xmalloc((n)*sizeof(t)))
#define TREALLOC(s,n,t) (s=((t*)xrealloc(s,(n)*sizeof(t))))

#define EXPAND_FAIL_STACK(stackx,stackb,len) \
    do {\
        /* Roughly double the size of the stack.  */			\
        stackx = DOUBLE_STACK(stackx,stackb,len,unsigned char*);	\
	/* Rearrange the pointers. */					\
	stackp = stackx + (stackp - stackb);				\
	stackb = stackx;						\
	stacke = stackb + 2 * len;					\
    } while (0)

/* Get the interface, including the syntax bits.  */
#include "regex.h"

/* Subroutines for re_compile_pattern.  */
static void store_jump P((char*, int, char*));
static void insert_jump P((int, char*, char*, char*));
static void store_jump_n P((char*, int, char*, unsigned));
static void insert_jump_n P((int, char*, char*, char*, unsigned));
static void insert_op P((int, char*, char*));
static void insert_op_2 P((int, char*, char*, int, int));
static int memcmp_translate P((unsigned char*, unsigned char*, int));
static int alt_match_null_string_p ();
static int common_op_match_null_string_p ();
static int group_match_null_string_p ();

/* Define the syntax stuff, so we can do the \<, \>, etc.  */

/* This must be nonzero for the wordchar and notwordchar pattern
   commands in re_match.  */
#ifndef Sword 
#define Sword 1
#endif

#define SYNTAX(c) re_syntax_table[c]

static char re_syntax_table[256];
static void init_syntax_once P((void));
static unsigned char *translate = 0;
static void init_regs P((struct re_registers*, unsigned int));
static void bm_init_skip P((int *, unsigned char*, int, char*));

#undef P

#include "util.h"

static void
init_syntax_once()
{
   register int c;
   static int done = 0;

   if (done)
     return;

   memset(re_syntax_table, 0, sizeof re_syntax_table);

   for (c = 'a'; c <= 'z'; c++)
     re_syntax_table[c] = Sword;

   for (c = 'A'; c <= 'Z'; c++)
     re_syntax_table[c] = Sword;

   for (c = '0'; c <= '9'; c++)
     re_syntax_table[c] = Sword;

   re_syntax_table['_'] = Sword;

   /* Add specific syntax for ISO Latin-1.  */
   for (c = 0300; c <= 0377; c++)
     re_syntax_table[c] = Sword;
   re_syntax_table[0327] = 0;
   re_syntax_table[0367] = 0;

   done = 1;
}

void
re_set_casetable(table)
     char *table;
{
  translate = (unsigned char*)table;
}

/* Jim Meyering writes:

   "... Some ctype macros are valid only for character codes that
   isascii says are ASCII (SGI's IRIX-4.0.5 is one such system --when
   using /bin/cc or gcc but without giving an ansi option).  So, all
   ctype uses should be through macros like ISPRINT...  If
   STDC_HEADERS is defined, then autoconf has verified that the ctype
   macros don't need to be guarded with references to isascii. ...
   Defining isascii to 1 should let any compiler worth its salt
   eliminate the && through constant folding."  */
#ifdef isblank
#define ISBLANK(c) isblank ((unsigned char)c)
#else
#define ISBLANK(c) ((c) == ' ' || (c) == '\t')
#endif
#ifdef isgraph
#define ISGRAPH(c) isgraph ((unsigned char)c)
#else
#define ISGRAPH(c) (isprint ((unsigned char)c) && !isspace ((unsigned char)c))
#endif

#define ISPRINT(c) isprint ((unsigned char)c)
#define ISDIGIT(c) isdigit ((unsigned char)c)
#define ISALNUM(c) isalnum ((unsigned char)c)
#define ISALPHA(c) isalpha ((unsigned char)c)
#define ISCNTRL(c) iscntrl ((unsigned char)c)
#define ISLOWER(c) islower ((unsigned char)c)
#define ISPUNCT(c) ispunct ((unsigned char)c)
#define ISSPACE(c) isspace ((unsigned char)c)
#define ISUPPER(c) isupper ((unsigned char)c)
#define ISXDIGIT(c) isxdigit ((unsigned char)c)

/* These are the command codes that appear in compiled regular
   expressions, one per byte.  Some command codes are followed by
   argument bytes.  A command code can specify any interpretation
   whatsoever for its arguments.  Zero-bytes may appear in the compiled
   regular expression.

   The value of `exactn' is needed in search.c (search_buffer) in emacs.
   So regex.h defines a symbol `RE_EXACTN_VALUE' to be 1; the value of
   `exactn' we use here must also be 1.  */

enum regexpcode
  {
    unused=0,
    exactn=1, /* Followed by one byte giving n, then by n literal bytes.  */
    begline,  /* Fail unless at beginning of line.  */
    endline,  /* Fail unless at end of line.  */
    begbuf,   /* Succeeds if at beginning of buffer (if emacs) or at beginning
                 of string to be matched (if not).  */
    endbuf,   /* Analogously, for end of buffer/string.  */
    endbuf2,  /* End of buffer/string, or newline just before it.  */
    jump,     /* Followed by two bytes giving relative address to jump to.  */
    jump_past_alt,/* Same as jump, but marks the end of an alternative.  */
    on_failure_jump,	 /* Followed by two bytes giving relative address of 
			    place to resume at in case of failure.  */
    finalize_jump,	 /* Throw away latest failure point and then jump to 
			    address.  */
    maybe_finalize_jump, /* Like jump but finalize if safe to do so.
			    This is used to jump back to the beginning
			    of a repeat.  If the command that follows
			    this jump is clearly incompatible with the
			    one at the beginning of the repeat, such that
			    we can be sure that there is no use backtracking
			    out of repetitions already completed,
			    then we finalize.  */
    dummy_failure_jump,  /* Jump, and push a dummy failure point. This 
			    failure point will be thrown away if an attempt 
                            is made to use it for a failure. A + construct 
                            makes this before the first repeat.  Also
                            use it as an intermediary kind of jump when
                            compiling an or construct.  */
    push_dummy_failure, /* Push a dummy failure point and continue.  Used at the end of
			   alternatives.  */
    succeed_n,	 /* Used like on_failure_jump except has to succeed n times;
		    then gets turned into an on_failure_jump. The relative
                    address following it is useless until then.  The
                    address is followed by two bytes containing n.  */
    jump_n,	 /* Similar to jump, but jump n times only; also the relative
		    address following is in turn followed by yet two more bytes
                    containing n.  */
    try_next,    /* Jump to next pattern for the first time,
		    leaving this pattern on the failure stack. */
    finalize_push,	/* Finalize stack and push the beginning of the pattern
			   on the stack to retry (used for non-greedy match) */
    finalize_push_n,	/* Similar to finalize_push, buf finalize n time only */
    set_number_at,	/* Set the following relative location to the
			   subsequent number.  */
    anychar,	 /* Matches any (more or less) one character.  */
    charset,     /* Matches any one char belonging to specified set.
		    First following byte is number of bitmap bytes.
		    Then come bytes for a bitmap saying which chars are in.
		    Bits in each byte are ordered low-bit-first.
		    A character is in the set if its bit is 1.
		    A character too large to have a bit in the map
		    is automatically not in the set.  */
    charset_not, /* Same parameters as charset, but match any character
                    that is not one of those specified.  */
    start_memory, /* Start remembering the text that is matched, for
		    storing in a memory register.  Followed by one
                    byte containing the register number.  Register numbers
                    must be in the range 0 through RE_NREGS.  */
    stop_memory, /* Stop remembering the text that is matched
		    and store it in a memory register.  Followed by
                    one byte containing the register number. Register
                    numbers must be in the range 0 through RE_NREGS.  */
    casefold_on,   /* Turn on casefold flag. */
    casefold_off,  /* Turn off casefold flag. */
    start_nowidth, /* Save string point to the stack. */
    stop_nowidth,  /* Restore string place at the point start_nowidth. */
    pop_and_fail,  /* Fail after popping nowidth entry from stack. */
    duplicate,   /* Match a duplicate of something remembered.
		    Followed by one byte containing the index of the memory 
                    register.  */
    wordchar,    /* Matches any word-constituent character.  */
    notwordchar, /* Matches any char that is not a word-constituent.  */
    wordbeg,	 /* Succeeds if at word beginning.  */
    wordend,	 /* Succeeds if at word end.  */
    wordbound,   /* Succeeds if at a word boundary.  */
    notwordbound,/* Succeeds if not at a word boundary.  */
  };


/* Number of failure points to allocate space for initially,
   when matching.  If this number is exceeded, more space is allocated,
   so it is not a hard limit.  */

#ifndef NFAILURES
#define NFAILURES 80
#endif

#if defined(CHAR_UNSIGNED) || defined(__CHAR_UNSIGNED__)
#define SIGN_EXTEND_CHAR(c) ((c)>(char)127?(c)-256:(c)) /* for IBM RT */
#endif
#ifndef SIGN_EXTEND_CHAR
#define SIGN_EXTEND_CHAR(x) (x)
#endif


/* Store NUMBER in two contiguous bytes starting at DESTINATION.  */
#define STORE_NUMBER(destination, number)				\
  { (destination)[0] = (number) & 0377;					\
    (destination)[1] = (number) >> 8; }

/* Same as STORE_NUMBER, except increment the destination pointer to
   the byte after where the number is stored.  Watch out that values for
   DESTINATION such as p + 1 won't work, whereas p will.  */
#define STORE_NUMBER_AND_INCR(destination, number)			\
  { STORE_NUMBER(destination, number);					\
    (destination) += 2; }


/* Put into DESTINATION a number stored in two contingous bytes starting
   at SOURCE.  */
#define EXTRACT_NUMBER(destination, source)				\
  { (destination) = *(source) & 0377;					\
    (destination) += SIGN_EXTEND_CHAR (*(char*)((source) + 1)) << 8; }

/* Same as EXTRACT_NUMBER, except increment the pointer for source to
   point to second byte of SOURCE.  Note that SOURCE has to be a value
   such as p, not, e.g., p + 1. */
#define EXTRACT_NUMBER_AND_INCR(destination, source)			\
  { EXTRACT_NUMBER(destination, source);				\
    (source) += 2; }


/* Specify the precise syntax of regexps for compilation.  This provides
   for compatibility for various utilities which historically have
   different, incompatible syntaxes.

   The argument SYNTAX is a bit-mask comprised of the various bits
   defined in regex.h.  */

long
re_set_syntax(syntax)
  long syntax;
{
  long ret;

  ret = re_syntax_options;
  re_syntax_options = syntax;
  return ret;
}

/* Set by re_set_syntax to the current regexp syntax to recognize.  */
long re_syntax_options = 0;


/* Macros for re_compile_pattern, which is found below these definitions.  */

#define TRANSLATE_P() ((options&RE_OPTION_IGNORECASE) && translate)
#define MAY_TRANSLATE() ((bufp->options&(RE_OPTION_IGNORECASE|RE_MAY_IGNORECASE)) && translate)
/* Fetch the next character in the uncompiled pattern---translating it 
   if necessary.  Also cast from a signed character in the constant
   string passed to us by the user to an unsigned char that we can use
   as an array index (in, e.g., `translate').  */
#define PATFETCH(c)							\
  do {if (p == pend) goto end_of_pattern;				\
    c = (unsigned char) *p++; 						\
    if (TRANSLATE_P()) c = (unsigned char)translate[c];	\
  } while (0)

/* Fetch the next character in the uncompiled pattern, with no
   translation.  */
#define PATFETCH_RAW(c)							\
  do {if (p == pend) goto end_of_pattern;				\
    c = (unsigned char)*p++; 						\
  } while (0)

/* Go backwards one character in the pattern.  */
#define PATUNFETCH p--


/* If the buffer isn't allocated when it comes in, use this.  */
#define INIT_BUF_SIZE  28

/* Make sure we have at least N more bytes of space in buffer.  */
#define GET_BUFFER_SPACE(n)						\
  {								        \
    while (b - bufp->buffer + (n) >= bufp->allocated)			\
      EXTEND_BUFFER;							\
  }

/* Make sure we have one more byte of buffer space and then add CH to it.  */
#define BUFPUSH(ch)							\
  {									\
    GET_BUFFER_SPACE(1);						\
    *b++ = (char)(ch);							\
  }

/* Extend the buffer by twice its current size via reallociation and
   reset the pointers that pointed into the old allocation to point to
   the correct places in the new allocation.  If extending the buffer
   results in it being larger than 1 << 16, then flag memory exhausted.  */
#define EXTEND_BUFFER							\
  { char *old_buffer = bufp->buffer;					\
    if (bufp->allocated == (1L<<16)) goto too_big;			\
    bufp->allocated *= 2;						\
    if (bufp->allocated > (1L<<16)) bufp->allocated = (1L<<16);		\
    bufp->buffer = (char*)xrealloc (bufp->buffer, bufp->allocated);	\
    if (bufp->buffer == 0)						\
      goto memory_exhausted;						\
    b = (b - old_buffer) + bufp->buffer;				\
    if (fixup_alt_jump)							\
      fixup_alt_jump = (fixup_alt_jump - old_buffer) + bufp->buffer;	\
    if (laststart)							\
      laststart = (laststart - old_buffer) + bufp->buffer;		\
    begalt = (begalt - old_buffer) + bufp->buffer;			\
    if (pending_exact)							\
      pending_exact = (pending_exact - old_buffer) + bufp->buffer;	\
  }


/* Set the bit for character C in a character set list.  */
#define SET_LIST_BIT(c)							\
  (b[(unsigned char)(c) / BYTEWIDTH]					\
   |= 1 << ((unsigned char)(c) % BYTEWIDTH))

/* Get the next unsigned number in the uncompiled pattern.  */
#define GET_UNSIGNED_NUMBER(num) 					\
  { if (p != pend) 							\
      { 								\
        PATFETCH(c); 							\
	while (ISDIGIT(c)) 						\
	  { 								\
	    if (num < 0) 						\
	       num = 0; 						\
            num = num * 10 + c - '0'; 					\
	    if (p == pend) 						\
	       break; 							\
	    PATFETCH(c); 						\
	  } 								\
        } 								\
  }

#define STREQ(s1, s2) ((strcmp (s1, s2) == 0))

#define CHAR_CLASS_MAX_LENGTH  6 /* Namely, `xdigit'.  */

#define IS_CHAR_CLASS(string)						\
   (STREQ (string, "alpha") || STREQ (string, "upper")			\
    || STREQ (string, "lower") || STREQ (string, "digit")		\
    || STREQ (string, "alnum") || STREQ (string, "xdigit")		\
    || STREQ (string, "space") || STREQ (string, "print")		\
    || STREQ (string, "punct") || STREQ (string, "graph")		\
    || STREQ (string, "cntrl") || STREQ (string, "blank"))

#define STORE_MBC(p, c) \
  ((p)[0] = (unsigned char)(c >> 8), (p)[1] = (unsigned char)(c))
#define STORE_MBC_AND_INCR(p, c) \
  (*(p)++ = (unsigned char)(c >> 8), *(p)++ = (unsigned char)(c))

#define EXTRACT_MBC(p) \
  ((unsigned short)((unsigned char)(p)[0] << 8 | (unsigned char)(p)[1]))
#define EXTRACT_MBC_AND_INCR(p) \
  ((unsigned short)((p) += 2, (unsigned char)(p)[-2] << 8 | (unsigned char)(p)[-1]))

#define EXTRACT_UNSIGNED(p) \
  ((unsigned char)(p)[0] | (unsigned char)(p)[1] << 8)
#define EXTRACT_UNSIGNED_AND_INCR(p) \
  ((p) += 2, (unsigned char)(p)[-2] | (unsigned char)(p)[-1] << 8)

/* Handle (mb)?charset(_not)?.

   Structure of mbcharset(_not)? in compiled pattern.

     struct {
       unsinged char id;		mbcharset(_not)?
       unsigned char sbc_size;
       unsigned char sbc_map[sbc_size];	same as charset(_not)? up to here.
       unsigned short mbc_size;		number of intervals.
       struct {
	 unsigned short beg;		beginning of interval.
	 unsigned short end;		end of interval.
       } intervals[mbc_size];
     }; */

static void
set_list_bits(c1, c2, b)
    unsigned short c1, c2;
    unsigned char *b;
{
  unsigned char sbc_size = b[-1];
  unsigned short mbc_size = EXTRACT_UNSIGNED(&b[sbc_size]);
  unsigned short beg, end, upb;

  if (c1 > c2)
    return;
  if ((int)c1 < 1 << BYTEWIDTH) {
    upb = c2;
    if (1 << BYTEWIDTH <= (int)upb)
      upb = (1 << BYTEWIDTH) - 1;	/* The last single-byte char */
    if (sbc_size <= (unsigned short)(upb / BYTEWIDTH)) {
      /* Allocate maximum size so it never happens again.  */
      /* NOTE: memcpy() would not work here.  */
      memmove(&b[(1 << BYTEWIDTH) / BYTEWIDTH], &b[sbc_size], 2 + mbc_size*4);
      memset(&b[sbc_size], 0, (1 << BYTEWIDTH) / BYTEWIDTH - sbc_size);
      b[-1] = sbc_size = (1 << BYTEWIDTH) / BYTEWIDTH;
    }
    for (; c1 <= upb; c1++)
	if (!ismbchar(c1))
	    SET_LIST_BIT(c1);
    if ((int)c2 < 1 << BYTEWIDTH)
      return;
    c1 = 0x8000;			/* The first wide char */
  }
  b = &b[sbc_size + 2];

  for (beg = 0, upb = mbc_size; beg < upb; ) {
    unsigned short mid = (unsigned short)(beg + upb) >> 1;

    if ((int)c1 - 1 > (int)EXTRACT_MBC(&b[mid*4 + 2]))
      beg = mid + 1;
    else
      upb = mid;
  }

  for (end = beg, upb = mbc_size; end < upb; ) {
    unsigned short mid = (unsigned short)(end + upb) >> 1;

    if ((int)c2 >= (int)EXTRACT_MBC(&b[mid*4]) - 1)
      end = mid + 1;
    else
      upb = mid;
  }

  if (beg != end) {
    if (c1 > EXTRACT_MBC(&b[beg*4]))
      c1 = EXTRACT_MBC(&b[beg*4]);
    if (c2 < EXTRACT_MBC(&b[(end - 1)*4+2]))
      c2 = EXTRACT_MBC(&b[(end - 1)*4+2]);
  }
  if (end < mbc_size && end != beg + 1)
    /* NOTE: memcpy() would not work here.  */
    memmove(&b[(beg + 1)*4], &b[end*4], (mbc_size - end)*4);
  STORE_MBC(&b[beg*4 + 0], c1);
  STORE_MBC(&b[beg*4 + 2], c2);
  mbc_size += beg - end + 1;
  STORE_NUMBER(&b[-2], mbc_size);
}

static int
is_in_list(c, b)
    unsigned short c;
    const unsigned char *b;
{
    unsigned short size;
    unsigned short i, j;
    int result = 0;

    size = *b++;
    if ((int)c < 1<<BYTEWIDTH) {
	if ((int)c / BYTEWIDTH < (int)size && b[c / BYTEWIDTH] & 1 << c % BYTEWIDTH) {
	    return 1;
	}
    }
    b += size + 2;
    size = EXTRACT_UNSIGNED(&b[-2]);
    if (size == 0) return 0;

    if (b[(size-1)*4] == 0xff) {
	i = c;
	if ((int)c >= 1<<BYTEWIDTH) {
	    i = i>>BYTEWIDTH;
	}
	while (size>0 && b[size*4-2] == 0xff) {
	    size--;
	    if (b[size*4+1] <= i && i <= b[size*4+3]) {
		result = 2;
		break;
	    }
	}
    }
    for (i = 0, j = size; i < j; ) {
	unsigned short k = (unsigned short)(i + j) >> 1;

	if (c > EXTRACT_MBC(&b[k*4+2]))
	    i = k + 1;
	else
	    j = k;
    }
    if (i < size && EXTRACT_MBC(&b[i*4]) <= c
	&& ((unsigned char)c != '\n' && (unsigned char)c != '\0'))
	return 1;
    return result;
}

static void
print_partial_compiled_pattern(start, end)
    unsigned char *start;
    unsigned char *end;
{
  int mcnt, mcnt2;
  unsigned char *p = start;
  unsigned char *pend = end;

  if (start == NULL)
    {
      printf ("(null)\n");
      return;
    }
    
  /* Loop over pattern commands.  */
  while (p < pend)
    {
      switch ((enum regexpcode)*p++)
	{
	case unused:
	  printf ("/unused");
	  break;

	case exactn:
	  mcnt = *p++;
          printf ("/exactn/%d", mcnt);
          do
	    {
              putchar('/');
	      printf("%c", *p++);
            }
          while (--mcnt);
          break;

	case start_memory:
          mcnt = *p++;
          printf ("/start_memory/%d/%d", mcnt, *p++);
          break;

	case stop_memory:
          mcnt = *p++;
	  printf ("/stop_memory/%d/%d", mcnt, *p++);
          break;

	case casefold_on:
	  printf ("/casefold_on");
	  break;

	case casefold_off:
	  printf ("/casefold_off");
	  break;

	case start_nowidth:
          EXTRACT_NUMBER_AND_INCR (mcnt, p);
	  printf ("/start_nowidth//%d", mcnt);
	  break;

	case stop_nowidth:
	  printf ("/stop_nowidth//");
	  p += 2;
	  break;

	case pop_and_fail:
	  printf ("/pop_and_fail");
	  break;

	case duplicate:
	  printf ("/duplicate/%d", *p++);
	  break;

	case anychar:
	  printf ("/anychar");
	  break;

	case charset:
        case charset_not:
          {
            register int c;

            printf ("/charset%s",
	            (enum regexpcode)*(p - 1) == charset_not ? "_not" : "");

            mcnt = *p++;
	    printf("/%d", mcnt);
            for (c = 0; c < mcnt; c++)
              {
                unsigned bit;
                unsigned char map_byte = p[c];

		putchar ('/');
                
		for (bit = 0; bit < BYTEWIDTH; bit++)
                  if (map_byte & (1 << bit))
		    printf("%c", c * BYTEWIDTH + bit);
              }
	    p += mcnt;
	    mcnt = EXTRACT_UNSIGNED_AND_INCR(p);
	    while (mcnt--) {
		int beg, end;
		beg = EXTRACT_MBC_AND_INCR(p);
		end = EXTRACT_MBC_AND_INCR(p);
		printf("/%c%c-%c%c", beg>>BYTEWIDTH, beg&0xff, end>>BYTEWIDTH, end&0xff);
	    }
	    break;
	  }

	case begline:
	  printf ("/begline");
          break;

	case endline:
          printf ("/endline");
          break;

	case on_failure_jump:
          EXTRACT_NUMBER_AND_INCR (mcnt, p);
  	  printf ("/on_failure_jump//%d", mcnt);
          break;

	case dummy_failure_jump:
          EXTRACT_NUMBER_AND_INCR (mcnt, p);
  	  printf ("/dummy_failure_jump//%d", mcnt);
          break;

	case push_dummy_failure:
          printf ("/push_dummy_failure");
          break;
          
        case finalize_jump:
          EXTRACT_NUMBER_AND_INCR (mcnt, p);
  	  printf ("/finalize_jump//%d", mcnt);
	  break;

        case maybe_finalize_jump:
          EXTRACT_NUMBER_AND_INCR (mcnt, p);
  	  printf ("/maybe_finalize_jump//%d", mcnt);
	  break;

        case jump_past_alt:
	  EXTRACT_NUMBER_AND_INCR (mcnt, p);
  	  printf ("/jump_past_alt//%d", mcnt);
	  break;

        case jump:
	  EXTRACT_NUMBER_AND_INCR (mcnt, p);
  	  printf ("/jump//%d", mcnt);
	  break;

        case succeed_n: 
          EXTRACT_NUMBER_AND_INCR (mcnt, p);
          EXTRACT_NUMBER_AND_INCR (mcnt2, p);
 	  printf ("/succeed_n//%d//%d", mcnt, mcnt2);
          break;
        
        case jump_n: 
          EXTRACT_NUMBER_AND_INCR (mcnt, p);
          EXTRACT_NUMBER_AND_INCR (mcnt2, p);
 	  printf ("/jump_n//%d//%d", mcnt, mcnt2);
          break;
        
        case set_number_at: 
          EXTRACT_NUMBER_AND_INCR (mcnt, p);
          EXTRACT_NUMBER_AND_INCR (mcnt2, p);
 	  printf ("/set_number_at//%d//%d", mcnt, mcnt2);
          break;
        
	case try_next:
	  EXTRACT_NUMBER_AND_INCR (mcnt, p);
  	  printf ("/try_next//%d", mcnt);
          break;

	case finalize_push:
	  EXTRACT_NUMBER_AND_INCR (mcnt, p);
  	  printf ("/finalize_push//%d", mcnt);
          break;

	case finalize_push_n:
          EXTRACT_NUMBER_AND_INCR (mcnt, p);
          EXTRACT_NUMBER_AND_INCR (mcnt2, p);
 	  printf ("/finalize_push_n//%d//%d", mcnt, mcnt2);
          break;

        case wordbound:
	  printf ("/wordbound");
	  break;

	case notwordbound:
	  printf ("/notwordbound");
          break;

	case wordbeg:
	  printf ("/wordbeg");
	  break;
          
	case wordend:
	  printf ("/wordend");
          
	case wordchar:
	  printf ("/wordchar");
          break;
	  
	case notwordchar:
	  printf ("/notwordchar");
          break;

	case begbuf:
	  printf ("/begbuf");
          break;

	case endbuf:
	  printf ("/endbuf");
          break;

	case endbuf2:
	  printf ("/endbuf2");
          break;

        default:
          printf ("?%d", *(p-1));
	}
    }
  printf ("/\n");
}


static void
print_compiled_pattern(bufp)
    struct re_pattern_buffer *bufp;
{
  unsigned char *buffer = (unsigned char*)bufp->buffer;

  print_partial_compiled_pattern (buffer, buffer + bufp->used);
}

static char*
calculate_must_string(start, end)
    char *start;
    char *end;
{
  int mcnt;
  int max = 0;
  char *p = start;
  char *pend = end;
  char *must = 0;

  if (start == NULL) return 0;
    
  /* Loop over pattern commands.  */
  while (p < pend)
    {
      switch ((enum regexpcode)*p++)
	{
	case unused:
	  break;

	case exactn:
	  mcnt = *p;
	  if (mcnt > max) {
	    must = p;
	    max = mcnt;
	  }
	  p += mcnt+1;
          break;

	case start_memory:
	case stop_memory:
	  p += 2;
	  break;

	case duplicate:
          p++;
          break;

	case casefold_on:
	case casefold_off:
	  return 0;		/* should not check must_string */

	case pop_and_fail:
	case anychar:
	case begline:
	case endline:
        case wordbound:
	case notwordbound:
	case wordbeg:
	case wordend:
	case wordchar:
	case notwordchar:
	case begbuf:
	case endbuf:
	case endbuf2:
        case push_dummy_failure:
	  break;

	case charset:
        case charset_not:
	  mcnt = *p++;
	  p += mcnt;
	  mcnt = EXTRACT_UNSIGNED_AND_INCR(p);
	  while (mcnt--) {
	    p += 4;
	  }
	  break;

	case on_failure_jump:
          EXTRACT_NUMBER_AND_INCR (mcnt, p);
	  if (mcnt > 0) p += mcnt;
	  if ((enum regexpcode)p[-3] == jump) {
	    p -= 3;
	    EXTRACT_NUMBER_AND_INCR (mcnt, p);
	    if (mcnt > 0) p += mcnt;
	  }
          break;

	case dummy_failure_jump:
        case succeed_n: 
	case try_next:
	case jump:
          EXTRACT_NUMBER_AND_INCR (mcnt, p);
	  if (mcnt > 0) p += mcnt;
          break;

	case start_nowidth:
	case stop_nowidth:
        case finalize_jump:
        case maybe_finalize_jump:
	case finalize_push:
	  p += 2;
	  break;

        case jump_n: 
        case set_number_at: 
	case finalize_push_n:
	  p += 4;
          break;

        default:
	  break;
	}
    }
  return must;
}


/* re_compile_pattern takes a regular-expression string
   and converts it into a buffer full of byte commands for matching.

   PATTERN   is the address of the pattern string
   SIZE      is the length of it.
   BUFP	    is a  struct re_pattern_buffer *  which points to the info
	     on where to store the byte commands.
	     This structure contains a  char *  which points to the
	     actual space, which should have been obtained with malloc.
	     re_compile_pattern may use realloc to grow the buffer space.

   The number of bytes of commands can be found out by looking in
   the `struct re_pattern_buffer' that bufp pointed to, after
   re_compile_pattern returns. */

char *
re_compile_pattern(pattern, size, bufp)
     char *pattern;
     size_t size;
     struct re_pattern_buffer *bufp;
{
    register char *b = bufp->buffer;
    register char *p = pattern;
    char *pend = pattern + size;
    register unsigned c, c1;
    char *p0;
    int numlen;

    /* Address of the count-byte of the most recently inserted `exactn'
       command.  This makes it possible to tell whether a new exact-match
       character can be added to that command or requires a new `exactn'
       command.  */

    char *pending_exact = 0;

    /* Address of the place where a forward-jump should go to the end of
       the containing expression.  Each alternative of an `or', except the
       last, ends with a forward-jump of this sort.  */

    char *fixup_alt_jump = 0;

    /* Address of start of the most recently finished expression.
       This tells postfix * where to find the start of its operand.  */

    char *laststart = 0;

    /* In processing a repeat, 1 means zero matches is allowed.  */

    char zero_times_ok;

    /* In processing a repeat, 1 means many matches is allowed.  */

    char many_times_ok;

    /* In processing a repeat, 1 means non-greedy matches.  */

    char greedy;

    /* Address of beginning of regexp, or inside of last (.  */

    char *begalt = b;

    /* Place in the uncompiled pattern (i.e., the {) to
       which to go back if the interval is invalid.  */
    char *beg_interval;

    /* In processing an interval, at least this many matches must be made.  */
    int lower_bound;

    /* In processing an interval, at most this many matches can be made.  */
    int upper_bound;

    /* Stack of information saved by ( and restored by ).
       Five stack elements are pushed by each (:
       First, the value of b.
       Second, the value of fixup_alt_jump.
       Third, the value of begalt.
       Fourth, the value of regnum.
       Fifth, the type of the paren. */

    int *stackb = RE_TALLOC(40, int);
    int *stackp = stackb;
    int *stacke = stackb + 40;
    int *stackt;

    /* Counts ('s as they are encountered.  Remembered for the matching ),
       where it becomes the register number to put in the stop_memory
       command.  */

    int regnum = 1;

    int range = 0;
    int had_char_class = 0;

    int options = bufp->options;

    bufp->fastmap_accurate = 0;
    bufp->must = 0;
    bufp->must_skip = 0;
    bufp->stclass = 0;

    /* Initialize the syntax table.  */
    init_syntax_once();

    if (bufp->allocated == 0) {
      bufp->allocated = INIT_BUF_SIZE;
      if (bufp->buffer)
	/* EXTEND_BUFFER loses when bufp->allocated is 0.  */
	bufp->buffer = (char*)xrealloc (bufp->buffer, INIT_BUF_SIZE);
      else
	/* Caller did not allocate a buffer.  Do it for them.  */
	bufp->buffer = (char*)xmalloc(INIT_BUF_SIZE);
      if (!bufp->buffer) goto memory_exhausted;
      begalt = b = bufp->buffer;
    }

    while (p != pend) {
      PATFETCH(c);

      switch (c)
	{
	case '$':
	  {
	    p0 = p;
	    /* When testing what follows the $,
	       look past the \-constructs that don't consume anything.  */

	    while (p0 != pend)
	      {
		if (*p0 == '\\' && p0 + 1 != pend
		    && (p0[1] == 'b' || p0[1] == 'B'))
		  p0 += 2;
		else
		  break;
	      }
	    /* $ means succeed if at end of line, but only in special contexts.
	      If validly in the middle of a pattern, it is a normal character. */

	    if (p0 == pend || *p0 == '\n'
		|| *p0 == ')'
		|| *p0 == '|')
	      {
		BUFPUSH(endline);
		break;
	      }
	    goto normal_char;
          }
	case '^':
	  /* ^ means succeed if at beg of line, but only if no preceding 
             pattern.  */

          if (laststart)
            goto invalid_pattern;
          if (laststart && p - 2 >= pattern && p[-2] != '\n')
	    goto normal_char;
	  BUFPUSH(begline);
	  break;

	case '+':
	case '?':
	case '*':
	  /* If there is no previous pattern, char not special. */
	  if (!laststart) {
	    goto invalid_pattern;
	  }
	  /* If there is a sequence of repetition chars,
	     collapse it down to just one.  */
	  zero_times_ok = c != '+';
	  many_times_ok = c != '?';
	  greedy = 1;
	  if (p != pend) {
	    PATFETCH(c);
	    switch (c) {
	      case '?':
		greedy = 0;
		break;
	      case '*':
	      case '+':
		goto nested_meta;
	      default:
		PATUNFETCH;
		break;
	    }
	  }

	repeat:
	  /* Star, etc. applied to an empty pattern is equivalent
	     to an empty pattern.  */
	  if (!laststart)  
	    break;

	  /* Now we know whether or not zero matches is allowed
	     and also whether or not two or more matches is allowed.  */
	  if (many_times_ok) {
	    /* If more than one repetition is allowed, put in at the
	       end a backward relative jump from b to before the next
	       jump we're going to put in below (which jumps from
	       laststart to after this jump).  */
	    GET_BUFFER_SPACE(3);
	    store_jump(b,greedy?maybe_finalize_jump:finalize_push,laststart-3);
	    b += 3;  	/* Because store_jump put stuff here.  */
	  }

	  /* On failure, jump from laststart to next pattern, which will be the
	     end of the buffer after this jump is inserted.  */
	  GET_BUFFER_SPACE(3);
	  insert_jump(on_failure_jump, laststart, b + 3, b);
	  b += 3;

	  if (zero_times_ok) {
	    if (greedy == 0) {
	      GET_BUFFER_SPACE(3);
	      insert_jump(try_next, laststart, b + 3, b);
	      b += 3;
	    }
	  }
	  else {
	    /* At least one repetition is required, so insert a
	       `dummy_failure_jump' before the initial
	       `on_failure_jump' instruction of the loop. This
	       effects a skip over that instruction the first time
	       we hit that loop.  */
	    GET_BUFFER_SPACE(3);
	    insert_jump(dummy_failure_jump, laststart, laststart + 6, b);
	    b += 3;
	  }
	  break;

	case '.':
	  laststart = b;
	  BUFPUSH(anychar);
	  break;

        case '[':
          if (p == pend)
            goto invalid_pattern;
	  while (b - bufp->buffer
		 > bufp->allocated - 9 - (1 << BYTEWIDTH) / BYTEWIDTH)
	    EXTEND_BUFFER;

	  laststart = b;
	  if (*p == '^')
	    {
              BUFPUSH(charset_not); 
              p++;
            }
	  else
	    BUFPUSH(charset);
	  p0 = p;

	  BUFPUSH((1 << BYTEWIDTH) / BYTEWIDTH);
	  /* Clear the whole map */
	  memset(b, 0, (1 << BYTEWIDTH) / BYTEWIDTH + 2);

	  if ((re_syntax_options & RE_HAT_NOT_NEWLINE) && b[-2] == charset_not)
            SET_LIST_BIT('\n');

	  had_char_class = 0;
	  /* Read in characters and ranges, setting map bits.  */
	  for (;;)
	    {
	      int size;
	      unsigned last = (unsigned)-1;

	      if ((size = EXTRACT_UNSIGNED(&b[(1 << BYTEWIDTH) / BYTEWIDTH]))) {
		/* Ensure the space is enough to hold another interval
		   of multi-byte chars in charset(_not)?.  */
		size = (1 << BYTEWIDTH) / BYTEWIDTH + 2 + size*4 + 4;
		while (b + size + 1 > bufp->buffer + bufp->allocated)
		  EXTEND_BUFFER;
	      }
	    range_retry:
	      PATFETCH(c);

              if (c == ']') {
                  if (p == p0 + 1) {
                      if (p == pend)
			  goto invalid_pattern;
		  }
                  else 
		    /* Stop if this isn't merely a ] inside a bracket
                       expression, but rather the end of a bracket
                       expression.  */
		      break;
	      }
	      /* Look ahead to see if it's a range when the last thing
		 was a character class.  */
	      if (had_char_class && c == '-' && *p != ']')
		  goto invalid_pattern;
	      if (ismbchar(c)) {
		PATFETCH(c1);
		c = c << BYTEWIDTH | c1;
	      }

	      /* \ escapes characters when inside [...].  */
	      if (c == '\\') {
	          PATFETCH(c);
		  switch (c) {
		    case 'w':
		      for (c = 0; c < (1 << BYTEWIDTH); c++)
		          if (SYNTAX(c) == Sword)
			      SET_LIST_BIT(c);
		      last = -1;
		      continue;

		    case 'W':
		      for (c = 0; c < (1 << BYTEWIDTH); c++)
		          if (SYNTAX(c) != Sword)
			      SET_LIST_BIT(c);
		      if (current_mbctype) {
			  set_list_bits(0x8000, 0xffff, (unsigned char*)b);
		      }
		      last = -1;
		      continue;

		    case 's':
		      for (c = 0; c < 256; c++)
			  if (ISSPACE(c))
			      SET_LIST_BIT(c);
		      last = -1;
		      continue;

		    case 'S':
		      for (c = 0; c < 256; c++)
			  if (!ISSPACE(c))
			      SET_LIST_BIT(c);
		      if (current_mbctype) {
			  set_list_bits(0x8000, 0xffff, (unsigned char*)b);
		      }
		      last = -1;
		      continue;

		    case 'd':
		      for (c = '0'; c <= '9'; c++)
			  SET_LIST_BIT(c);
		      last = -1;
		      continue;

		    case 'D':
		      for (c = 0; c < 256; c++)
			  if (!ISDIGIT(c))
			      SET_LIST_BIT(c);
		      if (current_mbctype) {
			  set_list_bits(0x8000, 0xffff, (unsigned char*)b);
		      }
		      last = -1;
		      continue;

		    case 'x':
		      c = scan_hex(p, 2, &numlen);
		      if (current_mbctype && c > 0x7f)
			  c = 0xff00 | c;
		      p += numlen;
		      break;

		    case '0': case '1': case '2': case '3': case '4':
		    case '5': case '6': case '7': case '8': case '9':
		      PATUNFETCH;
		      c = scan_oct(p, 3, &numlen);
		      if (ismbchar(c))
			  c |= 0xff00;
		      p += numlen;
		      break;

		    default:
		      if (ismbchar(c)) {
			  PATFETCH(c1);
			  c = c << 8 | c1;
		      }
		      break;
		  }
	      }

              /* Get a range.  */
	      if (range) {
		  if (last > c)
                    goto invalid_pattern;

		  range = 0;
		  if (last < 1 << BYTEWIDTH && c < 1 << BYTEWIDTH) {
		      for (;last<=c;last++)
			  SET_LIST_BIT(last);
		  }
		  else {
		      set_list_bits(last, c, (unsigned char*)b);
		  }
	      }
              else if (p[0] == '-' && p[1] != ']') {
		  last = c;
		  PATFETCH(c1);
		  range = 1;
		  goto range_retry;
	      }
	      else if ((re_syntax_options & RE_CHAR_CLASSES)
		       && c == '[' && *p == ':') {
		  /* Leave room for the null.  */
		  char str[CHAR_CLASS_MAX_LENGTH + 1];

		  PATFETCH_RAW (c);
		  c1 = 0;

		  /* If pattern is `[[:'.  */
		  if (p == pend) 
		      goto invalid_pattern;

		  for (;;) {
		      PATFETCH (c);
		      if (c == ':' || c == ']' || p == pend
			  || c1 == CHAR_CLASS_MAX_LENGTH)
                          break;
		      str[c1++] = c;
		  }
		  str[c1] = '\0';

		  /* If isn't a word bracketed by `[:' and:`]':
		     undo the ending character, the letters, and leave 
		     the leading `:' and `[' (but set bits for them).  */
		  if (c == ':' && *p == ']') {
		      int ch;
		      char is_alnum = STREQ (str, "alnum");
		      char is_alpha = STREQ (str, "alpha");
		      char is_blank = STREQ (str, "blank");
		      char is_cntrl = STREQ (str, "cntrl");
		      char is_digit = STREQ (str, "digit");
		      char is_graph = STREQ (str, "graph");
		      char is_lower = STREQ (str, "lower");
		      char is_print = STREQ (str, "print");
		      char is_punct = STREQ (str, "punct");
		      char is_space = STREQ (str, "space");
		      char is_upper = STREQ (str, "upper");
		      char is_xdigit = STREQ (str, "xdigit");

		      if (!IS_CHAR_CLASS (str))
			  goto invalid_pattern;

		      /* Throw away the ] at the end of the character
			 class.  */
		      PATFETCH (c);					

		      if (p == pend) 
			  goto invalid_pattern;


		      for (ch = 0; ch < 1 << BYTEWIDTH; ch++) {
			  if (   (is_alnum  && ISALNUM (ch))
			      || (is_alpha  && ISALPHA (ch))
			      || (is_blank  && ISBLANK (ch))
			      || (is_cntrl  && ISCNTRL (ch))
			      || (is_digit  && ISDIGIT (ch))
			      || (is_graph  && ISGRAPH (ch))
			      || (is_lower  && ISLOWER (ch))
			      || (is_print  && ISPRINT (ch))
			      || (is_punct  && ISPUNCT (ch))
			      || (is_space  && ISSPACE (ch))
			      || (is_upper  && ISUPPER (ch))
			      || (is_xdigit && ISXDIGIT (ch)))
			      SET_LIST_BIT (ch);
		      }
		      had_char_class = 1;
		  }
		  else {
		      c1++;
		      while (c1--)    
                          PATUNFETCH;
		      SET_LIST_BIT(translate?translate['[']:'[');
		      SET_LIST_BIT(translate?translate[':']:':');
		      had_char_class = 0;
		      last = ':';
                  }
	      }
	      else if (c < 1 << BYTEWIDTH)
		SET_LIST_BIT(c);
	      else
		set_list_bits(c, c, (unsigned char*)b);
	    }

          /* Discard any character set/class bitmap bytes that are all
             0 at the end of the map. Decrement the map-length byte too.  */
          while ((int)b[-1] > 0 && b[b[-1] - 1] == 0) 
            b[-1]--; 
	  if (b[-1] != (1 << BYTEWIDTH) / BYTEWIDTH)
	    memmove(&b[b[-1]], &b[(1 << BYTEWIDTH) / BYTEWIDTH],
		    2 + EXTRACT_UNSIGNED (&b[(1 << BYTEWIDTH) / BYTEWIDTH])*4);
	  b += b[-1] + 2 + EXTRACT_UNSIGNED (&b[b[-1]])*4;
          break;

	case '(':
	  PATFETCH(c);
	  if (c == '?') {
	      int negative = 0;
	      PATFETCH_RAW(c);
	      switch (c) {
		case 'x': case 'i': case '-':
		  for (;;) {
		    switch (c) {
		    case '-':
		      negative = 1;
		      break;

		    case ':':
		    case ')':
		      break;

		    case 'x':
		      if (negative)
			options &= ~RE_OPTION_EXTENDED;
		      else
			options |= RE_OPTION_EXTENDED;
		      break;
		    case 'i':
		      if (negative) {
			if (options&RE_OPTION_IGNORECASE) {
			  options &= ~RE_OPTION_IGNORECASE;
			  BUFPUSH(casefold_off);
			}
		      }
		      else if (!(options&RE_OPTION_IGNORECASE)) {
			options |= RE_OPTION_IGNORECASE;
			BUFPUSH(casefold_on);
		      }
		      break;

		    default:
		      FREE_AND_RETURN(stackb, "undefined (?...) inline option");
		    }
		    if (c == ')') {
		      c = '#';	/* read whole in-line options */
		      break;
		    }
		    if (c == ':') break;
		    PATFETCH_RAW(c);
		  }
		  break;

		case '#':
		  for (;;) {
		      PATFETCH(c);
		      if (c == ')') break;
		  }
		  c = '#';
		  break;

		case ':':
		case '=':
		case '!':
		  break;

		default:
		  FREE_AND_RETURN(stackb, "undefined (?...) sequence");
	      }
	  }
	  else {
	    PATUNFETCH;
	    c = '(';
	  }
	  if (c == '#') break;
	  if (stackp+8 >= stacke) {
	    int *stackx;
	    unsigned int len = stacke - stackb;

	    stackx = DOUBLE_STACK(stackx,stackb,len,int);
	    /* Rearrange the pointers. */
	    stackp = stackx + (stackp - stackb);
	    stackb = stackx;
	    stacke = stackb + 2 * len;
	  }

	  /* Laststart should point to the start_memory that we are about
	     to push (unless the pattern has RE_NREGS or more ('s).  */
	  /* obsolete: now RE_NREGS is just a default register size. */
	  *stackp++ = b - bufp->buffer;    
	  *stackp++ = fixup_alt_jump ? fixup_alt_jump - bufp->buffer + 1 : 0;
	  *stackp++ = begalt - bufp->buffer;
	  switch (c) {
	    case '(':
	      BUFPUSH(start_memory);
	      BUFPUSH(regnum);
	      *stackp++ = regnum++;
	      *stackp++ = b - bufp->buffer;
	      BUFPUSH(0);
	      /* too many ()'s to fit in a byte. (max 254) */
	      if (regnum >= RE_REG_MAX) goto too_big;
	      break;

	    case '=':
	    case '!':
	      BUFPUSH(start_nowidth);
	      *stackp++ = b - bufp->buffer;
	      BUFPUSH(0);	/* temporary value */
	      BUFPUSH(0);
	      if (c == '=') break;

	      BUFPUSH(on_failure_jump);
	      *stackp++ = b - bufp->buffer;
	      BUFPUSH(0);	/* temporary value */
	      BUFPUSH(0);
	      break;

	    case ':':
	      pending_exact = 0;
	    default:
	      break;
	  }
	  *stackp++ = c;
	  *stackp++ = options;
	  fixup_alt_jump = 0;
	  laststart = 0;
	  begalt = b;
	  break;

	case ')':
	  if (stackp == stackb) 
	    FREE_AND_RETURN(stackb, "unmatched )");
	  if ((options ^ stackp[-1]) & RE_OPTION_IGNORECASE) {
	    BUFPUSH((options&RE_OPTION_IGNORECASE)?casefold_off:casefold_on);
	  }
	  pending_exact = 0;
	  if (fixup_alt_jump)
	  { /* Push a dummy failure point at the end of the
	       alternative for a possible future
	       `finalize_jump' to pop.  See comments at
	       `push_dummy_failure' in `re_match'.  */
	      BUFPUSH(push_dummy_failure);
                  
	      /* We allocated space for this jump when we assigned
		 to `fixup_alt_jump', in the `handle_alt' case below.  */
	      store_jump(fixup_alt_jump, jump, b);
	  }
          options = *--stackp;
          switch (c = *--stackp) {
            case '(':
              {
		char *loc = bufp->buffer + *--stackp;
		*loc = regnum - stackp[-1];
		BUFPUSH(stop_memory);
		BUFPUSH(stackp[-1]);
		BUFPUSH(regnum - stackp[-1]);
		stackp--;
	      }
	      break;

	    case '!':
	      BUFPUSH(pop_and_fail);
	      /* back patch */
	      STORE_NUMBER(bufp->buffer+stackp[-1], b - bufp->buffer - stackp[-1] - 2);
	      stackp--;
	      /* fall through */
	    case '=':
	      BUFPUSH(stop_nowidth);
	      /* tell stack-pos place to start_nowidth */
	      STORE_NUMBER(bufp->buffer+stackp[-1], b - bufp->buffer - stackp[-1] - 2);
	      BUFPUSH(0);	/* space to hold stack pos */
	      BUFPUSH(0);
	      stackp--;
	      break;

	    case ':':
	    default:
	      break;
	  }
	  begalt = *--stackp + bufp->buffer;
	  stackp--;
	  fixup_alt_jump = *stackp ? *stackp + bufp->buffer - 1 : 0;
	  laststart = *--stackp + bufp->buffer;
	  if (c == '!' || c == '=') laststart = b;
	  break;

	case '|':
	  /* Insert before the previous alternative a jump which
	     jumps to this alternative if the former fails.  */
	  GET_BUFFER_SPACE(3);
	  insert_jump(on_failure_jump, begalt, b + 6, b);
	  pending_exact = 0;
	  b += 3;
	  /* The alternative before this one has a jump after it
	     which gets executed if it gets matched.  Adjust that
	     jump so it will jump to this alternative's analogous
	     jump (put in below, which in turn will jump to the next
	     (if any) alternative's such jump, etc.).  The last such
	     jump jumps to the correct final destination.  A picture:
	     	_____ _____ 
	     	|   | |   |   
	     	|   v |   v 
	     	a | b   | c   

	     If we are at `b', then fixup_alt_jump right now points to a
	     three-byte space after `a'.  We'll put in the jump, set
	     fixup_alt_jump to right after `b', and leave behind three
	     bytes which we'll fill in when we get to after `c'.  */

	  if (fixup_alt_jump)
	    store_jump(fixup_alt_jump, jump_past_alt, b);

	  /* Mark and leave space for a jump after this alternative,
	     to be filled in later either by next alternative or
	     when know we're at the end of a series of alternatives.  */
	  fixup_alt_jump = b;
	  GET_BUFFER_SPACE(3);
	  b += 3;

	  laststart = 0;
	  begalt = b;
	  break;

	case '{':
	  /* If there is no previous pattern, this isn't an interval.  */
	  if (!laststart || p == pend)
	    {
		goto normal_backsl;
	    }

	  beg_interval = p - 1;

	  lower_bound = -1;			/* So can see if are set.  */
	  upper_bound = -1;
	  GET_UNSIGNED_NUMBER(lower_bound);
	  if (c == ',') {
	    GET_UNSIGNED_NUMBER(upper_bound);
	  }
	  else
	    /* Interval such as `{1}' => match exactly once. */
	    upper_bound = lower_bound;

	  if (lower_bound < 0 || c != '}')
	    goto unfetch_interval;

	  if (lower_bound >= RE_DUP_MAX || upper_bound >= RE_DUP_MAX)
	    FREE_AND_RETURN(stackb, "too big quantifier in {,}");
	  if (upper_bound < 0) upper_bound = RE_DUP_MAX;
	  if (lower_bound > upper_bound)
	    FREE_AND_RETURN(stackb, "can't do {n,m} with n > m");

	  beg_interval = 0;
	  pending_exact = 0;

	  greedy = 1;
	  if (p != pend) {
	    PATFETCH(c);
	    if (c == '?') greedy = 0;
	    else PATUNFETCH;
	  }

	  if (lower_bound == 0) {
	    zero_times_ok = 1;
	    if (upper_bound == RE_DUP_MAX) {
	      many_times_ok = 1;
	      goto repeat;
	    }
	    if (upper_bound == 1) {
	      many_times_ok = 0;
	      goto repeat;
	    }
	  }
	  if (lower_bound == 1) {
	    if (upper_bound == 1) {
	      /* No need to repeat */
	      break;
	    }
	    if (upper_bound == RE_DUP_MAX) {
	      many_times_ok = 1;
	      zero_times_ok = 0;
	      goto repeat;
	    }
	  }

	  /* If upper_bound is zero, don't want to succeed at all; 
	     jump from laststart to b + 3, which will be the end of
	     the buffer after this jump is inserted.  */

	  if (upper_bound == 0) {
	    GET_BUFFER_SPACE(3);
	    insert_jump(jump, laststart, b + 3, b);
	    b += 3;
	    break;
	  }

	  /* Otherwise, we have a nontrivial interval.  When
	     we're all done, the pattern will look like:
	     set_number_at <jump count> <upper bound>
	     set_number_at <succeed_n count> <lower bound>
	     succeed_n <after jump addr> <succed_n count>
	     <body of loop>
	     jump_n <succeed_n addr> <jump count>
	     (The upper bound and `jump_n' are omitted if
	     `upper_bound' is 1, though.)  */
	  { /* If the upper bound is > 1, we need to insert
	       more at the end of the loop.  */
	    unsigned nbytes = upper_bound == 1 ? 10 : 20;

	    GET_BUFFER_SPACE(nbytes);
	    /* Initialize lower bound of the `succeed_n', even
	       though it will be set during matching by its
	       attendant `set_number_at' (inserted next),
	       because `re_compile_fastmap' needs to know.
	       Jump to the `jump_n' we might insert below.  */
	    insert_jump_n(succeed_n, laststart, b + (nbytes/2), 
			  b, lower_bound);
	    b += 5; 	/* Just increment for the succeed_n here.  */

	    /* Code to initialize the lower bound.  Insert 
	       before the `succeed_n'.  The `5' is the last two
	       bytes of this `set_number_at', plus 3 bytes of
	       the following `succeed_n'.  */
	    insert_op_2(set_number_at, laststart, b, 5, lower_bound);
	    b += 5;

	    if (upper_bound > 1)
	      { /* More than one repetition is allowed, so
		   append a backward jump to the `succeed_n'
		   that starts this interval.

		   When we've reached this during matching,
		   we'll have matched the interval once, so
		   jump back only `upper_bound - 1' times.  */
		GET_BUFFER_SPACE(5);
		store_jump_n(b, greedy?jump_n:finalize_push_n, laststart + 5,
			     upper_bound - 1);
		b += 5;

		/* The location we want to set is the second
		   parameter of the `jump_n'; that is `b-2' as
		   an absolute address.  `laststart' will be
		   the `set_number_at' we're about to insert;
		   `laststart+3' the number to set, the source
		   for the relative address.  But we are
		   inserting into the middle of the pattern --
		   so everything is getting moved up by 5.
		   Conclusion: (b - 2) - (laststart + 3) + 5,
		   i.e., b - laststart.

		   We insert this at the beginning of the loop
		   so that if we fail during matching, we'll
		   reinitialize the bounds.  */
		insert_op_2(set_number_at, laststart, b, b - laststart,
			    upper_bound - 1);
		b += 5;
	      }
	  }
	  break;

	unfetch_interval:
	  /* If an invalid interval, match the characters as literals.  */
	  p = beg_interval;
	  beg_interval = 0;

	  /* normal_char and normal_backslash need `c'.  */
	  PATFETCH (c);	
	  goto normal_char;

        case '\\':
	  if (p == pend) goto invalid_pattern;
          /* Do not translate the character after the \, so that we can
             distinguish, e.g., \B from \b, even if we normally would
             translate, e.g., B to b.  */
	  PATFETCH_RAW(c);
	  switch (c)
	    {
	    case 's':
	    case 'S':
	    case 'd':
	    case 'D':
	      while (b - bufp->buffer
		     > bufp->allocated - 9 - (1 << BYTEWIDTH) / BYTEWIDTH)
		EXTEND_BUFFER;

	      laststart = b;
	      if (c == 's' || c == 'd') {
		BUFPUSH(charset);
	      }
	      else {
		BUFPUSH(charset_not);
	      }

	      BUFPUSH((1 << BYTEWIDTH) / BYTEWIDTH);
	      memset(b, 0, (1 << BYTEWIDTH) / BYTEWIDTH + 2);
	      if (c == 's' || c == 'S') {
		SET_LIST_BIT(' ');
		SET_LIST_BIT('\t');
		SET_LIST_BIT('\n');
		SET_LIST_BIT('\r');
		SET_LIST_BIT('\f');
	      }
	      else {
		char cc;

		for (cc = '0'; cc <= '9'; cc++) {
		  SET_LIST_BIT(cc);
		}
	      }

	      while ((int)b[-1] > 0 && b[b[-1] - 1] == 0) 
		b[-1]--; 
	      if (b[-1] != (1 << BYTEWIDTH) / BYTEWIDTH)
		memmove(&b[b[-1]], &b[(1 << BYTEWIDTH) / BYTEWIDTH],
		  2 + EXTRACT_UNSIGNED(&b[(1 << BYTEWIDTH) / BYTEWIDTH])*4);
	      b += b[-1] + 2 + EXTRACT_UNSIGNED(&b[b[-1]])*4;
	      break;

	    case 'w':
	      laststart = b;
	      BUFPUSH(wordchar);
	      break;

	    case 'W':
	      laststart = b;
	      BUFPUSH(notwordchar);
	      break;

	    case '<':
	      BUFPUSH(wordbeg);
	      break;

	    case '>':
	      BUFPUSH(wordend);
	      break;

	    case 'b':
	      BUFPUSH(wordbound);
	      break;

	    case 'B':
	      BUFPUSH(notwordbound);
	      break;

	    case 'A':
	      BUFPUSH(begbuf);
	      break;

	    case 'Z':
	      BUFPUSH(endbuf2);
	      break;

	    case 'z':
	      BUFPUSH(endbuf);
	      break;

	      /* hex */
	    case 'x':
	      c1 = 0;
	      c = scan_hex(p, 2, &numlen);
	      p += numlen;
	      if (c > 0x7f)
		  c1 = 0xff;
	      goto numeric_char;

	      /* octal */
	    case '0':
	      c1 = 0;
	      c = scan_oct(p, 3, &numlen);
	      p += numlen;
	      if (c > 0x7f)
		  c1 = 0xff;
	      goto numeric_char;

	      /* back-ref or octal */
	    case '1': case '2': case '3':
	    case '4': case '5': case '6':
	    case '7': case '8': case '9':
	      {
		  char *p_save;

		  PATUNFETCH;
		  p_save = p;

		  c1 = 0;
		  GET_UNSIGNED_NUMBER(c1);
		  if (!ISDIGIT(c)) PATUNFETCH;

		  if (c1 >= regnum) {
		      /* need to get octal */
		      p = p_save;
		      c = scan_oct(p_save, 3, &numlen) & 0xff;
		      p = p_save + numlen;
		      c1 = 0;
		      if (c > 0x7f)
			  c1 = 0xff;
		      goto numeric_char;
		  }
	      }

              /* Can't back reference to a subexpression if inside of it.  */
              for (stackt = stackp - 2;  stackt > stackb;  stackt -= 5)
 		if (*stackt == c1)
		  goto normal_char;
	      laststart = b;
	      BUFPUSH(duplicate);
	      BUFPUSH(c1);
	      break;

            default:
	    normal_backsl:
	      goto normal_char;
	    }
	  break;

	case '#':
	  if (options & RE_OPTION_EXTENDED)
	    {
	      while (p != pend) {
		PATFETCH(c);
		if (c == '\n') break;
	      }
	      break;
	    }
	  goto normal_char;

	case ' ':
	case '\t':
	case '\f':
	case '\r':
	case '\n':
	  if (options & RE_OPTION_EXTENDED)
	    break;

	default:
	normal_char:		/* Expects the character in `c'.  */
	  c1 = 0;
	  if (ismbchar(c)) {
	    c1 = c;
	    PATFETCH_RAW(c);
	  }
	  else if (c > 0x7f) {
	    c1 = 0xff;
	  }
	numeric_char:
	  if (!pending_exact || pending_exact + *pending_exact + 1 != b
	      || *pending_exact >= (c1 ? 0176 : 0177)
	      || *p == '+' || *p == '?'
	      || *p == '*' || *p == '^'
	      || *p == '{')
	    {
	      laststart = b;
	      BUFPUSH(exactn);
	      pending_exact = b;
	      BUFPUSH(0);
	    }
	  if (c1) {
	    BUFPUSH(c1);
	    (*pending_exact)++;
	  }
	  BUFPUSH(c);
	  (*pending_exact)++;
	}
    }

  if (fixup_alt_jump)
    store_jump(fixup_alt_jump, jump, b);

  if (stackp != stackb)
    FREE_AND_RETURN(stackb, "unmatched (");

  /* set optimize flags */
  laststart = bufp->buffer;
  if (laststart != b) {
    if (*laststart == start_memory) laststart += 3;
    if (*laststart == dummy_failure_jump) laststart += 3;
    else if (*laststart == try_next) laststart += 3;
    if (*laststart == on_failure_jump) {
      int mcnt;

      laststart++;
      EXTRACT_NUMBER_AND_INCR(mcnt, laststart);
      if (mcnt == 4 && *laststart == anychar) {
	bufp->options |= RE_OPTIMIZE_ANCHOR;
      }
      else if (*laststart == charset || *laststart == charset_not) {
	p0 = laststart;
	mcnt = *++p0 ;
	p0 += mcnt+1;
	mcnt = EXTRACT_UNSIGNED_AND_INCR(p0);
	p0 += 4*mcnt;
	if (*p0 == maybe_finalize_jump) {
	  bufp->stclass = laststart;
	}
      }
    }
  }

  bufp->used = b - bufp->buffer;
  bufp->re_nsub = regnum;
  laststart = bufp->buffer;
  if (laststart != b) {
    if (*laststart == start_memory) laststart += 3;
    if (*laststart == exactn) {
      bufp->options |= RE_OPTIMIZE_EXACTN;
      bufp->must = laststart+1;
    }
  }
  else {
    bufp->must = calculate_must_string(bufp->buffer, b);
  }
  if (current_mbctype == MBCTYPE_SJIS) bufp->options |= RE_OPTIMIZE_NO_BM;
  else if (bufp->must) {
    int i;
    int len = (unsigned char)bufp->must[0];

    for (i=1; i<len; i++) {
      if ((unsigned char)bufp->must[i] == 0xff ||
	  (current_mbctype == MBCTYPE_EUC && ismbchar(bufp->must[i]))) {
	bufp->options |= RE_OPTIMIZE_NO_BM;
	break;
      }
    }
    if (!(bufp->options & RE_OPTIMIZE_NO_BM)) {
      bufp->must_skip = (int *) xmalloc((1 << BYTEWIDTH)*sizeof(int));
      bm_init_skip(bufp->must_skip, bufp->must+1,
		   (unsigned char)bufp->must[0],
		   MAY_TRANSLATE()?translate:0);
    }
  }

  FREE_AND_RETURN(stackb, 0);

 invalid_pattern:
  FREE_AND_RETURN(stackb, "invalid regular expression");

 end_of_pattern:
  FREE_AND_RETURN(stackb, "premature end of regular expression");

 too_big:
  FREE_AND_RETURN(stackb, "regular expression too big");

 memory_exhausted:
  FREE_AND_RETURN(stackb, "memory exhausted");

 nested_meta:
  FREE_AND_RETURN(stackb, "nested *?+ in regexp");
}

void
re_free_pattern(bufp)
    struct re_pattern_buffer *bufp;
{
    free(bufp->buffer);
    free(bufp->fastmap);
    if (bufp->must_skip) free(bufp->must_skip);
    free(bufp);
}

/* Store a jump of the form <OPCODE> <relative address>.
   Store in the location FROM a jump operation to jump to relative
   address FROM - TO.  OPCODE is the opcode to store.  */

static void
store_jump(from, opcode, to)
     char *from, *to;
     int opcode;
{
  from[0] = (char)opcode;
  STORE_NUMBER(from + 1, to - (from + 3));
}


/* Open up space before char FROM, and insert there a jump to TO.
   CURRENT_END gives the end of the storage not in use, so we know 
   how much data to copy up. OP is the opcode of the jump to insert.

   If you call this function, you must zero out pending_exact.  */

static void
insert_jump(op, from, to, current_end)
     int op;
     char *from, *to, *current_end;
{
  register char *pfrom = current_end;		/* Copy from here...  */
  register char *pto = current_end + 3;		/* ...to here.  */

  while (pfrom != from)			       
    *--pto = *--pfrom;
  store_jump(from, op, to);
}


/* Store a jump of the form <opcode> <relative address> <n> .

   Store in the location FROM a jump operation to jump to relative
   address FROM - TO.  OPCODE is the opcode to store, N is a number the
   jump uses, say, to decide how many times to jump.

   If you call this function, you must zero out pending_exact.  */

static void
store_jump_n(from, opcode, to, n)
     char *from, *to;
     int opcode;
     unsigned n;
{
  from[0] = (char)opcode;
  STORE_NUMBER(from + 1, to - (from + 3));
  STORE_NUMBER(from + 3, n);
}


/* Similar to insert_jump, but handles a jump which needs an extra
   number to handle minimum and maximum cases.  Open up space at
   location FROM, and insert there a jump to TO.  CURRENT_END gives the
   end of the storage in use, so we know how much data to copy up. OP is
   the opcode of the jump to insert.

   If you call this function, you must zero out pending_exact.  */

static void
insert_jump_n(op, from, to, current_end, n)
     int op;
     char *from, *to, *current_end;
     unsigned n;
{
  register char *pfrom = current_end;		/* Copy from here...  */
  register char *pto = current_end + 5;		/* ...to here.  */

  while (pfrom != from)			       
    *--pto = *--pfrom;
  store_jump_n(from, op, to, n);
}


/* Open up space at location THERE, and insert operation OP.
   CURRENT_END gives the end of the storage in use, so
   we know how much data to copy up.

   If you call this function, you must zero out pending_exact.  */

static void
insert_op(op, there, current_end)
     int op;
     char *there, *current_end;
{
  register char *pfrom = current_end;		/* Copy from here...  */
  register char *pto = current_end + 1;		/* ...to here.  */

  while (pfrom != there)			       
    *--pto = *--pfrom;

  there[0] = (char)op;
}


/* Open up space at location THERE, and insert operation OP followed by
   NUM_1 and NUM_2.  CURRENT_END gives the end of the storage in use, so
   we know how much data to copy up.

   If you call this function, you must zero out pending_exact.  */

static void
insert_op_2(op, there, current_end, num_1, num_2)
     int op;
     char *there, *current_end;
     int num_1, num_2;
{
  register char *pfrom = current_end;		/* Copy from here...  */
  register char *pto = current_end + 5;		/* ...to here.  */

  while (pfrom != there)			       
    *--pto = *--pfrom;

  there[0] = (char)op;
  STORE_NUMBER(there + 1, num_1);
  STORE_NUMBER(there + 3, num_2);
}


#define trans_eq(c1, c2, translate) (translate?(translate[c1]==translate[c2]):((c1)==(c2)))
static int
slow_match(little, lend, big, bend, translate)
     unsigned char *little, *lend;
     unsigned char *big, *bend;
     unsigned char *translate;
{
  int c;

  while (little < lend && big < bend) {
    c = *little++;
    if (c == 0xff)
      c = *little++;
    if (!trans_eq(*big++, c, translate)) break;
  }
  if (little == lend) return 1;
  return 0;
}

static int
slow_search(little, llen, big, blen, translate)
     unsigned char *little;
     int llen;
     unsigned char *big;
     int blen;
     char *translate;
{
  unsigned char *bsave = big;
  unsigned char *bend = big + blen;
  register int c;
  int fescape = 0;

  c = *little;
  if (c == 0xff) {
    c = little[1];
    fescape = 1;
  }
  else if (translate && !ismbchar(c)) {
    c = translate[c];
  }

  while (big < bend) {
    /* look for first character */
    if (fescape) {
      while (big < bend) {
	if (*big == c) break;
	big++;
      }
    }
    else if (translate && !ismbchar(c)) {
      while (big < bend) {
	if (ismbchar(*big)) big++;
	else if (translate[*big] == c) break;
	big++;
      }
    }
    else {
      while (big < bend) {
	if (*big == c) break;
	if (ismbchar(*big)) big++;
	big++;
      }
    }

    if (slow_match(little, little+llen, big, bend, translate))
      return big - bsave;

    if (ismbchar(*big)) big++;
    big++;
  }
  return -1;
}

static void
bm_init_skip(skip, pat, m, translate)
    int *skip;
    unsigned char *pat;
    int m;
    char *translate;
{
    int j, c;

    for (c=0; c<256; c++) {
	skip[c] = m;
    }
    if (translate) {
      for (j=0; j<m-1; j++) {
	skip[translate[pat[j]]] = m-1-j;
      }
    }
    else {
      for (j=0; j<m-1; j++) {
	skip[pat[j]] = m-1-j;
      }
    }
}

static int
bm_search(little, llen, big, blen, skip, translate)
     unsigned char *little;
     int llen;
     unsigned char *big;
     int blen;
     int *skip;
     unsigned char *translate;
{
  int i, j, k;

  i = llen-1;
  if (translate) {
    while (i < blen) {
      k = i;
      j = llen-1;
      while (j >= 0 && translate[big[k]] == translate[little[j]]) {
	k--;
	j--;
      }
      if (j < 0) return k+1;

      i += skip[translate[big[i]]];
    }
    return -1;
  }
  while (i < blen) {
    k = i;
    j = llen-1;
    while (j >= 0 && big[k] == little[j]) {
      k--;
      j--;
    }
    if (j < 0) return k+1;

    i += skip[big[i]];
  }
  return -1;
}

/* Given a pattern, compute a fastmap from it.  The fastmap records
   which of the (1 << BYTEWIDTH) possible characters can start a string
   that matches the pattern.  This fastmap is used by re_search to skip
   quickly over totally implausible text.

   The caller must supply the address of a (1 << BYTEWIDTH)-byte data 
   area as bufp->fastmap.
   The other components of bufp describe the pattern to be used.  */
void
re_compile_fastmap(bufp)
     struct re_pattern_buffer *bufp;
{
  unsigned char *pattern = (unsigned char*)bufp->buffer;
  int size = bufp->used;
  register char *fastmap = bufp->fastmap;
  register unsigned char *p = pattern;
  register unsigned char *pend = pattern + size;
  register int j, k;
  unsigned is_a_succeed_n;

  unsigned char **stackb = RE_TALLOC(NFAILURES, unsigned char*);
  unsigned char **stackp = stackb;
  unsigned char **stacke = stackb + NFAILURES;
  int options = bufp->options;

  memset(fastmap, 0, (1 << BYTEWIDTH));
  bufp->fastmap_accurate = 1;
  bufp->can_be_null = 0;

  while (p)
    {
      is_a_succeed_n = 0;
      if (p == pend)
	{
	  bufp->can_be_null = 1;
	  break;
	}
#ifdef SWITCH_ENUM_BUG
      switch ((int)((enum regexpcode)*p++))
#else
      switch ((enum regexpcode)*p++)
#endif
	{
	case exactn:
	  if (p[1] == 0xff) {
	    if (TRANSLATE_P())
	      fastmap[translate[p[2]]] = 2;
	    else
	      fastmap[p[2]] = 2;
	  }
	  else if (TRANSLATE_P())
	    fastmap[translate[p[1]]] = 1;
	  else
	    fastmap[p[1]] = 1;
	  break;

        case begline:
	case begbuf:
	case endbuf:
	case endbuf2:
	case wordbound:
	case notwordbound:
	case wordbeg:
	case wordend:
	case pop_and_fail:
        case push_dummy_failure:
	  continue;

	case casefold_on:
	  bufp->options |= RE_MAY_IGNORECASE;
	case casefold_off:
	  options ^= RE_OPTION_IGNORECASE;
	  continue;

	case endline:
	  if (TRANSLATE_P())
	    fastmap[translate['\n']] = 1;
	  else
	    fastmap['\n'] = 1;

	  if (bufp->can_be_null == 0)
	    bufp->can_be_null = 2;
	  break;

	case jump_n:
        case finalize_jump:
	case maybe_finalize_jump:
	case jump:
        case jump_past_alt:
	case dummy_failure_jump:
          EXTRACT_NUMBER_AND_INCR(j, p);
	  p += j;	
	  if (j > 0)
	    continue;
          /* Jump backward reached implies we just went through
	     the body of a loop and matched nothing.
	     Opcode jumped to should be an on_failure_jump.
	     Just treat it like an ordinary jump.
	     For a * loop, it has pushed its failure point already;
	     If so, discard that as redundant.  */

          if ((enum regexpcode)*p != on_failure_jump
	      && (enum regexpcode)*p != try_next
	      && (enum regexpcode)*p != succeed_n
	      && (enum regexpcode)*p != finalize_push
	      && (enum regexpcode)*p != finalize_push_n)
	    continue;
          p++;
          EXTRACT_NUMBER_AND_INCR(j, p);
          p += j;	
          if (stackp != stackb && *stackp == p)
            stackp--;		/* pop */
          continue;

        case start_nowidth:
	case stop_nowidth:
        case finalize_push:
	  p += 2;
	  continue;

        case finalize_push_n:
	  p += 4;
	  continue;

	case try_next:
        case on_failure_jump:
	handle_on_failure_jump:
          EXTRACT_NUMBER_AND_INCR(j, p);
          if (p + j < pend) {
	    if (stackp == stacke) {
	      unsigned char **stackx;
	      unsigned int len = stacke - stackb;

	      EXPAND_FAIL_STACK(stackx, stackb, len);
	    }
	    *++stackp = p + j;	/* push */
	  }
	  else {
            bufp->can_be_null = 1;
	  }
	  if (is_a_succeed_n)
            EXTRACT_NUMBER_AND_INCR(k, p);	/* Skip the n.  */
	  continue;

	case succeed_n:
	  is_a_succeed_n = 1;
          /* Get to the number of times to succeed.  */
          EXTRACT_NUMBER(k, p + 2);
	  /* Increment p past the n for when k != 0.  */
          if (k == 0) {
	    p += 4;
	  }
	  else {
	    goto handle_on_failure_jump;
	  }
          continue;

	case set_number_at:
          p += 4;
          continue;

        case start_memory:
	case stop_memory:
	  p += 2;
	  continue;

	case duplicate:
	  bufp->can_be_null = 1;
	  fastmap['\n'] = 1;
	case anychar:
	  for (j = 0; j < (1 << BYTEWIDTH); j++)
	    if (j != '\n')
	      fastmap[j] = 1;
	  if (bufp->can_be_null)
	    {
	      FREE_AND_RETURN_VOID(stackb);
	    }
	  /* Don't return; check the alternative paths
	     so we can set can_be_null if appropriate.  */
	  break;

	case wordchar:
	  for (j = 0; j < (1 << BYTEWIDTH); j++)
	    if (SYNTAX(j) == Sword)
	      fastmap[j] = 1;
	  break;

	case notwordchar:
	  for (j = 0; j < 0x80; j++)
	    if (SYNTAX(j) != Sword)
	      fastmap[j] = 1;
	  for (j = 0x80; j < (1 << BYTEWIDTH); j++)
	      fastmap[j] = 1;
	  break;

	case charset:
	  /* NOTE: Charset for single-byte chars never contain
		   multi-byte char.  See set_list_bits().  */
	  for (j = *p++ * BYTEWIDTH - 1; j >= 0; j--)
	    if (p[j / BYTEWIDTH] & (1 << (j % BYTEWIDTH)))
	      {
		if (TRANSLATE_P())
		  fastmap[translate[j]] = 1;
		else
		  fastmap[j] = 1;
	      }
	  {
	    unsigned short size;
	    unsigned c, end;

	    p += p[-1] + 2;
	    size = EXTRACT_UNSIGNED(&p[-2]);
	    for (j = 0; j < (int)size; j++) {
	      if ((unsigned char)p[j*4] == 0xff) {
		for (c = (unsigned char)p[j*4+1],
		    end = (unsigned char)p[j*4+3];
		     c <= end; c++) {
		  fastmap[c] = 2;
		}
	      }
	      else {
		/* set bits for 1st bytes of multi-byte chars.  */
		for (c = (unsigned char)p[j*4],
		     end = (unsigned char)p[j*4 + 2];
		     c <= end; c++) {
		  /* NOTE: Charset for multi-byte chars might contain
		     single-byte chars.  We must reject them. */
		  if (ismbchar(c))
		    fastmap[c] = 1;
		}
	      }
	    }
	  }
	  break;

	case charset_not:
	  /* S: set of all single-byte chars.
	     M: set of all first bytes that can start multi-byte chars.
	     s: any set of single-byte chars.
	     m: any set of first bytes that can start multi-byte chars.

	     We assume S+M = U.
	       ___      _   _
	       s+m = (S*s+M*m).  */
	  /* Chars beyond end of map must be allowed */
	  /* NOTE: Charset_not for single-byte chars might contain
		   multi-byte chars.  See set_list_bits(). */
	  for (j = *p * BYTEWIDTH; j < (1 << BYTEWIDTH); j++)
	    if (!ismbchar(j))
	      fastmap[j] = 1;

	  for (j = *p++ * BYTEWIDTH - 1; j >= 0; j--)
	    if (!(p[j / BYTEWIDTH] & (1 << (j % BYTEWIDTH))))
	      {
		if (!ismbchar(j))
		  fastmap[j] = 1;
	      }
	  {
	    unsigned short size;
	    unsigned char c, beg;
	    int byte_match = 0;

	    p += p[-1] + 2;
	    size = EXTRACT_UNSIGNED(&p[-2]);
	    if (size == 0) {
	      for (j = 0x80; j < (1 << BYTEWIDTH); j++)
		if (ismbchar(j))
		  fastmap[j] = 1;
	    }
	    for (j = 0,c = 0x80;j < (int)size; j++) {
	      if ((unsigned char)p[j*4] == 0xff) {
		byte_match = 1;
	        for (beg = (unsigned char)p[j*4+1]; c < beg; c++)
		  fastmap[c] = 1;
	        c = (unsigned char)p[j*4+3] + 1;
	      }
	      else {
	        for (beg = (unsigned char)p[j*4 + 0]; c < beg; c++)
		  if (ismbchar(c))
		    fastmap[c] = 1;
	        c = (unsigned char)p[j*4 + 2] + 1;
	      }
	    }
	    if (byte_match) {
	      for (j = c; j < (1 << BYTEWIDTH); j++)
		fastmap[j] = 1;
	      for (j = 0; j < (1 << BYTEWIDTH); j++)
		if (fastmap[j])
		  fastmap[j] = 2;
	    }
	    else {
	      for (j = c; j < (1 << BYTEWIDTH); j++)
		if (ismbchar(j))
		  fastmap[j] = 1;
	    }
	  }
	  break;

	case unused:	/* pacify gcc -Wall */
	  break;
	}

      /* Get here means we have successfully found the possible starting
         characters of one path of the pattern.  We need not follow this
         path any farther.  Instead, look at the next alternative
         remembered in the stack.  */
      if (stackp != stackb)
	p = *stackp--;		/* pop */
      else
	break;
    }
   FREE_AND_RETURN_VOID(stackb);
}


/* Using the compiled pattern in BUFP->buffer, first tries to match
   STRING, starting first at index STARTPOS, then at STARTPOS + 1, and
   so on.  RANGE is the number of places to try before giving up.  If
   RANGE is negative, it searches backwards, i.e., the starting
   positions tried are STARTPOS, STARTPOS - 1, etc.  STRING is of SIZE.
   In REGS, return the indices of STRING that matched the entire
   BUFP->buffer and its contained subexpressions.

   The value returned is the position in the strings at which the match
   was found, or -1 if no match was found, or -2 if error (such as
   failure stack overflow).  */

int
re_search(bufp, string, size, startpos, range, regs)
     struct re_pattern_buffer *bufp;
     char *string;
     int size, startpos, range;
     struct re_registers *regs;
{
  register char *fastmap = bufp->fastmap;
  int val, anchor = 0;

  /* Check for out-of-range starting position.  */
  if (startpos < 0  ||  startpos > size)
    return -1;

  /* Update the fastmap now if not correct already.  */
  if (fastmap && !bufp->fastmap_accurate) {
      re_compile_fastmap(bufp);
  }

  /* If the search isn't to be a backwards one, don't waste time in a
     search for a pattern that must be anchored.  */
  if (bufp->used>0) {
    switch ((enum regexpcode)bufp->buffer[0]) {
    case begbuf:
      if (range > 0) {
	if (startpos > 0)
	  return -1;
	else if (re_match(bufp, string, size, 0, regs) >= 0)
	  return 0;
	return -1;
      }
      break;

    case begline:
      anchor = 1;
      break;

    default:
      break;
    }
  }
  if (bufp->options & RE_OPTIMIZE_ANCHOR) {
    anchor = 1;
  }

  if (bufp->must) {
    int len = ((unsigned char*)bufp->must)[0];
    int pos, pbeg, pend;

    pbeg = startpos;
    pend = startpos + range;
    if (pbeg > pend) {		/* swap pbeg,pend */
      pos = pend; pend = pbeg; pbeg = pos;
    }
    if (pend > size) pend = size;
    if (bufp->options & RE_OPTIMIZE_NO_BM) {
      pos = slow_search(bufp->must+1, len,
			string+pbeg, pend-pbeg,
			MAY_TRANSLATE()?translate:0);
    }
    else {
      pos = bm_search(bufp->must+1, len,
		      string+pbeg, pend-pbeg,
		      bufp->must_skip,
		      MAY_TRANSLATE()?translate:0);
    }
    if (pos == -1) return -1;
    if (range > 0 && (bufp->options & RE_OPTIMIZE_EXACTN)) {
      startpos += pos;
      range -= pos;
    }
  }

  for (;;)
    {
      /* If a fastmap is supplied, skip quickly over characters that
         cannot possibly be the start of a match.  Note, however, that
         if the pattern can possibly match the null string, we must
         test it at each starting point so that we take the first null
         string we get.  */

      if (fastmap && startpos < size
	  && bufp->can_be_null != 1 && !(anchor && startpos == 0))
	{
	  if (range > 0)	/* Searching forwards.  */
	    {
	      register unsigned char *p, c;
	      int irange = range;

	      p = (unsigned char*)string+startpos;

	      while (range > 0) {
		c = *p++;
		if (ismbchar(c)) {
		  if (fastmap[c])
		    break;
		  c = *p++;
		  range--;
		  if (fastmap[c] == 2)
		    break;
		}
		else 
		  if (fastmap[MAY_TRANSLATE() ? translate[c] : c])
		    break;
		range--;
	      }
	      startpos += irange - range;
	    }
	  else			/* Searching backwards.  */
	    {
	      register unsigned char c;

	      c = string[startpos];
              c &= 0xff;
	      if (MAY_TRANSLATE() ? !fastmap[translate[c]] : !fastmap[c])
		goto advance;
	    }
	}

      if (startpos > size) return -1;
      if (anchor && size > 0 && startpos == size) return -1;
      if (fastmap && startpos == size && range >= 0
	  && (bufp->can_be_null == 0 ||
	      (bufp->can_be_null && size > 0
	       && string[startpos-1] == '\n')))
	return -1;

      val = re_match(bufp, string, size, startpos, regs);
      if (val >= 0)
	return startpos;
      if (val == -2)
	return -2;

#ifndef NO_ALLOCA
#ifdef C_ALLOCA
      alloca(0);
#endif /* C_ALLOCA */
#endif /* NO_ALLOCA */

      if (range > 0) {
	if (anchor && startpos < size && startpos > 0 && string[startpos-1] != '\n') {
	  while (range > 0 && string[startpos] != '\n') {
	    range--;
	    startpos++;
	  }
	}
	else if (fastmap && (bufp->stclass)) {
	  register unsigned char *p;
	  register unsigned short c;
	  int irange = range;

	  p = (unsigned char*)string+startpos;
	  while (range > 0) {
	    c = *p++;
	    if (ismbchar(c) && fastmap[c] != 2) {
	      c = c << 8 | *p++;
	    }
	    else if (MAY_TRANSLATE())
	      c = translate[c];
	    if (*bufp->stclass == charset) {
	      if (!is_in_list(c, bufp->stclass+1)) break;
	    }
	    else {
	      if (is_in_list(c, bufp->stclass+1)) break;
	    }
	    range--;
	    if (c > 256) range--;
	  }
	  startpos += irange - range;
	}
      }

    advance:
      if (!range) 
        break;
      else if (range > 0) {
	const char *d = string + startpos;

	if (ismbchar(*d)) {
	  range--, startpos++;
	  if (!range)
	    break;
	}
	range--, startpos++;
      }
      else {
	range++, startpos--;
	{
	  const char *s, *d, *p;

	  s = string; d = string + startpos;
	  for (p = d; p-- > s && ismbchar(*p); )
	    /* --p >= s would not work on 80[12]?86. 
	      (when the offset of s equals 0 other than huge model.)  */
	    ;
	  if (!((d - p) & 1)) {
	    if (!range)
	      break;
	    range++, startpos--;
	  }
	}
      }
    }
  return -1;
}




/* The following are used for re_match, defined below:  */

/* Roughly the maximum number of failure points on the stack.  Would be
   exactly that if always pushed MAX_NUM_FAILURE_ITEMS each time we failed.  */

int re_max_failures = 2000;

/* Routine used by re_match.  */
/* static int memcmp_translate(); *//* already declared */


/* Structure and accessing macros used in re_match:  */

typedef union
{
  unsigned char *word;
  struct
  {
      /* This field is one if this group can match the empty string,
         zero if not.  If not yet determined,  `MATCH_NULL_UNSET_VALUE'.  */
#define MATCH_NULL_UNSET_VALUE 3
    unsigned match_null_string_p : 2;
    unsigned is_active : 1;
    unsigned matched_something : 1;
    unsigned ever_matched_something : 1;
  } bits;
} register_info_type;

#define REG_MATCH_NULL_STRING_P(R)  ((R).bits.match_null_string_p)
#define IS_ACTIVE(R)  ((R).bits.is_active)
#define MATCHED_SOMETHING(R)  ((R).bits.matched_something)
#define EVER_MATCHED_SOMETHING(R)  ((R).bits.ever_matched_something)


/* Macros used by re_match:  */

/* I.e., regstart, regend, and reg_info.  */
#define NUM_REG_ITEMS  3

/* Individual items aside from the registers.  */
#define NUM_NONREG_ITEMS 3

/* We push at most this many things on the stack whenever we
   fail.  The `+ 2' refers to PATTERN_PLACE and STRING_PLACE, which are
   arguments to the PUSH_FAILURE_POINT macro.  */
#define MAX_NUM_FAILURE_ITEMS   (num_regs * NUM_REG_ITEMS + NUM_NONREG_ITEMS)

/* We push this many things on the stack whenever we fail.  */
#define NUM_FAILURE_ITEMS  (last_used_reg * NUM_REG_ITEMS + NUM_REG_ITEMS)


/* This pushes most of the information about the current state we will want
   if we ever fail back to it.  */

#define PUSH_FAILURE_POINT(pattern_place, string_place)			\
  do {									\
    long last_used_reg, this_reg;					\
									\
    /* Find out how many registers are active or have been matched.	\
       (Aside from register zero, which is only set at the end.) */	\
    for (last_used_reg = num_regs - 1; last_used_reg > 0; last_used_reg--)\
      if (!REG_UNSET(regstart[last_used_reg]))				\
        break;								\
									\
    if (stacke - stackp <= NUM_FAILURE_ITEMS)				\
      {									\
	unsigned char **stackx;						\
	unsigned int len = stacke - stackb;				\
	/* if (len > re_max_failures * MAX_NUM_FAILURE_ITEMS)		\
	  {								\
	    FREE_VARIABLES();						\
	    FREE_AND_RETURN(stackb,(-2));				\
	  }*/								\
									\
        /* Roughly double the size of the stack.  */			\
        EXPAND_FAIL_STACK(stackx, stackb, len);				\
      }									\
									\
    /* Now push the info for each of those registers.  */		\
    for (this_reg = 1; this_reg <= last_used_reg; this_reg++)		\
      {									\
        *stackp++ = regstart[this_reg];					\
        *stackp++ = regend[this_reg];					\
        *stackp++ = reg_info[this_reg].word;				\
      }									\
									\
    /* Push how many registers we saved.  */				\
    *stackp++ = (unsigned char*)last_used_reg;				\
									\
    *stackp++ = pattern_place;                                          \
    *stackp++ = string_place;                                           \
    *stackp++ = (unsigned char*)0; /* non-greedy flag */		\
  } while(0)


/* This pops what PUSH_FAILURE_POINT pushes.  */

#define POP_FAILURE_POINT()						\
  do {									\
    int temp;								\
    stackp -= NUM_NONREG_ITEMS;	/* Remove failure points (and flag). */	\
    temp = (int)*--stackp;	/* How many regs pushed.  */	        \
    temp *= NUM_REG_ITEMS;	/* How much to take off the stack.  */	\
    stackp -= temp; 		/* Remove the register info.  */	\
  } while(0)

/* Registers are set to a sentinel when they haven't yet matched.  */
#define REG_UNSET_VALUE ((unsigned char*)-1)
#define REG_UNSET(e) ((e) == REG_UNSET_VALUE)

#define PREFETCH if (d == dend) goto fail

/* Call this when have matched something; it sets `matched' flags for the
   registers corresponding to the subexpressions of which we currently
   are inside.  */
#define SET_REGS_MATCHED 						\
  do { unsigned this_reg;						\
    for (this_reg = 0; this_reg < num_regs; this_reg++) 		\
      { 								\
        if (IS_ACTIVE(reg_info[this_reg]))				\
          MATCHED_SOMETHING(reg_info[this_reg])				\
            = EVER_MATCHED_SOMETHING (reg_info[this_reg])		\
            = 1;							\
        else								\
          MATCHED_SOMETHING(reg_info[this_reg]) = 0;			\
      } 								\
  } while(0)

#define AT_STRINGS_BEG(d)  (d == string)
#define AT_STRINGS_END(d)  (d == dend)

/* We have two special cases to check for: 
     1) if we're past the end of string1, we have to look at the first
        character in string2;
     2) if we're before the beginning of string2, we have to look at the
        last character in string1; we assume there is a string1, so use
        this in conjunction with AT_STRINGS_BEG.  */
#define IS_A_LETTER(d) (SYNTAX(*(d)) == Sword)

static void
init_regs(regs, num_regs)
    struct re_registers *regs;
    unsigned int num_regs;
{
    int i;

    regs->num_regs = num_regs;
    if (num_regs < RE_NREGS)
	num_regs = RE_NREGS;

    if (regs->allocated == 0) {
	regs->beg = TMALLOC(num_regs, int);
	regs->end = TMALLOC(num_regs, int);
	regs->allocated = num_regs;
    }
    else if (regs->allocated < num_regs) {
	TREALLOC(regs->beg, num_regs, int);
	TREALLOC(regs->end, num_regs, int);
    }
    for (i=0; i<num_regs; i++) {
	regs->beg[i] = regs->end[i] = -1;
    }
}

/* Match the pattern described by BUFP against STRING, which is of
   SIZE.  Start the match at index POS in STRING.  In REGS, return the
   indices of STRING that matched the entire BUFP->buffer and its
   contained subexpressions.

   If bufp->fastmap is nonzero, then it had better be up to date.

   The reason that the data to match are specified as two components
   which are to be regarded as concatenated is so this function can be
   used directly on the contents of an Emacs buffer.

   -1 is returned if there is no match.  -2 is returned if there is an
   error (such as match stack overflow).  Otherwise the value is the
   length of the substring which was matched.  */

int
re_match(bufp, string_arg, size, pos, regs)
     struct re_pattern_buffer *bufp;
     char *string_arg;
     int size, pos;
     struct re_registers *regs;
{
  register unsigned char *p = (unsigned char*)bufp->buffer;
  unsigned char *p1;

  /* Pointer to beyond end of buffer.  */
  register unsigned char *pend = p + bufp->used;

  unsigned num_regs = bufp->re_nsub;

  unsigned char *string = (unsigned char*)string_arg;

  register unsigned char *d, *dend;
  register int mcnt;			/* Multipurpose.  */
  int options = bufp->options;

 /* Failure point stack.  Each place that can handle a failure further
    down the line pushes a failure point on this stack.  It consists of
    restart, regend, and reg_info for all registers corresponding to the
    subexpressions we're currently inside, plus the number of such
    registers, and, finally, two char *'s.  The first char * is where to
    resume scanning the pattern; the second one is where to resume
    scanning the strings.  If the latter is zero, the failure point is a
    ``dummy''; if a failure happens and the failure point is a dummy, it
    gets discarded and the next next one is tried.  */

  unsigned char **stackb;
  unsigned char **stackp;
  unsigned char **stacke;


  /* Information on the contents of registers. These are pointers into
     the input strings; they record just what was matched (on this
     attempt) by a subexpression part of the pattern, that is, the
     regnum-th regstart pointer points to where in the pattern we began
     matching and the regnum-th regend points to right after where we
     stopped matching the regnum-th subexpression.  (The zeroth register
     keeps track of what the whole pattern matches.)  */

  unsigned char **regstart = RE_TALLOC(num_regs, unsigned char*);
  unsigned char **regend = RE_TALLOC(num_regs, unsigned char*);

  /* If a group that's operated upon by a repetition operator fails to
     match anything, then the register for its start will need to be
     restored because it will have been set to wherever in the string we
     are when we last see its open-group operator.  Similarly for a
     register's end.  */
  unsigned char **old_regstart = RE_TALLOC(num_regs, unsigned char*);
  unsigned char **old_regend = RE_TALLOC(num_regs, unsigned char*);

  /* The is_active field of reg_info helps us keep track of which (possibly
     nested) subexpressions we are currently in. The matched_something
     field of reg_info[reg_num] helps us tell whether or not we have
     matched any of the pattern so far this time through the reg_num-th
     subexpression.  These two fields get reset each time through any
     loop their register is in.  */

  register_info_type *reg_info = RE_TALLOC(num_regs, register_info_type);

  /* The following record the register info as found in the above
     variables when we find a match better than any we've seen before. 
     This happens as we backtrack through the failure points, which in
     turn happens only if we have not yet matched the entire string.  */

  unsigned best_regs_set = 0;
  unsigned char **best_regstart = RE_TALLOC(num_regs, unsigned char*);
  unsigned char **best_regend = RE_TALLOC(num_regs, unsigned char*);

  if (regs) {
    init_regs(regs, num_regs);
  }

  /* Initialize the stack. */
  stackb = RE_TALLOC(MAX_NUM_FAILURE_ITEMS * NFAILURES, unsigned char*);
  stackp = stackb;
  stacke = &stackb[MAX_NUM_FAILURE_ITEMS * NFAILURES];

#ifdef DEBUG_REGEX
  fprintf (stderr, "Entering re_match(%s%s)\n", string1_arg, string2_arg);
#endif

  /* Initialize subexpression text positions to -1 to mark ones that no
     ( or ( and ) or ) has been seen for. Also set all registers to
     inactive and mark them as not having matched anything or ever
     failed. */
  for (mcnt = 0; mcnt < num_regs; mcnt++) {
    regstart[mcnt] = regend[mcnt]
      = old_regstart[mcnt] = old_regend[mcnt]
      = best_regstart[mcnt] = best_regend[mcnt] = REG_UNSET_VALUE;
#ifdef __CHECKER__
    reg_info[mcnt].word = 0;
#endif
    REG_MATCH_NULL_STRING_P (reg_info[mcnt]) = MATCH_NULL_UNSET_VALUE;
    IS_ACTIVE (reg_info[mcnt]) = 0;
    MATCHED_SOMETHING (reg_info[mcnt]) = 0;
    EVER_MATCHED_SOMETHING (reg_info[mcnt]) = 0;
  }

  /* Set up pointers to ends of strings.
     Don't allow the second string to be empty unless both are empty.  */


  /* `p' scans through the pattern as `d' scans through the data. `dend'
     is the end of the input string that `d' points within. `d' is
     advanced into the following input string whenever necessary, but
     this happens before fetching; therefore, at the beginning of the
     loop, `d' can be pointing at the end of a string, but it cannot
     equal string2.  */

  d = string + pos, dend = string + size;


  /* This loops over pattern commands.  It exits by returning from the
     function if match is complete, or it drops through if match fails
     at this starting point in the input data.  */

  for (;;)
    {
#ifdef DEBUG_REGEX
      fprintf(stderr,
	      "regex loop(%d):  matching 0x%02d\n",
	      p - (unsigned char*)bufp->buffer,
	      *p);
#endif
      /* End of pattern means we might have succeeded.  */
      if (p == pend)
	{
	  /* If not end of string, try backtracking.  Otherwise done.  */
          if (d != dend)
	    {
	      while (stackp != stackb && (int)stackp[-1] == 1)
		POP_FAILURE_POINT();
              if (stackp != stackb)
                {
		  /* More failure points to try.  */

                  /* If exceeds best match so far, save it.  */
                  if (! best_regs_set || (d > best_regend[0]))
                    {
                      best_regs_set = 1;
                      best_regend[0] = d;	/* Never use regstart[0].  */

                      for (mcnt = 1; mcnt < num_regs; mcnt++)
                        {
                          best_regstart[mcnt] = regstart[mcnt];
                          best_regend[mcnt] = regend[mcnt];
                        }
                    }
                  goto fail;	       
                }
              /* If no failure points, don't restore garbage.  */
              else if (best_regs_set)   
                {
	      restore_best_regs:
                  /* Restore best match.  */
                  d = best_regend[0];

		  for (mcnt = 0; mcnt < num_regs; mcnt++)
		    {
		      regstart[mcnt] = best_regstart[mcnt];
		      regend[mcnt] = best_regend[mcnt];
		    }
                }
            }

	  /* If caller wants register contents data back, convert it 
	     to indices.  */
	  if (regs)
	    {
	      regs->beg[0] = pos;
	      regs->end[0] = d - string;
	      for (mcnt = 1; mcnt < num_regs; mcnt++)
		{
		  if (REG_UNSET(regend[mcnt]))
		    {
		      regs->beg[mcnt] = -1;
		      regs->end[mcnt] = -1;
		      continue;
		    }
		  regs->beg[mcnt] = regstart[mcnt] - string;
		  regs->end[mcnt] = regend[mcnt] - string;
		}
	    }
	  FREE_VARIABLES();
	  FREE_AND_RETURN(stackb, (d - pos - string));
        }

      /* Otherwise match next pattern command.  */
#ifdef SWITCH_ENUM_BUG
      switch ((int)((enum regexpcode)*p++))
#else
      switch ((enum regexpcode)*p++)
#endif
	{

	/* ( [or `(', as appropriate] is represented by start_memory,
           ) by stop_memory.  Both of those commands are followed by
           a register number in the next byte.  The text matched
           within the ( and ) is recorded under that number.  */
	case start_memory:
          /* Find out if this group can match the empty string.  */
	  p1 = p;		/* To send to group_match_null_string_p.  */
          if (REG_MATCH_NULL_STRING_P (reg_info[*p]) == MATCH_NULL_UNSET_VALUE)
            REG_MATCH_NULL_STRING_P (reg_info[*p]) 
              = group_match_null_string_p (&p1, pend, reg_info);

          /* Save the position in the string where we were the last time
             we were at this open-group operator in case the group is
             operated upon by a repetition operator, e.g., with `(a*)*b'
             against `ab'; then we want to ignore where we are now in
             the string in case this attempt to match fails.  */
          old_regstart[*p] = REG_MATCH_NULL_STRING_P (reg_info[*p])
                             ? REG_UNSET (regstart[*p]) ? d : regstart[*p]
                             : regstart[*p];
          regstart[*p] = d;
          IS_ACTIVE(reg_info[*p]) = 1;
          MATCHED_SOMETHING(reg_info[*p]) = 0;
          p += 2;
	  continue;

	case stop_memory:
          /* We need to save the string position the last time we were at
             this close-group operator in case the group is operated
             upon by a repetition operator, e.g., with `((a*)*(b*)*)*'
             against `aba'; then we want to ignore where we are now in
             the string in case this attempt to match fails.  */
          old_regend[*p] = REG_MATCH_NULL_STRING_P (reg_info[*p])
                           ? REG_UNSET (regend[*p]) ? d : regend[*p]
			   : regend[*p];

          regend[*p] = d;
          IS_ACTIVE(reg_info[*p]) = 0;

          /* If just failed to match something this time around with a sub-
	     expression that's in a loop, try to force exit from the loop.  */
          if ((p + 1) != pend &&
	      (! MATCHED_SOMETHING(reg_info[*p])
	       || (enum regexpcode)p[-3] == start_memory))
            {
	      p1 = p + 2;
              mcnt = 0;
              switch (*p1++)
                {
                  case jump_n:
		  case finalize_push_n:
                  case finalize_jump:
		  case maybe_finalize_jump:
		  case jump:
		  case dummy_failure_jump:
                    EXTRACT_NUMBER_AND_INCR(mcnt, p1);
                    break;
                }
	      p1 += mcnt;

              /* If the next operation is a jump backwards in the pattern
	         to an on_failure_jump, exit from the loop by forcing a
                 failure after pushing on the stack the on_failure_jump's 
                 jump in the pattern, and d.  */
	      if (mcnt < 0 && (enum regexpcode)*p1 == on_failure_jump
                  && (enum regexpcode)p1[3] == start_memory && p1[4] == *p)
		{
                  /* If this group ever matched anything, then restore
                     what its registers were before trying this last
                     failed match, e.g., with `(a*)*b' against `ab' for
                     regstart[1], and, e.g., with `((a*)*(b*)*)*'
                     against `aba' for regend[3].
                     
                     Also restore the registers for inner groups for,
                     e.g., `((a*)(b*))*' against `aba' (register 3 would
                     otherwise get trashed).  */
                     
                  if (EVER_MATCHED_SOMETHING (reg_info[*p]))
		    {
		      unsigned r; 
        
                      EVER_MATCHED_SOMETHING (reg_info[*p]) = 0;
                      
		      /* Restore this and inner groups' (if any) registers.  */
                      for (r = *p; r < *p + *(p + 1); r++)
                        {
                          regstart[r] = old_regstart[r];

                          /* xx why this test?  */
                          if ((int)old_regend[r] >= (int)regstart[r])
                            regend[r] = old_regend[r];
                        }     
                    }
		  p1++;
                  EXTRACT_NUMBER_AND_INCR(mcnt, p1);
                  PUSH_FAILURE_POINT(p1 + mcnt, d);
                  goto fail;
                }
            }
          p += 2;
	  continue;

	/* \<digit> has been turned into a `duplicate' command which is
           followed by the numeric value of <digit> as the register number.  */
        case duplicate:
	  {
	    int regno = *p++;   /* Get which register to match against */
	    register unsigned char *d2, *dend2;

	    if (IS_ACTIVE(reg_info[regno])) break;

	    /* Where in input to try to start matching.  */
            d2 = regstart[regno];
	    if (REG_UNSET(d2)) break;

            /* Where to stop matching; if both the place to start and
               the place to stop matching are in the same string, then
               set to the place to stop, otherwise, for now have to use
               the end of the first string.  */

            dend2 = regend[regno];
	    if (REG_UNSET(dend2)) break;
	    for (;;)
	      {
		/* At end of register contents => success */
		if (d2 == dend2) break;

		/* If necessary, advance to next segment in data.  */
		PREFETCH;

		/* How many characters left in this segment to match.  */
		mcnt = dend - d;

		/* Want how many consecutive characters we can match in
                   one shot, so, if necessary, adjust the count.  */
                if (mcnt > dend2 - d2)
		  mcnt = dend2 - d2;

		/* Compare that many; failure if mismatch, else move
                   past them.  */
		if ((options & RE_OPTION_IGNORECASE) 
                    ? memcmp_translate(d, d2, mcnt) 
                    : memcmp((char*)d, (char*)d2, mcnt))
		  goto fail;
		d += mcnt, d2 += mcnt;
	      }
	  }
	  break;

	case start_nowidth:
          PUSH_FAILURE_POINT(0, d);
	  EXTRACT_NUMBER_AND_INCR(mcnt, p);
	  STORE_NUMBER(p+mcnt, stackp - stackb);
	  continue;

	case stop_nowidth:
	  EXTRACT_NUMBER_AND_INCR(mcnt, p);
	  stackp = stackb + mcnt;
	  d = stackp[-2];
	  POP_FAILURE_POINT();
	  continue;

	case pop_and_fail:
	  EXTRACT_NUMBER(mcnt, p+1);
	  stackp = stackb + mcnt;
	  POP_FAILURE_POINT();
	  goto fail;

	case anychar:
	  PREFETCH;
	  /* Match anything but a newline, maybe even a null.  */
	  if (ismbchar(*d)) {
	    if (d + 1 == dend || d[1] == '\n' || d[1] == '\0')
	      goto fail;
	    SET_REGS_MATCHED;
	    d += 2;
	    break;
	  }
	  if (((TRANSLATE_P()) ? translate[*d] : *d) == '\n')
	    goto fail;
	  SET_REGS_MATCHED;
          d++;
	  break;

	case charset:
	case charset_not:
	  {
	    int not;	    /* Nonzero for charset_not.  */
	    int half;	    /* 2 if need to match latter half of mbc */
	    int c;

	    PREFETCH;
	    c = (unsigned char)*d;
	    if (ismbchar(c)) {
	      if (d + 1 != dend) {
	        c <<= 8;
		c |= (unsigned char)d[1];
	      }
	    }
	    else if (TRANSLATE_P())
	      c = (unsigned char)translate[c];

	    half = not = is_in_list(c, p);
	    if (*(p - 1) == (unsigned char)charset_not) {
		not = !not;
	    }

	    p += 1 + *p + 2 + EXTRACT_UNSIGNED(&p[1 + *p])*4;

	    if (!not) goto fail;
	    SET_REGS_MATCHED;

            d++;
	    if (half != 2 && d != dend && c >= 1 << BYTEWIDTH)
		d++;
	    break;
	  }

	case begline:
          if (size == 0
	      || AT_STRINGS_BEG(d)
              || (d && d[-1] == '\n'))
            break;
          else
            goto fail;

	case endline:
	  if (AT_STRINGS_END(d) || *d == '\n')
	    break;
	  goto fail;

	/* Match at the very beginning of the string. */
	case begbuf:
          if (AT_STRINGS_BEG(d))
            break;
          goto fail;

	/* Match at the very end of the data. */
        case endbuf:
	  if (AT_STRINGS_END(d))
	    break;
          goto fail;

	/* Match at the very end of the data. */
        case endbuf2:
	  if (AT_STRINGS_END(d))
	    break;
	  /* .. or newline just before the end of the data. */
	  if (*d == '\n' && AT_STRINGS_END(d+1))
	    break;
          goto fail;

	/* `or' constructs are handled by starting each alternative with
           an on_failure_jump that points to the start of the next
           alternative.  Each alternative except the last ends with a
           jump to the joining point.  (Actually, each jump except for
           the last one really jumps to the following jump, because
           tensioning the jumps is a hassle.)  */

	/* The start of a stupid repeat has an on_failure_jump that points
	   past the end of the repeat text. This makes a failure point so 
           that on failure to match a repetition, matching restarts past
           as many repetitions have been found with no way to fail and
           look for another one.  */

	/* A smart repeat is similar but loops back to the on_failure_jump
	   so that each repetition makes another failure point.  */

	case on_failure_jump:
        on_failure:
          EXTRACT_NUMBER_AND_INCR(mcnt, p);
          PUSH_FAILURE_POINT(p + mcnt, d);
          continue;

	/* The end of a smart repeat has a maybe_finalize_jump back.
	   Change it either to a finalize_jump or an ordinary jump.  */
	case maybe_finalize_jump:
          EXTRACT_NUMBER_AND_INCR(mcnt, p);
	  {
	    register unsigned char *p2 = p;

            /* Compare the beginning of the repeat with what in the
               pattern follows its end. If we can establish that there
               is nothing that they would both match, i.e., that we
               would have to backtrack because of (as in, e.g., `a*a')
               then we can change to pop_failure_jump, because we'll
               never have to backtrack.
               
               This is not true in the case of alternatives: in
               `(a|ab)*' we do need to backtrack to the `ab' alternative
               (e.g., if the string was `ab').  But instead of trying to
               detect that here, the alternative has put on a dummy
               failure point which is what we will end up popping.  */

	    /* Skip over open/close-group commands.  */
	    while (p2 + 2 < pend) {
	      if ((enum regexpcode)*p2 == stop_memory
		  || (enum regexpcode)*p2 == start_memory)
		p2 += 3;	/* Skip over args, too.  */
	      else
		break;
	    }

	    if (p2 == pend)
	      p[-3] = (unsigned char)finalize_jump;
	    else if (*p2 == (unsigned char)exactn
		     || *p2 == (unsigned char)endline)
	      {
		register int c = *p2 == (unsigned char)endline ? '\n' : p2[2];
		register unsigned char *p1 = p + mcnt;
		/* p1[0] ... p1[2] are an on_failure_jump.
		   Examine what follows that.  */
		if (p1[3] == (unsigned char)exactn && p1[5] != c)
		  p[-3] = (unsigned char)finalize_jump;
		else if (p1[3] == (unsigned char)charset
			 || p1[3] == (unsigned char)charset_not) {
		    int not;
		    if (ismbchar(c))
		      c = c << 8 | p2[3];
		    /* `is_in_list()' is TRUE if c would match */
		    /* That means it is not safe to finalize.  */
		    not = is_in_list(c, p1 + 4);
		    if (p1[3] == (unsigned char)charset_not)
			not = !not;
		    if (!not)
			p[-3] = (unsigned char)finalize_jump;
		  }
	      }
	  }
	  p -= 2;		/* Point at relative address again.  */
	  if (p[-1] != (unsigned char)finalize_jump)
	    {
	      p[-1] = (unsigned char)jump;	
	      goto nofinalize;
	    }
        /* Note fall through.  */

	/* The end of a stupid repeat has a finalize_jump back to the
           start, where another failure point will be made which will
           point to after all the repetitions found so far.  */

        /* Take off failure points put on by matching on_failure_jump 
           because didn't fail.  Also remove the register information
           put on by the on_failure_jump.  */
        case finalize_jump:
          POP_FAILURE_POINT();
        /* Note fall through.  */

	/* Jump without taking off any failure points.  */
        case jump:
	nofinalize:
	  EXTRACT_NUMBER_AND_INCR(mcnt, p);
	  p += mcnt;
	  continue;

        /* We need this opcode so we can detect where alternatives end
           in `group_match_null_string_p' et al.  */
        case jump_past_alt:
          goto nofinalize;

        case dummy_failure_jump:
          /* Normally, the on_failure_jump pushes a failure point, which
             then gets popped at finalize_jump.  We will end up at
             finalize_jump, also, and with a pattern of, say, `a+', we
             are skipping over the on_failure_jump, so we have to push
             something meaningless for finalize_jump to pop.  */
          PUSH_FAILURE_POINT(0, 0);
          goto nofinalize;

        /* At the end of an alternative, we need to push a dummy failure
           point in case we are followed by a `finalize_jump', because
           we don't want the failure point for the alternative to be
           popped.  For example, matching `(a|ab)*' against `aab'
           requires that we match the `ab' alternative.  */
        case push_dummy_failure:
          /* See comments just above at `dummy_failure_jump' about the
             two zeroes.  */
          PUSH_FAILURE_POINT(0, 0);
          break;

        /* Have to succeed matching what follows at least n times.  Then
          just handle like an on_failure_jump.  */
        case succeed_n: 
          EXTRACT_NUMBER(mcnt, p + 2);
          /* Originally, this is how many times we HAVE to succeed.  */
          if (mcnt > 0)
            {
               mcnt--;
	       p += 2;
               STORE_NUMBER_AND_INCR(p, mcnt);
	       PUSH_FAILURE_POINT(0, 0);
            }
	  else if (mcnt == 0)
            {
	      p[2] = unused;
              p[3] = unused;
              goto on_failure;
            }
	  continue;

        case jump_n:
          EXTRACT_NUMBER(mcnt, p + 2);
          /* Originally, this is how many times we CAN jump.  */
          if (mcnt)
            {
               mcnt--;
               STORE_NUMBER(p + 2, mcnt);
	       goto nofinalize;	     /* Do the jump without taking off
			                any failure points.  */
            }
          /* If don't have to jump any more, skip over the rest of command.  */
	  else      
	    p += 4;		     
	  continue;

	case set_number_at:
	  EXTRACT_NUMBER_AND_INCR(mcnt, p);
	  p1 = p + mcnt;
	  EXTRACT_NUMBER_AND_INCR(mcnt, p);
	  STORE_NUMBER(p1, mcnt);
	  continue;

	case try_next:
	  EXTRACT_NUMBER_AND_INCR(mcnt, p);
	  if (p + mcnt < pend) {
	    PUSH_FAILURE_POINT(p, d);
	    stackp[-1] = (unsigned char*)1;
	  }
	  p += mcnt;
	  continue;

	case finalize_push:
          POP_FAILURE_POINT();
	  EXTRACT_NUMBER_AND_INCR(mcnt, p);
          PUSH_FAILURE_POINT(p + mcnt, d);
	  stackp[-1] = (unsigned char*)1;
	  continue;

	case finalize_push_n:
          EXTRACT_NUMBER(mcnt, p + 2); 
         /* Originally, this is how many times we CAN jump.  */
          if (mcnt) {
	    int pos, i;

	    mcnt--;
	    STORE_NUMBER(p + 2, mcnt);
	    EXTRACT_NUMBER(pos, p);
	    EXTRACT_NUMBER(i, p+pos+5);
	    if (i > 0) goto nofinalize;
	    POP_FAILURE_POINT();
	    EXTRACT_NUMBER_AND_INCR(mcnt, p);
	    PUSH_FAILURE_POINT(p + mcnt, d);
	    stackp[-1] = (unsigned char*)1;
	    p += 2;		/* skip n */
	  }
          /* If don't have to push any more, skip over the rest of command.  */
	  else 
	    p += 4;   
	  continue;

        /* Ignore these.  Used to ignore the n of succeed_n's which
           currently have n == 0.  */
        case unused:
	  continue;

        case casefold_on:
	  options |= RE_OPTION_IGNORECASE;
	  continue;

        case casefold_off:
	  options &= ~RE_OPTION_IGNORECASE;
	  continue;

        case wordbound:
	  if (AT_STRINGS_BEG(d)) {
	    if (IS_A_LETTER(d)) break;
	    else goto fail;
	  }
	  if (AT_STRINGS_BEG(d)) {
	    if (IS_A_LETTER(d-1)) break;
	    else goto fail;
	  }
	  if (IS_A_LETTER(d - 1) != IS_A_LETTER(d))
	    break;
	  goto fail;

	case notwordbound:
	  if (AT_STRINGS_BEG(d)) {
	    if (IS_A_LETTER(d)) goto fail;
	    else break;
	  }
	  if (AT_STRINGS_END(d)) {
	    if (IS_A_LETTER(d-1)) goto fail;
	    else break;
	  }
	  if (IS_A_LETTER(d - 1) != IS_A_LETTER(d))
	    goto fail;
	  break;

	case wordbeg:
	  if (IS_A_LETTER(d) && (AT_STRINGS_BEG(d) || !IS_A_LETTER(d - 1)))
	    break;
          goto fail;

	case wordend:
	  if (!AT_STRINGS_BEG(d) && IS_A_LETTER(d - 1)
              && (!IS_A_LETTER(d) || AT_STRINGS_END(d)))
	    break;
          goto fail;

	case wordchar:
	  PREFETCH;
          if (!IS_A_LETTER(d))
            goto fail;
	  d++;
	  SET_REGS_MATCHED;
	  break;

	case notwordchar:
	  PREFETCH;
	  if (IS_A_LETTER(d))
            goto fail;
	  if (ismbchar(*d) && d + 1 != dend)
	    d++;
	  d++;
          SET_REGS_MATCHED;
	  break;

	case exactn:
	  /* Match the next few pattern characters exactly.
	     mcnt is how many characters to match.  */
	  mcnt = *p++;
	  /* This is written out as an if-else so we don't waste time
             testing `translate' inside the loop.  */
          if (TRANSLATE_P())
	    {
	      do
		{
		  unsigned char c;

		  PREFETCH;
		  c = *d++;
		  if (*p == 0xff) {
		    p++;  
		    if (!--mcnt
			|| AT_STRINGS_END(d)
			|| (unsigned char)*d++ != (unsigned char)*p++)
		      goto fail;
		    continue;
		  }
		  if (ismbchar(c)) {
		    if (c != (unsigned char)*p++
			|| !--mcnt	/* redundant check if pattern was
					   compiled properly. */
			|| AT_STRINGS_END(d)
			|| (unsigned char)*d++ != (unsigned char)*p++)
		      goto fail;
		    continue;
		  }
		  /* compiled code translation needed for ruby */
		  if ((unsigned char)translate[c]
		      != (unsigned char)translate[*p++])
		    goto fail;
		}
	      while (--mcnt);
	    }
	  else
	    {
	      do
		{
		  PREFETCH;
		  if (*p == 0xff) {p++; mcnt--;}
		  if (*d++ != *p++) goto fail;
		}
	      while (--mcnt);
	    }
	  SET_REGS_MATCHED;
          break;
	}
      while (stackp != stackb && (int)stackp[-1] == 1)
	POP_FAILURE_POINT();
      continue;  /* Successfully executed one pattern command; keep going.  */

    /* Jump here if any matching operation fails. */
    fail:
      if (stackp != stackb)
	/* A restart point is known.  Restart there and pop it. */
	{
          short last_used_reg, this_reg;

          /* If this failure point is from a dummy_failure_point, just
             skip it.  */
	  if (stackp[-3] == 0) {
	    POP_FAILURE_POINT();
	    goto fail;
	  }
	  stackp--;		/* discard flag */
          d = *--stackp;
	  p = *--stackp;
          /* Restore register info.  */
          last_used_reg = (long)*--stackp;

          /* Make the ones that weren't saved -1 or 0 again. */
          for (this_reg = num_regs - 1; this_reg > last_used_reg; this_reg--)
            {
              regend[this_reg] = REG_UNSET_VALUE;
              regstart[this_reg] = REG_UNSET_VALUE;
              IS_ACTIVE(reg_info[this_reg]) = 0;
              MATCHED_SOMETHING(reg_info[this_reg]) = 0;
            }

          /* And restore the rest from the stack.  */
          for ( ; this_reg > 0; this_reg--)
            {
              reg_info[this_reg].word = *--stackp;
              regend[this_reg] = *--stackp;
              regstart[this_reg] = *--stackp;
            }
          if (p < pend)
            {
              int is_a_jump_n = 0;

	      p1 = p;
              /* If failed to a backwards jump that's part of a repetition
                 loop, need to pop this failure point and use the next one.  */
              switch ((enum regexpcode)*p1)
                {
                case jump_n:
                case finalize_push_n:
                  is_a_jump_n = 1;
                case maybe_finalize_jump:
                case finalize_jump:
                case finalize_push:
                case jump:
                  p1++;
                  EXTRACT_NUMBER_AND_INCR (mcnt, p1);
                  p1 += mcnt;

		  if (p1 >= pend) break;
                  if ((is_a_jump_n && (enum regexpcode)*p1 == succeed_n)
                      || (!is_a_jump_n
                          && (enum regexpcode)*p1 == on_failure_jump))
                    goto fail;
                  break;
                default:
                  /* do nothing */ ;
                }
            }
        }
      else
        break;   /* Matching at this starting point really fails.  */
    }

  if (best_regs_set)
    goto restore_best_regs;

  FREE_AND_RETURN(stackb,(-1)); 	/* Failure to match.  */
}


/* We are passed P pointing to a register number after a start_memory.
   
   Return true if the pattern up to the corresponding stop_memory can
   match the empty string, and false otherwise.
   
   If we find the matching stop_memory, sets P to point to one past its number.
   Otherwise, sets P to an undefined byte less than or equal to END.

   We don't handle duplicates properly (yet).  */

static int
group_match_null_string_p (p, end, reg_info)
    unsigned char **p, *end;
    register_info_type *reg_info;
{
  int mcnt;
  /* Point to after the args to the start_memory.  */
  unsigned char *p1 = *p + 2;
  
  while (p1 < end)
    {
      /* Skip over opcodes that can match nothing, and return true or
	 false, as appropriate, when we get to one that can't, or to the
         matching stop_memory.  */
      
      switch ((enum regexpcode)*p1)
        {
        /* Could be either a loop or a series of alternatives.  */
        case on_failure_jump:
          p1++;
          EXTRACT_NUMBER_AND_INCR (mcnt, p1);
          
          /* If the next operation is not a jump backwards in the
	     pattern.  */

	  if (mcnt >= 0)
	    {
              /* Go through the on_failure_jumps of the alternatives,
                 seeing if any of the alternatives cannot match nothing.
                 The last alternative starts with only a jump,
                 whereas the rest start with on_failure_jump and end
                 with a jump, e.g., here is the pattern for `a|b|c':

                 /on_failure_jump/0/6/exactn/1/a/jump_past_alt/0/6
                 /on_failure_jump/0/6/exactn/1/b/jump_past_alt/0/3
                 /exactn/1/c						

                 So, we have to first go through the first (n-1)
                 alternatives and then deal with the last one separately.  */


              /* Deal with the first (n-1) alternatives, which start
                 with an on_failure_jump (see above) that jumps to right
                 past a jump_past_alt.  */

              while ((enum regexpcode)p1[mcnt-3] == jump_past_alt)
                {
                  /* `mcnt' holds how many bytes long the alternative
                     is, including the ending `jump_past_alt' and
                     its number.  */

                  if (!alt_match_null_string_p (p1, p1 + mcnt - 3, 
				                      reg_info))
                    return 0;

                  /* Move to right after this alternative, including the
		     jump_past_alt.  */
                  p1 += mcnt;	

                  /* Break if it's the beginning of an n-th alternative
                     that doesn't begin with an on_failure_jump.  */
                  if ((enum regexpcode)*p1 != on_failure_jump)
                    break;
		
		  /* Still have to check that it's not an n-th
		     alternative that starts with an on_failure_jump.  */
		  p1++;
                  EXTRACT_NUMBER_AND_INCR (mcnt, p1);
                  if ((enum regexpcode)p1[mcnt-3] != jump_past_alt)
                    {
		      /* Get to the beginning of the n-th alternative.  */
                      p1 -= 3;
                      break;
                    }
                }

              /* Deal with the last alternative: go back and get number
                 of the `jump_past_alt' just before it.  `mcnt' contains
                 the length of the alternative.  */
              EXTRACT_NUMBER (mcnt, p1 - 2);
#if 0
              if (!alt_match_null_string_p (p1, p1 + mcnt, reg_info))
                return 0;
#endif
              p1 += mcnt;	/* Get past the n-th alternative.  */
            } /* if mcnt > 0 */
          break;

          
        case stop_memory:
          *p = p1 + 2;
          return 1;

        
        default: 
          if (!common_op_match_null_string_p (&p1, end, reg_info))
            return 0;
        }
    } /* while p1 < end */

  return 0;
} /* group_match_null_string_p */


/* Similar to group_match_null_string_p, but doesn't deal with alternatives:
   It expects P to be the first byte of a single alternative and END one
   byte past the last. The alternative can contain groups.  */
   
static int
alt_match_null_string_p (p, end, reg_info)
    unsigned char *p, *end;
    register_info_type *reg_info;
{
  int mcnt;
  unsigned char *p1 = p;
  
  while (p1 < end)
    {
      /* Skip over opcodes that can match nothing, and break when we get 
         to one that can't.  */
      
      switch ((enum regexpcode)*p1)
        {
	/* It's a loop.  */
        case on_failure_jump:
          p1++;
          EXTRACT_NUMBER_AND_INCR (mcnt, p1);
          p1 += mcnt;
          break;
          
	default: 
          if (!common_op_match_null_string_p (&p1, end, reg_info))
            return 0;
        }
    }  /* while p1 < end */

  return 1;
} /* alt_match_null_string_p */


/* Deals with the ops common to group_match_null_string_p and
   alt_match_null_string_p.  
   
   Sets P to one after the op and its arguments, if any.  */

static int
common_op_match_null_string_p (p, end, reg_info)
    unsigned char **p, *end;
    register_info_type *reg_info;
{
  int mcnt;
  int ret;
  int reg_no;
  unsigned char *p1 = *p;

  switch ((enum regexpcode)*p1++)
    {
    case unused:
    case begline:
    case endline:
    case begbuf:
    case endbuf:
    case endbuf2:
    case wordbeg:
    case wordend:
    case wordbound:
    case notwordbound:
#ifdef emacs
    case before_dot:
    case at_dot:
    case after_dot:
#endif
      break;

    case start_memory:
      reg_no = *p1;
      ret = group_match_null_string_p (&p1, end, reg_info);
      
      /* Have to set this here in case we're checking a group which
         contains a group and a back reference to it.  */

      if (REG_MATCH_NULL_STRING_P (reg_info[reg_no]) == MATCH_NULL_UNSET_VALUE)
        REG_MATCH_NULL_STRING_P (reg_info[reg_no]) = ret;

      if (!ret)
        return 0;
      break;
          
    /* If this is an optimized succeed_n for zero times, make the jump.  */
    case jump:
      EXTRACT_NUMBER_AND_INCR (mcnt, p1);
      if (mcnt >= 0)
        p1 += mcnt;
      else
        return 0;
      break;

    case succeed_n:
      /* Get to the number of times to succeed.  */
      p1 += 2;		
      EXTRACT_NUMBER_AND_INCR (mcnt, p1);

      if (mcnt == 0)
        {
          p1 -= 4;
          EXTRACT_NUMBER_AND_INCR (mcnt, p1);
          p1 += mcnt;
        }
      else
        return 0;
      break;

    case duplicate: 
      if (!REG_MATCH_NULL_STRING_P (reg_info[*p1]))
        return 0;
      break;

    case set_number_at:
      p1 += 4;

    default:
      /* All other opcodes mean we cannot match the empty string.  */
      return 0;
  }

  *p = p1;
  return 1;
} /* common_op_match_null_string_p */


static int
memcmp_translate(s1, s2, len)
     unsigned char *s1, *s2;
     register int len;
{
  register unsigned char *p1 = s1, *p2 = s2, c;
  while (len)
    {
      c = *p1++;
      if (ismbchar(c)) {
	if (c != *p2++ || !--len || *p1++ != *p2++)
	  return 1;
      }
      else
	if (translate[c] != translate[*p2++])
	  return 1;
      len--;
    }
  return 0;
}

void
re_copy_registers(regs1, regs2)
     struct re_registers *regs1, *regs2;
{
    int i;

    if (regs1 == regs2) return;
    if (regs1->allocated == 0) {
	regs1->beg = TMALLOC(regs2->num_regs, int);
	regs1->end = TMALLOC(regs2->num_regs, int);
	regs1->allocated = regs2->num_regs;
    }
    else if (regs1->allocated < regs2->num_regs) {
	TREALLOC(regs1->beg, regs2->num_regs, int);
	TREALLOC(regs1->end, regs2->num_regs, int);
	regs1->allocated = regs2->num_regs;
    }
    for (i=0; i<regs2->num_regs; i++) {
	regs1->beg[i] = regs2->beg[i];
	regs1->end[i] = regs2->end[i];
    }
    regs1->num_regs = regs2->num_regs;
}

void
re_free_registers(regs)
     struct re_registers *regs;
{
    if (regs->allocated == 0) return;
    if (regs->beg) free(regs->beg);
    if (regs->end) free(regs->end);
}

/* Functions for multi-byte support.
   Created for grep multi-byte extension Jul., 1993 by t^2 (Takahiro Tanimoto)
   Last change: Jul. 9, 1993 by t^2  */
static const unsigned char mbctab_ascii[] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const unsigned char mbctab_euc[] = { /* 0xA1-0xFE */
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0
};

static const unsigned char mbctab_sjis[] = { /* 0x80-0x9f,0xE0-0xFF */
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
};

const unsigned char *mbctab = mbctab_ascii;
int current_mbctype = MBCTYPE_ASCII;

void
re_mbcinit(mbctype)
     int mbctype;
{
  switch (mbctype) {
  case MBCTYPE_ASCII:
    mbctab = mbctab_ascii;
    current_mbctype = MBCTYPE_ASCII;
    break;
  case MBCTYPE_EUC:
    mbctab = mbctab_euc;
    current_mbctype = MBCTYPE_EUC;
    break;
  case MBCTYPE_SJIS:
    mbctab = mbctab_sjis;
    current_mbctype = MBCTYPE_SJIS;
    break;
  }
}
