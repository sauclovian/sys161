#include <sys/types.h>
#include <stdbool.h>
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

#include "riscv-insn.h"
#include "riscv-ex.h"
#include "riscv-csr.h"
#include "bootrom.h"


/*************************************************************/
/* memory layout */

/*
 * For now we will use the following physical address layout, which
 * allows kernels to be mapped in the top of every user process (as
 * the architecture demands) but also still load with virtual ==
 * physical before MMU initialization. Taking advantage of this means
 * kernel space _must_ start at 0xc0000000, but we can live with that
 * constraint.
 *
 * Like for mips we need a 2M LAMEbus mapping area and we'll allow a
 * 2M bootrom. There is no hardware constraint on where these go,
 * but to allow the kernel page mapping to be 1-1 we'll put them at
 * the top of physical memory.
 */

#define PADDR_RAMBASE 0xc0000000
#define PADDR_ROMBASE 0xffc00000
#define PADDR_BUSBASE 0xffe00000

/* memory access types (size) */
enum memstyles {
	S_SBYTE,
	S_UBYTE,
	S_SHALF,
	S_UHALF,
	S_WORD,
};

/* memory access types (direction/mode) */
enum memrwx {
	RWX_READ,
	RWX_WRITE,
	RWX_EXECUTE,
};


/*************************************************************/
/* register fields */

/*
 * Field masks for control registers
 */

/* Status */
#define STATUS_MXR	0x00080000
#define STATUS_SUM	0x00040000
#define STATUS_SPP	0x00000100
#define STATUS_SPIE	0x00000020
#define STATUS_UPIE	0x00000010
#define STATUS_SIE	0x00000002
#define STATUS_UIE	0x00000001

/* Interrupt Enable */
#define IE_SEIE		0x00000200
#define IE_UEIE		0x00000100
#define IE_STIE		0x00000020
#define IE_UTIE		0x00000010
#define IE_SSIE		0x00000002
#define IE_USIE		0x00000001

/* Interrupt Pending */
#define IP_SEIP		0x00000200
#define IP_UEIP		0x00000100
#define IP_STIP		0x00000020
#define IP_UTIP		0x00000010
#define IP_SSIP		0x00000002
#define IP_USIP		0x00000001

/* Cause */
#define CAUSE_IRQ	0x80000000
#define CAUSE_CODE	0x7fffffff

/* SATP */
#define SATP_MODE	0x80000000
#define SATP_ASID	0x7fc00000
#define SATP_PPN	0x003fffff
#define SATP_ASID_SHIFT 22

/* Page table entries */
#define PTE_V		0x00000001
#define PTE_R		0x00000002
#define PTE_W		0x00000004
#define PTE_X		0x00000008
#define PTE_U		0x00000010
#define PTE_G		0x00000020
#define PTE_A		0x00000040
#define PTE_D		0x00000080
#define PTE_RSW		0x00000300
#define PTE_PPN		0x3ffffc00
#define PTE_UPPER_PPN	0xc0000000


/*************************************************************/
/* cpu definition */

#define NREGS	32

/* possible states for a cpu */
enum cpustates {
	CPU_DISABLED,
	CPU_IDLE,
	CPU_RUNNING,
};

struct riscvcpu {
	// state of this cpu
	enum cpustates state;

	// my cpu number
	unsigned cpunum;

	// are we in supervisor mode?
	bool super;

	// other mode state
	bool Cext;		// C extension (small insns) enabled

	// general registers
	int32_t x[NREGS];

	// program counter
	uint32_t pc;            // pc
	uint32_t pcoff;	        // page offset of pc
	const uint32_t *pcpage;	// precomputed memory page of pc
	uint32_t nextpc;	// pc for next cycle
	uint32_t nextpcoff;	// pcoff for next cycle
	bool trapped;		// took an exception this cycle

	// mmu (satp register at 0x180)
	bool mmu_enable;
	unsigned mmu_asid;
	uint32_t mmu_ptbase_pa;	// paddr of page table base
	const uint32_t *mmu_pttoppage; // memory page of page table base

