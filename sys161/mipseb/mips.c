#include <sys/types.h>
#include <string.h>
#include "config.h"

#include "util.h" 
#include "bswap.h"
#include "cpu.h"
#include "bus.h"
#include "console.h"
#include "clock.h"
#include "gdb.h"
#include "main.h"
#include "trace.h"
#include "prof.h"
#include "memdefs.h"
#include "inlinemem.h"

#include "mips-insn.h"
#include "mips-ex.h"
#include "bootrom.h"

// XXX: For now, leave this off, as it isn't compatible with address space ids
//#define USE_TLBMAP


/* number of tlb entries */
#define NTLB  64

/* tlb fields */
#define TLBLO_GLOBAL		0x00000100
#define TLBLO_VALID		0x00000200
#define TLBLO_DIRTY		0x00000400
#define TLBLO_NOCACHE		0x00000800
#define TLBHI_PID		0x00000fc0
#define TLB_PAGEFRAME		0xfffff000

/* status register fields */
/*
 * The status register varies by MIPS model.
 * On all models, bits 28-31 enable coprocessors 0-3.
 * On all models, bits 8-15 enable interrupt lines.
 * On all models, bit 22 enables the bootstrap exception vectors.
 * Bits 0-7 are for interrupt and user/supervisor control.
 * Bits 16-21 and 23-27 are for miscellaneous control.
 *
 * Bits 0-7 on the r2000/r3000:
 *    0		interrupt enable
 *    1		processor is in user mode
 *    2-3	saved copy of bits 0-1 from exception
 *    4-5	saved copy of bits 2-3 from exception
 *    6-7	0
 *
 * Bits 0-7 on post-r3000 (incl. mips32):
 *    0		interrupt enable
 *    1		exception level (set for exceptions)
 *    2		error level (set for error exceptions)
 *    3		processor is in "supervisor" mode
 *    4		processor is in user mode
 *    5		0 (enable 64-bit user space on mips64)
 *    6		0 (enable 64-bit "supervisor" address space on mips64)
 *    7		0 (enable 64-bit kernel address space on mips64)
 * Turning on both the user and "supervisor" mode bits is an error.
 *
 * Bits 16-21, 23-27 on the r3000:
 *    16	isolate data cache
 *    17	swap caches
 *    18	disable cache parity
 *    19	becomes 1 if a cache miss occurs with cache isolated
 *    20	becomes 1 if a cache parity error occurs
 *    21	becomes 1 on duplicate TLB entries (irretrievable)
 *    23-24	0
 *    25	reverse endianness (later r3000 only AFAIK)
 *    26-27	0
 *
 * Bits 16-21, 23-27 on mips32:
 *    16-17	implementation-dependent
 *    18	0
 *    19	becomes 1 on NMI reset (write 0 to clear; don't write 1)
 *    20	becomes 1 on soft reset (write 0 to clear; don't write 1)
 *    21	becomes 1 on dup TLB entries (write 0 to clear; don't write 1)
 *    23	0 (enable 64-bit mode on mips64)
 *    24	0 (enable MDMX extensions on mips64)
 *    25	reverse endianness
 *    26	0 (enable extended FPU mode on mips64)
 *    27	enable reduced power mode
 *
 * Other meanings for bits 16-21, 23-27 seen in docs:
 *    16	disable cache parity exceptions
 *    17	enable use of an ECC register
 *    18	hit or miss indicator for last CACHE instruction
 *    23	instruction cache lock mode
 *    24	data cache lock mode
 *    27	enable non-blocking loads (whatever those are)
 *
 * We use the r2k/r3k meaning of bits 0-7, although at some future point
 * I'd like to have a way to enable the later version and its accompanying
 * exception handling.
 *
 * For bits 16-21, 23-27:
 *    16-17	machine check if enabled
 *    18	0 (compatible with both r3k and mips32)
 *    19	mips32 meaning (note: we don't support NMIs yet)
 *    20	mips32 meaning (note: we don't support soft reset yet)
 *    21	mips32 meaning (compatible with r3k) (note: we machine check
 *                on duplicate TLB entries)
 *    23-24	mips32 meaning, except machine check if set to 1
 *    25	reverse endianness (not supported; machine check if set to 1)
 *    26	mips32 meaning, except machine check if set to 1
 *    27	0 (might implement a reduced power mode in the future)
 * Note that we don't support machine check exceptions; instead we do the
 * debugger thing.
 */
#define STATUS_COPENABLE	0xf0000000	/* coprocessor enable bits */
/*      STATUS_LOWPOWER		0x08000000	   reduced power mode */
#define STATUS_XFPU64		0x04000000	/* 64-bit only FPU mode */
#define STATUS_REVENDIAN	0x02000000	/* reverse endian mode */
#define STATUS_MDMX64		0x01000000	/* 64-bit only MDMX exts */
#define STATUS_MODE64		0x00800000	/* 64-bit instructions */
#define STATUS_BOOTVECTORS	0x00400000	/* boot exception vectors */
#define STATUS_ERRORCAUSES	0x00380000	/* cause bits for error exns */
/*      STATUS_CACHEPARITY	0x00040000	   disable cache parity */
#define STATUS_R3KCACHE		0x00030000	/* r3k cache control bits */
#define STATUS_HARDMASK_TIMER	0x00008000	/* on-chip timer irq enabled */
#define STATUS_HARDMASK_UNUSED4	0x00004000	/* unused hardware irq lines */
#define STATUS_HARDMASK_FPU	0x00002000	/* FPU irq enabled */
#define STATUS_HARDMASK_UNUSED2	0x00001000	/* unused hardware irq lines */
#define STATUS_HARDMASK_IPI	0x00000800	/* lamebus ipi enabled */
#define STATUS_HARDMASK_LB	0x00000400	/* lamebus irq enabled */
#define STATUS_SOFTMASK		0x00000300	/* mask bits for soft irqs */
/*				0x000000c0  	   RESERVED set to 0 */
#define STATUS_KUo		0x00000020	/* old_usermode */
#define STATUS_IEo		0x00000010	/* old_irqon */
#define STATUS_KUp		0x00000008	/* prev_usermode */
#define STATUS_IEp		0x00000004	/* prev_irqon */
#define STATUS_KUc		0x00000002	/* current_usermode */
#define STATUS_IEc		0x00000001	/* current_irqon */

