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

const char rcsid_main_c[] = "$Id: main.c,v 1.11 2001/02/07 19:09:23 dholland Exp $";

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

	msg("%llu cycles (%lluk, %lluu, %llui) in %lu.%06lu seconds (%g mhz)",
	    totcycles,
	    g_stats.s_kcycles,
	    g_stats.s_ucycles,
	    g_stats.s_icycles,
	    endtime.tv_sec,
	    endtime.tv_usec,
	    totcycles/(time*1000000.0));

	msg("%u irqs %u exns %ur/%uw disk %ur/%uw console",
	    g_stats.s_irqs,
	    g_stats.s_exns,
	    g_stats.s_rsects,
	    g_stats.s_wsects,
	    g_stats.s_rchars,
	    g_stats.s_wchars);
}

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
	int i,j;
	size_t argsize=0;
	int debugwait=0;
	
	if (sizeof(u_int32_t)!=4) {
		/*
		 * Just in case.
		 */
		msg("sys161 requires sizeof(u_int32_t)==4");
		die();
	}
	
	for (i=1; i<argc; i++) {
		if (argv[i][0]!='-') break;
		switch (argv[i][1]) {
		    case 'c': config = argv[++i]; break;
		    case 'p': port = atoi(argv[++i]); usetcp=1; break;
		    case 'w': debugwait = 1; break;
		    default: usage();
		}
	}
	if (i==argc) {
		usage();
	}
	kernel = argv[i++];
	
	for (j=i; j<argc; j++) {
		argsize += strlen(argv[j])+1;
	}
	argstr = malloc(argsize+1);
	if (!argstr) {
		msg("malloc failed");
		die();
	}
	*argstr = 0;
	for (j=i; j<argc; j++) {
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