	// one-entry cache of pagetable lookups
	// (XXX: why cache the paddr and not the page pointer?)
	uint32_t mmu_cached_vpage;
	uint32_t mmu_cached_ppage;
	bool mmu_cached_readable;
	bool mmu_cached_writeable;
	bool mmu_cached_executable;

	/*
         * status register
         *    mstatus at 0x300 (not supported)
         *    sstatus at 0x100
         *    ustatus at 0x000
         *
         * Not-supported bits:
         *    fs (fpu dirty state), reads as 0
         *    xs (extensions dirty state), reads as 0
	 *    sd (true if either fs or xs is nonzero), reads as 0
	 *    mprv (like sum for machine mode), mstatus only
	 *    mpp (like spp for machine mode), mstatus only
	 *    mpie (like spie for machine mode), mstatus only
	 *    mie (like sie for machine mode), mstatus only
	 *    tsr (trap on SRET instruction), mstatus only
	 *    tvm (trap on virtual memory ops), mstatus only
	 *    tw (trap on WFI instruction), mstatus only
         */
	bool status_mxr;	// make executable pages readable
	bool status_sum;	// permit supervisor access to user memory
	bool status_spp;	// 1 if trap was from supervisor mode
	bool status_sie;	// interrupt enable bit
	bool status_spie;	// pre-trap state of sie
	bool status_uie;	
	bool status_upie;
	
        /*
         * interrupt enable register
         *    mie at 0x304 (not supported)
         *    sie at 0x104
         *    uie at 0x004 (not supported)
         */
	bool ie_seie;
	bool ie_stie;
	bool ie_ssie;

        /*
         * interrupt pending register
         *    mip at 0x344 (not supported)
         *    sip at 0x144
         *    uip at 0x044 (not supported)
         */
	bool irq_lamebus;	/* seip */
	bool irq_timer;		/* stip */
	bool irq_ipi;		/* ssip */

        /*
         * trap vectors
         *    mtvec at 0x305 (not supported)
         *    stvec at 0x105
         *    utvec at 0x005 (not supported *)
	 *
	 * Note: we don't support vectored traps.
         */
	uint32_t stvec;

	/*
	 * trap scratch space
	 *    mscratch at 0x340 (not supported)
	 *    sscratch at 0x140
	 *    uscratch at 0x040 (not supported)
	 */
	uint32_t sscratch;

	/*
	 * trap info
	 *    mepc at 0x341 (not supported)
	 *    mcause at 0x342 (not supported)
	 *    mtval at 0x343 (not supported)
	 *    sepc at 0x141
	 *    scause at 0x142
	 *    stval at 0x143
	 *    uepc at 0x041 (not supported)
	 *    ucause at 0x042 (not supported)
	 *    utval at 0x043 (not supported)
	 */
	bool scause_interrupt;
	unsigned scause_trapcode;
	uint32_t stval;
	uint32_t sepc;

	/*
	 * trap delegation
	 *    medeleg (0x302) (not supported)
	 *    mideleg (0x303) (not supported)
	 *    sedeleg at 0x102 (not supported)
	 *    sideleg at 0x103 (not supported)
	 *
	 * The definitions for supervisor-mode delegation are
	 * currently withdrawn.
	 */

	/*
	 * performance counters
	 *    mcounteren at 0x306 (not supported)
	 *    scounteren at 0x106
	 */
	

	/*
         * additional machine-mode-only control registers not modeled:
         *    mvendorid (0xf11)
         *    marchid (0xf12)
         *    mimpid (0xf13)
         *    misa (0x301)
         *    mtime
         *    mtimecmp
	 *    mcycle (0xb00)
	 *    mcountinhibit (0x320)
	 *    minstret (0xb02)
	 *    mhpmcounter* (0xb03-0xb1f)
	 *    mhpmevent* (0x323-0x33f)
	 *    pmpcfg*
	 *    pmpaddr*
         */

