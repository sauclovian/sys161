#include <sys/types.h>

#include "util.h"
#include "speed.h"
#include "console.h"
#include "clock.h"

#include "busids.h"
#include "lamebus.h"



const char rcsid_dev_serial_c[] = "$Id: dev_serial.c,v 1.5 2001/01/25 04:49:46 dholland Exp $";

#define SERREG_CHAR   0x0
#define SERREG_WIRQ   0x4
#define SERREG_RIRQ   0x8

#define SCNREG_POSN   0x0
#define SCNREG_SIZE   0x4
#define SCNREG_CHAR   0x8
#define SCNREG_RIRQ   0xc

#define IRQF_ON    0x1
#define IRQF_READY 0x2

//////////////////////////////////////////////////

struct serirq {
	int si_on;
	int si_ready;
};

struct ser_data {
	int sd_slot;
	u_int32_t sd_readch;
	int sd_wbusy;
	struct serirq sd_rirq;
	struct serirq sd_wirq;
};

static
u_int32_t
fetchirq(struct serirq *si)
{
	u_int32_t val = 0;
	if (si->si_on) val |= IRQF_ON;
	if (si->si_ready) val |= IRQF_READY;
	return val;
}

static
void
storeirq(struct serirq *si, u_int32_t val)
{
	si->si_on = (val & IRQF_ON)!=0;
	si->si_ready = (val & IRQF_READY)!=0;
}

static
void
setirq(struct ser_data *sd)
{
	int rirq = sd->sd_rirq.si_on && sd->sd_rirq.si_ready;
	int wirq = sd->sd_wirq.si_on && sd->sd_wirq.si_ready;
	if (rirq || wirq) {
		RAISE_IRQ(sd->sd_slot);
	}
	else {
		LOWER_IRQ(sd->sd_slot);
	}
}

static
void
serial_writedone(void *d, u_int32_t gen)
{
	struct ser_data *sd = d;
	(void)gen;

	sd->sd_wbusy = 0;
	sd->sd_wirq.si_ready = 1;
	setirq(sd);
}

static
void
serial_input(void *d, int ch)
{
	struct ser_data *sd = d;
	sd->sd_readch = ch;
	sd->sd_rirq.si_ready = 1;
	setirq(sd);
}

static
int
serial_fetch(void *d, u_int32_t offset, u_int32_t *val)
{
	struct ser_data *sd = d;
	switch (offset) {
	    case SERREG_CHAR: *val = sd->sd_readch; return 0;
	    case SERREG_RIRQ: *val = fetchirq(&sd->sd_rirq); return 0;
	    case SERREG_WIRQ: *val = fetchirq(&sd->sd_wirq); return 0;
	}
	return -1;
}

static
int
serial_store(void *d, u_int32_t offset, u_int32_t val)
{
	struct ser_data *sd = d;
	switch (offset) {
	    case SERREG_CHAR: 
		    if (!sd->sd_wbusy) {
			    sd->sd_wbusy = 1;
			    console_putc(val);
			    schedule_event(SERIAL_NSECS, sd, 0, 
					   serial_writedone);
		    }
		    return 0;
	    case SERREG_RIRQ: 
		    storeirq(&sd->sd_rirq, val);
		    setirq(sd);
		    return 0;
	    case SERREG_WIRQ:
		    storeirq(&sd->sd_wirq, val);
		    setirq(sd);
		    return 0;
	}
	return -1;
}

static
void *
serial_init(int slot, int argc, char *argv[])
{
	struct ser_data *sd = domalloc(sizeof(struct ser_data));
	sd->sd_slot = slot;
	sd->sd_readch = 0;
	sd->sd_wbusy = 0;
	sd->sd_rirq.si_on = 0;
	sd->sd_rirq.si_ready = 0;
	sd->sd_wirq.si_on = 0;
	sd->sd_wirq.si_ready = 0;

	console_onkey(sd, serial_input);

	return sd;
}

const struct lamebus_device_info serial_device_info = {
	LBVEND_CS161,
	LBVEND_CS161_SERIAL,
	SERIAL_REVISION,
	serial_init,
	serial_fetch,
	serial_store,
};