/* cause register fields */
#define CAUSE_BD		0x80000000	/* branch-delay flag */
/*				0x40000000	   RESERVED set to 0 */
#define CAUSE_CE		0x30000000	/* coprocessor # of exn */
/*				0x0fff0000	   RESERVED set to 0 */
#define CAUSE_HARDIRQ_TIMER	0x00008000	/* on-chip timer bit */
/*				0x00007000	   unused hardware irqs */
#define CAUSE_HARDIRQ_IPI	0x00000800	/* lamebus IPI bit */
#define CAUSE_HARDIRQ_LB	0x00000400	/* lamebus hardware irq bit */
#define CAUSE_SOFTIRQ		0x00000300	/* soft interrupt triggers */
/*				0x000000c0	   RESERVED set to 0 */
#define CAUSE_EXCODE		0x0000003c	/* exception code */
/*				0x00000003	   RESERVED Set to 0 */

/* tlb random register parameters (it ranges from 8 to 63) */
#define RANDREG_MAX		56
#define RANDREG_OFFSET		8

/* top bit in all config registers tells if the next one exists or not */
#define CONFIG_NEXTSEL_PRESENT	0x80000000

/* config0 register fields */
/*      CONFIG0_LOCAL           0x7fff0000 	for local use */
#define CONFIG0_ENDIAN   	0x00008000	/* endianness */
#define CONFIG0_TYPE            0x00006000	/* architecture type */
#define CONFIG0_REVISION        0x00001c00	/* architecture revision */
#define CONFIG0_MMU             0x000003f0	/* mmu type */
/*      zero                    0x0000007f */
#define CONFIG0_KSEG0_COHERE	0x00000007	/* cache coherence for kseg0 */

/* values for CONFIG0_ENDIAN */
#define CONFIG0_ENDIAN_BIG	0x00008000
#define CONFIG0_ENDIAN_LITTLE	0x00000000

/* values for CONFIG0_TYPE */
#define CONFIG0_TYPE_MIPS32     0x00000000
#define CONFIG0_TYPE_MIPS64_32  0x00002000
#define CONFIG0_TYPE_MIPS64     0x00004000
/*      reserved                0x00006000 */

/* value for CONFIG0_REVISION (others reserved) */
#define CONFIG0_REVISION_1	0x00000000

/* values for CONFIG0_MMU (_VINTAGE is a reserved value in mips32) */
#define CONFIG0_MMU_NONE        0x00000000	/* no mmu */
#define CONFIG0_MMU_TLB         0x000000f0	/* mips32 tlb */
#define CONFIG0_MMU_BAT         0x00000100	/* mips32 base-and-bounds */
#define CONFIG0_MMU_FIXED       0x000001f0	/* standard fixed mappings */
#define CONFIG0_MMU_VINTAGE     0x000003f0	/* mips-I MMU (sys161 only) */

/* values for CONFIG0_KSEG0_COHERE */
#define CONFIG0_KSEG0_COHERE_UNCACHED	2
#define CONFIG0_KSEG0_COHERE_CACHED	3

/* config1 register fields */
#define CONFIG1_TLBSIZE		0x7e000000	/* number of TLB entries - 1 */
#define CONFIG1_ICACHE_SETS	0x01c00000	/* icache sets per way */
#define CONFIG1_ICACHE_LINE	0x00380000	/* icache line size */
#define CONFIG1_ICACHE_ASSOC	0x00070000	/* icache associativity */
#define CONFIG1_DCACHE_SETS	0x0000e000	/* dcache sets per way */
#define CONFIG1_DCACHE_LINE	0x00001c00	/* dcache line size */
#define CONFIG1_DCACHE_ASSOC	0x00000380	/* dcache associativity */
#define CONFIG1_COP2            0x00000040	/* cop2 exists */
#define CONFIG1_MDMX64		0x00000020	/* MDMX64 implemented */
#define CONFIG1_PERFCTRS	0x00000010	/* perf counters implemented */
#define CONFIG1_WATCH		0x00000008	/* watch regs implemented */
#define CONFIG1_MIPS16		0x00000004	/* mips16 implemented */
#define CONFIG1_EJTAG		0x00000002	/* ejtag implemented */
#define CONFIG1_FPU		0x00000001	/* fpu implemented */

/* values for CONFIG1_TLBSIZE (valid inputs 1-64) */
#define CONFIG1_MK_TLBSIZE(n)	(((n)-1) << 25)

