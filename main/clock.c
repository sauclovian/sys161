#include <sys/types.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

#include "console.h"
#include "speed.h"
#include "clock.h"
#include "cpu.h"
#include "bus.h"
#include "onsel.h"
#include "main.h"

#ifndef __GNUC__
#define inline
#endif

const char rcsid_clock_c[] = "$Id: clock.c,v 1.5 2001/01/27 01:43:16 dholland Exp $";

struct timed_action {
	u_int32_t ta_when_secs;
	u_int32_t ta_when_nsecs;
	void *ta_data;
	u_int32_t ta_code;
	void (*ta_func)(void *, u_int32_t);
};

static u_int32_t now_secs;
static u_int32_t now_nsecs;

/**************************************************************/

/* up to 16 simultaneous timed actions per device */
#define MAXACTIONS 1024
static struct timed_action action_storage[MAXACTIONS];
static int action_alloc_stack[MAXACTIONS];
static int action_alloc_ptr;

static
void
acalloc_init(void)
{
	int i;
	for (i=0; i<MAXACTIONS; i++) {
		action_alloc_stack[i] = i;
	}
	action_alloc_ptr = MAXACTIONS;
}

static
struct timed_action *acalloc(void)
{
	int num;
	if (action_alloc_ptr==0) {
		smoke("Too many pending hardware interrupts");
	}
	num = action_alloc_stack[--action_alloc_ptr];
	return &action_storage[num];
}

static
void acfree(struct timed_action *ta)
{
	int ix = ta-action_storage;
	if (ix<0 || ix>=MAXACTIONS || action_alloc_ptr>=MAXACTIONS) {
		smoke("Invalid argument to acfree");
	}
	action_alloc_stack[action_alloc_ptr++] = ix;
}

/*************************************************************/

static struct timed_action *queue[MAXACTIONS];
static int nqueue;

static
int
sortfunc(const void *aa, const void *bb)
{
	const struct timed_action *a, *b;
	a = *(const struct timed_action *const *)aa;
	b = *(const struct timed_action *const *)bb;
	if (a->ta_when_secs > b->ta_when_secs) return 1;
	if (a->ta_when_secs < b->ta_when_secs) return -1;
	if (a->ta_when_nsecs > b->ta_when_nsecs) return 1;
	if (a->ta_when_nsecs < b->ta_when_nsecs) return -1;
	return 0;
}

static 
void
sort_queue(void)
{
	qsort(queue, nqueue, sizeof(struct timed_action *), sortfunc);
}

static
void
check_queue(void)
{
	while (nqueue > 0) {
		struct timed_action *ta = queue[0];
		if (ta->ta_when_secs > now_secs ||
		    (ta->ta_when_secs == now_secs && 
		     ta->ta_when_nsecs > now_nsecs)) {
			return;
		}
		
		ta->ta_func(ta->ta_data, ta->ta_code);
		acfree(ta);
		nqueue--;
		memmove(&queue[0], &queue[1], 
			nqueue*sizeof(struct timed_action *));
	}
}

static
void
reschedule_queue(int32_t dsecs, int32_t dnsecs)
{
	int i;
	for (i=0; i<nqueue; i++) {
		queue[i]->ta_when_secs += dsecs;
		queue[i]->ta_when_nsecs += dnsecs;
	}
}

void
schedule_event(u_int64_t nsecs, void *data, u_int32_t code,
	       void (*func)(void *, u_int32_t))
{
	u_int32_t when_nsecs, when_secs;
	struct timed_action *ta;

	nsecs += now_nsecs;
	when_nsecs = nsecs % 1000000000;
	when_secs = nsecs / 1000000000;
	when_secs += now_secs;

	ta = acalloc();
	ta->ta_when_secs = when_secs;
	ta->ta_when_nsecs = when_nsecs;
	ta->ta_data = data;
	ta->ta_code = code;
	ta->ta_func = func;
	queue[nqueue++] = ta;
	
	sort_queue();
}

void
clock_time(u_int32_t *secs, u_int32_t *nsecs)
{
	if (secs) *secs = now_secs;
	if (nsecs) *nsecs = now_nsecs;
}

void
clock_setsecs(u_int32_t secs)
{
	reschedule_queue(secs - now_secs, 0);
	now_secs = secs;
}

void
clock_setnsecs(u_int32_t nsecs)
{
	reschedule_queue(0, nsecs - now_nsecs);
	now_nsecs = nsecs;
}


void
clock_init(void)
{
	struct timeval tv;
	acalloc_init();
	gettimeofday(&tv, NULL);
	now_secs = tv.tv_sec;
	now_nsecs = 1000*tv.tv_usec;
}

static
inline
void
clock_advance(u_int32_t secs, u_int32_t nsecs)
{
	now_secs += secs;
	now_nsecs += nsecs;
	if (now_nsecs >= 1000000000) {
		now_nsecs -= 1000000000;
		now_secs++;
	}
	check_queue();
}

void
clock_tick(void)
{
	clock_advance(0, NSECS_PER_CLOCK);
}

static
void
report_idletime(u_int32_t secs, u_int32_t nsecs)
{
	u_int64_t idlensecs;
	static u_int32_t slop;

	idlensecs = secs * (u_int64_t)1000000000 + nsecs + slop;

	g_stats.s_icycles += idlensecs / NSECS_PER_CLOCK;
	slop = idlensecs % NSECS_PER_CLOCK;
}

static
void
clock_dowait(u_int32_t secs, u_int32_t nsecs)
{
	struct timeval tv;
	int32_t wsecs, wnsecs;

	clock_advance(secs, nsecs);
	report_idletime(secs, nsecs);

	/*
	 * Figure out how far ahead of real wall time we are.  If we
	 * aren't, don't sleep. If we are, sleep to synchronize, as
	 * long as it's more than 10 ms. (If it's less than that,
	 * we're not likely to return from select in anything
	 * approaching an expeditions manner. Also, on some systems,
	 * select with small timeouts does timing loops to implement
	 * usleep(), and we don't want that. The only point of
	 * sleeping at all is to be nice to other users on the system.
	 */
	gettimeofday(&tv, NULL);
	wsecs = now_secs - tv.tv_sec;
	wnsecs = now_nsecs - 1000*tv.tv_usec;
	if (wnsecs < 0) {
		wnsecs += 1000000000;
		wsecs--;
	}

	if (wsecs >= 0 && wnsecs > 10000000) {
		tryselect(1, wsecs, wnsecs);
	}
	else {
		tryselect(1, 0, 0);
	}
}

void
clock_waitirq(void)
{
	while (cpu_irq_line==0) {
		if (nqueue > 0) {
			int32_t secs = queue[0]->ta_when_secs - now_secs;
			int32_t nsecs = queue[0]->ta_when_nsecs - now_nsecs;
			if (nsecs < 0) {
				nsecs += 1000000000;
				secs--;
			}

			if (secs < 0) {
				/* ? */
				check_queue();
			}
			else {
				clock_dowait(secs, nsecs);
			}

			bus_forward_interrupts();
		}
		else {
			struct timeval tv1, tv2;

			gettimeofday(&tv1, NULL);
			tryselect(0, 0, 0);
			gettimeofday(&tv2, NULL);

			tv2.tv_sec -= tv1.tv_sec;
			if (tv2.tv_usec < tv1.tv_usec) {
				tv2.tv_usec += 1000000;
			}
			tv2.tv_usec -= tv1.tv_usec;

			clock_advance(tv2.tv_sec, tv2.tv_usec * 1000);
			report_idletime(tv2.tv_sec, tv2.tv_usec * 1000);
		}
	}
}
