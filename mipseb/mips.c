#include <sys/types.h>
#include <string.h>
#include "config.h"
 
#include "cpu.h"
#include "bus.h"
#include "console.h"
#include "clock.h"
#include "gdb.h"
#include "main.h"
#include "trace.h"

#include "mips-insn.h"
#include "mips-ex.h"
#include "bootrom.h"

#ifndef __GNUC__
#define inline
#endif

const char rcsid_mips_c[] =
	"$Id: mips.c,v 1.43 2001/06/07 20:02:21 dholland Exp $";


#ifndef QUAD_HIGHWORD
#error "Need QUAD_HIGHWORD and QUAD_LOWWORD"
// (1/0 respectively on a big-endian system, reverse on a little-endian one)
#endif

#define NTLB  64

#define TLB_GLOBAL(tlb)             (((tlb)&0x0000000000000100ULL)!=0)
#define TLB_VALID(tlb)              (((tlb)&0x0000000000000200ULL)!=0)
#define TLB_DIRTY(tlb)              (((tlb)&0x0000000000000400ULL)!=0)
#define TLB_NOCACHE(tlb)            (((tlb)&0x0000000000000800ULL)!=0)
#define TLB_PFN(tlb)     ((u_int32_t)((tlb)&0x00000000fffff000ULL))
#define TLB_VPN(tlb)    ((u_int32_t)(((tlb)&0xfffff00000000000ULL)>>32))
#define TLB_PID(tlb)    ((u_int32_t)(((tlb)&0x00000fc000000000ULL)>>38))

// Value to initialize TLB entries to at boot time.
// (0 would be virtual page 0, which might cause unwanted matches.
// So use a virtual page in one of the non-mapped segments.)
#define JUNK_TLBENTRY           (0x8100000000000000ULL)


// MIPS hardwired memory segments
#define KSEG2	0xc0000000
#define KSEG1	0xa0000000
#define KSEG0	0x80000000
#define KUSEG	0x00000000


/* number of general registers */
#define NREGS 32

struct mipscpu {
	// general registers
	int32_t r[NREGS];

	// special registers
	int32_t lo, hi;

	// pipeline stall logic
	int lowait, hiwait; // cycles to wait for lo/hi to become ready

	// "jumping" is set by the jump instruction.
	// "in_jumpdelay" is set during decoding of the instruction in a jump 
	// delay slot.
	int jumping;
	int in_jumpdelay;

	// pc/exception stuff
	//
	// at instruction decode time, pc points to the delay slot and
	// nextpc points to the instruction after. thus, a branch
	// instruction alters nextpc. expc points to the instruction
	// being executed (unless it's in the delay slot of a jump).

	u_int32_t expc;         // pc for exception (not incr. while jumping)
	
	u_int32_t pc;           // pc
	u_int32_t nextpc;       // succeeding pc

	// mmu
	u_int64_t tlb[NTLB];
	u_int64_t tlbentry;	// cop0 register 2 (lo) and 10 (hi)

	u_int32_t tlbindex;	// cop0 register 0
	u_int32_t tlbrandom;	// cop0 register 1

	// exception stuff
	u_int32_t ex_status;	// cop0 register 12
	u_int32_t ex_cause;	// cop0 register 13
	u_int32_t ex_context;	// cop0 register 4
	u_int32_t ex_epc;	// cop0 register 14
	u_int32_t ex_vaddr;	// cop0 register 8
	u_int32_t ex_prid;	// cop0 register 15
};

#define IS_USERMODE(cpu) (((cpu)->ex_status & 0x2)!=0)

/*************************************************************/

#ifdef USE_TRACE
static const char *exception_names[13] = {
	"interrupt",
	"TLB modify",
	"TLB miss - load",
	"TLB miss - store",
	"Address error - load",
	"Address error - store",
	"Bus error - code",
	"Bus error - data",
	"System call",
	"Breakpoint",
	"Illegal instruction",
	"Coprocessor unusable",
	"Arithmetic overflow",
};

static
const char *
exception_name(int code)
{
	if (code >= 0 && code < 13) {
		return exception_names[code];
	}
	smoke("Name of invalid exception code requested");
	return "???";
}
#endif /* USE_TRACE */

/*************************************************************/

static
void
mips_init(struct mipscpu *cpu)
{
	int i;
	for (i=0; i<NREGS; i++) {
		cpu->r[i] = 0;
	}
	cpu->lo = cpu->hi = 0;
	cpu->lowait = cpu->hiwait = 0;

	cpu->jumping = cpu->in_jumpdelay = 0;

	cpu->expc = 0;
	cpu->pc = 0xbfc00000;
	cpu->nextpc = 0xbfc00004;

	for (i=0; i<NTLB; i++) {
		cpu->tlb[i] = JUNK_TLBENTRY;
	}
	cpu->tlbentry = 0;
	cpu->tlbindex = 0;
	cpu->tlbrandom = 63 << 8;
	cpu->ex_status = 0x00400000;
	cpu->ex_cause = 0;
	cpu->ex_context = 0;
	cpu->ex_epc = 0;
	cpu->ex_vaddr = 0;
	cpu->ex_prid = 0xbeef;  // implementation 0xbe, revision 0xef (XXX)
}

static
void
tlbmsg(struct mipscpu *cpu, int index, const char *what)
{
	if (TLB_GLOBAL(cpu->tlb[index])) {
		msg("%s: index %d, vpn 0x%08lx, global", what, index, 
		    (unsigned long)(TLB_VPN(cpu->tlb[index])));
	}
	else {
		msg("%s: index %d, vpn 0x%08lx, pid %ld", what, index,
		    (unsigned long)(TLB_VPN(cpu->tlb[index])),
		    (unsigned long)(TLB_PID(cpu->tlb[index])));
	}
}

static
void
check_tlb_dups(struct mipscpu *cpu, int newix)
{
	u_int32_t vpn, pid;
	int gbl, i;

	vpn = TLB_VPN(cpu->tlb[newix]);
	pid = TLB_PID(cpu->tlb[newix]);
	gbl = TLB_GLOBAL(cpu->tlb[newix]);

	for (i=0; i<NTLB; i++) {
		if (i == newix) {
			continue;
		}
		if (vpn != TLB_VPN(cpu->tlb[i])) {
			continue;
		}

		/*
		 * We've got two translations for the same virtual page.
		 * If both translations would ever match at once, it's bad.
		 * This is true if *either* is global or if the pids are
		 * the same. Note that it doesn't matter if the valid bits
		 * are set - translations that are not valid are still 
		 * accessed.
		 */

		if (gbl ||
		    TLB_GLOBAL(cpu->tlb[i]) ||
		    pid == TLB_PID(cpu->tlb[i])) {
			msg("Duplicate TLB entries!");
			tlbmsg(cpu, newix, "New entry");
			tlbmsg(cpu, i, "Old entry");
			hang("Duplicate TLB entries for vpage ");
		}
	}
}

static
int
findtlb(struct mipscpu *cpu, u_int32_t vpage)
{
	int i;
	for (i=0; i<NTLB; i++) {
		u_int64_t t = cpu->tlb[i];
		if (TLB_VPN(t)!=vpage) continue;
		if (TLB_GLOBAL(t) || TLB_PID(t)==TLB_PID(cpu->tlbentry)) {
			return i;
		}
	}

	return -1;
}

