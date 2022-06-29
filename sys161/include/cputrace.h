/*
 * This can be included multiple times with different USE_TRACE settings
 */

#undef CPUTRACEL
#undef CPUTRACE
#undef HWTRACEL
#undef HWTRACE

#ifdef USE_TRACE

#define CPUTRACEL(k, cn, ...) \
	(g_traceflags[(k)] ? cputracel(cn, __VA_ARGS__) : (void)0)
#define CPUTRACE(k, cn, ...)  \
	(g_traceflags[(k)] ? cputrace(cn, __VA_ARGS__) : (void)0)

#else /* not USE_TRACE */


#define CPUTRACEL(k, ...)
#define CPUTRACE(k, ...)


#endif /* USE_TRACE */