/* values for CONFIG1_[ID]CACHE_* */
#define CONFIG1_SETS_64		0
#define CONFIG1_SETS_128	1
#define CONFIG1_SETS_256	2
#define CONFIG1_SETS_512	3
#define CONFIG1_SETS_1024	4
#define CONFIG1_SETS_2048	5
#define CONFIG1_SETS_4096	6
#define CONFIG1_LINE_NONE	0
#define CONFIG1_LINE_4		1
#define CONFIG1_LINE_8		2
#define CONFIG1_LINE_16		3
#define CONFIG1_LINE_32		4
#define CONFIG1_LINE_64		5
#define CONFIG1_LINE_128	6
#define CONFIG1_MK_ASSOC(n)	(n-1)	/* valid values 1-8 */
#define CONFIG1_MK_CACHE(s, l, a) (((s) << 6) | ((l) << 3) | (a))
#define CONFIG1_MK_ICACHE(s, l, a) (CONFIG1_MK_CACHE(s, l, a) << 16)
#define CONFIG1_MK_DCACHE(s, l, a) (CONFIG1_MK_CACHE(s, l, a) << 7)

/*
 * Coprocessor registers have a register number (0-31) and then also
 * a "select" number 0-7, which is basically a bank number. So you can
 * have up to 256 registers per coprocessor.
 */
#define REGSEL(reg, sel) (((reg) << 3) | (sel))

/* system coprocessor (cop0) registers and selects */
#define C0_INDEX   REGSEL(0, 0)
#define C0_RANDOM  REGSEL(1, 0)
#define C0_TLBLO   REGSEL(2, 0)
#define C0_CONTEXT REGSEL(4, 0)
#define C0_VADDR   REGSEL(8, 0)
#define C0_COUNT   REGSEL(9, 0)
#define C0_TLBHI   REGSEL(10, 0)
#define C0_COMPARE REGSEL(11, 0)
#define C0_STATUS  REGSEL(12, 0)
#define C0_CAUSE   REGSEL(13, 0)
#define C0_EPC     REGSEL(14, 0)
#define C0_PRID    REGSEL(15, 0)
#define C0_CFEAT   REGSEL(15, 1)
#define C0_IFEAT   REGSEL(15, 2)
#define C0_CONFIG0 REGSEL(16, 0)
#define C0_CONFIG1 REGSEL(16, 1)
#define C0_CONFIG2 REGSEL(16, 2)
#define C0_CONFIG3 REGSEL(16, 3)
#define C0_CONFIG4 REGSEL(16, 4)
#define C0_CONFIG5 REGSEL(16, 5)
#define C0_CONFIG6 REGSEL(16, 6)
#define C0_CONFIG7 REGSEL(16, 7)

/* Version IDs for C0_PRID */
#define PRID_VALUE_ANCIENT	0xbeef    /* sys161 <= 0.95 */
#define PRID_VALUE_OLD		0x03ff    /* sys161 1.x and 1.99.x<=1.99.06 */
#define PRID_VALUE_CURRENT	0x00a1    /* sys161 2.x */

/* Feature flags for C0_CFEAT and C0_IFEAT */
/* (none yet) */

/* MIPS hardwired memory segments */
#define KSEG2	0xc0000000
#define KSEG1	0xa0000000
#define KSEG0	0x80000000
#define KUSEG	0x00000000

#ifdef USE_TLBMAP
/* tlbmap value for "nothing" */
#define TM_NOPAGE    255
#endif

/* number of general registers */
#define NREGS 32

typedef enum {
	S_SBYTE,
	S_UBYTE,
	S_SHALF,
	S_UHALF,
	S_WORDL,
	S_WORDR,
} memstyles;

struct mipstlb {
	int mt_global;		// 1: translation is global
	int mt_valid;		// 1: translation is valid for use
	int mt_dirty;		// 1: write enable
	int mt_nocache;		// 1: cache disable
	uint32_t mt_pfn;	// page number part of physical address
	uint32_t mt_vpn;	// page number part of virtual address
	uint32_t mt_pid;	// address space id
};

/* possible states for a cpu */
enum cpustates {
	CPU_DISABLED,
	CPU_IDLE,
	CPU_RUNNING,
};

struct mipscpu {
	// state of this cpu
	enum cpustates state;

	// my cpu number
	unsigned cpunum;

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

	uint32_t expc;         // pc for exception (not incr. while jumping)
	
	uint32_t pc;           // pc
	uint32_t nextpc;       // succeeding pc
	uint32_t pcoff;	// page offset of pc
	uint32_t nextpcoff;	// page offset of nextpc
	const uint32_t *pcpage;	// precomputed memory page of pc
	const uint32_t *nextpcpage;	// precomputed memory page of nextpc

	// mmu
	struct mipstlb tlb[NTLB];
	struct mipstlb tlbentry;	// cop0 register 2 (lo) and 10 (hi)
#ifdef USE_TLBMAP
	uint8_t tlbmap[1024*1024];	// vpn -> tlbentry map
#endif

	/*
	 * tlb index register (cop0 register 0)
	 */
	int tlbindex;		// not shifted; 0-63
	int tlbpf;		// true if a tlpb failed

	/*
	 * tlb random register (cop0 register 1)
	 */
	int tlbrandom;		// not shifted, not bounded, 0-based

	// exception stuff