static
void
probetlb(struct mipscpu *cpu)
{
	u_int32_t vpage;
	int ix;

	vpage = TLB_VPN(cpu->tlbentry);
	ix = findtlb(cpu, vpage);

	TRACEL(DOTRACE_TLB, ("tlbp:       %05x/%03x -> ", vpage >> 12,
			     TLB_PID(cpu->tlbentry)));

	if (ix<0) {
		TRACE(DOTRACE_TLB, ("NOT FOUND"));
		cpu->tlbindex |= 0x80000000;
	}
	else {
		TRACE(DOTRACE_TLB, ("%05x %s%s%s%s: [%d]", 
				    TLB_PFN(cpu->tlb[ix]) >> 12,
				    TLB_GLOBAL(cpu->tlb[ix])?"G":"-",
				    TLB_VALID(cpu->tlb[ix])?"V":"-",
				    TLB_DIRTY(cpu->tlb[ix])?"D":"-",
				    TLB_NOCACHE(cpu->tlb[ix])?"N":"-",
				    ix));
		cpu->tlbindex = (ix << 8);
	}
}

static
void
do_wait(struct mipscpu *cpu)
{
	(void)cpu;
	TRACE(DOTRACE_IRQ, ("Waiting for interrupt"));
	clock_waitirq();
}

static
void
do_rfe(struct mipscpu *cpu)
{
	u_int32_t bits;

	if (IS_USERMODE(cpu)) {
		smoke("RFE in usermode not caught by instruction decoder");
	}
	
	bits = cpu->ex_status & 0x3f;
	bits >>= 2;
	cpu->ex_status = (cpu->ex_status & 0xfffffff0) | bits;
	TRACE(DOTRACE_EXN, ("Return from exception: %s mode, interrupts %s",
			    (cpu->ex_status & 0x2) ? "user" : "kernel",
			    (cpu->ex_status & 0x1) ? "on" : "off"));
}

/*
 * This corrects the state of the processor if we're handling a
 * breakpoint with the remote gdb code. The remote gdb code should use
 * cpu->expc as the place execution stopped; in that case something
 * vaguely reasonable will happen if some nimrod puts a breakpoint in
 * a branch delay slot.
 */
static
void
phony_exception(struct mipscpu *cpu)
{
	cpu->jumping = 0;
	cpu->in_jumpdelay = 0;
	cpu->pc = cpu->expc;
	cpu->nextpc = cpu->pc + 4;
}

static
void
exception(struct mipscpu *cpu, int code, int cn_or_user, u_int32_t vaddr)
{
	u_int32_t bits;
	int boot = (cpu->ex_status & 0x00400000)!=0;

	TRACE(DOTRACE_EXN, ("exception: code %d (%s), expc %x, vaddr %x", 
	      code, exception_name(code), cpu->expc, vaddr));

	if (code==EX_IRQ) {
		g_stats.s_irqs++;
	}
	else {
		g_stats.s_exns++;
	}

	// clear everything but pending interrupts from cause register
	cpu->ex_cause &= 0x0000ff00;
	cpu->ex_cause |= code << 2;   // or in code for exception
	if (cpu->in_jumpdelay) {
		cpu->ex_cause |= 0x80000000;
	}
	if (code==EX_CPU) {
		cpu->ex_cause |= ((u_int32_t)cn_or_user << 28);
	}
	cpu->jumping = 0;
	cpu->in_jumpdelay = 0;

	// roll the status bits
	bits = cpu->ex_status & 0x3f;
	bits <<= 2;
	cpu->ex_status = (cpu->ex_status & 0xffffffc0) | (bits & 0x3f);

	cpu->ex_vaddr = vaddr;
	cpu->ex_context &= 0xffe00000;
	cpu->ex_context |= ((vaddr & 0x7ffff000) >> 10);
	
	cpu->ex_epc = cpu->expc;

	if ((code==EX_TLBL || code==EX_TLBS) && cn_or_user) {
		// use special UTLB exception vector
		cpu->pc = boot ? 0xbfc00100 : 0x80000000;
	}
	else {
		cpu->pc = boot ? 0xbfc00180 : 0x80000080;
	}
	cpu->nextpc = cpu->pc + 4;
}

typedef enum {
	S_SBYTE,
	S_UBYTE,
	S_SHALF,
	S_UHALF,
	S_WORD,
	S_WORDL,
	S_WORDR,
} memstyles;

static
int
domem(struct mipscpu *cpu, u_int32_t vaddr, u_int32_t *val, 
      int iswrite, int isinsnfetch)
{
	u_int32_t vpage = vaddr & 0xfffff000;
	u_int32_t off   = vaddr & 0x00000fff;
	u_int32_t seg;
	u_int32_t ppage;
	u_int32_t paddr;
	int buserr;
	
	// MIPS hardwired memory layout:
	//    0xc0000000 - 0xffffffff   kseg2 (kernel, tlb-mapped)
	//    0xa0000000 - 0xbfffffff   kseg1 (kernel, unmapped, uncached)
	//    0x80000000 - 0x9fffffff   kseg0 (kernel, unmapped, cached)
	//    0x00000000 - 0x7fffffff   kuseg (user, tlb-mapped)
	//
	// Since we don't implement cache, we can consider kseg0 and kseg1
	// equivalent (except remember that the base of each maps to paddr 0.)
	// Thus we have four segments:
	//    3: kernel mapped
	//    2: kernel unmapped
	//    0-1: user

	seg = vpage >> 30;

	if ((seg > 1 && IS_USERMODE(cpu)) || (vaddr & 0x3)!=0) {
		exception(cpu, iswrite ? EX_ADES : EX_ADEL, 0, vaddr);
		return -1;
	}

	if (seg==2) {
		ppage = vpage & 0x1fffffff;
	}
	else {
		int ix, exc;

		TRACEL(DOTRACE_TLB, ("tlblookup:  %05x/%03x -> ", 
				     vpage >> 12, TLB_PID(cpu->tlbentry)));

		ix = findtlb(cpu, vpage);
		exc = iswrite ? EX_TLBS : EX_TLBL;

		if (ix<0) {
			TRACE(DOTRACE_TLB, ("MISS"));
			exception(cpu, exc, seg<=1, vaddr);
			return -1;
		}
		TRACEL(DOTRACE_TLB, ("%05x %s%s%s%s: [%d]", 
				     TLB_PFN(cpu->tlb[ix]) >> 12,
				     TLB_GLOBAL(cpu->tlb[ix])?"G":"-",
				     TLB_VALID(cpu->tlb[ix])?"V":"-",
				     TLB_DIRTY(cpu->tlb[ix])?"D":"-",
				     TLB_NOCACHE(cpu->tlb[ix])?"N":"-",
				     ix));

		if (!TLB_VALID(cpu->tlb[ix])) {
			TRACE(DOTRACE_TLB, (" - INVALID"));
			exception(cpu, exc, 0, vaddr);
			return -1;
		}
		if (iswrite && !TLB_DIRTY(cpu->tlb[ix])) {
			TRACE(DOTRACE_TLB, (" - READONLY"));
			exception(cpu, EX_MOD, 0, vaddr);
			return -1;
		}
		TRACE(DOTRACE_TLB, (" - OK"));
		ppage = TLB_PFN(cpu->tlb[ix]);
	}
	
	paddr = ppage|off;
	
	/*
	 * Physical memory layout: 
	 *    0x00000000 - 0x1fbfffff     RAM
	 *    0x1fc00000 - 0x1fdfffff     Boot ROM
	 *    0x1fe00000 - 0x1fffffff     LAMEbus mapped I/O
	 *    0x20000000 - 0xffffffff     RAM
	 */
	
	if (paddr < 0x1fc00000) {
		if (iswrite) {
			buserr = bus_mem_store(paddr, *val);
		}
		else {
			buserr = bus_mem_fetch(paddr, val);
		}
	}
	else if (paddr < 0x1fe00000) {
		if (iswrite) {
			buserr = -1; /* ROM is, after all, read-only */
		}
		else {
			buserr = bootrom_fetch(paddr - 0x1fe00000, val);
		}
	}
	else if (paddr < 0x20000000) {
		if (iswrite) {
			buserr = bus_io_store(paddr-0x1fe00000, *val);
		}
		else {
			buserr = bus_io_fetch(paddr-0x1fe00000, val);
		}
	}
	else {
		if (iswrite) {
			buserr = bus_mem_store(paddr-0x00400000, *val);
		}
		else {
			buserr = bus_mem_fetch(paddr-0x00400000, val);
		}
	}

	if (buserr) {
		exception(cpu, isinsnfetch ? EX_IBE : EX_DBE, 0, 0);
		return -1;
	}
	
	return 0;
}