	/*
	 * timer
	 */
	uint64_t cyclecount;
	uint64_t cycletrigger;

	/*
	 * LL/SC hooks
	 */
	bool lr_active;
	uint32_t lr_addr;
	uint32_t lr_value;

	/*
	 * debugger hooks
	 */
	bool hit_breakpoint;
};

/* use a non-aligned page address (unmatchable) to mean nothing cached */
#define INVALID_CACHED_VPAGE	0xffffffff

#define IS_USERMODE(cpu) (!(cpu)->super)

static struct riscvcpu *mycpus;
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
/* names */

#define EXCEPTION_NUM 16
static const char *exception_names[EXCEPTION_NUM] = {
	"Unaligned access - instruction",
	"Access fault - instruction",
	"Illegal instruction",
	"Breakpoint",
	"Unaligned access - load",
	"Access fault - load",
	"Unaligned access - store",
	"Access fault - store",
	"System call",
	"Hypervisor call",
	"<reserved #10>",
	"<reserved #11>",
	"Page fault - instruction",
	"Page fault - load",
	"<reserved #14>",
	"Page fault - store",
};

#define INTERRUPT_NUM 10
static const char *interrupt_names[INTERRUPT_NUM] = {
	"Software interrupt - user",
	"Software interrupt - supervisor",
	"<reserved #2>",
	"<reserved #3>",
	"Timer interrupt - user",
	"Timer interrupt - supervisor",
	"<reserved #6>",
	"<reserved #7>",
	"External interrupt - user",
	"External interrupt - supervisor",
};

static
const char *
exception_name(int code)
{
	if (code >= 0 && code < EXCEPTION_NUM) {
		return exception_names[code];
	}
	smoke("Name of invalid exception code requested");
	return "???";
}

static
const char *
interrupt_name(int code)
{
	if (code >= 0 && code < INTERRUPT_NUM) {
		return interrupt_names[code];
	}
	smoke("Name of invalid interrupt code requested");
	return "???";
}

static
const char *
regname(unsigned reg)
{
	switch (reg) {
	    case 0: return "zero";
	    case 1: return "ra";
	    case 2: return "sp";
	    case 3: return "gp";
	    case 4: return "tp";
	    case 5: return "t0";
	    case 6: return "t1";
	    case 7: return "t2";
	    case 8: return "s0";
	    case 9: return "s1";
	    case 10: return "a0";
	    case 11: return "a1";
	    case 12: return "a2";
	    case 13: return "a3";
	    case 14: return "a4";
	    case 15: return "a5";
	    case 16: return "a6";
	    case 17: return "a7";
	    case 18: return "s2";
	    case 19: return "s3";
	    case 20: return "s4";
	    case 21: return "s5";
	    case 22: return "s6";
	    case 23: return "s7";
	    case 24: return "s8";
	    case 25: return "s9";
	    case 26: return "s10";
	    case 27: return "s11";
	    case 28: return "t3";
	    case 29: return "t4";
	    case 30: return "t5";
	    case 31: return "t6";
	}
	return "??";
}


/*************************************************************/
/* support code used by the CPU builds */

/* XXX move this to somewhere all the cpu code can share it */
static
inline
uint32_t
signedshift(uint32_t val, unsigned amt)
{
	/* There's no way to express a signed shift directly in C. */
	uint32_t result;
	result = val >> amt;
	if (val & 0x80000000) {
		result |= (0xffffffff << (31-amt));
	}
	return result;
}

/*
 * How to trace the current instruction. This is set only when
 * tracing; it is overwritten by each cpu in turn as its instruction
 * executes. It caches IS_USERMODE(cpu) ? DOTRACE_UINSN : DOTRACE_KINSN.
 */
static int tracehow;		// how to trace the current instruction

/*
 * hook for stopping cpu_cycles() early
 */
static int cpu_cycling;

/*
 * Control register consing
 */