	/*
	 * status register (cop0 register 12)
	 */
	int old_usermode;
	int old_irqon;
	int prev_usermode;
	int prev_irqon;
	int current_usermode;
	int current_irqon;
	uint32_t status_hardmask_lb;	// nonzero if lamebus irq is enabled
	uint32_t status_hardmask_ipi;	// nonzero if ipi is enabled
	uint32_t status_hardmask_fpu;	// nonzero if FPU irq is enabled
	uint32_t status_hardmask_void;	// unused hard irq bits
	uint32_t status_hardmask_timer;	// nonzero if timer irq is enabled
	uint32_t status_softmask;	// soft interrupt masking bits
	uint32_t status_bootvectors;	// nonzero if BEV bit on
	uint32_t status_copenable;	// coprocessor 0-3 enable bits
	
	/*
	 * cause register (cop0 register 13)
	 */
	int cause_bd;			// NOT shifted
	uint32_t cause_ce;		// already shifted
	uint32_t cause_softirq;	// already shifted
	uint32_t cause_code;		// already shifted

	/*
	 * config registers
	 */
	uint32_t ex_config0;	// cop0 register 16 sel 0
	uint32_t ex_config1;	// cop0 register 16 sel 1
	uint32_t ex_config2;	// cop0 register 16 sel 2
	uint32_t ex_config3;	// cop0 register 16 sel 3
	uint32_t ex_config4;	// cop0 register 16 sel 4
	uint32_t ex_config5;	// cop0 register 16 sel 5
	uint32_t ex_config6;	// cop0 register 16 sel 6
	uint32_t ex_config7;	// cop0 register 16 sel 7

	/*
	 * other cop0 registers
	 */
	uint32_t ex_context;	// cop0 register 4
	uint32_t ex_epc;	// cop0 register 14
	uint32_t ex_vaddr;	// cop0 register 8
	uint32_t ex_prid;	// cop0 register 15
	uint32_t ex_cfeat;	// cop0 register 15 sel 1
	uint32_t ex_ifeat;	// cop0 register 15 sel 2
	uint32_t ex_count;	// cop0 register 9
	uint32_t ex_compare;	// cop0 register 11
	int ex_compare_used;	// timer irq disabled if not set

	/*
	 * interrupt bits
	 */
	int irq_lamebus;
	int irq_ipi;
	int irq_timer;

	/*
	 * LL/SC hooks
	 */
	int ll_active;
	uint32_t ll_addr;
	uint32_t ll_value;

	/*
	 * debugger hooks
	 */
	int hit_breakpoint;
};

#define IS_USERMODE(cpu) ((cpu)->current_usermode)

static struct mipscpu *mycpus;
static unsigned ncpus;

/*
 * Hold cpu->state == CPU_RUNNING across all cpus, for rapid testing.
 */
uint32_t cpu_running_mask;

#define RUNNING_MASK_OFF(cn) (cpu_running_mask &= ~((uint32_t)1 << (cn)))
#define RUNNING_MASK_ON(cn)  (cpu_running_mask |= (uint32_t)1 << (cn))

/*
 * Number of cycles into cpu_cycles().
 */
uint64_t cpu_cycles_count;

/*************************************************************/

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

/*************************************************************/

// disassembly support
static
const char *
regname(unsigned reg)
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

/*************************************************************/

#define TLBTRP(t) CPUTRACEL(DOTRACE_TLB, \
			    cpu->cpunum, \
			    "%05x %s%s%s%s", \
				(t)->mt_pfn >> 12, \
				(t)->mt_global ? "G" : "-", \
				(t)->mt_valid ? "V" : "-", \
				(t)->mt_dirty ? "D" : "-", \
				(t)->mt_nocache ? "N" : "-")
#define TLBTRV(t) CPUTRACEL(DOTRACE_TLB, \
			    cpu->cpunum, \
			    "%05x/%03x -> ", \
				(t)->mt_vpn >> 12, (t)->mt_pid)
#define TLBTR(t) {TLBTRV(t);TLBTRP(t);}

/*
 * How to trace the current instruction. This is set only when
 * tracing; it is overwritten by each cpu in turn as its instruction
 * executes. It caches IS_USERMODE(cpu) ? DOTRACE_UINSN : DOTRACE_KINSN.
 */
static int tracehow;		// how to trace the current instruction

/*************************************************************/

/*
 * These are further down.
 */
static int precompute_pc(struct mipscpu *cpu);
static int precompute_nextpc(struct mipscpu *cpu);

/*
 * The MIPS doesn't clear the TLB on reset, so it's perfectly correct
 * to do nothing here. However, let's initialize it to all entries that
 * can't be matched.
 */

static
void
reset_tlbentry(struct mipstlb *mt, int index)
{
	mt->mt_global = 0;
	mt->mt_valid = 0;
	mt->mt_dirty = 0;
	mt->mt_nocache = 0;
	mt->mt_pfn = 0;
	mt->mt_vpn = 0x81000000 + index*0x1000;
	mt->mt_pid = 0;
}

static
uint32_t
tlbgetlo(const struct mipstlb *mt)
{
	uint32_t val = mt->mt_pfn;
	if (mt->mt_global) {
		val |= TLBLO_GLOBAL;
	}
	if (mt->mt_valid) {
		val |= TLBLO_VALID;
	}
	if (mt->mt_dirty) {
		val |= TLBLO_DIRTY;
	}
	if (mt->mt_nocache) {
		val |= TLBLO_NOCACHE;
	}
	return val;
}

static
uint32_t
tlbgethi(const struct mipstlb *mt)
{
	uint32_t val = mt->mt_vpn;
	val |= (mt->mt_pid << 6);
	return val;
}