static
void
doload(struct mipscpu *cpu, memstyles ms, u_int32_t addr, u_int32_t *res)
{
	switch (ms) {
	    case S_SBYTE:
	    case S_UBYTE:
	    {
		u_int32_t val;
		u_int8_t bval = 0;
		if (domem(cpu, addr & 0xfffffffc, &val, 0, 0)) return;
		switch (addr & 3) {
			case 0: bval = (val & 0xff000000)>>24; break;
			case 1: bval = (val & 0x00ff0000)>>16; break;
			case 2: bval = (val & 0x0000ff00)>>8; break;
			case 3: bval = val & 0x000000ff; break;
		}
		if (ms==S_SBYTE) *res = (int32_t)(int8_t)bval;
		else *res = bval;
	    }
	    break;

	    case S_SHALF:
	    case S_UHALF:
	    {
		u_int32_t val;
		u_int16_t hval = 0;
		if (domem(cpu, addr & 0xfffffffd, &val, 0, 0)) return;
		switch (addr & 2) {
			case 0: hval = (val & 0xffff0000)>>16; break;
			case 2: hval = val & 0x0000ffff; break;
		}
		if (ms==S_SHALF) *res = (int32_t)(int16_t)hval;
		else *res = hval;
	    }
	    break;
     
	    case S_WORD:
		domem(cpu, addr, res, 0, 0);
	        break;

	    case S_WORDL:
	    {
		u_int32_t val;
		u_int32_t mask = 0;
		int shift = 0;
		if (domem(cpu, addr & 0xfffffffc, &val, 0, 0)) return;
		switch (addr & 0x3) {
		    case 0: mask = 0xffffffff; shift=0; break;
		    case 1: mask = 0xffffff00; shift=8; break;
		    case 2: mask = 0xffff0000; shift=16; break;
		    case 3: mask = 0xff000000; shift=24; break;
		}
		val <<= shift;
		*res = (*res & ~mask) | (val & mask);
	    }
	    break;
	    case S_WORDR:
	    {
		u_int32_t val;
		u_int32_t mask = 0;
		int shift = 0;
		if (domem(cpu, addr & 0xfffffffc, &val, 0, 0)) return;
		switch (addr & 0x3) {
			case 0: mask = 0x000000ff; shift=24; break;
			case 1: mask = 0x0000ffff; shift=16; break;
			case 2: mask = 0x00ffffff; shift=8; break;
			case 3: mask = 0xffffffff; shift=0; break;
		}
		val >>= shift;
		*res = (*res & ~mask) | (val & mask);
	    }
	    break;
	    default:
		smoke("doload: Illegal addressing mode");
	}
}

static
void
dostore(struct mipscpu *cpu, memstyles ms, u_int32_t addr, u_int32_t val)
{
	switch (ms) {
	    case S_UBYTE:
	    {
		u_int32_t wval;
		u_int32_t mask = 0;
		int shift = 0;
		switch (addr & 3) {
		    case 0: mask = 0xff000000; shift=24; break;
		    case 1: mask = 0x00ff0000; shift=16; break;
		    case 2: mask = 0x0000ff00; shift=8; break;
		    case 3: mask = 0x000000ff; shift=0; break;
		}
		if (domem(cpu, addr & 0xfffffffc, &wval, 0, 0)) return;
		wval = (wval & ~mask) | ((val&0xff) << shift);
		if (domem(cpu, addr & 0xfffffffc, &wval, 1, 0)) return;
	    }
	    break;

	    case S_UHALF:
	    {
		u_int32_t wval;
		u_int32_t mask = 0;
		int shift = 0;
		switch (addr & 2) {
			case 0: mask = 0xffff0000; shift=16; break;
			case 2: mask = 0x0000ffff; shift=0; break;
		}
		if (domem(cpu, addr & 0xfffffffd, &wval, 0, 0)) return;
		wval = (wval & ~mask) | ((val&0xffff) << shift);
		if (domem(cpu, addr & 0xfffffffd, &wval, 1, 0)) return;
	    }
	    break;
	
	    case S_WORD:
		domem(cpu, addr, &val, 1, 0);
		break;
		
	    case S_WORDL:
	    {
		u_int32_t wval;
		u_int32_t mask = 0;
		int shift = 0;
		if (domem(cpu, addr & 0xfffffffc, &wval, 0, 0)) return;
		switch (addr & 0x3) {
			case 0: mask = 0xffffffff; shift=0; break;
			case 1: mask = 0x00ffffff; shift=8; break;
			case 2: mask = 0x0000ffff; shift=16; break;
			case 3: mask = 0x000000ff; shift=24; break;
		}
		val >>= shift;
		wval = (wval & ~mask) | (val & mask);

		if (domem(cpu, addr & 0xfffffffc, &wval, 1, 0)) return;
	    }
	    break;
	    case S_WORDR:
	    {
		u_int32_t wval;
		u_int32_t mask = 0;
		int shift = 0;
		if (domem(cpu, addr & 0xfffffffc, &wval, 0, 0)) return;
		switch (addr & 0x3) {
			case 0: mask = 0xff000000; shift=24; break;
			case 1: mask = 0xffff0000; shift=16; break;
			case 2: mask = 0xffffff00; shift=8; break;
			case 3: mask = 0xffffffff; shift=0; break;
		}
		val <<= shift;
		wval = (wval & ~mask) | (val & mask);

		if (domem(cpu, addr & 0xfffffffc, &wval, 1, 0)) return;
	    }
	    break;

	    default:
		smoke("dostore: Illegal addressing mode");
	}
}

static 
void
abranch(struct mipscpu *cpu, u_int32_t addr)
{
	// Branches update nextpc (which points to the insn after 
	// the delay slot).
	cpu->nextpc = addr;
	cpu->jumping = 1;
}

static
void
ibranch(struct mipscpu *cpu, u_int32_t imm)
{
	// The mips book is helpfully not specific about whether the
	// address to take the upper bits of is the address of the
	// jump or the delay slot or what. it just says "the current
	// program counter", which I shall interpret as the address of
	// the delay slot. Fortunately, one doesn't isn't likely to
	// ever be executing a jump that lies across a boundary where
	// it would matter.
	//
	// (Note that cpu->pc aims at the delay slot by the time we
	// get here.)
   
	u_int32_t addr = (cpu->pc & 0xf0000000) | imm;
	abranch(cpu, addr);
}

