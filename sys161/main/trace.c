#include <sys/types.h>
#include <stdarg.h>
#include <string.h>
#include "config.h"

#include "console.h"
#include "trace.h"
#include "cpu.h"


static const struct {
	int ch;
	int flag;
	const char *name;
	const char *desc;
} flaginfo[] = {
	/* Note: not necessarily in same order as DOTRACE flags */
	{ 'k', DOTRACE_KINSN, "kinsn", "Kernel-mode instructions" },
	{ 'u', DOTRACE_UINSN, "uinsn", "User-mode instructions" },
	{ 'j', DOTRACE_JUMP,  "jump",  "Jumps" },
	{ 't', DOTRACE_TLB,   "tlb",   "TLB operations" },
	{ 'x', DOTRACE_EXN,   "exn",   "Exceptions" },
	{ 'i', DOTRACE_IRQ,   "irq",   "Interrupts" },
	{ 'd', DOTRACE_DISK,  "disk",  "Disk activity" },
	{ 'n', DOTRACE_NET,   "net",   "Network activity" },
	{ 'e', DOTRACE_EMUFS, "emufs", "Emufs activity" },
	{ -1, -1, NULL, NULL }
};

int g_traceflags[NDOTRACES];

static
void
update_cpu_tracing(void)
{
	int on;

	on = g_traceflags[DOTRACE_KINSN] ||
		g_traceflags[DOTRACE_UINSN] ||
		g_traceflags[DOTRACE_JUMP] ||
		g_traceflags[DOTRACE_TLB] ||
		g_traceflags[DOTRACE_EXN] ||
		g_traceflags[DOTRACE_IRQ];
	cpu_set_tracing(on);
}

static
int
set_traceflag(int ch)
{
	int j, f;

	for (j=0; flaginfo[j].ch >= 0; j++) {
		if (flaginfo[j].ch == ch) {
			f = flaginfo[j].flag;
			g_traceflags[f] = !g_traceflags[f];
			update_cpu_tracing();
			return 0;
		}
	}
	return -1;
}

int
adjust_traceflag(int letter, int onoff)
{
	int j, f;
	for (j=0; flaginfo[j].ch >= 0; j++) {
		if (flaginfo[j].ch == letter) {
			f = flaginfo[j].flag;
			g_traceflags[f] = onoff;
			update_cpu_tracing();
			return 0;
		}
	}
	return -1;
}

void
set_traceflags(const char *letters)
{
	int i;

	for (i=0; letters[i]; i++) {
		if (set_traceflag(letters[i])) {
			msg("Unknown trace flag %c", letters[i]);
			die();
		}
	}
}

void
print_traceflags(void)
{
	int i, k=0;
	for (i=0; i<NDOTRACES; i++) {
		if (g_traceflags[i]) k++;
	}

	if (k==0) {
		return;
	}

	msgl("Tracing enabled:");
	for (i=0; i<NDOTRACES; i++) {
		if (g_traceflags[i]) {
			msgl(" %s", flaginfo[i].name);
		}
	}
	msg(" ");
}

void
print_traceflags_usage(void)
{
	int i;
	size_t len;

	for (i=0; i<NDOTRACES; i++) {
		msgl("        %c %s", flaginfo[i].ch, flaginfo[i].name);
		len = strlen(flaginfo[i].name);
		if (len < 12) {
			msgl("%.*s", (int)(12-len), "         ");
		}
		msg(" %s", flaginfo[i].desc);
	}
}
