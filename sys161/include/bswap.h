/*
 * Byte swapping.
 */

#ifdef ENDIAN_HEADER
#include ENDIAN_HEADER
#endif

#ifdef USE_BSD_BSWAP
/* already included ENDIAN_HEADER, which defines bswap16/32/64 for us */
#endif

#ifdef USE_LINUX_BSWAP
#include <byteswap.h>
#define bswap16 bswap_16
#define bswap32 bswap_32
#define bswap64 bswap_64
#endif

#ifdef USE_OWN_BSWAP

static
inline
uint16_t
bswap16(uint16_t x)
{
	return ((x & 0xff) << 8) | (x >> 8);
}

static
inline
uint32_t
bswap32(uint32_t x)
{
	return (bswap16(x & 0xffff) << 16) | bswap16(x >> 16);
}

static
inline
uint64_t
bswap64(uint64_t x)
{
	return (bswap32(x & 0xffffffff) << 32) | bswap32(x >> 32);
}

#endif /* USE_OWN_BSWAP */

/*
 * Provide these so that the code has the same integer sign and
 * truncation behavior regardless of the endianness situation.
 */

static
inline
uint16_t
nop16(uint16_t x)
{
	return x;
}

static
inline
uint32_t
nop32(uint32_t x)
{
	return x;
}

static
inline
uint64_t
nop64(uint64_t x)
{
	return x;
}

/*
 * Since in the near to medium term we want to be able to compile in
 * more than one CPU type (same as we now compile in the fast and
 * trace versions of the same CPU) it would be better not to have
 * cpu-endian.h or have htoc* and ctoh* appear anywhere but in CPU
 * code. However, that requires reorganizing the way bus accesses are
 * done (they are currently in host endianness, which is good for
 * registers but bad for memory/transfer buffers) which is something I
 * don't want to get into right now. Soon(TM).
 */

#include "cpu-endian.h"

#if HOST_ENDIAN == CPU_ENDIAN
#define htoc16(x) nop16(x)
#define htoc32(x) nop32(x)
#define htoc64(x) nop64(x)
#define ctoh16(x) nop16(x)
#define ctoh32(x) nop32(x)
#define ctoh64(x) nop64(x)
#else
#define htoc16(x) bswap16(x)
#define htoc32(x) bswap32(x)
#define htoc64(x) bswap64(x)
#define ctoh16(x) bswap16(x)
#define ctoh32(x) bswap32(x)
#define ctoh64(x) bswap64(x)
#endif
