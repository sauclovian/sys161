#include <sys/types.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "speed.h"
#include "clock.h"


const char rcsid_clock_c[] = "$Id: clock.c,v 1.3 2001/01/25 04:49:47 dholland Exp $";

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

void
clock_tick(void)
{
	now_nsecs += NSECS_PER_CLOCK;
	if (now_nsecs > 1000000000) {
		now_nsecs -= 1000000000;
		now_secs++;
	}
	check_queue();
}
