#include <sys/types.h>
#include "config.h"
#include "bootrom.h"


const char rcsid_bootrom_c[] = "$Id: bootrom.c,v 1.3 2001/01/27 01:43:16 dholland Exp $";

int bootrom_fetch(u_int32_t offset, u_int32_t *val) {
   /* No ROM, hopefully */
   return -1;
}