static
void
rbranch(struct mipscpu *cpu, int32_t rel)
{
	u_int32_t addr = cpu->pc + rel;  // relative to addr of delay slot
	abranch(cpu, addr);
}

/*************************************************************/

// disassembly support
#ifdef USE_TRACE
static
const char *regname(unsigned reg)
{
	switch (reg) {
	    case 0: return "$z0";
	    case 1: return "$at";
	    case 2: return "$v0";
	    case 3: return "$v1";
	    case 4: return "$a0";
	    case 5: return "$a1";
	    case 6: return "$a2";
	    case 7: return "$a3";
	    case 8: return "$t0";
	    case 9: return "$t1";
	    case 10: return "$t2";
	    case 11: return "$t3";
	    case 12: return "$t4";
	    case 13: return "$t5";
	    case 14: return "$t6";
	    case 15: return "$t7";
	    case 16: return "$s0";
	    case 17: return "$s1";
	    case 18: return "$s2";
	    case 19: return "$s3";
	    case 20: return "$s4";
	    case 21: return "$s5";
	    case 22: return "$s6";
	    case 23: return "$s7";
	    case 24: return "$t8";
	    case 25: return "$t9";
	    case 26: return "$k0";
	    case 27: return "$k1";
	    case 28: return "$gp";
	    case 29: return "$sp";
	    case 30: return "$s8";
	    case 31: return "$ra";
	}
	return "$??";
}
#endif

// Map the 6-bit MIPS opcode field to operations.
// The operations are listed in mips-insn.h.
static short ops[64] = {
   OP_SPECIAL, OP_BCOND, OP_J,      OP_JAL,
   OP_BEQ,    OP_BNE,    OP_BLEZ,   OP_BGTZ,
   OP_ADDI,   OP_ADDIU,  OP_SLTI,   OP_SLTIU,
   OP_ANDI,   OP_ORI,    OP_XORI,   OP_LUI,
   OP_COP,    OP_COP,    OP_COP,    OP_COP,
   OP_ILL,    OP_ILL,    OP_ILL,    OP_ILL,
   OP_ILL,    OP_ILL,    OP_ILL,    OP_ILL,
   OP_ILL,    OP_ILL,    OP_ILL,    OP_ILL,
   OP_LB,     OP_LH,     OP_LWL,    OP_LW,
   OP_LBU,    OP_LHU,    OP_LWR,    OP_ILL,
   OP_SB,     OP_SH,     OP_SWL,    OP_SW,
   OP_ILL,    OP_ILL,    OP_SWR,    OP_ILL,
   OP_LWC,    OP_LWC,    OP_LWC,    OP_LWC,
   OP_ILL,    OP_ILL,    OP_ILL,    OP_ILL,
   OP_SWC,    OP_SWC,    OP_SWC,    OP_SWC,
   OP_ILL,    OP_ILL,    OP_ILL,    OP_ILL,
};

// When the opcode field contains "SPECIAL", this table is used to map
// the 6-bit special function code to operations.
static short sops[64] = {
   OP_SLL,    OP_ILL,    OP_SRL,    OP_SRA,
   OP_SLLV,   OP_ILL,    OP_SRLV,   OP_SRAV,
   OP_JR,     OP_JALR,   OP_ILL,    OP_ILL,
   OP_SYSCALL, OP_BREAK, OP_ILL,    OP_ILL,
   OP_MFHI,   OP_MTHI,   OP_MFLO,   OP_MTLO,
   OP_ILL,    OP_ILL,    OP_ILL,    OP_ILL,
   OP_MULT,   OP_MULTU,  OP_DIV,    OP_DIVU,
   OP_ILL,    OP_ILL,    OP_ILL,    OP_ILL,
   OP_ADD,    OP_ADDU,   OP_SUB,    OP_SUBU,
   OP_AND,    OP_OR,     OP_XOR,    OP_NOR,
   OP_ILL,    OP_ILL,    OP_SLT,    OP_SLTU,
   OP_ILL,    OP_ILL,    OP_ILL,    OP_ILL,
   OP_ILL,    OP_ILL,    OP_ILL,    OP_ILL,
   OP_ILL,    OP_ILL,    OP_ILL,    OP_ILL,
   OP_ILL,    OP_ILL,    OP_ILL,    OP_ILL,
   OP_ILL,    OP_ILL,    OP_ILL,    OP_ILL,
};

/*
 * Note: OP_WAIT is not defined for mips r2000/r3000 - it's from later
 * MIPS versions. However, we support it here anyway because spinning
 * in an idle loop is just plain stupid.
 */
static
int
decode_copz(struct mipscpu *cpu, int cn, u_int32_t insn, u_int32_t *ret)
{
	u_int32_t op;
	if (cn!=0) {
		exception(cpu, EX_CPU, cn, 0);
		return -1;
	}
	if (IS_USERMODE(cpu)) {
		exception(cpu, EX_CPU, cn, 0);
		return -1;
	}

	op = (insn & 0x03e00000) >> 21;	// coprocessor opcode

	if (op & 0x10) {
		op = (insn & 0x01ffffff);	// real coprocessor opcode
		switch (op) {
			case 1: *ret = OP_TLBR; break;
			case 2: *ret = OP_TLBWI; break;
			case 6: *ret = OP_TLBWR; break;
			case 8: *ret = OP_TLBP; break;
			case 16: *ret = OP_RFE; break;
		        case 32: *ret = OP_WAIT; break;
			default: *ret = OP_ILL; break;
		}
	}
	else if (op==0) {
		*ret = OP_MF;
	}
	else if (op==2) {
		*ret = OP_CF;
	}
	else if (op==4) {
		*ret = OP_MT;
	}
	else if (op==6) {
		*ret = OP_CT;
	}
	else if (op==8 || op==12) {
		if (insn & 0x00010000) {
			*ret = OP_BCF;
		}
		else {
			*ret = OP_BCT;
		}
	}
	else {
		*ret = OP_ILL;
	}
	return 0;
}

#define LINK2(rg)  (cpu->r[rg] = cpu->nextpc)
#define LINK LINK2(31)
#define RT   (cpu->r[rt])
#define RS   (cpu->r[rs])
#define RD   (cpu->r[rd])
#define RTU  ((u_int32_t)RT)
#define RSU  ((u_int32_t)RS)

#define TLBIX(r) (((r)>>8)&0x3f)

#define WHILO {if (cpu->hiwait >0 || cpu->lowait >0) goto pipeline_stall;}
#define WHI   {if (cpu->hiwait >0) goto pipeline_stall;}
#define WLO   {if (cpu->lowait >0) goto pipeline_stall;}
#define SETHILO(n) (cpu->hiwait = cpu->lowait = (n))
#define SETHI(n)   (cpu->hiwait = (n))
#define SETLO(n)   (cpu->lowait = (n))

#define CHKOVF(v) {if (((int64_t)(int32_t)(v))!=(v)) goto overflow;}

#define TRL(args)  TRACEL(tracehow, args)
#define TR(args)   TRACE(tracehow, args)

