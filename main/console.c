#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include "config.h"

#include "onsel.h"
#include "console.h"
#include "main.h"


const char rcsid_console_c[] = "$Id: console.c,v 1.7 2001/06/06 15:43:18 dholland Exp $";

static struct termios savetios;
static int console_up=0;

static void (*onkey)(void *data, int ch);
static void *onkeydata;

/*****************************************/

struct output {
	int at_bol;
	int doclose;
	int needcr;
	FILE *f;
};

static struct output mainout;

#ifdef USE_TRACE
static struct output traceout;
#endif

static
void
init_output(struct output *o, FILE *f, int isfile)
{
	o->at_bol = 1;
	o->doclose = isfile;
	o->needcr = !isfile;
	o->f = f;
}

#ifdef USE_TRACE
static
void
close_output(struct output *o)
{
	if (!o->at_bol) {
		if (o->needcr) {
			fprintf(o->f, "\r");
		}
		fprintf(o->f, "\n");
		o->at_bol = 1;
	}
	if (o->doclose) {
		fclose(o->f);
	}
	init_output(o, stderr, 0);
}
#endif

static
void
vmsg(struct output *o, const char *fmt, va_list ap)
{
	if (o->at_bol) {
		fprintf(o->f, "sys161: ");
	}
	vfprintf(o->f, fmt, ap);
	if (o->needcr) {
		fprintf(o->f, "\r");
	}
	fprintf(o->f, "\n");
	o->at_bol = 1;
}

static
void
vmsgl(struct output *o, const char *fmt, va_list ap)
{
	if (o->at_bol) {
		fprintf(o->f, "sys161: ");
	}
	vfprintf(o->f, fmt, ap);
	o->at_bol = 0;
}

/*****************************************/


static
int
console_getc(void)
{
	char ch;
	int r;

	r = read(STDIN_FILENO, &ch, 1);
	if (r<0) {
		smoke("Read error on stdin: %s", strerror(errno));
	}
	else if (r==0) {
		/* EOF - send back -1 and hope nothing breaks */
		return -1;
	}
	return (unsigned char)ch;
}

static
int
console_sel(void *unused)
{
	int ch;
	(void)unused;

	ch = console_getc();
	if (ch=='\a') {
		/* ^G (BEL) - interrupt */
		main_stop();
	}
	else if (onkey) {
		onkey(onkeydata, ch);
	}

	return 0;
}

void
console_onkey(void *data, void (*func)(void *, int))
{
	onkeydata = data;
	onkey = func;
}

void
console_putc(int c)
{
	char ch = c;
	int r;

	r = write(STDOUT_FILENO, &ch, 1);
	if (r<=0) {
		if (r<0) {
			msg("stdout: %s", strerror(errno));
		}
		else {
			msg("stdout: zero-length write");
		}
		console_cleanup();
		exit(1);
	}
}

void
console_beep(void)
{
	console_putc('\a');
}

/*****************************************/

void
console_earlyinit(void)
{
	init_output(&mainout, stderr, 0);
#ifdef USE_TRACE
	init_output(&traceout, stderr, 0);
#endif
}

void
console_init(int pass_signals)
{
	struct termios t;

	if (console_up) {
		return;
	}

	if (tcgetattr(STDIN_FILENO, &savetios)) {
		if (errno!=ENOTTY) {
			fprintf(stderr, "stdin: %s\n", strerror(errno));
			exit(1);
		}
		return;
	}

	console_up = 1;

	t = savetios;

#ifdef XCASE
	t.c_lflag &= ~(XCASE); 
#endif
	t.c_lflag &= ~(ECHONL|NOFLSH); 
	t.c_lflag &= ~(ICANON | ECHO); 
	if (pass_signals) {
		t.c_lflag &= ~ISIG;
	}
	else {
		t.c_lflag |= ISIG;
	}
	t.c_iflag &= ~(ICRNL | INLCR);
	t.c_cflag |= CREAD;
	t.c_cc[VTIME] = 0;
	t.c_cc[VMIN] = 0;

	tcsetattr(STDIN_FILENO, TCSADRAIN, &t);

	onselect(STDIN_FILENO, NULL, console_sel, NULL);
}

void
console_cleanup(void)
{
	if (console_up) {
		console_up = 0;
		fflush(mainout.f);
#ifdef USE_TRACE
		fflush(traceout.f);
		close_output(&traceout);
#endif
		tcsetattr(STDIN_FILENO, TCSADRAIN, &savetios);
	}
}

/*****************************************/

void
die(void)
{
	console_cleanup();
	exit(1);
}

void
msg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vmsg(&mainout, fmt, ap);
	va_end(ap);
}

void
msgl(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vmsgl(&mainout, fmt, ap);
	va_end(ap);
}

#ifdef USE_TRACE

void
set_tracefile(const char *filename)
{
	FILE *f;

	close_output(&traceout);

	if (filename && !strcmp(filename, "-")) {
		init_output(&traceout, stdout, 0);
	}
	else if (filename) {
		f = fopen(filename, "w");
		if (!f) {
			msg("Cannot open tracefile %s", filename);
			die();
		}
		init_output(&traceout, f, 1);
	}
	else {
		init_output(&traceout, stderr, 0);
	}
}

void
trace(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vmsg(&traceout, fmt, ap);
	va_end(ap);
}

void
tracel(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vmsgl(&traceout, fmt, ap);
	va_end(ap);
}

#endif

void
smoke(const char *fmt, ...)
{
	va_list ap;
	
	va_start(ap, fmt);
	vmsg(&mainout, fmt, ap);
	va_end(ap);
	
	msg("The hardware has failed.");
	msg("In real life this is where the smoke starts pouring out.");
	
	console_cleanup();
	abort();
}

void
hang(const char *fmt, ...)  // was crash()
{
	va_list ap;
	
	va_start(ap, fmt);
	vmsg(&mainout, fmt, ap);
	va_end(ap);
	
	msg("You did something the hardware didn't like.");
	msg("In real life the machine would hang for no apparent reason,");
	msg("or maybe start to act strangely.");

	// wait for debugger connection
	main_stop();
	
	//console_cleanup();
	//exit(1);
}

void
console_pause(void)
{
	if (isatty(1)) {
		fprintf(stderr, "sys161: PAUSE");
		fflush(stderr);
		console_getc();
		fprintf(stderr, "\r\n");
	}
}
