#include <sys/types.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "config.h"

#include "console.h"
#include "gdb.h"
#include "cpu.h"
#include "bus.h"
#include "main.h"

#include "context.h"


const char rcsid_gdb_be_c[] = "$Id: gdb_be.c,v 1.18 2001/02/02 03:25:10 dholland Exp $";

// XXX
#define debug printf

extern struct gdbcontext g_ctx;

static void debug_notsupp(struct gdbcontext *);
static void debug_send(struct gdbcontext *, const char *);
static unsigned hex_int_decode(const char *, unsigned int nybs);
static void debug_register_print(struct gdbcontext *);
static void debug_write_mem(struct gdbcontext *ctx, const char *spec);
static void debug_read_mem(struct gdbcontext *ctx, const char *spec);
static void debug_restart(struct gdbcontext *ctx, const char *addr);

void
unset_breakcond(void)
{
	//inbreakpoint = 0;
	main_continue();
}

void
set_breakcond(void)
{
	//inbreakpoint = 1;
}

/* pkt is null-terminated */
void 
debug_exec(struct gdbcontext *ctx, const char *pkt) 
{
	char *cs;  /* start of the checksum */
	int i;
	int check = 0, scheck;

	debug("Got packet %s\n", pkt);

	if (pkt[0] != '$') {
		return;
	}

	cs = strchr(pkt, '#');
	if (cs == NULL) {
		return;
	}
	*cs = 0;
	cs++;

	for (i=1; pkt[i]; i++) {
		check += pkt[i];
	}

	scheck = strtoul(cs, NULL, 16);
	if (scheck != check % 256) {
		write(ctx->myfd, "-", 1);
	} else {
		write(ctx->myfd, "+", 1);
	}

	switch(pkt[1]) {
	    case 'H':
		debug_send(ctx, "OK");
		set_breakcond();
		break;
	    case 'D':
	    case 'k':
		debug_send(ctx, "OK");
		unset_breakcond();
		break;
	    case 'q':
		if(strcmp(pkt+2, "Offsets") == 0) {
			debug_notsupp(ctx);
		} else if(strcmp(pkt + 2, "C") == 0) {
			debug_send(ctx,"C=000");
		}
		break;
	    case 'Z':
	    case 'z':
		debug_send(ctx, "E01");
		break;
	    case 'X':
		debug_notsupp(ctx);
		break;
	    case '?':
		debug_send(ctx, "S05");
		break;
	    case 'g':
		debug_register_print(ctx);
		break;
	    case 'm':
		debug_read_mem(ctx, pkt + 2);
		break;
	    case 'M':
		debug_write_mem(ctx, pkt + 2);
		break;
	    case 'c':
		unset_breakcond();
		debug_restart(ctx, pkt + 2);
		break;
	    case 's':
		debug_restart(ctx, pkt + 2);
		onecycle();
		debug_send(ctx, "S05");
		break;
	}
}

static
void 
debug_send(struct gdbcontext *ctx, const char *string) 
{
	char checkstr[8];
	int check=0;
	int i;

	for (i=0; string[i]; i++) {
		check += string[i];
	}

	check %= 256;
	snprintf(checkstr, sizeof(checkstr),  "#%02x", check);

	debug("Sending $%s%s\n", string, checkstr);

	write(ctx->myfd, "$", 1);
	write(ctx->myfd, string, strlen(string)); 
	write(ctx->myfd, checkstr, strlen(checkstr));
}

static
void
debug_notsupp(struct gdbcontext *ctx)
{
	const char rep[] = "$\0#00";
	write(ctx->myfd, rep, sizeof(rep) - 1);
}

void
gdb_startbreak(void)
{
	struct gdbcontext *ctx = &g_ctx;
	debug_send(ctx, "S05");
	set_breakcond();
}

static
void
printval(char *buf, size_t maxlen, u_int32_t val)
{
	size_t len = strlen(buf);
	Assert(len < maxlen);

	snprintf(buf+len, maxlen - len, "%08x", val);
}

static
void
debug_register_print(struct gdbcontext *ctx)
{
	u_int32_t regs[256];
	int i, nregs;
	char buf[BUFLEN];

	cpudebug_getregs(regs, 256, &nregs);
	Assert(nregs <= 256);

	buf[0] = 0;
	for (i=0; i<nregs; i++) {
		printval(buf, sizeof(buf), regs[i]);
	}

	debug_send(ctx, buf);
}

static
void
debug_read_mem(struct gdbcontext *ctx, const char *spec)
{
	long start;
	long length;
	char *curptr;
	char buf[BUFLEN];
	unsigned int memloc;
	int i;
	unsigned int realaddr;

	start = strtoul(spec, &curptr, 16);
	length = strtoul(curptr+1, NULL, 16);

	if (start % 4 != 0) {
		debug_send(ctx, "E04");
		return;
	}

	buf[0] = 0;
	for (i = 0; i < length; i+=4) {
		if (cpudebug_translate_address(start + i, 4, &realaddr)) {
			debug_send(ctx, "E03");
			return;
		}
		bus_mem_fetch(realaddr, &memloc);
		printval(buf, sizeof(buf), memloc);
	}
	debug_send(ctx, buf);
}

static
void
debug_write_mem(struct gdbcontext *ctx, const char *spec)
{
	unsigned int start, length;
	int i;
	u_int32_t realaddr;
	unsigned int val;

	char *curptr;
	// AAAAAAA,LLL:XXXX
	// address,len,data
	start = strtoul(spec, &curptr, 16);
	length = strtoul(curptr + 1, &curptr, 16);

	if (start % 4 != 0) {
		debug_send(ctx, "E04");
		return;
	}

	// curptr now points to the ':' which 
	// delimits the length from the data
	// so we advance it a little
	curptr++;

	for (i = 0; i < length; i+=4) {
		if (cpudebug_translate_address(start + i, 4, &realaddr)) {
			debug_send(ctx, "E03");
			return;
		}
		val = hex_int_decode(curptr + 2*i, 8);
		if (bus_mem_store(realaddr, val) != 0) {
			debug_send(ctx, "E02");
			return;
		}
	}
	debug_send(ctx, "OK");
}

static
unsigned
hex_int_decode(const char *in, unsigned int nybs)
{
	char buf[BUFLEN];

	if(nybs >= BUFLEN) {
		return 0;
	}
	
	memcpy(buf, in, nybs);
	buf[nybs] = '\0';
	return(strtoul(buf, NULL, 16));
}

static
void
debug_restart(struct gdbcontext *ctx, const char *addr)
{
	unsigned int realaddr;

	if(*addr == '\0') {
		return;
	}
	printf("whee!  changing address\n");
	realaddr = strtoul(addr, NULL, 16);
	cpu_set_entrypoint(realaddr);
}

