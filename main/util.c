#include <stdlib.h>

#include "console.h"
#include "util.h"


const char rcsid_util_c[] = "$Id: util.c,v 1.2 2001/01/25 04:49:47 dholland Exp $";

void *
domalloc(size_t len)
{
	void *x = malloc(len);
	if (!x) {
		smoke("Out of memory");
	}
	return x;
}
