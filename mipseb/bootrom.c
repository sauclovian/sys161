#include <sys/types.h>
#include "bootrom.h"


const char rcsid_bootrom_c[] = "$Id: bootrom.c,v 1.2 2001/01/25 04:49:47 dholland Exp $";

int bootrom_fetch(u_int32_t offset, u_int32_t *val) {
   /* No ROM, hopefully */
   return -1;
}