static
void
tlbsetlo(struct mipstlb *mt, uint32_t val)
{
	mt->mt_global = val & TLBLO_GLOBAL;
	mt->mt_valid = val & TLBLO_VALID;
	mt->mt_dirty = val & TLBLO_DIRTY;
	mt->mt_nocache = val & TLBLO_NOCACHE;
	mt->mt_pfn = val & TLB_PAGEFRAME;
}

static
void
tlbsethi(struct mipstlb *mt, uint32_t val)
{
	mt->mt_vpn = val & TLB_PAGEFRAME;
	mt->mt_pid = (val & TLBHI_PID) >> 6;
}

static
void
tlbmsg(const char *what, int index, const struct mipstlb *t)
{
	msgl("%s: ", what);
	if (index>=0) {
		msgl("index %d, %s", index, index < 10 ? " " : "");
	}
	else {
		msgl("tlbhi/lo, ");
	}
	msgl("vpn 0x%08lx, ", (unsigned long) t->mt_vpn);
	     
	if (t->mt_global) {
		msgl("global, ");
	}
	else {
		msgl("pid %d, %s", (int) t->mt_pid,
		     t->mt_pid < 10 ? " " : "");
	}

	msg("ppn 0x%08lx (%s%s%s)",
	    (unsigned long) t->mt_pfn,
	    t->mt_valid ? "V" : "-",
	    t->mt_dirty ? "D" : "-",
	    t->mt_nocache ? "N" : "-");
}

static
void
check_tlb_dups(struct mipscpu *cpu, int newix)
{
	uint32_t vpn, pid;
	int gbl, i;

	vpn = cpu->tlb[newix].mt_vpn;
	pid = cpu->tlb[newix].mt_pid;
	gbl = cpu->tlb[newix].mt_global;

	for (i=0; i<NTLB; i++) {
		if (i == newix) {
			continue;
		}
		if (vpn != cpu->tlb[i].mt_vpn) {
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
		    cpu->tlb[i].mt_global ||
		    pid == cpu->tlb[i].mt_pid) {
			msg("Duplicate TLB entries!");
			tlbmsg("New entry", newix, &cpu->tlb[newix]);
			tlbmsg("Old entry", i, &cpu->tlb[i]);
			hang("Duplicate TLB entries for vpage %x",
			     cpu->tlb[i].mt_vpn);
		}
	}
}

static
void
mips_init(struct mipscpu *cpu, unsigned cpunum)
{
	int i;

	cpu->state = CPU_DISABLED;
	cpu->cpunum = cpunum;
	for (i=0; i<NREGS; i++) {
		cpu->r[i] = 0;
	}
	cpu->lo = cpu->hi = 0;
	cpu->lowait = cpu->hiwait = 0;

	for (i=0; i<NTLB; i++) {
		reset_tlbentry(&cpu->tlb[i], i);
	}
	reset_tlbentry(&cpu->tlbentry, NTLB);
#ifdef USE_TLBMAP
	memset(cpu->tlbmap, TM_NOPAGE, sizeof(cpu->tlbmap));
#endif
	cpu->tlbindex = 0;
	cpu->tlbpf = 0;
	cpu->tlbrandom = RANDREG_MAX-1;

	cpu->old_usermode = 0;
	cpu->old_irqon = 0;
	cpu->prev_usermode = 0;
	cpu->prev_irqon = 0;
	cpu->current_usermode = 0;
	cpu->current_irqon = 0;
	cpu->status_hardmask_lb = 0;
	cpu->status_hardmask_ipi = 0;
	cpu->status_hardmask_fpu = 0;
	cpu->status_hardmask_void = 0;
	cpu->status_hardmask_timer = 0;
	cpu->status_softmask = 0;
	cpu->status_bootvectors = STATUS_BOOTVECTORS;
	cpu->status_copenable = 0;

	cpu->cause_bd = 0;
	cpu->cause_ce = 0;
	cpu->cause_softirq = 0;
	cpu->cause_code = 0;

	/* config register 0 - misc info */
	cpu->ex_config0 = CONFIG_NEXTSEL_PRESENT |
		CONFIG0_ENDIAN_BIG |
		CONFIG0_TYPE_MIPS32 |
		CONFIG0_REVISION_1 |
		CONFIG0_MMU_VINTAGE |
		CONFIG0_KSEG0_COHERE_CACHED;

	/* config register 1 - mostly L1 cache info */
	/* for now report a 4K each 4-way 16-byte-line icache and dcache */
	cpu->ex_config1 =
		CONFIG1_MK_TLBSIZE(NTLB) |
		CONFIG1_MK_ICACHE(CONFIG1_SETS_64, CONFIG1_LINE_16,
				  CONFIG1_MK_ASSOC(4)) |
		CONFIG1_MK_DCACHE(CONFIG1_SETS_64, CONFIG1_LINE_16,
				  CONFIG1_MK_ASSOC(4));

	/* config register 2 - L2/L3 cache info */
	cpu->ex_config2 = 0;

	/* config register 3 - architecture extensions */
	cpu->ex_config3 = 0;

	/* config registers 4 and 5 - not defined in docs I have */
	cpu->ex_config4 = 0;
	cpu->ex_config5 = 0;

	/* config registers 6 and 7 - implementation-specific */
	cpu->ex_config6 = 0;
	cpu->ex_config7 = 0;

	/* other cop0 registers */
	cpu->ex_context = 0;
	cpu->ex_epc = 0;
	cpu->ex_vaddr = 0;
	cpu->ex_prid = PRID_VALUE_CURRENT;
	cpu->ex_cfeat = 0;
	cpu->ex_ifeat = 0;
	cpu->ex_count = 1;
	cpu->ex_compare = 0;
	cpu->ex_compare_used = 0;

	cpu->irq_lamebus = 0;
	cpu->irq_ipi = 0;
	cpu->irq_timer = 0;

	cpu->ll_active = 0;
	cpu->ll_addr = 0;
	cpu->ll_value = 0;

	cpu->jumping = cpu->in_jumpdelay = 0;
	cpu->expc = 0;

	/* pc starts at 0xbfc00000; nextpc at 0xbfc00004 */
	cpu->pc = 0xbfc00000;
	cpu->nextpc = 0xbfc00004;

	/* this must be last after other stuff is initialized */
	if (precompute_pc(cpu)) {
		smoke("precompute_pc failed in mips_init");
	}
	if (precompute_nextpc(cpu)) {
		smoke("precompute_nextpc failed in mips_init");
	}

}