#define TLBTR(t)  TRACEL(DOTRACE_TLB, \
			("%05x/%03x -> %05x %s%s%s%s", \
				TLB_VPN(t) >> 12, TLB_PID(t), \
				TLB_PFN(t) >> 12, \
				TLB_GLOBAL(t)?"G":"-", \
				TLB_VALID(t)?"V":"-", \
				TLB_DIRTY(t)?"D":"-", \
				TLB_NOCACHE(t)?"N":"-"))

static
u_int32_t *
getcop0reg(struct mipscpu *cpu, int reg)
{
	switch (reg) {
	    case 0: return &cpu->tlbindex;
	    case 1: return &cpu->tlbrandom;
	    case 2: return &((u_int32_t *)(&cpu->tlbentry))[QUAD_LOWWORD];
	    case 4: return &cpu->ex_context;
	    case 8: return &cpu->ex_vaddr;
	    case 10: return &((u_int32_t *)(&cpu->tlbentry))[QUAD_HIGHWORD];
	    case 12: return &cpu->ex_status;
	    case 13: return &cpu->ex_cause;
	    case 14: return &cpu->ex_epc;
	    case 15: return &cpu->ex_prid;
	}
	return NULL;
}

static
void
domf(struct mipscpu *cpu, int cn, int reg, int32_t *greg)
{
	u_int32_t *creg;
	if (cn!=0 || IS_USERMODE(cpu)) {
		exception(cpu, EX_CPU, cn, 0);
		return;
	}
	creg = getcop0reg(cpu, reg);
	if (!creg) {
		exception(cpu, EX_RI, cn, 0);
		return;
	}
	*greg = *creg;
}

static
void
domt(struct mipscpu *cpu, int cn, int reg, int32_t greg)
{
	u_int32_t *creg;
	if (cn!=0 || IS_USERMODE(cpu)) {
		exception(cpu, EX_CPU, cn, 0);
		return;
	}
	creg = getcop0reg(cpu, reg);
	if (!creg) {
		exception(cpu, EX_RI, cn, 0);
		return;
	}
	*creg = greg;

#ifdef USE_TRACE
	if (reg==12) {
		TRACE(DOTRACE_EXN, ("Status register updated: %s mode, "
				    "interrupts %s",
				    (cpu->ex_status & 0x2) ? "user" : "kernel",
				    (cpu->ex_status & 0x1) ? "on" : "off"));
	}
#endif
}

static
void
dolwc(struct mipscpu *cpu, int cn, u_int32_t addr, int reg)
{
	(void)addr;
	(void)reg;
	exception(cpu, EX_CPU, cn, 0);
}

static
void
doswc(struct mipscpu *cpu, int cn, u_int32_t addr, int reg)
{
	(void)addr;
	(void)reg;
	exception(cpu, EX_CPU, cn, 0);
}

