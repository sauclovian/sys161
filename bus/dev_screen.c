#include <sys/types.h>
#include <string.h>
#include "config.h"

#include "console.h"

#include "busids.h"
#include "lamebus.h"

const char rcsid_dev_screen_c[] = 
    "$Id: dev_screen.c,v 1.5 2001/02/12 23:05:49 dholland Exp $";

static
void *screen_init(int slot, int argc, char *argv[])
{
	msg("Screen device not supported");
	die();

	(void)slot;
	(void)argc;
	(void)argv;
	return NULL;
}

const struct lamebus_device_info screen_device_info = {
	LBVEND_CS161,
	LBVEND_CS161_SCREEN,
	SCREEN_REVISION,
	screen_init,
	NULL,  /* fetch */
	NULL,  /* store */
	NULL   /* cleanup */
};

