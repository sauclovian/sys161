#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "gdb.h"
#include "cpu.h"
#include "bus.h"
#include "clock.h"
#include "speed.h"
#include "onsel.h"


const char rcsid_main_c[] = "$Id: main.c,v 1.3 2001/01/25 04:49:47 dholland Exp $";

/* Total cycles executed. */
static int totcycles = 0;

/* Cycles until poweroff, or -1 */
static int shutofftime = -1;

/* Flag for interrupting stoploop() */
static int continue_flag;

/* Flag for interrupting runloop() */
static int stop_flag;


void
main_poweroff(void)
{
	if (shutofftime < 0) {
		shutofftime = POWEROFF_CLOCKS;
	}
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
	stop_flag = 0;
	while (!stop_flag) {
		tryselect(0);
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
		if (shutofftime > 0) shutofftime--;
		totcycles++;
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

	while (shutofftime != 0) {
		stop_flag = 0;

		if (onecycle()) {
			/* Hit breakpoint */
			stop_flag = 1;
		}
		else {
			rotor++;
			if (rotor >= ROTOR) {
				rotor = 0;
				tryselect(1);
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
	double time;

	gettimeofday(&starttime, NULL);
	totcycles = 0;

	runloop();

	gettimeofday(&endtime, NULL);

	endtime.tv_sec -= starttime.tv_sec;
	if (endtime.tv_usec < starttime.tv_usec) {
		endtime.tv_sec--;
		endtime.tv_usec += 1000000;
	}
	endtime.tv_usec -= starttime.tv_usec;

	time = endtime.tv_sec + endtime.tv_usec/1000000.0;

	msg("%u cycles in %lu.%06lu seconds (%g mhz)", 
	       totcycles, 
	       endtime.tv_sec,
	       endtime.tv_usec,
	       totcycles/(time*1000000));
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
		    case 'p': port = atoi(argv[++i]); break;
		    case 'w': debugwait = 1; break;
		    default: usage();
		}
	}
	if (i==argc) {
		usage();
	}
	kernel = argv[i++];
	
	for (j=i; j<argc; j++) {
		argsize += strlen(argv[i])+1;
	}
	argstr = malloc(argsize);
	if (!argstr) {
		msg("malloc failed");
		die();
	}
	*argstr = 0;
	for (j=i; j<argc; j++) {
		strcat(argstr, argv[i]);
		if (j<argc-1) strcat(argstr, " ");
	}
	
	bus_config(config);

	console_init();
	cpu_init();
	clock_init();
 	gdb_inet_init(port);
	//mkdir(".gdb", 0700);
	//gdb_unix_init(".gdb/socket");

	load_kernel(kernel, argstr);


	if (debugwait) {
		stoploop();
	}
	
	run();

	bus_cleanup();
	console_cleanup();
	
	return 0;
}