static
int
mips_run(struct mipscpu *cpu)
{
	u_int32_t insn;
	u_int32_t op, realop;
	u_int32_t rs, rt, rd;	// register fields of instruction
	u_int32_t targ;		// target part of absolute jump
	u_int32_t sh;		// shift count of shift instruction
	u_int32_t imm;		// immediate part of instruction
	int32_t smm;		// signed immediate part of instruction
	
	int64_t t64;		// temporary 64-bit value

	int hitbp=0;		// true if we hit a builtin breakpoint

#ifdef USE_TRACE
	int tracehow;		// how to trace this instruction
#endif

	/*
	 * First, update exception PC.
	 *
	 * Once we've done this, we can take exceptions this cycle and
	 * have them report the correct PC.
	 *
	 * If we're executing the delay slot of a jump, expc remains pointing
	 * to the jump and we clear the flag that tells us to do this.
	 */
	if (cpu->jumping) {
		cpu->jumping = 0;
		cpu->in_jumpdelay = 1;
	}
	else {
		cpu->expc = cpu->pc;
	}

	/*
	 * Check for interrupts.
	 */
	if (cpu->ex_status & 1) {
		// interrupts are enabled, check if any are pending
		u_int32_t intpending = (cpu->ex_cause & 0x0000ff00);
		u_int32_t intenabled = (cpu->ex_status& 0x0000ff00);
		if (intpending & intenabled) {
			TRACE(DOTRACE_IRQ, ("Taking interrupt"));
			exception(cpu, EX_IRQ, 0, 0);
			/*
			 * Start processing the interrupt this cycle.
			 *
			 * That is, repeat the code above us in this
			 * function. This amounts to just setting expc,
			 * and at this point we can be sure we're not in
			 * a jump delay slot, so it's only one line.
			 */
			cpu->expc = cpu->pc;
		}
	}

	if (IS_USERMODE(cpu)) {
		g_stats.s_ucycles++;
#ifdef USE_TRACE
		tracehow = DOTRACE_UINSN;
#endif
	}
	else {
		g_stats.s_kcycles++;
#ifdef USE_TRACE
		tracehow = DOTRACE_KINSN;
#endif
	}
	
	/*
	 * Fetch instruction.
	 */
	if (domem(cpu, cpu->pc, &insn, 0, 1)) {
		/* 
		 * Exception during instruction fetch; stop. On the next cycle
		 * we'll fetch the first instruction of the exception handler.
		 */
		return 0;
	}
	
	// Update PC. 
	cpu->pc = cpu->nextpc;
	cpu->nextpc += 4;

	
	/*
	 * Decode instruction.
	 */
	
	op = (insn & 0xfc000000) >> 26;   // opcode

	// decode opcode
	realop = ops[op];
	if (realop == OP_SPECIAL) {
		realop = sops[insn & 0x3f];  // function field
	}
	else if (realop == OP_BCOND) {
		// use rt field
		switch ((insn & 0x001f0000) >> 16) {
			case 0: realop = OP_BLTZ; break;
			case 1: realop = OP_BGEZ; break;
			case 16: realop = OP_BLTZAL; break;
			case 17: realop = OP_BGEZAL; break;
			default: realop = OP_ILL;
		}
	}
	else if (realop == OP_COP) {
		if (decode_copz(cpu, op&3, insn, &realop)) {
			// exception
			return 0;
		}
	}


	// get fields from opcode
	rs = (insn & 0x03e00000) >> 21;   // register
	rt = (insn & 0x001f0000) >> 16;   // register
	rd = (insn & 0x0000f800) >> 11;   // register
	targ=(insn & 0x03ffffff);         // target of jump
	sh = (insn & 0x000007c0) >> 6;    // shift count
	
	imm= (insn & 0x0000ffff);         // immediate value
	smm= (int32_t)(int16_t)imm;       // sign-extended immediate value

	TRL(("at %08x: ", cpu->expc));

	switch (realop) {
	    case OP_ADD:
		TRL(("add %s, %s, %s: %d + %d -> ",
		     regname(rd), regname(rs), regname(rt), RS, RT));
		t64 = (int64_t)RS + (int64_t)RT;
		CHKOVF(t64);
		RD = (int32_t)t64;
		TR(("%d", RD));
		break;
	    case OP_ADDI:
		TRL(("addi %s, %s, %u: %d + %d -> ",
			regname(rt), regname(rs), smm, RS, smm));
		t64 = (int64_t)RS + smm;
		CHKOVF(t64);
		RT = (int32_t)t64;
		TR(("%d", RT));
		break;
	    case OP_ADDIU: 
		TRL(("addiu %s, %s, %d: %d + %d -> ", 
			regname(rt), regname(rs), smm, RS, smm));
		RT = RS + smm;
		TR(("%d", RT));
		break;
	    case OP_ADDU:
		TRL(("addu %s, %s, %s: %d + %d -> ",
			regname(rd), regname(rs), regname(rt), RS, RT));
		RD = RS + RT;
		TR(("%d", RD));
		break;
	    case OP_AND:
		TRL(("and %s, %s, %s: 0x%x & 0x%x -> ", 
			regname(rd), regname(rs), regname(rt), RS, RT));
		RD = RS & RT;
		TR(("%d", RD));
		break;
	    case OP_ANDI:
		TRL(("andi %s, %s, %u: 0x%x & 0x%x -> ", 
			regname(rd), regname(rs), imm, RS, imm));
		RT = RS & imm;
		TR(("%d", RT));
		break;
	    case OP_BCF:
		TR(("bc%df %d", (op&3), smm));
		exception(cpu, EX_CPU, (op&3), 0);
		break;
	    case OP_BCT:
		TR(("bc%dt %d", (op&3), smm));
		exception(cpu, EX_CPU, (op&3), 0);
		break;
	    case OP_BEQ:
		TRL(("beq %s, %s, %d: %u==%u? ", 
		      regname(rs), regname(rt), smm, RS, RT));
		if (RS==RT) {
			rbranch(cpu, smm<<2);
			TR(("yes"));
		}
		else {
			TR(("no"));
		}
		break;
	    case OP_BGEZAL:
		TRL(("bgezal %s, %d: %d>=0? ", regname(rs), smm, RS));
		LINK;
		if (RS>=0) {
			rbranch(cpu, smm<<2); 
			TR(("yes"));
		}
		else {
			TR(("no"));
		}
		break;
	    case OP_BGEZ:
		TRL(("bgez %s, %d: %d>=0? ", regname(rs), smm, RS));
		if (RS>=0) {
			rbranch(cpu, smm<<2);
			TR(("yes"));
		}
		else {
			TR(("no"));
		}
		break;
	    case OP_BLTZAL:
		TRL(("bltzal %s, %d: %d<0? ", regname(rs), smm, RS));
		LINK;
		if (RS<0) {
			rbranch(cpu, smm<<2);
			TR(("yes"));
		}
		else {
			TR(("no"));
		}
		break;
	    case OP_BLTZ:
		TRL(("bltz %s, %d: %d<0? ", regname(rs), smm, RS));
		if (RS<0) {
			rbranch(cpu, smm<<2);
			TR(("yes"));
		}
		else {
			TR(("no"));
		}
		break;
	    case OP_BGTZ:
		TRL(("bgtz %s, %d: %d>0? ", regname(rs), smm, RS));
		if (RS>0) {
			rbranch(cpu, smm<<2);
			TR(("yes"));
		}
		else {
			TR(("no"));
		}
		break;
	    case OP_BLEZ:
		TRL(("blez %s, %d: %d<=0? ", regname(rs), smm, RS));
		if (RS<=0) {
			rbranch(cpu, smm<<2);
			TR(("yes"));
		}
		else {
			TR(("no"));
		}
		break;
	    case OP_BNE:
		TRL(("bne %s, %s, %d: %u!=%u? ", 
		      regname(rs), regname(rt), smm, RS, RT));
		if (RS!=RT) {
			rbranch(cpu, smm<<2);
			TR(("yes"));
		}
		else {
			TR(("no"));
		}
		break;
	    case OP_CF:
		TR(("cfc%d %s, $%u", op&3, regname(rt), rd));
		exception(cpu, EX_CPU, (op&3), 0);
		break;
	    case OP_CT:
		TR(("ctc%d %s, $%u", op&3, regname(rt), rd));
		exception(cpu, EX_CPU, (op&3), 0);
		break;
	    case OP_J:
		TR(("j 0x%x", targ<<2));
		ibranch(cpu, targ<<2);
		break;
	    case OP_JAL:
		TR(("jal 0x%x", targ<<2));
		LINK;
		ibranch(cpu, targ<<2);
		break;
	    case OP_LB:
		TRL(("lb %s, %d(%s): [%x] -> ", 
		     regname(rt), smm, regname(rs), RS+smm));
		doload(cpu, S_SBYTE, RS+smm, (u_int32_t *) &RT);
		TR(("%d", RT));
		break;
	    case OP_LBU:
		TRL(("lbu %s, %d(%s): [%x] -> ",
		     regname(rt), smm, regname(rs), RS+smm));
		doload(cpu, S_UBYTE, RS+smm, (u_int32_t *) &RT);
		TR(("%d", RT));
		break;
	    case OP_LH:
		TRL(("lh %s, %d(%s): [%x] -> ", 
		     regname(rt), smm, regname(rs), RS+smm));
		doload(cpu, S_SHALF, RS+smm, (u_int32_t *) &RT);
		TR(("%d", RT));
		break;
	    case OP_LHU:
		TRL(("lhu %s, %d(%s): [%x] -> ", 
		      regname(rt), smm, regname(rs), RS+smm));
		doload(cpu, S_UHALF, RS+smm, (u_int32_t *) &RT);
		TR(("%d", RT));
		break;
	    case OP_LUI:
		TR(("lui %s 0x%x", regname(rt), imm));
		RT = imm << 16;
		break;
	    case OP_LW:
		TRL(("lw %s, %d(%s): [%x] -> ", 
		      regname(rt), smm, regname(rs), RS+smm));
		doload(cpu, S_WORD, RS+smm, (u_int32_t *) &RT);
		TR(("%d", RT));
		break;
	    case OP_LWC:
		TR(("lwc%d $%u, %d(%s)", op&3, rt, smm, regname(rs)));
		dolwc(cpu, op&3, RS+smm, rt);
		break;
	    case OP_LWL:
		TRL(("lwl %s, %d(%s): [%x] -> ", 
		     regname(rt), smm, regname(rs), RS+smm));
		doload(cpu, S_WORDL, RS+smm, (u_int32_t *) &RT);
		TR(("0x%x", RT));
		break;
	    case OP_LWR:
		TRL(("lwr %s, %d(%s): [%x] -> ", 
		     regname(rt), smm, regname(rs), RS+smm));
		doload(cpu, S_WORDR, RS+smm, (u_int32_t *) &RT);
		TR(("0x%x", RT));
		break;
	    case OP_SB:
		TR(("sb %s, %d(%s): %d -> [%x]", 
		    regname(rt), smm, regname(rs), RT&0xff, RS+smm));
		dostore(cpu, S_UBYTE, RS+smm, RT);
		break;
	    case OP_SH:
		TR(("sh %s, %d(%s): %d -> [%x]", 
		      regname(rt), smm, regname(rs), RT&0xffff, RS+smm));
		dostore(cpu, S_UHALF, RS+smm, RT);
		break;
	    case OP_SW:
		TR(("sw %s, %d(%s): %d -> [%x]", 
		    regname(rt), smm, regname(rs), RT, RS+smm));
		dostore(cpu, S_WORD, RS+smm, RT);
		break;
	    case OP_SWC:
		TR(("swc%d $%u, %d(%s)", op&3, rt, smm, regname(rs)));
		doswc(cpu, op&3, RS+smm, rt);
		break;
	    case OP_SWL:
		TR(("swl %s, %d(%s): 0x%x -> [%x]", 
		    regname(rt), smm, regname(rs), RT, RS+smm));
		dostore(cpu, S_WORDL, RS+smm, RT);
		break;
	    case OP_SWR:
		TR(("swr %s, %d(%s): 0x%x -> [%x]", 
		    regname(rt), smm, regname(rs), RT, RS+smm));
		dostore(cpu, S_WORDR, RS+smm, RT);
		break;
	    case OP_BREAK:
		TR(("break"));

		/*
		 * If we're in the range that we can debug in (that
		 * is, not the TLB-mapped segments), activate the
		 * kernel debugging hooks.
		 */
		
		if (gdb_canhandle(cpu->expc)) {
			phony_exception(cpu);
			hitbp = 1;
		}
		else {
			exception(cpu, EX_BP, 0, 0);
		}
		break;
	    case OP_DIV:
		TRL(("div %s %s: %d / %d -> ", 
		     regname(rs), regname(rt), RS, RT));
		if (RT==0) {
			/* What's the right exception? XXX */
			exception(cpu, EX_RI, 0, 0);
			TR(("ERR"));
		}
		else {
			WHILO;
			cpu->lo = RS/RT;
			cpu->hi = RS%RT;
			SETHILO(2);
			TR(("%d, remainder %d", cpu->lo, cpu->hi));
		}
		break;
	    case OP_DIVU:
		TRL(("divu %s %s: %u / %u -> ", 
		     regname(rs), regname(rt), RS, RT));
		if (RTU==0) {
			/* What's the right exception? XXX */
			exception(cpu, EX_RI, 0, 0);
			TR(("ERR"));
		}
		else {
			WHILO;
			cpu->lo=RSU/RTU;
			cpu->hi=RSU%RTU;
			SETHILO(2);
			TR(("%u, remainder %u", cpu->lo, cpu->hi));
		}
		break;
	    case OP_JR:
		TR(("jr %s: 0x%x", regname(rs), RS));
		abranch(cpu, RS);
		break;
	    case OP_JALR:
		TR(("jalr %s, %s: 0x%x", regname(rd), regname(rs), RS));
		LINK2(rd);
		abranch(cpu, RS);
		break;
	    case OP_MF:
		TRL(("mfc%d %s, $%u: ... -> ", op&3, regname(rt), rd));
		domf(cpu, (op&3), rd, &RT);
		TR(("0x%x", RT));
		break;
	    case OP_MFHI:
		TRL(("mfhi %s: ... -> ", regname(rd)));
		WHI;
		RD = cpu->hi;
		SETHI(2);
		TR(("0x%x", RD));
		break;
	    case OP_MFLO:
		TRL(("mflo %s: ... -> ", regname(rd)));
		WLO;
		RD = cpu->lo;
		SETLO(2);
		TR(("0x%x", RD));
		break;
	    case OP_MT:
		TR(("mtc%d %s, $%u: 0x%x -> ...", op&3, regname(rt), rd, RT));
		domt(cpu, (op&3), rd, RT);
		break;
	    case OP_MTHI:
		TR(("mthi %s: 0x%x -> ...", regname(rs), RS));
		WHI;
		cpu->hi = RS;
		SETHI(2);
		break;
	    case OP_MTLO:
		TR(("mtlo %s: 0x%x -> ...", regname(rs), RS));
		WLO;
		cpu->lo = RS;
		SETLO(2);
		break;
	    case OP_MULT:
		TRL(("mult %s, %s: %d * %d -> ", 
		     regname(rs), regname(rt), RS, RT));
		WHILO;
		t64=(int64_t)RS*(int64_t)RT;
		cpu->hi = (t64>>32);
		cpu->lo = (u_int32_t)(u_int64_t)t64;
		SETHILO(2);
		TR(("%d %d", cpu->hi, cpu->lo));
		break;
	    case OP_MULTU:
		TRL(("multu %s, %s: %u * %u -> ", 
		     regname(rs), regname(rt), RS, RT));
		WHILO;
		t64=(u_int64_t)RSU*(u_int64_t)RTU;
		cpu->hi = (t64>>32);
		cpu->lo = (u_int32_t)(u_int64_t)t64;
		SETHILO(2);
		TR(("%u %u", cpu->hi, cpu->lo));
		break;
	    case OP_NOR:
		TRL(("nor %s, %s, %s: ~(0x%x | 0x%x) -> ",
		     regname(rd), regname(rs), regname(rt), RS, RT));
		RD = ~(RS | RT);
		TR(("0x%x", RD));
		break;
	    case OP_OR:
		TRL(("or %s, %s, %s: 0x%x | 0x%x -> ", 
		     regname(rd), regname(rs), regname(rt), RS, RT));
		RD = RS | RT;
		TR(("0x%x", RD));
		break;
	    case OP_ORI:
		TRL(("ori %s, %s, %u: 0x%x | 0x%x -> ", 
		     regname(rt), regname(rs), imm, RS, imm));
		RT = RS | imm;
		TR(("0x%x", RT));
		break;
	    case OP_RFE:
		TR(("rfe"));
		do_rfe(cpu);
		break;
	    case OP_SLL:
		TRL(("sll %s, %s, %u: 0x%x << %d -> ", 
		     regname(rd), regname(rt), sh, RT, sh));
		RD = RT << sh;
		TR(("0x%x", RD));
		break;
	    case OP_SLLV:
		TRL(("sllv %s, %s, %s: 0x%x << %d -> ", 
		     regname(rd), regname(rt), regname(rs), RT, (RS&31)));
		RD = RT << (RS&31);
		TR(("0x%x", RD));
		break;
	    case OP_SLT:
		TRL(("slt %s, %s, %s: %d < %d -> ", 
		       regname(rd), regname(rs), regname(rt), RS, RT));
		RD = RS < RT;
		TR(("0x%x", RD));
		break;
	    case OP_SLTI:
		TRL(("slti %s, %s, %d: %d < %d -> ", 
		     regname(rt), regname(rs), smm, RS, smm));
		RT = RS < smm;
		TR(("%d", RT));
		break;
	    case OP_SLTIU:
		TRL(("sltiu %s, %s, %u: %u < %u -> ", 
		     regname(rt), regname(rs), imm, RS, smm));
		// Yes, the immediate is sign-extended then treated as
		// unsigned, according to my mips book. Blech.
		RT = RSU < (u_int32_t) smm;
		TR(("%d", RT));
		break;
	    case OP_SLTU:
		TRL(("sltu %s, %s, %s: %u < %u -> ", 
		     regname(rd), regname(rs), regname(rt), RS, RT));
		RD = RSU < RTU;
		TR(("0x%x", RD));
		break;
	    case OP_SRA:
		TRL(("sra %s, %s, %u: 0x%x >> %d -> ", 
		     regname(rd), regname(rt), sh, RT, sh));
		RD = RT >> sh;
		TR(("0x%x", RD));
		break;
	    case OP_SRAV:
		TRL(("srav %s, %s, %s: 0x%x >> %d -> ", 
		     regname(rd), regname(rt), regname(rs), RT, (RS&31)));
		RD = RT >> (RS&31);
		TR(("0x%x", RD));
		break;
	    case OP_SRL:
		TRL(("srl %s, %s, %u: 0x%x >> %d -> ", 
		     regname(rd), regname(rt), sh, RT, sh));
		RD = RTU >> sh;
		TR(("0x%x", RD));
		break;
	    case OP_SRLV:
		TRL(("srlv %s, %s, %s: 0x%x >> %d -> ", 
		     regname(rd), regname(rt), regname(rs), RT,  (RS&31)));
		RD = RTU >> (RS&31);
		TR(("0x%x", RD));
		break;
	    case OP_SUB:
		TRL(("sub %s, %s, %s: %d - %d -> ", 
		     regname(rd), regname(rs), regname(rt), RS, RT));
		t64 = (int64_t)RS - (int64_t)RT;
		CHKOVF(t64);
		RD = (int32_t)t64;
		TR(("%d", RD));
		break;
	    case OP_SUBU:
		TRL(("subu %s, %s, %s: %d - %d -> ", 
		     regname(rd), regname(rs), regname(rt), RS, RT));
		RD = RS - RT;
		TR(("%d", RD));
		break;
	    case OP_SYSCALL:
		TR(("syscall"));
		exception(cpu, EX_SYS, 0, 0);
		break;
	    case OP_TLBP:
		TR(("tlbp"));
		probetlb(cpu);
		break;
	    case OP_TLBR:
		TR(("tlbr"));
		cpu->tlbentry = cpu->tlb[TLBIX(cpu->tlbindex)];
		TRACEL(DOTRACE_TLB, ("tlbr:  [%2d] ", TLBIX(cpu->tlbindex)));
		TLBTR(cpu->tlbentry);
		TRACE(DOTRACE_TLB, (" "));
		break;
	    case OP_TLBWI:
		TR(("tlbwi"));
		TRACEL(DOTRACE_TLB,("tlbwi: [%2d] ", TLBIX(cpu->tlbindex)));
		TLBTR(cpu->tlb[TLBIX(cpu->tlbindex)]);
		TRACEL(DOTRACE_TLB, (" ==> "));
		TLBTR(cpu->tlbentry);
		TRACE(DOTRACE_TLB, (" "));
		cpu->tlb[TLBIX(cpu->tlbindex)] = cpu->tlbentry;
		check_tlb_dups(cpu, TLBIX(cpu->tlbindex));
		break;
	    case OP_TLBWR:
		TR(("tlbwr"));
		TRACEL(DOTRACE_TLB,("tlbwr: [%2d] ", TLBIX(cpu->tlbrandom)));
		TLBTR(cpu->tlb[TLBIX(cpu->tlbrandom)]);
		TRACEL(DOTRACE_TLB, (" ==> "));
		TLBTR(cpu->tlbentry);
		TRACE(DOTRACE_TLB, (" "));
		cpu->tlb[TLBIX(cpu->tlbrandom)] = cpu->tlbentry;
		check_tlb_dups(cpu, TLBIX(cpu->tlbrandom));
		break;
	    case OP_WAIT:
		TR(("wait"));
		do_wait(cpu);
		break;
	    case OP_XOR:
		TRL(("xor %s, %s, %s: 0x%x ^ 0x%x -> ",
		     regname(rd), regname(rs), regname(rt), RS, RT));
		RD = RS ^ RT;
		TR(("0x%x", RD));
		break;
	    case OP_XORI:
		TRL(("xori %s, %s, %u: 0x%x ^ 0x%x -> ",
		     regname(rd), regname(rs), imm, RS, imm));
		RT = RS ^ imm;
		TR(("0x%x", RT));
		break;
	    case OP_ILL:
	    default:
		TR(("[illegal instruction]"));
		exception(cpu, EX_RI, 0, 0); break; 
            pipeline_stall:
		cpu->nextpc = cpu->pc;
		cpu->pc = cpu->expc;
		break;
            overflow:
		exception(cpu, EX_OVF, 0, 0); break;
	}

	if (cpu->lowait > 0) {
		cpu->lowait--;
	}
	if (cpu->hiwait > 0) {
		cpu->hiwait--;
	}

	cpu->in_jumpdelay = 0;
	
	{
		int ix = ((cpu->tlbrandom >> 8) & 0x3f);
		ix = (ix+1)&0x3f;
		if (ix<8) ix = 8;
		cpu->tlbrandom = ix<<8;
	}

	return hitbp;
}