/*************************************************************/

static int cpu_cycling;
static int tracing;

void
cpu_stopcycling(void)
{
	cpu_cycling = 0;
}

void
cpu_set_tracing(int on)
{
	tracing = on;
}

/*************************************************************/

#define CPUNAME mips161

#undef USE_TRACE
#include "mipscore.h"

#define USE_TRACE
#include "mipscore.h"
#undef USE_TRACE

/*************************************************************/

#define DISPATCH_0(sym) \
	(tracing ? mips161_trace_##sym() : mips161_##sym())

#define DISPATCH_1(sym, a0) \
	(tracing ? mips161_trace_##sym(a0) : mips161_##sym(a0))

#define DISPATCH_2(sym, a0, a1) \
	(tracing ? mips161_trace_##sym(a0, a1) : mips161_##sym(a0, a1))

#define DISPATCH_3(sym, a0, a1, a2) \
	(tracing ? mips161_trace_##sym(a0, a1, a2) : mips161_##sym(a0, a1, a2))

#define DISPATCH_4(sym, a0, a1, a2, a3) \
	(tracing ? mips161_trace_##sym(a0, a1, a2, a3) : \
		   mips161_##sym(a0, a1, a2, a3))

uint64_t
cpu_cycles(uint64_t maxcycles)
{
	return DISPATCH_1(cpu_cycles, maxcycles);
}

static
int
precompute_pc(struct mipscpu *cpu)
{
	return DISPATCH_1(precompute_pc, cpu);
}

static
int
precompute_nextpc(struct mipscpu *cpu)
{
	return DISPATCH_1(precompute_nextpc, cpu);
}

static
int
debug_translatemem(const struct mipscpu *cpu, uint32_t vaddr, 
		   int iswrite, uint32_t *ret)
{
	return DISPATCH_4(debug_translatemem, cpu, vaddr, iswrite, ret);
}

static
uint32_t
getstatus(struct mipscpu *cpu)
{
	return DISPATCH_1(getstatus, cpu);
}

static
uint32_t
getcause(struct mipscpu *cpu)
{
	return DISPATCH_1(getcause, cpu);
}

static
uint32_t
getindex(struct mipscpu *cpu)
{
	return DISPATCH_1(getindex, cpu);
}

static
uint32_t
getrandom(struct mipscpu *cpu)
{
	return DISPATCH_1(getrandom, cpu);
}

/*************************************************************/

void
cpu_init(unsigned numcpus)
{
	unsigned i;

	Assert(numcpus <= 32);

	ncpus = numcpus;
	mycpus = domalloc(ncpus * sizeof(*mycpus));
	for (i=0; i<numcpus; i++) {
		mips_init(&mycpus[i], i);
	}

	mycpus[0].state = CPU_RUNNING;
	cpu_running_mask = 0x1;
}

void
cpu_dumpstate(void)
{
	struct mipscpu *cpu;
	int i;
	unsigned j;

	msg("%u cpus: MIPS r3000", ncpus);
	for (j=0; j<ncpus; j++) {
		cpu = &mycpus[j];
		msg("cpu %d:", j);

	/* BEGIN INDENT HORROR */

	for (i=0; i<NREGS; i++) {
		msgl("r%d:%s 0x%08lx  ", i, i<10 ? " " : "",
		     (unsigned long)(uint32_t) cpu->r[i]);
		if (i%4==3) {
			msg(" ");
		}
	}
	msg("lo:  0x%08lx  hi:  0x%08lx  pc:  0x%08lx  npc: 0x%08lx", 
	    (unsigned long)(uint32_t) cpu->lo,
	    (unsigned long)(uint32_t) cpu->hi,
	    (unsigned long)(uint32_t) cpu->pc,
	    (unsigned long)(uint32_t) cpu->nextpc);

	for (i=0; i<NTLB; i++) {
		tlbmsg("TLB", i, &cpu->tlb[i]);
	}
	tlbmsg("TLB", -1, &cpu->tlbentry);
	msg("tlb index: %d %s", cpu->tlbindex, 
	    cpu->tlbpf ? "[last probe failed]" : "");
	msg("tlb random: %d", (cpu->tlbrandom%RANDREG_MAX)+RANDREG_OFFSET);

	msgl("Status register: ");
	/* coprocessor enable bits */
	msgl("%s%s%s%s",
	     cpu->status_copenable & 0x80000000 ? "3" : "-",
	     cpu->status_copenable & 0x40000000 ? "2" : "-",
	     cpu->status_copenable & 0x20000000 ? "1" : "-",
	     cpu->status_copenable & 0x10000000 ? "0" : "-");
	/* misc control bits */
	msgl("%s%s%s%s%s%s%s%s%s%s%s%s",
	     0 ? "P" : "-",	/* reduced power mode */
	     0 ? "F" : "-",	/* 64-bit extended FPU mode */
	     0 ? "R" : "-",	/* reverse endianness */
	     0 ? "M" : "-",	/* 64-bit MDMX extensions */
	     0 ? "6" : "-",	/* 64-bit mode */
	     cpu->status_bootvectors ? "B" : "-",
	     0 ? "T" : "-",	/* duplicate TLB entries */
	     0 ? "S" : "-",	/* soft reset */
	     0 ? "N" : "-",	/* NMI reset */
	     0 ? "C" : "-",	/* cache parity */
	     0 ? "W" : "-",	/* r3k swap caches */
	     0 ? "I" : "-");	/* r3k isolate cache */
	/* interrupt mask */
	msgl("%s%s%s%s%s%s%s%s",
	     cpu->status_hardmask_timer ? "H" : "-",
	     cpu->status_hardmask_void & 0x00004000 ? "h" : "-",
	     cpu->status_hardmask_void & 0x00002000 ? "h" : "-",
	     cpu->status_hardmask_fpu ? "h" : "-",
	     cpu->status_hardmask_ipi ? "H" : "-",
	     cpu->status_hardmask_lb ? "H" : "-",
	     cpu->status_softmask & 0x0200 ? "S" : "-",
	     cpu->status_softmask & 0x0100 ? "S" : "-");
	/* mode control */
	msg("--%s%s%s%s%s%s",
	    cpu->old_usermode ? "U" : "-",
	    cpu->old_irqon ? "I" : "-",
	    cpu->prev_usermode ? "U" : "-",
	    cpu->prev_irqon ? "I" : "-",
	    cpu->current_usermode ? "U" : "-",
	    cpu->current_irqon ? "I" : "-");

	msg("Cause register: %s %d %s---%s%s%s%s %d [%s]",
	    cpu->cause_bd ? "B" : "-",
	    cpu->cause_ce >> 28,
	    cpu->irq_timer ? "H" : "-",
	    cpu->irq_ipi ? "H" : "-",
	    cpu->irq_lamebus ? "H" : "-",
	    (cpu->cause_softirq & 0x200) ? "S" : "-",
	    (cpu->cause_softirq & 0x100) ? "S" : "-",
	    cpu->cause_code >> 2,
	    exception_name(cpu->cause_code>>2));

	msg("VAddr register: 0x%08lx", (unsigned long)cpu->ex_vaddr);
	msg("Context register: 0x%08lx", (unsigned long)cpu->ex_context);
	msg("EPC register: 0x%08lx", (unsigned long)cpu->ex_epc);

	/* END INDENT HORROR */

	}
}

unsigned
cpu_numcpus(void)
{
	return ncpus;
}

void
cpu_enable(unsigned cpunum)
{
	struct mipscpu *cpu;

	Assert(cpunum < ncpus);
	cpu = &mycpus[cpunum];

	cpu->state = CPU_RUNNING;
	RUNNING_MASK_ON(cpunum);
}

void
cpu_disable(unsigned cpunum)
{
	struct mipscpu *cpu;

	Assert(cpunum < ncpus);
	cpu = &mycpus[cpunum];

	cpu->state = CPU_DISABLED;
	RUNNING_MASK_OFF(cpunum);
}

int
cpu_enabled(unsigned cpunum)
{
	struct mipscpu *cpu;

	Assert(cpunum < ncpus);
	cpu = &mycpus[cpunum];

	return (cpu->state != CPU_DISABLED);
}

#define BETWEEN(addr, size, base, top) \
          ((addr) >= (base) && (size) <= (top)-(base) && (addr)+(size) < (top))

int
cpu_get_load_paddr(uint32_t vaddr, uint32_t size, uint32_t *paddr)
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
cpu_get_load_vaddr(uint32_t paddr, uint32_t size, uint32_t *vaddr)
{
	uint32_t zero = 0;  /* suppresses silly gcc warning */
	if (!BETWEEN(paddr, size, zero, KSEG1-KSEG0)) {
		return -1;
	}
	*vaddr = paddr + KSEG0;
	return 0;
}

uint32_t
cpu_get_ram_paddr(void)
{
	/* paddr of base of physical ram */
	return 0;
}

void
cpu_set_entrypoint(unsigned cpunum, uint32_t addr)
{
	struct mipscpu *cpu;

	Assert(cpunum < ncpus);
	cpu = &mycpus[cpunum];

	if ((addr & 0x3) != 0) {
		hang("Kernel entry point is not properly aligned");
		addr &= 0xfffffffc;
	}
	cpu->r[25] = addr; /* register 25: t9, ELF "abicalls" function addr */
	cpu->expc = addr;
	cpu->pc = addr;
	cpu->nextpc = addr+4;
	if (precompute_pc(cpu)) {
		hang("Kernel entry point is an invalid address");
	}
	if (precompute_nextpc(cpu)) {
		hang("Kernel entry point is an invalid address");
	}
}

void
cpu_set_stack(unsigned cpunum, uint32_t stackaddr, uint32_t argument)
{
	struct mipscpu *cpu;

	Assert(cpunum < ncpus);
	cpu = &mycpus[cpunum];

	cpu->r[29] = stackaddr;   /* register 29: stack pointer */
	cpu->r[4] = argument;     /* register 4: first argument */
	
	/* don't need to set $gp - in the ELF model it's start's problem */
}

uint32_t
cpu_get_secondary_start_stack(uint32_t lboffset)
{
	/* lboffset is the offset from the LAMEbus mapping base. */
	/* XXX why don't we have a constant for 1fe00000? */
	return KSEG0 + 0x1fe00000 + lboffset;
}

void
cpu_set_irqs(unsigned cpunum, int lamebus, int ipi)
{
	Assert(cpunum < ncpus);

	struct mipscpu *cpu;

	Assert(cpunum < ncpus);
	cpu = &mycpus[cpunum];
	cpu->irq_lamebus = lamebus;
	cpu->irq_ipi = ipi;

	/* cpu->irq_timer is on-chip, and cannot get set when CPU_IDLE */

	/* XXX shouldn't be using CPUTRACE outside mipscore.h */
	CPUTRACE(DOTRACE_IRQ, cpunum,
		 "cpu_set_irqs: LB %s IPI %s",
		 lamebus ? "ON" : "off",
		 ipi ? "ON" : "off");
	if (cpu->state == CPU_IDLE && (lamebus || ipi)) {
		cpu->state = CPU_RUNNING;
		RUNNING_MASK_ON(cpunum);
	}
}

/*
 * Return which CPU hit a breakpoint. If more than one did, use the
 * first. If none did, use CPU 0.
 */
unsigned
cpudebug_get_break_cpu(void)
{
	unsigned i;

	for (i=0; i<ncpus; i++) {
		if (mycpus[i].hit_breakpoint) {
			return i;
		}
	}
	return 0;
}

void
cpudebug_get_bp_region(uint32_t *start, uint32_t *end)
{
	*start = KSEG0;
	*end = KSEG2;
}

int
cpudebug_fetch_byte(unsigned cpunum, uint32_t va, uint8_t *byte)
{
	uint32_t pa;
	uint32_t aligned_va;
	struct mipscpu *cpu;

	Assert(cpunum < ncpus);

	aligned_va = va & 0xfffffffc;

	/*
	 * For now, only allow KSEG0/1
	 */

	cpu = &mycpus[cpunum];
	if (debug_translatemem(cpu, aligned_va, 0, &pa)) {
		return -1;
	}

	pa |= (va & 3);

	if (bus_mem_fetchbyte(pa, byte)) {
		return -1;
	}
	return 0;
}

int
cpudebug_fetch_word(unsigned cpunum, uint32_t va, uint32_t *word)
{
	uint32_t pa;
	struct mipscpu *cpu;

	Assert(cpunum < ncpus);

	/*
	 * For now, only allow KSEG0/1
	 */
	
	cpu = &mycpus[cpunum];
	if (debug_translatemem(cpu, va, 0, &pa)) {
		return -1;
	}

	if (bus_mem_fetch(pa, word)) {
		return -1;
	}
	return 0;
}

int
cpudebug_store_byte(unsigned cpunum, uint32_t va, uint8_t byte)
{
	uint32_t pa;
	struct mipscpu *cpu;

	Assert(cpunum < ncpus);

	/*
	 * For now, only allow KSEG0/1
	 */

	cpu = &mycpus[cpunum];

	if (debug_translatemem(cpu, va, 1, &pa)) {
		return -1;
	}

	if (bus_mem_storebyte(pa, byte)) {
		return -1;
	}
	return 0;
}

int
cpudebug_store_word(unsigned cpunum, uint32_t va, uint32_t word)
{
	uint32_t pa;
	struct mipscpu *cpu;

	Assert(cpunum < ncpus);

	/*
	 * For now, only allow KSEG0/1.
	 */
	
	cpu = &mycpus[cpunum];
	if (debug_translatemem(cpu, va, 1, &pa)) {
		return -1;
	}

	if (bus_mem_store(pa, word)) {
		return -1;
	}
	return 0;
}

static
inline
void
addreg(uint32_t *regs, int maxregs, int pos, uint32_t val)
{
	if (pos < maxregs) {
		regs[pos] = val;
	}
}

#define GETREG(r) addreg(regs, maxregs, j++, r)

void
cpudebug_getregs(unsigned cpunum, uint32_t *regs, int maxregs, int *nregs)
{
	int i, j=0;
	struct mipscpu *cpu;

	/* choose a CPU */
	Assert(cpunum < ncpus);
	cpu = &mycpus[cpunum];

	for (i=0; i<NREGS; i++) {
		GETREG(cpu->r[i]);
	}
	GETREG(getstatus(cpu));
	GETREG(cpu->lo);
	GETREG(cpu->hi);
	GETREG(cpu->ex_vaddr);
	GETREG(getcause(cpu));
	GETREG(cpu->pc);
	GETREG(0); /* fp status? */
	GETREG(0); /* fp something-else? */
	GETREG(0); /* fp ? */
	GETREG(getindex(cpu));
	GETREG(getrandom(cpu));
	GETREG(tlbgetlo(&cpu->tlbentry));
	GETREG(cpu->ex_context);
	GETREG(tlbgethi(&cpu->tlbentry));
	GETREG(cpu->ex_epc);
	GETREG(cpu->ex_prid);
	*nregs = j;
}

uint32_t
cpuprof_sample(void)
{
	/* for now always use CPU 0 (XXX) */
	return mycpus[0].pc;
}
