#include <sys/types.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

#include "console.h"

#include "lamebus.h"
#include "busids.h"


const char rcsid_dev_random_c[] = "$Id: dev_random.c,v 1.6 2001/01/27 01:43:15 dholland Exp $";

static
void *
rand_init(int slot, int argc, char *argv[])
{
	u_int32_t seed = 0;
	int i;

	for (i=1; i<argc; i++) {
		if (!strncmp(argv[i], "seed=", 5)) {
			seed = atol(argv[i]+5);
		}
		else if (!strcmp(argv[i], "autoseed")) {
			struct timeval tv;
			gettimeofday(&tv, NULL);
			seed = tv.tv_sec ^ (tv.tv_usec << 8);
		}
		else {
			msg("random: slot %d: invalid option %s",
			    slot, argv[i]);
			die();
		}
	}

	srandom(seed);

	return NULL;
}

static
int
rand_fetch(void *data, u_int32_t offset, u_int32_t *ret)
{
	(void)data;  // not used

	if (offset==0) {
		*ret = random();
		return 0;
	}
	return -1;
}

static
int
rand_store(void *data, u_int32_t offset, u_int32_t val)
{
	(void)data;
	(void)offset;
	(void)val;
	return -1;
}

static
void
rand_cleanup(void *data)
{
	(void)data;
}

const struct lamebus_device_info random_device_info = {
	LBVEND_CS161,
	LBVEND_CS161_RANDOM,
	RANDOM_REVISION,
	rand_init,
	rand_fetch,
	rand_store,
	rand_cleanup,
};
