#include <sys/types.h>
#include <string.h>
#include "config.h"
#include "bswap.h"
#include "bootrom.h"

/*
 * The purpose of setting this up the way it is is that no matter what
 * entry to the ROM is taken, it'll eventually hit the breakpoint. The
 * breakpoint will trigger the debugger support. So if you set off an
 * exception before setting up your own trap handler, it summons a
 * debugger for you.
 *
 * Note that if anyone wanted to this could be replaced by real firmware,
 * whether or not accompanied by an implementation of machine-mode in the
 * CPU, but there is little point. (Loading of kernel images is done in
 * host code.)
 */

/*
 * NOP and EBREAK (breakpoint) instructions.
 *
 * Note that these need to be encoded as target-endian words, not
 * host-endian. They are flipped on the memory access path, and it
 * needs to be that way because parts of the path are shared with
 * access to main memory and device memory, and at least device memory
 * needs to be target-endian so I/O works.
 */

#if 0 /* doesn't work because htoc32() isn't a constant */
#define NOP 	htoc32(0x00000010)
#define EBREAK	htoc32(0x00100070)
#else

#if CPU_ENDIAN == LITTLE_ENDIAN
#define NOP 	0x00000013
#define EBREAK	0x00100073
#else
#define NOP 	0x13000000
#define EBREAK	0x73001000
#endif

#endif /* 0 */


#define ROMWORDS 1024 /* one page */

static const uint32_t fakerom[ROMWORDS] = {
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x000 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x010 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x020 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x030 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x040 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x050 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x060 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x070 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x080 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x090 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x0a0 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x0b0 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x0c0 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x0d0 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x0e0 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x0f0 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x100 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x110 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x120 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x130 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x140 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x150 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x160 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x170 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x180 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x190 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x1a0 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x1b0 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x1c0 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x1d0 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x1e0 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x1f0 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x200 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x210 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x220 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x230 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x240 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x250 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x260 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x270 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x280 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x290 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x2a0 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x2b0 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x2c0 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x2d0 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x2e0 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x2f0 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x300 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x310 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x320 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x330 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x340 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x350 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x360 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x370 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x380 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x390 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x3a0 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x3b0 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x3c0 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x3d0 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x3e0 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP,
	NOP, NOP, NOP, NOP, NOP, NOP, NOP, NOP, /* 0x3f0 */
	NOP, NOP, NOP, NOP, NOP, NOP, NOP,	
	EBREAK					/* 0x3ff */
};

int
bootrom_fetch(uint32_t offset, uint32_t *val)
{
	if (offset >= ROMWORDS * sizeof(uint32_t)) {
		return -1;
	}

	/*
	 * Because the pointers returned by bootrom_map are
	 * accessed with bus_use_map(), the rom must be stored
	 * as target endianness.
	 */

	*val = ctoh32(fakerom[offset/sizeof(uint32_t)]);

	return 0;
}

const uint32_t *
bootrom_map(uint32_t offset)
{
	if (offset >= ROMWORDS * sizeof(uint32_t)) {
		return NULL;
	}

	return &fakerom[offset/sizeof(uint32_t)];
}
