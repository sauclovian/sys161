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


const char rcsid_console_c[] = "$Id: console.c,v 1.5 2001/02/26 18:41:45 dholland Exp $";

static struct termios savetios;
static int console_up=0;

static void (*onkey)(void *data, int ch);
static void *onkeydata;

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
console_init(void)
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
	t.c_lflag |= ISIG;
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
		tcsetattr(STDIN_FILENO, TCSADRAIN, &savetios);
	}
}

/*****************************************/

static int at_bol = 1;

void
die(void)
{
	console_cleanup();
	exit(1);
}

static
void
vmsg(const char *fmt, va_list ap)
{
	if (at_bol) {
		fprintf(stderr, "sys161: ");
	}
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\r\n");
	at_bol = 1;
}

static
void
vmsgl(const char *fmt, va_list ap)
{
	if (at_bol) {
		fprintf(stderr, "sys161: ");
	}
	vfprintf(stderr, fmt, ap);
	at_bol = 0;
}

void
msg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vmsg(fmt, ap);
	va_end(ap);
}

void
msgl(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vmsgl(fmt, ap);
	va_end(ap);
}

void
smoke(const char *fmt, ...)
{
	va_list ap;
	
	va_start(ap, fmt);
	vmsg(fmt, ap);
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
	vmsg(fmt, ap);
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