/*************************************************************/

static struct mipscpu mycpu;

void
cpu_init(void)
{
	mips_init(&mycpu);
}

int
cpu_cycle(void)
{
	/*
	 * Use hardware interrupt 0 for IRQ and 1 for NMI.
	 * The other four we don't use.
	 */
	const u_int32_t irqmask = 0x00000400;
	const u_int32_t nmimask = 0x00000800;
	const u_int32_t allmask = 0x0000fc00;

	u_int32_t mask = 0;

	if (cpu_irq_line) {
		// generates too much crap
		//TRACE(DOTRACE_IRQ, ("Raising MIPS irq"));
		mask |= irqmask;
	}
	if (cpu_nmi_line) {
		//TRACE(DOTRACE_IRQ, ("Raising MIPS nmi"));
		mask |= nmimask;
	}
	mycpu.ex_cause = (mycpu.ex_cause & ~allmask) | mask;

	return mips_run(&mycpu);
}

#define BETWEEN(addr, size, base, top) \
          ((addr) >= (base) && (size) <= (top)-(base) && (addr)+(size) < (top))

int
cpu_get_load_paddr(u_int32_t vaddr, u_int32_t size, u_int32_t *paddr)
{
	if (!BETWEEN(vaddr, size, KSEG0, KSEG2)) {
		return -1;
	}

	if (vaddr >= KSEG1) {
		*paddr = vaddr - KSEG1;
	}
	else {
		*paddr = vaddr - KSEG0;
	}
	return 0;
}

