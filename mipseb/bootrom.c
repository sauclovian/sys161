#include <sys/types.h>
#include "config.h"
#include "bootrom.h"


const char rcsid_bootrom_c[] = "$Id: bootrom.c,v 1.4 2001/06/04 21:41:49 dholland Exp $";

int
bootrom_fetch(u_int32_t offset, u_int32_t *val)
{
	/* No ROM, hopefully */
	(void)offset;
	(void)val;

	return -1;
}
