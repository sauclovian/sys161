#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h> // for mkdir()
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

#include "console.h"
#include "gdb.h"
#include "cpu.h"
#include "bus.h"
#include "clock.h"
#include "speed.h"
#include "onsel.h"
#include "main.h"
#include "version.h"

#ifndef __GNUC__
#define inline
#endif

const char rcsid_main_c[] = "$Id: main.c,v 1.13 2001/02/26 19:07:14 dholland Exp $";

/* Global stats */
struct stats g_stats;

/* Flag for interrupting runloop or stoploop due to poweroff */
static int shutoff_flag;

/* Flag for interrupting stoploop() */
static int continue_flag;

/* Flag for interrupting runloop() */
static int stop_flag;

void
main_poweroff(void)
{
	shutoff_flag = 1;
}

void
main_stop(void)
{
	stop_flag = 1;
}

void
main_continue(void)
{
	continue_flag = 1;
}

static
void
stoploop(void)
{
	gdb_startbreak();
	continue_flag = 0;
	while (!continue_flag && !shutoff_flag) {
		tryselect(0, 0, 0);
	}
}

inline
int
onecycle(void)
{
	int hitbp;

	bus_forward_interrupts();

	hitbp = cpu_cycle();
	if (!hitbp) {
		clock_tick();
	}
	return hitbp;
}

/* number of cpu cycles between select polls */
#define ROTOR 5000

static
void
runloop(void)
{
	int rotor=0;

	while (!shutoff_flag) {
		stop_flag = 0;

		if (onecycle()) {
			/* Hit breakpoint */
			stop_flag = 1;
		}
		else {
			rotor++;
			if (rotor >= ROTOR) {
				rotor = 0;
				tryselect(1, 0, 0);
			}
		}

		if (stop_flag) {
			stoploop();
		}
	}
}

static
void
run(void)
{
	struct timeval starttime, endtime;
	u_int64_t totcycles;
	double time;

	gettimeofday(&starttime, NULL);

	runloop();

	gettimeofday(&endtime, NULL);

	endtime.tv_sec -= starttime.tv_sec;
	if (endtime.tv_usec < starttime.tv_usec) {
		endtime.tv_sec--;
		endtime.tv_usec += 1000000;
	}
	endtime.tv_usec -= starttime.tv_usec;

	time = endtime.tv_sec + endtime.tv_usec/1000000.0;

	totcycles = g_stats.s_kcycles + g_stats.s_ucycles + g_stats.s_icycles;

	if (sizeof(totcycles)==sizeof(long)) {
		msg("%lu cycles (%luk, %luu, %lui) in %lu.%06lu seconds"
		    " (%g mhz)",
		    (unsigned long)totcycles,
		    (unsigned long)g_stats.s_kcycles,
		    (unsigned long)g_stats.s_ucycles,
		    (unsigned long)g_stats.s_icycles,
		    endtime.tv_sec,
		    endtime.tv_usec,
		    totcycles/(time*1000000.0));
	}
	else {
		msg("%llu cycles (%lluk, %lluu, %llui) in %lu.%06lu seconds"
		    " (%g mhz)",
		    totcycles,
		    g_stats.s_kcycles,
		    g_stats.s_ucycles,
		    g_stats.s_icycles,
		    endtime.tv_sec,
		    endtime.tv_usec,
		    totcycles/(time*1000000.0));
	}

	msg("%u irqs %u exns %ur/%uw disk %ur/%uw console",
	    g_stats.s_irqs,
	    g_stats.s_exns,
	    g_stats.s_rsects,
	    g_stats.s_wsects,
	    g_stats.s_rchars,
	    g_stats.s_wchars);
}

////////////////////////////////////////////////////////////

/*
 * We don't use normal getopt because we need to stop on the
 * first non-option argument, and normal getopt has no standard
 * way to specify that.
 */

static const char *myoptarg;
static int myoptind, myoptchr;

static
int
mygetopt(int argc, char **argv, const char *myopts)
{
	int myopt;
	const char *p;

	if (myoptind==0) {
		myoptind = 1;
	}

	do {
		if (myoptind >= argc) {
			return -1;
		}
		
		if (myoptchr==0) {
			if (argv[myoptind][0] != '-') {
				return -1;
			}
			myoptchr = 1;
		}

		myopt = argv[myoptind][myoptchr];

		if (myopt==0) {
			myoptind++;
			myoptchr = 0;
		}
		else {
			myoptchr++;
		}

	} while (myopt == 0);

	if (myopt == ':' || (p = strchr(myopts, myopt))==NULL) {
		return '?';
	}
	if (p[1]==':') {
		/* option takes argument */
		if (strlen(argv[myoptind]+myoptchr)>0) {
			myoptarg = argv[myoptind]+myoptchr;
		}
		else {
			myoptarg = argv[++myoptind];
			if (myoptarg==NULL) {
				return '?';
			}
		}
		myoptind++;
		myoptchr = 0;
	}

	return myopt;
}

////////////////////////////////////////////////////////////

static
void
usage(void)
{
	msg("Usage: sys161 [-c config] kernel [args...]");
	die();
}

int
main(int argc, char *argv[])
{
	int port = 2344;
	const char *config = "sys161.conf";
	const char *kernel = NULL;
	int usetcp=0;
	char *argstr = NULL;
	int j, opt;
	size_t argsize=0;
	int debugwait=0;
	
	if (sizeof(u_int32_t)!=4) {
		/*
		 * Just in case.
		 */
		msg("sys161 requires sizeof(u_int32_t)==4");
		die();
	}

	while ((opt = mygetopt(argc, argv, "c:p:w"))!=-1) {
		switch (opt) {
		    case 'c': config = myoptarg; break;
		    case 'p': port = atoi(myoptarg); usetcp=1; break;
		    case 'w': debugwait = 1; break;
		    default: usage();
		}
	}
	if (myoptind==argc) {
		usage();
	}
	kernel = argv[myoptind++];
	
	for (j=myoptind; j<argc; j++) {
		argsize += strlen(argv[j])+1;
	}
	argstr = malloc(argsize+1);
	if (!argstr) {
		msg("malloc failed");
		die();
	}
	*argstr = 0;
	for (j=myoptind; j<argc; j++) {
		strcat(argstr, argv[j]);
		if (j<argc-1) strcat(argstr, " ");
	}
	
	bus_config(config);

	console_init();
	cpu_init();
	clock_init();

	if (usetcp) {
		gdb_inet_init(port);
	}
	else {
		mkdir(".sockets", 0700);
		unlink(".sockets/gdb");
		gdb_unix_init(".sockets/gdb");
	}

	load_kernel(kernel, argstr);

	msg("System/161 %s, compiled %s %s", VERSION, __DATE__, __TIME__);

	if (debugwait) {
		stoploop();
	}
	
	run();

	bus_cleanup();
	console_cleanup();
	
	return 0;
}