int
cpu_get_load_vaddr(u_int32_t paddr, u_int32_t size, u_int32_t *vaddr)
{
	u_int32_t zero = 0;  /* suppresses silly gcc warning */
	if (!BETWEEN(paddr, size, zero, KSEG1-KSEG0)) {
		return -1;
	}
	*vaddr = paddr + KSEG0;
	return 0;
}

void
cpu_set_entrypoint(u_int32_t addr)
{
	mycpu.pc = addr;
	mycpu.expc = addr;
	mycpu.nextpc = addr+4;
}

void
cpu_set_stack(u_int32_t stackaddr, u_int32_t argument)
{
	mycpu.r[29] = stackaddr;   /* register 29: stack pointer */
	mycpu.r[4] = argument;     /* register 4: first argument */
	
	/* don't need to set $gp - in the ELF model it's start's problem */
}

void
cpudebug_get_bp_region(u_int32_t *start, u_int32_t *end)
{
	*start = KSEG0;
	*end = KSEG2;
}

int
cpudebug_translate_address(u_int32_t va, u_int32_t size, u_int32_t *pa)
{
	/* Same behavior as cpu_get_load_paddr wanted. */
	return cpu_get_load_paddr(va, size, pa);
}

static
inline
void
addreg(u_int32_t *regs, int maxregs, int pos, u_int32_t val)
{
	if (pos < maxregs) {
		regs[pos] = val;
	}
}

#define GETREG(r) addreg(regs, maxregs, j++, r)

void
cpudebug_getregs(u_int32_t *regs, int maxregs, int *nregs)
{
	int i, j=0;
	for (i=0; i<NREGS; i++) {
		GETREG(mycpu.r[i]);
	}
	GETREG(mycpu.ex_status);
	GETREG(mycpu.lo);
	GETREG(mycpu.hi);
	GETREG(mycpu.ex_vaddr);
	GETREG(mycpu.ex_cause);
	GETREG(mycpu.pc);
	*nregs = j;
}