static
uint32_t
read_csr_sstatus(struct riscvcpu *cpu)
{
	return (cpu->status_mxr ? STATUS_MXR : 0) |
		(cpu->status_sum ? STATUS_SUM : 0) |
		(cpu->status_spp ? STATUS_SPP : 0) |
		(cpu->status_spie ? STATUS_SPIE : 0) |
		(cpu->status_upie ? STATUS_UPIE : 0) |
		(cpu->status_sie ? STATUS_SIE : 0) |
		(cpu->status_uie ? STATUS_UIE : 0);
}

static
void
write_csr_sstatus(struct riscvcpu *cpu, uint32_t val)
{
	cpu->status_mxr = (val & STATUS_MXR) != 0;
	cpu->status_sum = (val & STATUS_SUM) != 0;
	cpu->status_spp = (val & STATUS_SPP) != 0;
	cpu->status_spie = (val & STATUS_SPIE) != 0;
	cpu->status_upie = (val & STATUS_UPIE) != 0;
	cpu->status_sie = (val & STATUS_SIE) != 0;
	cpu->status_uie = (val & STATUS_UIE) != 0;
}

static
uint32_t
read_csr_sie(struct riscvcpu *cpu)
{
	return (cpu->ie_seie ? IE_SEIE : 0) |
		(cpu->ie_stie ? IE_STIE : 0) |
		(cpu->ie_ssie ? IE_SSIE : 0);
}

static
void
write_csr_sie(struct riscvcpu *cpu, uint32_t val)
{
	cpu->ie_seie = (val & IE_SEIE) != 0;
	cpu->ie_stie = (val & IE_STIE) != 0;
	cpu->ie_ssie = (val & IE_SSIE) != 0;
}

static
uint32_t
read_csr_sip(struct riscvcpu *cpu)
{
	return (cpu->irq_lamebus ? IP_SEIP : 0) |
		(cpu->irq_timer ? IP_STIP : 0) |
		(cpu->irq_ipi ? IP_SSIP : 0);
}

static
void
write_csr_sip(struct riscvcpu *cpu, uint32_t val)
{
	// changing these from software is not allowed
	//cpu->irq_lamebus = (val & IP_SEIP) != 0;
	//cpu->irq_timer = (val & IP_STIP) != 0;
	cpu->irq_ipi = (val & IP_SSIP) != 0;
}

static
uint32_t
read_csr_sepc(struct riscvcpu *cpu)
{
	/* C extension disabled -> bit 1 is hidden (bit 0 is 0) */
	if (!cpu->Cext) {
		return cpu->sepc & 0xfffffffc;
	}
	else {
		return cpu->sepc;
	}
}

static
void
write_csr_sepc(struct riscvcpu *cpu, uint32_t val)
{
	/* bit 0 is always 0 */
	cpu->sepc = val & 0xfffffffe;
}

static
uint32_t
read_csr_scause(struct riscvcpu *cpu)
{
	return (cpu->scause_interrupt ? CAUSE_IRQ : 0) |
		cpu->scause_trapcode;
}

static
void
write_csr_scause(struct riscvcpu *cpu, uint32_t val)
{
	cpu->scause_interrupt = (val & CAUSE_IRQ) != 0;
	/*
	 * While the code can theoretically fill the whole register,
	 * we only allow the bottom four bits.
	 */
	cpu->scause_trapcode = (val & CAUSE_CODE) & 0xf;
}

static
uint32_t
read_csr_satp(struct riscvcpu *cpu)
{
	return (cpu->mmu_enable ? SATP_MODE : 0) |
		(cpu->mmu_asid << SATP_ASID_SHIFT) |
		(cpu->mmu_ptbase_pa >> 12);
}

static
void
write_csr_satp(struct riscvcpu *cpu, uint32_t val)
{
	cpu->mmu_enable = (val & SATP_MODE) != 0;
	cpu->mmu_asid = (val >> SATP_ASID_SHIFT) & 0x1ff;
	/* we don't support the extra two upper bits of PA */
	cpu->mmu_ptbase_pa = ((val & SATP_PPN) & 0xfffff) << 12;

	// clear the translation cache because it isn't asid-indexed
	cpu->mmu_cached_vpage = 0xffffffff;
}


