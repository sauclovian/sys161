#include <sys/types.h>
#include <stdint.h>
#include <string.h>
#include "config.h"

#include "main.h"
#include "cpu.h"
#include "prof.h"

#include "lamebus.h"
#include "busids.h"


#define TRACEREG_ON     0
#define TRACEREG_OFF    4
#define TRACEREG_PRINT  8
#define TRACEREG_DUMP   12
#define TRACEREG_STOP	16
#define TRACEREG_PROFEN	20
#define TRACEREG_PROFCL	24


static
void *
trace_init(int slot, int argc, char *argv[])
{
	(void)argv;

	if (argc != 1) {
		msg("trace: slot %d: Invalid argument %s", slot, argv[1]);
		die();
	}

	return NULL;
}

static
int
trace_fetch(unsigned cpunum, void *data, uint32_t offset, uint32_t *ret)
{
	(void)cpunum;
	(void)data;

	switch (offset) {
	    case TRACEREG_PROFEN:
		*ret = prof_isenabled() ? 1 : 0;
		break;
	    default:
		return -1;
	}

	return 0;
}

static
int
trace_store(unsigned cpunum, void *data, uint32_t offset, uint32_t val)
{
	(void)cpunum;
	(void)data;

	switch (offset) {
	    case TRACEREG_ON:
		if (adjust_traceflag(val, 1)) {
			hang("Invalid trace code %c (%d)", val, val);
		}
		break;
	    case TRACEREG_OFF:
		if (adjust_traceflag(val, 0)) {
			hang("Invalid trace code %c (%d)", val, val);
		}
		break;
	    case TRACEREG_PRINT:
		msg("trace: code %lu (0x%lx)", 
		    (unsigned long)val, (unsigned long)val);
		break;
	    case TRACEREG_DUMP:
		msg("----------------------------------------"
		    "--------------------------------");
		msg("trace: dump with code %lu (0x%lx)",
		    (unsigned long)val, (unsigned long)val);

		main_dumpstate();

		msg("trace: dump complete");
		msg("----------------------------------------"
		    "--------------------------------");
		break;
	    case TRACEREG_STOP:
		msg("trace: software-requested debugger stop");
		main_note_debugrequest();
		cpu_stopcycling();
		main_enter_debugger(0 /* not lethal */);
		break;
	    case TRACEREG_PROFEN:
		if (val) {
			prof_enable();
		}
		else {
			prof_disable();
		}
		break;
	    case TRACEREG_PROFCL:
		prof_clear();
		break;
	    default:
		return -1;
	}
	return 0;
}

static
void
trace_dumpstate(void *data)
{
	(void)data;
	msg("System/161 trace control device rev %d", TRACE_REVISION);
}

static
void
trace_cleanup(void *data)
{
	(void)data;
}

const struct lamebus_device_info trace_device_info = {
	LBVEND_SYS161,
	LBVEND_SYS161_TRACE,
	TRACE_REVISION,
	trace_init,
	trace_fetch,
	trace_store,
	trace_dumpstate,
	trace_cleanup,
};
