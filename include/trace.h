
#include "console.h"   // make sure we have PF()

#ifndef TRACE_H
#define TRACE_H

#ifdef USE_TRACE

#define DOTRACE_KINSN	0	/* trace instructions in kernel mode */
#define DOTRACE_UINSN	1	/* trace instructions in user mode */
#define DOTRACE_TLB     2	/* trace tlb operations */
#define DOTRACE_EXN     3	/* trace exceptions */
#define DOTRACE_IRQ     4	/* trace exceptions */
#define DOTRACE_DISK	5	/* trace disk ops */
#define DOTRACE_NET	6	/* trace net ops */
#define DOTRACE_EMUFS	7	/* trace emufs ops */
#define NDOTRACES	8

extern int g_traceflags[NDOTRACES];

void set_traceflags(const char *letters);
void print_traceflags(void);


/* These three functions are actually in console.c */

void set_tracefile(const char *filename);    /* set output destination */
void trace(const char *fmt, ...) PF(1,2);    /* trace output */
void tracel(const char *fmt, ...) PF(1,2);   /* trace w/o newline */


#define TRACEL(k, args)   (g_traceflags[(k)] ? tracel args : 0)
#define TRACE(k, args)    (g_traceflags[(k)] ? trace args : 0)



#else /* not USE_TRACE */


#define TRACEL(k, args)
#define TRACE(k, args)


#endif /* USE_TRACE */

#endif /* TRACE_H */