/*************************************************************/
/* CPU builds */

#define CPUNAME rv32

#undef USE_TRACE
#include "riscvcore.h"

#define USE_TRACE
#include "riscvcore.h"
#undef USE_TRACE


/*************************************************************/
/* dispatching to CPU builds */

static int tracing;

#define DISPATCH_0(sym) \
	(tracing ? rv32_trace_##sym() : rv32_##sym())

#define DISPATCH_1(sym, a0) \
	(tracing ? rv32_trace_##sym(a0) : rv32_##sym(a0))

#define DISPATCH_2(sym, a0, a1) \
	(tracing ? rv32_trace_##sym(a0, a1) : rv32_##sym(a0, a1))

#define DISPATCH_3(sym, a0, a1, a2) \
	(tracing ? rv32_trace_##sym(a0, a1, a2) : rv32_##sym(a0, a1, a2))

#define DISPATCH_4(sym, a0, a1, a2, a3) \
	(tracing ? rv32_trace_##sym(a0, a1, a2, a3) : \
		   rv32_##sym(a0, a1, a2, a3))

uint64_t
cpu_cycles(uint64_t maxcycles)
{
	return DISPATCH_1(cpu_cycles, maxcycles);
}

static
int
precompute_pc(struct riscvcpu *cpu)
{
	return DISPATCH_1(precompute_pc, cpu);
}

static
int
debug_translatemem(const struct riscvcpu *cpu, uint32_t vaddr, 
		   int iswrite, uint32_t *ret)
{
	return DISPATCH_4(debug_translatemem, cpu, vaddr, iswrite, ret);
}

/*************************************************************/
/* common code across all cpu builds */

static
void
riscv_init(struct riscvcpu *cpu, unsigned cpunum)
{
	unsigned i;

	cpu->state = CPU_DISABLED;
	cpu->cpunum = cpunum;
	cpu->super = true;
	cpu->Cext = true;
	for (i=0; i<NREGS; i++) {
		cpu->x[i] = 0;
	}
	cpu->pc = PADDR_ROMBASE + 0x100;
	/* pcoff and pcpage are set by precompute_pc below */
	cpu->nextpc = 0; /* computed during execution */
	cpu->nextpcoff = 0; /* computed during execution */
	cpu->trapped = false;

	cpu->mmu_enable = false;
	cpu->mmu_asid = 0;
	cpu->mmu_ptbase_pa = 0;
	cpu->mmu_pttoppage = NULL;

	cpu->mmu_cached_vpage = INVALID_CACHED_VPAGE;
	cpu->mmu_cached_ppage = 0;
	cpu->mmu_cached_readable = false;
	cpu->mmu_cached_writeable = false;
	cpu->mmu_cached_executable = false;

	cpu->status_mxr = 0;
	cpu->status_sum = 0;
	cpu->status_spp = 0;
	cpu->status_sie = 0;
	cpu->status_spie = 0;
	cpu->status_uie = 0;
	cpu->status_upie = 0;

	cpu->ie_seie = 0;
	cpu->ie_stie = 0;
	cpu->ie_ssie = 0;

	cpu->irq_lamebus = 0;
	cpu->irq_timer = 0;
	cpu->irq_ipi = 0;

	cpu->stvec = PADDR_ROMBASE;

	cpu->sscratch = 0;

	cpu->scause_interrupt = 0;
	cpu->scause_trapcode = 0;
	cpu->stval = 0;
	cpu->sepc = 0;

	cpu->cyclecount = 0;
	cpu->cycletrigger = (uint64_t)-1;

	cpu->lr_active = false;
	cpu->lr_addr = 0;
	cpu->lr_value = 0;

	cpu->hit_breakpoint = false;

	if (precompute_pc(cpu)) {
		smoke("precompute_pc failed in riscv_init");
	}
}

/*************************************************************/
/* external interface: initialization and loading */

void
cpu_init(unsigned numcpus)
{
	unsigned i;

	Assert(numcpus <= 32);

	ncpus = numcpus;
	mycpus = domalloc(ncpus * sizeof(*mycpus));
	for (i=0; i<numcpus; i++) {
		riscv_init(&mycpus[i], i);
	}

	mycpus[0].state = CPU_RUNNING;
	cpu_running_mask = 0x1;
}

#define BETWEEN(addr, size, base, top) \
          ((addr) >= (base) && (size) <= (top)-(base) && (addr)+(size) < (top))

int
cpu_get_load_paddr(uint32_t vaddr, uint32_t size, uint32_t *paddr)
{
	/*
	 * Load with PA == VA between 0xc0000000 and 0xffc00000
	 */
	if (!BETWEEN(vaddr, size, PADDR_RAMBASE, PADDR_ROMBASE)) {
		return -1;
	}
	*paddr = vaddr;
	return 0;
}

int
cpu_get_load_vaddr(uint32_t paddr, uint32_t size, uint32_t *vaddr)
{
	if (!BETWEEN(paddr, size, PADDR_RAMBASE, PADDR_ROMBASE)) {
		return -1;
	}
	*vaddr = paddr;
	return 0;
}

uint32_t
cpu_get_ram_paddr(void)
{
	/* paddr of base of physical ram */
	return 0xc0000000;
}

void
cpu_set_entrypoint(unsigned cpunum, uint32_t addr)
{
	struct riscvcpu *cpu;
	uint32_t alignmask;

	Assert(cpunum < ncpus);
	cpu = &mycpus[cpunum];

	alignmask = cpu->Cext ? 0xfffffffe : 0xfffffffc;
	if ((addr & alignmask) != addr) {
		hang("Kernel entry point is not properly aligned");
		addr &= alignmask;
	}
	cpu->pc = addr;
	if (precompute_pc(cpu)) {
		hang("Kernel entry point is an invalid address");
	}
}

void
cpu_set_stack(unsigned cpunum, uint32_t stackaddr, uint32_t argument)
{
	struct riscvcpu *cpu;

	Assert(cpunum < ncpus);
	cpu = &mycpus[cpunum];

	cpu->x[2] = stackaddr;   /* register 2: stack pointer */
	cpu->x[10] = argument;   /* register 10: first argument */
	
	/* don't need to set gp - in the ELF model it's start's problem */
}

/*************************************************************/
/* external interface: operation */

void
cpu_stopcycling(void)
{
	cpu_cycling = 0;
}

void
cpu_set_irqs(unsigned cpunum, int lamebus, int ipi)
{
	bool eie, sie;
	Assert(cpunum < ncpus);

	struct riscvcpu *cpu;

	Assert(cpunum < ncpus);
	cpu = &mycpus[cpunum];
	cpu->irq_lamebus = lamebus;
	cpu->irq_ipi = ipi;

	/* cpu->irq_timer is on-chip, and cannot get set when CPU_IDLE */
	/* XXX: not entirely true for riscv... */

	eie = cpu->super ? cpu->ie_seie : true/*cpu->ie_ueie*/;
	/*tie = cpu->super ? cpu->ie_stie : cpu->ie_utie;*/
	sie = cpu->super ? cpu->ie_ssie : true/*cpu->ie_usie*/;

	/* XXX shouldn't be using CPUTRACE outside riscvcore.h */
	CPUTRACE(DOTRACE_IRQ, cpunum,
		 "cpu_set_irqs: LB %s%s IPI %s%s",
		 lamebus ? "ON" : "off", eie ? "" : " (masked)",
		 ipi ? "ON" : "off", sie ? "" : " (masked)");

	// only wake up if not masked in the IE register
	if (cpu->state == CPU_IDLE && ((lamebus && eie) || (ipi && sie))) {
		cpu->state = CPU_RUNNING;
		RUNNING_MASK_ON(cpunum);
		CPUTRACE(DOTRACE_IRQ, cpunum, "cpu_set_irqs: waking up");
	}
}

uint32_t
cpu_get_secondary_start_stack(uint32_t lboffset)
{
	/* lboffset is the offset from the LAMEbus mapping base. */
	return PADDR_BUSBASE + lboffset;
}

void
cpu_enable(unsigned cpunum)
{
	struct riscvcpu *cpu;

	Assert(cpunum < ncpus);
	cpu = &mycpus[cpunum];

	cpu->state = CPU_RUNNING;
	RUNNING_MASK_ON(cpunum);
}

void
cpu_disable(unsigned cpunum)
{
	struct riscvcpu *cpu;

	Assert(cpunum < ncpus);
	cpu = &mycpus[cpunum];

	cpu->state = CPU_DISABLED;
	RUNNING_MASK_OFF(cpunum);
}

int
cpu_enabled(unsigned cpunum)
{
	struct riscvcpu *cpu;

	Assert(cpunum < ncpus);
	cpu = &mycpus[cpunum];

	return (cpu->state != CPU_DISABLED);
}


/*************************************************************/
/* external interface: misc */

void
cpu_set_tracing(int on)
{
	tracing = on;
}

void
cpu_dumpstate(void)
{
	struct riscvcpu *cpu;
	int i;
	unsigned j;

	msg("%u cpus: RV32 (RV32IMAZicsr_Zifencei)", ncpus);
	for (j=0; j<ncpus; j++) {
		cpu = &mycpus[j];
		msg("cpu %d:", j);

		for (i=0; i<NREGS; i++) {
			msgl("x%d:%s 0x%08lx  ", i, i<10 ? " " : "",
			     (unsigned long)(uint32_t) cpu->x[i]);
			if (i%4==3) {
				msg(" ");
			}
		}
		msg("pc:  0x%08lx  mode: %s", 
		    (unsigned long)(uint32_t) cpu->pc,
		    cpu->super ? "supervisor" : "user");
		msg("mmu: %s  asid: 0x%03x  base: 0x%08lx",
		    cpu->mmu_enable ? "enabled " : "disabled",
		    cpu->mmu_asid,
		    (unsigned long)(uint32_t) cpu->mmu_ptbase_pa);
		msgl("status register: -........");
		msg("---%s%s-00--..%s-.%s%s-.%s%s",
		     cpu->status_mxr ? "X" : "-",
		     cpu->status_sum ? "U" : "-",
		     cpu->status_spp ? "p" : "-",
		     cpu->status_spie ? "s" : "-",
		     cpu->status_upie ? "u" : "-",
		     cpu->status_sie ? "S" : "-",
		     cpu->status_uie ? "U" : "-");
		msg("interrupt enable register: -.%s%s-.%s%s-.%s%s",
		    cpu->ie_seie ? "e" : "-",
		    /*cpu->ie_ueie*/ true ? "e" : "-",
		    cpu->ie_stie ? "t" : "-",
		    /*cpu->ie_utie*/ true ? "t" : "-",
		    cpu->ie_ssie ? "s" : "-",
		    /*cpu->ie_usie*/ true ? "s" : "-");
		msg("interrupt pending register: -.%s%s-.%s%s-.%s%s",
		    cpu->irq_lamebus ? "e" : "-",
		    /*cpu->ip_ueip*/ false ? "e" : "-",
		    cpu->irq_timer ? "t" : "-",
		    /*cpu->ip_utip*/ false ? "t" : "-",
		    cpu->irq_ipi ? "s" : "-",
		    /*cpu->ip_usip*/ false ? "s" : "-");
#if 0
		msg("trap vectors: supervisor 0x%08lx  user 0x%08lx",
		    (unsigned long)(uint32_t) cpu->stvec,
		    (unsigned long)(uint32_t) cpu->utvec);
		msg("scratch: supervisor 0x%08lx  user 0x%08lx",
		    (unsigned long)(uint32_t) cpu->sscratch,
		    (unsigned long)(uint32_t) cpu->uscratch);
#else
		msg("trap vectors: supervisor 0x%08lx  user not implemented",
		    (unsigned long)(uint32_t) cpu->stvec);
		msg("scratch: supervisor 0x%08lx  user not implemented",
		    (unsigned long)(uint32_t) cpu->sscratch);
#endif
		msg("scause: %s %u, stval 0x%08lx",
		    cpu->scause_interrupt ? "irq" : "exn",
		    cpu->scause_trapcode,
		    (unsigned long)(uint32_t) cpu->stval);
#if 0
		msg("ucause: %s %u, utval 0x%08lx",
		    cpu->ucause_interrupt ? "irq" : "exn",
		    cpu->ucause_trapcode,
		    (unsigned long)(uint32_t) cpu->utval);
#endif
	}
}

uint32_t
cpuprof_sample(void)
{
	/* for now always use CPU 0 (XXX) */
	return mycpus[0].pc;
}


/*************************************************************/
/* external interface: debugging */

unsigned
cpu_numcpus(void)
{
	return ncpus;
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
	*start = PADDR_RAMBASE;
	*end = PADDR_ROMBASE;
}

int
cpudebug_fetch_byte(unsigned cpunum, uint32_t va, uint8_t *byte)
{
	uint32_t pa;
	uint32_t aligned_va;
	struct riscvcpu *cpu;

	Assert(cpunum < ncpus);

	aligned_va = va & 0xfffffffc;

	cpu = &mycpus[cpunum];
	if (debug_translatemem(cpu, aligned_va, 0, &pa)) {
		return -1;
	}

	pa |= (va & 3);

	if (pa < PADDR_RAMBASE || pa >= PADDR_ROMBASE) {
		return -1;
	}

	if (bus_mem_fetchbyte(pa - PADDR_RAMBASE, byte)) {
		return -1;
	}
	return 0;
}

int
cpudebug_fetch_word(unsigned cpunum, uint32_t va, uint32_t *word)
{
	uint32_t pa;
	struct riscvcpu *cpu;

	Assert(cpunum < ncpus);

	cpu = &mycpus[cpunum];
	if (debug_translatemem(cpu, va, 0, &pa)) {
		return -1;
	}

	if (pa < PADDR_RAMBASE || pa >= PADDR_ROMBASE) {
		return -1;
	}

	if (bus_mem_fetch(pa - PADDR_RAMBASE, word)) {
		return -1;
	}
	return 0;
}

int
cpudebug_store_byte(unsigned cpunum, uint32_t va, uint8_t byte)
{
	uint32_t pa;
	struct riscvcpu *cpu;

	Assert(cpunum < ncpus);

	/*
	 * For now, only allow KSEG0/1
	 */

	cpu = &mycpus[cpunum];

	if (debug_translatemem(cpu, va, 1, &pa)) {
		return -1;
	}

	if (pa < PADDR_RAMBASE || pa >= PADDR_ROMBASE) {
		return -1;
	}

	if (bus_mem_storebyte(pa - PADDR_RAMBASE, byte)) {
		return -1;
	}
	return 0;
}

int
cpudebug_store_word(unsigned cpunum, uint32_t va, uint32_t word)
{
	uint32_t pa;
	struct riscvcpu *cpu;

	Assert(cpunum < ncpus);

	/*
	 * For now, only allow KSEG0/1.
	 */
	
	cpu = &mycpus[cpunum];
	if (debug_translatemem(cpu, va, 1, &pa)) {
		return -1;
	}

	if (pa < PADDR_RAMBASE || pa >= PADDR_ROMBASE) {
		return -1;
	}

	if (bus_mem_store(pa - PADDR_RAMBASE, word)) {
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
	struct riscvcpu *cpu;

	/* choose a CPU */
	Assert(cpunum < ncpus);
	cpu = &mycpus[cpunum];

	for (i=0; i<NREGS; i++) {
		GETREG(cpu->x[i]);
	}
	/* XXX what registers should be here and in what order? */
	GETREG(cpu->pc);
	*nregs = j;
}

