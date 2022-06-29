/*
 * This file is included repeatedly, once for each distinct build of
 * the cpu.
 */

#undef FN

#define FN__(name, sym) name ## sym
#define FN_(name, sym) FN__(name, sym)
#ifdef USE_TRACE
#define FN(sym) FN_(CPUNAME, _trace_ ## sym)
#else
#define FN(sym) FN_(CPUNAME, _ ## sym)
#endif

#include "cputrace.h"

/* this is further down */
static int FN(precompute_pc)(struct riscvcpu *cpu);

static
void
FN(trap)(struct riscvcpu *cpu, bool isirq, int code, uint32_t val,
	 const char *info)
{
	CPUTRACE(DOTRACE_EXN, cpu->cpunum,
		 "trap: %s, code %d (%s%s), pc %x, value %x, sp %x", 
		 isirq ? "interrupt" : "exception",
		 code, isirq ? interrupt_name(code) : exception_name(code),
		 info,
		 cpu->pc, val, cpu->x[2]);

	if (isirq) {
		g_stats.s_irqs++;
	}
	else {
		g_stats.s_exns++;
	}

	if (cpu->pc == cpu->stvec) {
		msg("Recursive trap: faulted entering trap handler");
		msg("Exception PC from original trap: 0x%lx",
		    (unsigned long)cpu->sepc);
		msg("This trap: %u (%s%s)",
		    code,
		    isirq ? interrupt_name(code) : exception_name(code),
		    info);
		msg("Trap handler: 0x%lx", (unsigned long)cpu->stvec);
		hang("The system is wedged, sorry.");
		/*
		 * Note: hang causes execution to stop and triggers the
		 * debugger, but for that to work we need to unwind,
		 * which means we need to complete this cycle. As things
		 * are currently set up, we can't, because we need a
		 * mapping for the address under the PC and there isn't
		 * one. FUTURE: maybe move this test into translatemem
		 * and provide a private extra page of memory containing
		 * an infinite loop (or maybe, the poweroff sequence)
		 * that we can map in an emergency. Then we can continue
		 * from the hang and let the debugger do its thing.
		 *
		 * Note also that we'll get here not just if there's no
		 * translation for the address in stvec (which is what
		 * this case is intended to catch) but also if it's an
		 * illegal instruction. XXX: this is not optimal.
		 */
		crashdie();
	}

	// load the trap info
	cpu->scause_interrupt = isirq;
	cpu->scause_trapcode = code;
	cpu->stval = val;

	// save the current processor status and go to supervisor mode
	cpu->status_spp = cpu->super;
	cpu->status_spie = cpu->status_sie;
	cpu->status_sie = false;
	cpu->super = true;

	CPUTRACE(DOTRACE_IRQ, cpu->cpunum,
		 "after trap: spie %s sie %s",
		 cpu->status_spie ? "enabled" : "disabled",
		 cpu->status_sie ? "enabled" : "disabled");

	// save the current pc and start at the trap vector
	cpu->sepc = cpu->pc;
	cpu->pc = cpu->stvec;
	cpu->nextpc = cpu->pc;

	// remember that we hit a trap (used by progress monitoring)
	cpu->trapped = true;

	// Now update the cached pc mapping.
	//
	// If this triggers another exception we'll hit the recursive
	// trap check above and not come back.
	(void) FN(precompute_pc)(cpu);
}

static
inline
void
FN(exception)(struct riscvcpu *cpu, int code, uint32_t val, const char *info)
{
	FN(trap)(cpu, false/*isirq*/, code, val, info);
}

static
inline
void
FN(interrupt)(struct riscvcpu *cpu, int code, const char *info)
{
	FN(trap)(cpu, true/*isirq*/, code, 0/*val*/, info);
}

static
inline
int
FN(accessmem)(const struct riscvcpu *cpu, uint32_t paddr, int iswrite, uint32_t *val)
{
	/*
	 * Physical memory layout: 
	 *    0x00000000 - 0xc0000000     nothing
	 *    0xc0000000 - 0xffbfffff     RAM
	 *    0xffc00000 - 0xffdfffff     Boot ROM
	 *    0xffe00000 - 0xffffffff     LAMEbus mapped I/O
	 */
	
	if (paddr < PADDR_RAMBASE) {
		return -1;
	}
	else if (paddr < PADDR_ROMBASE) {
		/* between RAM base and ROM base: RAM */
		if (iswrite) {
			return bus_mem_store(paddr - PADDR_RAMBASE, *val);
		}
		else {
			return bus_mem_fetch(paddr - PADDR_RAMBASE, val);
		}
	}
	else if (paddr < PADDR_BUSBASE) {
		/* between ROM base and I/O base: ROM */
		if (iswrite) {
			return -1; /* ROM is, after all, read-only */
		}
		else {
			return bootrom_fetch(paddr - PADDR_ROMBASE, val);
		}
	}
	else {
		/* at or above I/O base: bus I/O */
		if (iswrite) {
			return bus_io_store(cpu->cpunum,
					    paddr - PADDR_BUSBASE, *val);
		}
		else {
			return bus_io_fetch(cpu->cpunum,
					    paddr - PADDR_BUSBASE, val);
		}
	}

	return 0;
}

/*
 * helper for translatemem: post a page fault exception
 */
static
void
FN(pagefault)(struct riscvcpu *cpu, uint32_t vaddr, enum memrwx rwx,
	      const char *msg)
{
	int code;

	switch (rwx) {
	    case RWX_READ: code = EX_LPAGE; break;
	    case RWX_WRITE: code = EX_SPAGE; break;
	    case RWX_EXECUTE: code = EX_IPAGE; break;
	}
	CPUTRACE(DOTRACE_TLB, cpu->cpunum, msg);
	FN(exception)(cpu, code, vaddr, msg);
}

/*
 * helper for translatemem: post an access fault exception
 */
static
void
FN(accessfault)(struct riscvcpu *cpu, uint32_t vaddr, enum memrwx rwx,
		const char *msg)
{
	int code;

	switch (rwx) {
	    case RWX_READ: code = EX_LACCESS; break;
	    case RWX_WRITE: code = EX_SACCESS; break;
	    case RWX_EXECUTE: code = EX_IACCESS; break;
	}
	CPUTRACE(DOTRACE_TLB, cpu->cpunum, msg);
	FN(exception)(cpu, code, vaddr, msg);
}

/*
 * Warning: do not change this function without making corresponding
 * changes to debug_translatemem (below).
 */
static
inline
int
FN(translatemem)(struct riscvcpu *cpu, uint32_t vaddr, enum memrwx rwx,
		 uint32_t *ret)
{
	uint32_t vpage, offset;
	uint32_t entry, upperbits;
	uint32_t ptpaddr, paddr;
	unsigned ix;
	bool ok;
	bool superpage;

	/*
	 * Unaligned accesses should be handled (or rejected) before
	 * we get here.
	 * Update: yes, but not for branches to half-sized instructions.
	 */
	//Assert((vaddr & 0x3) == 0);
	Assert((vaddr & 0x1) == 0);

	/* first thing: check if paging is enabled at all... */
	if (!cpu->mmu_enable) {
		*ret = vaddr;
		return 0;
	}

	vpage = vaddr & 0xfffff000;

	CPUTRACEL(DOTRACE_TLB, cpu->cpunum,
		  "mmu: vpn 0x%x", vpage >> 12);

	if (vpage == cpu->mmu_cached_vpage) {
		switch (rwx) {
		    case RWX_READ:
			if (!cpu->mmu_cached_readable) {
				FN(pagefault)(cpu, vaddr, rwx,
					      ", not readable (cached)");
				return -1;
			}
			break;
		    case RWX_WRITE:
			if (!cpu->mmu_cached_writeable) {
				FN(pagefault)(cpu, vaddr, rwx,
					      ", not writeable (cached)");
				return -1;
			}
			break;
		    case RWX_EXECUTE:
			if (!cpu->mmu_cached_executable) {
				FN(pagefault)(cpu, vaddr, rwx,
					      " - not executable (cached)"); 
				return -1;
			}
			break;
		}
		CPUTRACE(DOTRACE_TLB, cpu->cpunum,
			 " - cached ppn 0x%x", cpu->mmu_cached_ppage >> 12);
		/* the cached page is never a superpage */
		offset = vaddr & 0x00000fff;
		*ret = cpu->mmu_cached_ppage | offset;
		return 0;
	}

	/*
	 * Need to visit the pagetable.
	 *
	 * The pagetable is a normal 32-bit, 4k-page pagetable.
	 */

	if (cpu->mmu_pttoppage == NULL) {
		FN(accessfault)(cpu, vaddr, rwx, ", invalid pagetable base");
		return -1;
	}

	// index the top level (>> 22, << 2)
	ix = (vaddr & 0xffc00000) >> 20;
	entry = bus_use_map(cpu->mmu_pttoppage, ix);

	CPUTRACEL(DOTRACE_TLB, cpu->cpunum,
		  ", pd at 0x%05x, index 0x%02x, entry 0x%08x",
		  cpu->mmu_ptbase_pa, ix, entry);

	if ((entry & PTE_V) == 0) {
		/* top-level entry not valid */
		FN(pagefault)(cpu, vaddr, rwx, ", top-level entry not valid");
		return -1;
	}
	if ((entry & (PTE_R | PTE_W | PTE_X)) != 0) {
		/* Upper-level entry is a leaf, so it's a superpage */
		CPUTRACEL(DOTRACE_TLB, cpu->cpunum,
			  " - superpage");
		/* save the extra upper bits of the paddr for later */
		upperbits = entry & PTE_UPPER_PPN;
		paddr = (entry & PTE_PPN) << 2;
		offset = vaddr & 0x003fffff;

		if ((paddr & 0x003ff000) != 0) {
			/* unaligned superpage */
			/* spec specifically says to use PAGE not ALIGN here */
			FN(pagefault)(cpu, vaddr, rwx,", unaligned superpage");
			return -1;
		}
		superpage = true;
	}
	else {
		/*
		 * Physical page numbers have two extra bits, which we
		 * don't support, so if they're set it's a bus error.
		 * We need to check this here (regardless of exception
		 * priority) because we need to use the page number we
		 * got.
		 */
		if ((entry & PTE_UPPER_PPN) != 0) {
			CPUTRACE(DOTRACE_TLB, cpu->cpunum, " - OUT OF RANGE");
			FN(accessfault)(cpu, vaddr, rwx,
					", pagetable PPN out of range");
			return -1;
		}

		ptpaddr = (entry & PTE_PPN) << 2;
		CPUTRACEL(DOTRACE_TLB, cpu->cpunum, ", ptp at 0x%x",
			  ptpaddr >> 12);

		// index the second level (>> 12, << 2)
		ix = (vaddr & 0x003ff000) >> 10;
		CPUTRACEL(DOTRACE_TLB, cpu->cpunum, ", index 0x%x", ix);
		if (FN(accessmem)(cpu, ptpaddr + ix, false/*iswrite*/, &entry)) {
			FN(accessfault)(cpu, vaddr, rwx,
					 ", bus error on pagetable");
			CPUTRACE(DOTRACE_TLB, cpu->cpunum, " - BUS ERROR");
			return -1;
		}

		CPUTRACEL(DOTRACE_TLB, cpu->cpunum, ", pte 0x%08x", entry);
		if ((entry & PTE_V) == 0) {
			/* entry not valid */
			FN(pagefault)(cpu, vaddr, rwx, ", entry not valid");
			return -1;
		}

		if ((entry & PTE_UPPER_PPN) != 0) {
			CPUTRACE(DOTRACE_TLB, cpu->cpunum, " - OUT OF RANGE");
			FN(accessfault)(cpu, vaddr, rwx, ", PPN out of range");
			return -1;
		}

		/* save the extra upper bits of the paddr for later */
		upperbits = entry & PTE_UPPER_PPN;
		paddr = (entry & PTE_PPN) << 2;
		offset = vaddr & 0x00000fff;
		superpage = false;
	}
	CPUTRACEL(DOTRACE_TLB, cpu->cpunum, ", ppn 0x%x", paddr >> 12);

	if (cpu->super && (entry & PTE_U) != 0) {
		if (rwx == RWX_EXECUTE || !cpu->status_sum) {
			FN(pagefault)(cpu, vaddr, rwx,
				      ", user page from kernel");
			return -1;
		}
	}
	else if (!cpu->super && (entry & PTE_U) == 0) {
		FN(pagefault)(cpu, vaddr, rwx, ", kernel page from user");
		return -1;
	}

	switch (rwx) {
	    case RWX_READ: ok = entry & PTE_R; break;
	    case RWX_WRITE: ok = entry & PTE_W; break;
	    case RWX_EXECUTE: ok = entry & PTE_X; break;
	}
	if (ok == false) {
		FN(pagefault)(cpu, vaddr, rwx, ", no page permission");
		return -1;
	}

	if (rwx == RWX_WRITE && (entry & PTE_D) == 0) {
		FN(pagefault)(cpu, vaddr, rwx, ", page not marked dirty");
		return -1;
	}

	if ((entry & PTE_A) == 0) {
		FN(pagefault)(cpu, vaddr, rwx, ", page not marked accessed");
		return -1;
	}

	/*
	 * Defer checking the upper bits of the paddr until after
	 * we've checked for page faults, to preserve the exception
	 * priorities documented in the architecture manual.
	 */
	if (upperbits != 0) {
		FN(accessfault)(cpu, vaddr,rwx,", superpage PPN out of range");
		return -1;
	}

	CPUTRACE(DOTRACE_TLB, cpu->cpunum, " - OK");

	/* update the cache */
	if (!superpage) {
		cpu->mmu_cached_vpage = vpage;
		cpu->mmu_cached_ppage = paddr;
		cpu->mmu_cached_readable = (entry & PTE_R) != 0;
		cpu->mmu_cached_writeable = (entry & (PTE_W | PTE_D)) == (PTE_W | PTE_D);
		cpu->mmu_cached_executable = (entry & PTE_X) != 0;
	}

	*ret = paddr | offset;
	return 0;
}

/*
 * Special version of translatemem for use from the gdb access code. It
 * does not touch the cpu state and always considers itself to be in 
 * supervisor mode.
 *
 * Warning: do not change this function without making corresponding
 * changes to the original translatemem (above).
 */
static
inline
int
FN(debug_translatemem)(const struct riscvcpu *cpu, uint32_t vaddr, 
		   enum memrwx rwx, uint32_t *ret)
{
	uint32_t vpage, offset;
	uint32_t entry, upperbits;
	uint32_t ptpaddr, paddr;
	unsigned ix;
	bool ok;
	//bool superpage;

	/*
	 * Unaligned accesses should be handled (or rejected) before
	 * we get here.
	 */
	if ((vaddr & 0x3) != 0) {
		return -1;
	}

	/* first thing: check if paging is enabled at all... */
	if (!cpu->mmu_enable) {
		*ret = vaddr;
		return 0;
	}

	vpage = vaddr & 0xfffff000;

	CPUTRACEL(DOTRACE_TLB, cpu->cpunum,
		  "mmu (debugger): vpn 0x%x", vpage >> 12);

	if (vpage == cpu->mmu_cached_vpage) {
		switch (rwx) {
		    case RWX_READ:
			if (!cpu->mmu_cached_readable) {
				CPUTRACE(DOTRACE_TLB, cpu->cpunum,
					      ", not readable (cached)");
				return -1;
			}
			break;
		    case RWX_WRITE:
			if (!cpu->mmu_cached_writeable) {
				CPUTRACE(DOTRACE_TLB, cpu->cpunum,
					      ", not writeable (cached)");
				return -1;
			}
			break;
		    case RWX_EXECUTE:
			if (!cpu->mmu_cached_executable) {
				CPUTRACE(DOTRACE_TLB, cpu->cpunum,
					      " - not executable (cached)"); 
				return -1;
			}
			break;
		}
		CPUTRACE(DOTRACE_TLB, cpu->cpunum,
			 " - cached ppn 0x%x", cpu->mmu_cached_ppage >> 12);
		/* the cached page is never a superpage */
		offset = vaddr & 0x00000fff;
		*ret = cpu->mmu_cached_ppage | offset;
		return 0;
	}

	/*
	 * Need to visit the pagetable.
	 *
	 * The pagetable is a normal 32-bit, 4k-page pagetable.
	 */

	if (cpu->mmu_pttoppage == NULL) {
		CPUTRACE(DOTRACE_TLB, cpu->cpunum, ", invalid pagetable base");
		return -1;
	}

	// index the top level (>> 22, << 2)
	ix = (vaddr & 0xffc00000) >> 20;
	entry = bus_use_map(cpu->mmu_pttoppage, ix);

	CPUTRACEL(DOTRACE_TLB, cpu->cpunum,
		  ", pd at 0x%05x, entry 0x%08x", cpu->mmu_ptbase_pa, entry);

	if ((entry & PTE_V) == 0) {
		/* top-level entry not valid */
		CPUTRACE(DOTRACE_TLB, cpu->cpunum,
			 ", top-level entry not valid");
		return -1;
	}
	if ((entry & (PTE_R | PTE_W | PTE_X)) != 0) {
		/* Upper-level entry is a leaf, so it's a superpage */
		CPUTRACEL(DOTRACE_TLB, cpu->cpunum,
			  " - superpage");
		/* save the extra upper bits of the paddr for later */
		upperbits = entry & PTE_UPPER_PPN;
		paddr = (entry & PTE_PPN) << 2;
		offset = vaddr & 0x003fffff;

		if ((paddr & 0x003ff000) != 0) {
			/* unaligned superpage */
			/* spec specifically says to use PAGE not ALIGN here */
			CPUTRACE(DOTRACE_TLB, cpu->cpunum,
				 ", unaligned superpage");
			return -1;
		}
		//superpage = true;
	}
	else {
		/*
		 * Physical page numbers have two extra bits, which we
		 * don't support, so if they're set it's a bus error.
		 * We need to check this here (regardless of exception
		 * priority) because we need to use the page number we
		 * got.
		 */
		if ((entry & PTE_UPPER_PPN) != 0) {
			CPUTRACE(DOTRACE_TLB, cpu->cpunum, " - OUT OF RANGE");
			return -1;
		}

		ptpaddr = (entry & PTE_PPN) << 2;
		CPUTRACEL(DOTRACE_TLB, cpu->cpunum, ", ptp at 0x%x",
			  ptpaddr >> 12);

		// index the second level (>> 12, << 2)
		ix = (vaddr & 0x003ff000) >> 10;
		CPUTRACEL(DOTRACE_TLB, cpu->cpunum, ", index 0x%x", ix);
		if (FN(accessmem)(cpu, ptpaddr + ix, false/*iswrite*/, &entry)) {
			CPUTRACE(DOTRACE_TLB, cpu->cpunum, " - BUS ERROR");
			return -1;
		}

		CPUTRACEL(DOTRACE_TLB, cpu->cpunum, ", pte 0x%08x", entry);
		if ((entry & PTE_V) == 0) {
			/* entry not valid */
			CPUTRACE(DOTRACE_TLB, cpu->cpunum, ", entry not valid");
			return -1;
		}

		if ((entry & PTE_UPPER_PPN) != 0) {
			CPUTRACE(DOTRACE_TLB, cpu->cpunum, " - OUT OF RANGE");
			return -1;
		}

		/* save the extra upper bits of the paddr for later */
		upperbits = entry & PTE_UPPER_PPN;
		paddr = (entry & PTE_PPN) << 2;
		offset = vaddr & 0x00000fff;
		//superpage = false;
	}
	CPUTRACEL(DOTRACE_TLB, cpu->cpunum, ", ppn 0x%x", paddr >> 12);

	switch (rwx) {
	    case RWX_READ: ok = entry & PTE_R; break;
	    case RWX_WRITE: ok = entry & PTE_W; break;
	    case RWX_EXECUTE: ok = entry & PTE_X; break;
	}
	if (ok == false) {
		CPUTRACE(DOTRACE_TLB, cpu->cpunum, ", no page permission");
		return -1;
	}

	if (rwx == RWX_WRITE && (entry & PTE_D) == 0) {
		CPUTRACE(DOTRACE_TLB, cpu->cpunum, ", page not marked dirty");
		return -1;
	}

	if ((entry & PTE_A) == 0) {
		CPUTRACE(DOTRACE_TLB, cpu->cpunum, ", page not marked accessed");
		return -1;
	}

	/*
	 * Defer checking the upper bits of the paddr until after
	 * we've checked for page faults, to preserve the exception
	 * priorities documented in the architecture manual.
	 */
	if (upperbits != 0) {
		CPUTRACE(DOTRACE_TLB, cpu->cpunum, 
			 ", superpage PPN out of range");
		return -1;
	}

	CPUTRACE(DOTRACE_TLB, cpu->cpunum, " - OK");

#if 0
	/* update the cache */
	if (superpage) {
		cpu->mmu_cached_vpage = vpage;
		cpu->mmu_cached_ppage = paddr;
		cpu->mmu_cached_readable = (entry & PTE_R) != 0;
		cpu->mmu_cached_writeable = (entry & (PTE_W | PTE_D)) == (PTE_W | PTE_D);
		cpu->mmu_cached_executable = (entry & PTE_X) != 0;
	}
#endif

	*ret = paddr | offset;
	return 0;
}

/*
 * This is a special version of accessmem used for instruction fetch.
 * It returns a pointer to an entire page of memory which can then be
 * used repeatedly. Note that it doesn't call exception() - it just
 * returns NULL if the memory doesn't exist.
 */
static
inline
const uint32_t *
FN(mapmem)(uint32_t paddr)
{
	/*
	 * Physical memory layout: 
	 *    0x00000000 - 0xc0000000     nothing
	 *    0xc0000000 - 0xffbfffff     RAM
	 *    0xffc00000 - 0xffdfffff     Boot ROM
	 *    0xffe00000 - 0xffffffff     LAMEbus mapped I/O
	 */
	paddr &= 0xfffff000;

	if (paddr < 0xc0000000) {
		return NULL;
	}

	if (paddr < 0xffc00000) {
		return bus_mem_map(paddr - 0xc0000000);
	}

	if (paddr < 0xffe00000) {
		return bootrom_map(paddr - 0xffc00000);
	}

	/* don't allow executing from (or pagetables to be in) I/O registers */
	return NULL;
}

/*
 * isrwx should reflect the type of *this* domem operation.
 *
 * willrwx should be promoted from RWX_READ to RWX_WRITE if any domem
 * operation on this cycle is (or will be) a write, even if *this* one
 * isn't. (This is for preventing silly exception behavior when
 * writing a sub-word quantity.)
 */
static
int
FN(domem)(struct riscvcpu *cpu, uint32_t vaddr, uint32_t *val, 
	  enum memrwx isrwx, enum memrwx willrwx)
{
	uint32_t paddr;
	
	if (FN(translatemem)(cpu, vaddr, willrwx, &paddr)) {
		return -1;
	}

	if (FN(accessmem)(cpu, paddr, isrwx == RWX_WRITE, val)) {
		FN(accessfault)(cpu, vaddr, willrwx, ", bus error");
		return -1;
	}

	return 0;
}

static
int
FN(precompute_pc)(struct riscvcpu *cpu)
{
	uint32_t physpc;

	if (FN(translatemem)(cpu, cpu->pc, RWX_EXECUTE, &physpc)) {
		return -1;
	}
	cpu->pcpage = FN(mapmem)(physpc);
	if (cpu->pcpage == NULL) {
		/*
		 * No memory there; throw a bus error.
		 */
		FN(exception)(cpu, EX_IACCESS, cpu->pc,
			      ", instruction fetch");

		/*
		 * If we come back from exception() at all, a mapping
		 * should have been produced. (And the PC might have
		 * changed, since we'll now be in the trap handler...)
		 */
		Assert(cpu->pcpage != NULL);
		return -1;
	}
	cpu->pcoff = physpc & 0xfff;
	cpu->nextpcoff = cpu->pcoff;
	return 0;
}

static
int
FN(reload_pagetables)(struct riscvcpu *cpu)
{
	cpu->mmu_pttoppage = FN(mapmem)(cpu->mmu_ptbase_pa);
	if (cpu->mmu_pttoppage == NULL) {
		/*
		 * No memory there; throw a bus error.
		 */
		CPUTRACE(DOTRACE_TLB, cpu->cpunum,
			 "reload_pagetables: bad base address 0x%lx",
			 (unsigned long)cpu->mmu_ptbase_pa);
		FN(exception)(cpu, EX_LACCESS, cpu->pc,
			      ", top-level pagetable access");
		return -1;
	}
	return 0;
}

static
int
FN(try_breakpoint)(struct riscvcpu *cpu)
{
	/*
	 * If we're in the range that we can debug in (that is, not
	 * the TLB-mapped segments), activate the kernel debugging
	 * hooks.
	 */
	if (gdb_canhandle(cpu->pc)) {
		/*FN(phony_exception)(cpu);*/ /* not needed on riscv */
		cpu_stopcycling();
		/* 0 == not lethal */
		main_enter_debugger(0);
		/*
		 * Don't bill time for hitting the breakpoint.
		 */
		cpu->cyclecount--;
		cpu->hit_breakpoint = 1;
		/*
		 * Don't advance to the next instruction, or we skip
		 * one and everything goes wahooni-shaped.
		 */
		cpu->nextpc = cpu->pc;
		cpu->nextpcoff = cpu->pcoff;
		return 1;
	}
	return 0;
}

static
inline
uint32_t
FN(signedshift)(uint32_t val, unsigned amt)
{
	/* There's no way to express a signed shift directly in C. */
	uint32_t result;
	result = val >> amt;
	if (val & 0x80000000) {
		result |= (0xffffffff << (31-amt));
	}
	return result;
}


////////////////////////////////////////////////////////////
// memory

/*
 * Note that anything that bypasses doload or dostore and goes
 * straight to domem should also clear cpu->lr_active. Currently
 * that's only LR and SC (which adjust it explicitly in any event).
 */

static
int
FN(doload)(struct riscvcpu *cpu, enum memstyles ms, bool willbewrite,
	   uint32_t addr, uint32_t *res)
{
	enum memrwx willrwx;

	cpu->lr_active = false;

	willrwx = willbewrite ? RWX_WRITE : RWX_READ;
	switch (ms) {
	    case S_SBYTE:
	    case S_UBYTE:
	    {
		uint32_t val;
		uint8_t bval = 0;
		if (FN(domem)(cpu, addr & 0xfffffffc, &val, RWX_READ, willrwx)) {
			return -1;
		}
		switch (addr & 3) {
		    case 0: bval = val & 0x000000ff; break;
		    case 1: bval = (val & 0x0000ff00)>>8; break;
		    case 2: bval = (val & 0x00ff0000)>>16; break;
		    case 3: bval = (val & 0xff000000)>>24; break;
		}
		*res = (ms == S_SBYTE) ? (int32_t)(int8_t)bval : bval;
	    }
	    break;

	    case S_SHALF:
	    case S_UHALF:
	    {
		uint32_t val;
		uint16_t hval = 0;
		if (FN(domem)(cpu, addr & 0xfffffffc, &val, RWX_READ, willrwx)) {
			return -1;
		}
		switch (addr & 3) {
		    case 0: hval = val & 0x0000ffff; break;
		    case 1: hval = (val & 0x00ffff00)>> 8; break;
		    case 2: hval = (val & 0xffff0000)>>16; break;
		    case 3:
			hval = (val & 0xff000000)>>24;
			if (FN(domem)(cpu, (addr & 0xfffffffc)+4, &val,
				      RWX_READ, willrwx)) {
				return -1;
			}
			hval |= (val & 0xff) << 8;
			break;
		}
		*res = (ms==S_SHALF) ? (int32_t)(int16_t)hval : hval;
	    }
	    break;
     
	    case S_WORD:
	    {
		if ((addr & 0x3) == 0) {
			if (FN(domem)(cpu, addr & 0xfffffffc, res, RWX_READ,
				      willrwx)) {
				return -1;
			}
		}
		else {
			uint32_t val1, val2, wval;

			if (FN(domem)(cpu, addr & 0xfffffffc, &val1, RWX_READ,
				      willrwx)) {
				return -1;
			}
			if (FN(domem)(cpu, (addr & 0xfffffffc)+4, &val2,
				      RWX_READ, willrwx)){
				return -1;
			}
			switch (addr & 0x3) {
			    case 1:
				wval = (val1 & 0xffffff00) >> 8;
				wval |= (val2 & 0x000000ff) << 24;
				break;
			    case 2:
				wval = (val1 & 0xffff0000) >> 16;
				wval |= (val2 & 0x0000ffff) << 16;
				break;
			    case 3:
				wval = (val1 & 0xff000000) >> 24;
				wval |= (val2 & 0x00ffffff) << 8;
				break;
			}
			*res = wval;
		}
	    }
	    break;
	    default:
		smoke("doload: Illegal addressing mode");
	}
	return 0;
}

static
int
FN(dostore)(struct riscvcpu *cpu, enum memstyles ms, uint32_t addr, uint32_t val)
{
	cpu->lr_active = false;

	switch (ms) {
	    case S_UBYTE:
	    {
		uint32_t wval;
		uint32_t mask = 0;
		int shift = 0;
		switch (addr & 3) {
		    case 0: mask = 0x000000ff; shift=0; break;
		    case 1: mask = 0x0000ff00; shift=8; break;
		    case 2: mask = 0x00ff0000; shift=16; break;
		    case 3: mask = 0xff000000; shift=24; break;
		}
		if (FN(domem)(cpu, addr & 0xfffffffc, &wval,
			      RWX_READ, RWX_WRITE)) {
			return -1;
		}
		wval = (wval & ~mask) | ((val&0xff) << shift);
		if (FN(domem)(cpu, addr & 0xfffffffc, &wval,
			      RWX_WRITE, RWX_WRITE)) {
			return -1;
		}
	    }
	    break;

	    case S_UHALF:
	    {
		uint32_t wval;
		uint32_t mask = 0;
		int shift = 0;
		switch (addr & 3) {
		    case 0: mask = 0x0000ffff; shift=0; break;
		    case 1: mask = 0x00ffff00; shift=8; break;
		    case 2: mask = 0xffff0000; shift=16; break;
		    case 3:
			if (FN(dostore)(cpu, S_UBYTE, addr, val & 0xff)) {
				return -1;
			}
			if (FN(dostore)(cpu, S_UBYTE, addr+1,
					(val >> 8) & 0xff)) {
				return -1;
			}
			return 0;
		}
		if (FN(domem)(cpu, addr & 0xfffffffc, &wval,
			      RWX_READ, RWX_WRITE)) {
			return -1;
		}
		wval = (wval & ~mask) | ((val&0xffff) << shift);
		if (FN(domem)(cpu, addr & 0xfffffffc, &wval,
			      RWX_WRITE, RWX_WRITE)) {
			return -1;
		}
	    }
	    break;
	
	    case S_WORD:
	    {
		if ((addr & 3) == 0) {
			return FN(domem)(cpu, addr, &val, RWX_WRITE,RWX_WRITE);
		}
		else {
			uint32_t val1, val2;

			if (FN(domem)(cpu, addr & 0xfffffffc, &val1,
				      RWX_READ, RWX_WRITE)) {
				return -1;
			}
			if (FN(domem)(cpu, (addr & 0xfffffffc)+4, &val2,
				      RWX_READ, RWX_WRITE)) {
				return -1;
			}

			switch (addr & 0x3) {
			    case 1:
				val1 &= 0x000000ff;
				val2 &= 0xffffff00;
				val1 |= (val & 0xffffff00) >> 8;
				val2 |= (val & 0x000000ff) << 24;
				break;
			    case 2:
				val1 &= 0x0000ffff;
				val2 &= 0xffff0000;
				val1 |= (val & 0xffff0000) >> 16;
				val2 |= (val & 0x0000ffff) << 16;
				break;
			    case 3:
				val1 &= 0x00ffffff;
				val2 &= 0xff000000;
				val1 |= (val & 0xff000000) >> 24;
				val2 |= (val & 0x00ffffff) << 8;
				break;
			}

			if (FN(domem)(cpu, addr & 0xfffffffc, &val1,
				      RWX_WRITE, RWX_WRITE)) {
				return -1;
			}
			if (FN(domem)(cpu, (addr & 0xfffffffc)+4, &val2,
				      RWX_WRITE, RWX_WRITE)) {
				return -1;
			}
		}
	    }
	    break;

	    default:
		smoke("dostore: Illegal addressing mode");
	}
	return 0;
}


////////////////////////////////////////////////////////////
// executing instructions

/* registers as lvalues */
#define RDx   (cpu->x[rd])
#define RS1x  (cpu->x[rs1])
#define RS2x  (cpu->x[rs2])
#define PCx   (cpu->pc)

/* registers as signed 32-bit rvalues */
#define RDs   ((int32_t)RDx)
#define RS1s  ((int32_t)RS1x)
#define RS2s  ((int32_t)RS2x)
#define PCs   ((uint32_t)PCx)

/* registers as unsigned 32-bit rvalues */
#define RDu   ((uint32_t)RDx)
#define RS1u  ((uint32_t)RS1x)
#define RS2u  ((uint32_t)RS2x)
#define PCu   ((uint32_t)PCx)

/* registers as printf-able signed values */
#define RDsp   ((long)RDs)
#define RS1sp  ((long)RS1s)
#define RS2sp  ((long)RS2s)
#define PCsp   ((unsigned long)PCs)

/* registers as printf-able unsigned values */
#define RDup   ((unsigned long)RDu)
#define RS1up  ((unsigned long)RS1u)
#define RS2up  ((unsigned long)RS2u)
#define PCup   ((unsigned long)PCu)

/* immediates as printable value */
#define IMMsp  ((long)(int32_t)imm)
#define IMMup  ((unsigned long)(uint32_t)imm)

/* trace shorthand */
#define TRL(...)  CPUTRACEL(tracehow, cpu->cpunum, __VA_ARGS__)
#define TR(...)   CPUTRACE(tracehow, cpu->cpunum, __VA_ARGS__)


static
void
FN(rx_do_addi)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, uint32_t imm)
{
	TRL("addi %s, %s, %lu: %ld + %ld -> ",
	    regname(rd), regname(rs1), IMMup, RS1sp, IMMsp);

	/* add as unsigned so overflow works reliably */
	if (rd != 0) {
		RDx = RS1u + imm;
		TR("%ld", RDsp);
	}
	else {
		TR("discard %ld", (long)(unsigned long)(RS1u + imm));
	}
}

static
void
FN(rx_do_slti)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, uint32_t imm)
{
	TRL("slti %s, %s, %lu: %ld < %ld -> ", 
	    regname(rd), regname(rs1), IMMup, RS1sp, IMMsp);
	if (rd != 0) {
		RDx = RS1s < (int32_t)imm;
		TR("%ld", RDsp);
	}
	else {
		TR("discard %d", RS1s < (int32_t)imm);
	}
}

static
void
FN(rx_do_sltiu)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, uint32_t imm)
{
	TRL("sltiu %s, %s, %lu: %lu < %lu -> ", 
	    regname(rd), regname(rs1), IMMup, RS1up, IMMup);
	if (rd != 0) {
		RDx = RS1u < imm;
		TR("%ld", RDsp);
	}
	else {
		TR("discard %d", RS1u < imm);
	}
}

static
void
FN(rx_do_andi)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, uint32_t imm)
{
	TRL("andi %s, %s, %lu: 0x%lx & 0x%lx -> ", 
	    regname(rd), regname(rs1), IMMup, RS1up, IMMup);
	if (rd != 0) {
		RDx = RS1u & imm;
		TR("0x%lx", RDup);
	}
	else {
		TR("discard");
	}
}

static
void
FN(rx_do_ori)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, uint32_t imm)
{
	TRL("ori %s, %s, %lu: 0x%lx | 0x%lx -> ", 
	    regname(rd), regname(rs1), IMMup, RS1up, IMMup);
	if (rd != 0) {
		RDx = RS1u | imm;
		TR("0x%lx", RDup);
	}
	else {
		TR("discard");
	}
}

static
void
FN(rx_do_xori)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, uint32_t imm)
{
	TRL("xori %s, %s, %lu: 0x%lx ^ 0x%lx -> ", 
	    regname(rd), regname(rs1), IMMup, RS1up, IMMup);
	if (rd != 0) {
		RDx = RS1u ^ imm;
		TR("0x%lx", RDup);
	}
	else {
		TR("discard");
	}
}

static
void
FN(rx_do_slli)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, uint32_t imm)
{
	TRL("slli %s, %s, %lu: 0x%lx << %lu -> ", 
	    regname(rd), regname(rs1), IMMup, RS1up, IMMup);
	if (rd != 0) {
		RDx = RS1u << imm;
		TR("0x%lx", RDup);
	}
	else {
		TR("discard");
	}
}

static
void
FN(rx_do_srli)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, uint32_t imm)
{
	TRL("srli %s, %s, %lu: 0x%lx >> %lu -> ", 
	    regname(rd), regname(rs1), IMMup, RS1up, IMMup);
	if (rd != 0) {
		RDx = RS1u >> imm;
		TR("0x%lx", RDup);
	}
	else {
		TR("discard");
	}
}

static
void
FN(rx_do_srai)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, uint32_t imm)
{
	TRL("srai %s, %s, %lu: 0x%lx >> %lu -> ", 
	    regname(rd), regname(rs1), IMMup, RS1up, IMMup);
	if (rd != 0) {
		RDx = signedshift(RS1u, imm);
		TR("0x%lx", RDup);
	}
	else {
		TR("discard");
	}
}

static
void
FN(rx_do_lui)(struct riscvcpu *cpu, unsigned rd, uint32_t imm)
{
	/* The immediate comes in already shifted (avoids useless frobbing) */
	TRL("lui %s, 0x%lx: ", regname(rd), IMMup >> 12);
	if (rd != 0) {
		RDx = imm;
		TR("0x%lx", RDup);
	}
	else {
		TR("discard");
	}
}

static
void
FN(rx_do_auipc)(struct riscvcpu *cpu, unsigned rd, uint32_t imm)
{
	/* The immediate comes in already shifted (avoids useless frobbing) */
	TRL("auipc %s, 0x%lx: %lu + %lu -> ",
	    regname(rd), IMMup, PCup, IMMup >> 12);
	if (rd != 0) {
		RDx = PCu + imm;
		TR("%lu", RDup);
	}
	else {
		TR("discard");
	}
}

static
void
FN(rx_do_add)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, unsigned rs2)
{
	TRL("add %s, %s, %s: %lu + %lu -> ",
	    regname(rd), regname(rs1), regname(rs2), RS1up, RS2up);
	if (rd != 0) {
		RDx = RS1u + RS2u;
		TR("%lu", RDup);
	}
	else {
		TR("discard");
	}
}

static
void
FN(rx_do_sub)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, unsigned rs2)
{
	TRL("sub %s, %s, %s: %lu - %lu -> ",
	    regname(rd), regname(rs1), regname(rs2), RS1up, RS2up);
	if (rd != 0) {
		RDx = RS1u - RS2u;
		TR("%lu", RDup);
	}
	else {
		TR("discard");
	}
}

static
void
FN(rx_do_slt)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, unsigned rs2)
{
	TRL("slt %s, %s, %s: %ld < %ld -> ",
	    regname(rd), regname(rs1), regname(rs2), RS1sp, RS2sp);
	if (rd != 0) {
		RDx = RS1s < RS2s;
		TR("%lu", RDup);
	}
	else {
		TR("discard");
	}
}

static
void
FN(rx_do_sltu)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, unsigned rs2)
{
	TRL("sltu %s, %s, %s: %lu < %lu -> ",
	    regname(rd), regname(rs1), regname(rs2), RS1up, RS2up);
	if (rd != 0) {
		RDx = RS1u < RS2u;
		TR("%lu", RDup);
	}
	else {
		TR("discard");
	}
}

static
void
FN(rx_do_and)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, unsigned rs2)
{
	TRL("and %s, %s, %s: 0x%lx & 0x%lx -> ",
	    regname(rd), regname(rs1), regname(rs2), RS1up, RS2up);
	if (rd != 0) {
		RDx = RS1u & RS2u;
		TR("0x%lx", RDup);
	}
	else {
		TR("discard");
	}
}

static
void
FN(rx_do_or)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, unsigned rs2)
{
	TRL("or %s, %s, %s: 0x%lx | 0x%lx -> ",
	    regname(rd), regname(rs1), regname(rs2), RS1up, RS2up);
	if (rd != 0) {
		RDx = RS1u | RS2u;
		TR("0x%lx", RDup);
	}
	else {
		TR("discard");
	}
}

static
void
FN(rx_do_xor)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, unsigned rs2)
{
	TRL("xor %s, %s, %s: 0x%lx ^ 0x%lx -> ",
	    regname(rd), regname(rs1), regname(rs2), RS1up, RS2up);
	if (rd != 0) {
		RDx = RS1u ^ RS2u;
		TR("0x%lx", RDup);
	}
	else {
		TR("discard");
	}
}

static
void
FN(rx_do_sll)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, unsigned rs2)
{
	TRL("sll %s, %s, %s: 0x%lx << %lu -> ",
	    regname(rd), regname(rs1), regname(rs2), RS1up, RS2up);
	if (rd != 0) {
		RDx = RS1u << (RS2u & 31);
		TR("0x%lx", RDup);
	}
	else {
		TR("discard");
	}
}

static
void
FN(rx_do_srl)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, unsigned rs2)
{
	TRL("srl %s, %s, %s: 0x%lx >> %lu -> ",
	    regname(rd), regname(rs1), regname(rs2), RS1up, RS2up);
	if (rd != 0) {
		RDx = RS1u >> (RS2u & 31);
		TR("0x%lx", RDup);
	}
	else {
		TR("discard");
	}
}

static
void
FN(rx_do_sra)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, unsigned rs2)
{
	TRL("sra %s, %s, %s: 0x%lx >> %lu -> ",
	    regname(rd), regname(rs1), regname(rs2), RS1up, RS2up);
	if (rd != 0) {
		RDx = signedshift(RS1u, (RS2u & 31));
		TR("0x%lx", RDup);
	}
	else {
		TR("discard");
	}
}

static
int
FN(rx_jump)(struct riscvcpu *cpu, uint32_t dest)
{
	/* check if the C extension has been turned off */
	if (!cpu->Cext && (dest & 0x2) != 0) {
		/* Unaligned. */
		TR("unaligned");
		FN(exception)(cpu, EX_IALIGN, dest, "");
		return -1;
	}
	else {
		cpu->pc = dest;
		cpu->nextpc = cpu->pc;
		return FN(precompute_pc)(cpu);
	}
}

static
void
FN(rx_do_jal)(struct riscvcpu *cpu, unsigned rd, uint32_t imm, uint32_t retpc)
{
	uint32_t dest;

	TRL("jal %s, 0x%lx: 0x%lx + 0x%lx -> ",
	    regname(rd), IMMup, PCup, IMMup);
	dest = (PCu + imm) & 0xfffffffe;
	TRL("0x%lx; ", (unsigned long)dest);
	if (FN(rx_jump)(cpu, dest) == 0) {
		if (rd != 0) {
			RDx = retpc;
			TR("return addr 0x%lx", RDup);
		}
		else {
			TR("no return addr");
		}
	}
}

static
void
FN(rx_do_jalr)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, uint32_t imm,
	       uint32_t retpc)
{
	uint32_t dest;

	TRL("jalr %s, %s, 0x%lx: 0x%lx + 0x%lx -> ",
	    regname(rd), regname(rs1), IMMup, RS1up, IMMup);
	dest = (RS1u + imm) & 0xfffffffe;
	TRL("0x%lx; ", (unsigned long)dest);
	if (FN(rx_jump)(cpu, dest) == 0) {
		if (rd != 0) {
			RDx = retpc;
			TR("return addr 0x%lx", RDup);
		}
		else {
			TR("no return addr");
		}
	}
}

static
void
FN(rx_do_beq)(struct riscvcpu *cpu, unsigned rs1, unsigned rs2, uint32_t imm)
{
	uint32_t dest;

	TRL("beq %s, %s, 0x%lx: %lu==%lu? ",
	    regname(rs1), regname(rs2), (unsigned long)imm, RS1up, RS2up);
	if (RS1u == RS2u) {
		dest = cpu->pc + imm;
		TRL("yes! off to 0x%lx... ", (unsigned long)dest);
		if (FN(rx_jump)(cpu, dest) == 0) {
			TR("succeeded");
		}
		else {
			TR("exception");
		}
	}
	else {
		TR("no");
	}
}

static
void
FN(rx_do_bne)(struct riscvcpu *cpu, unsigned rs1, unsigned rs2, uint32_t imm)
{
	uint32_t dest;

	TRL("bne %s, %s, 0x%lx: %lu!=%lu? ",
	    regname(rs1), regname(rs2), (unsigned long)imm, RS1up, RS2up);
	if (RS1u != RS2u) {
		dest = cpu->pc + imm;
		TR("yes! off to 0x%lx", (unsigned long)dest);
		if (FN(rx_jump)(cpu, dest) == 0) {
			TR("succeeded");
		}
		else {
			TR("exception");
		}
	}
	else {
		TR("no");
	}
}

static
void
FN(rx_do_blt)(struct riscvcpu *cpu, unsigned rs1, unsigned rs2, uint32_t imm)
{
	uint32_t dest;

	TRL("blt %s, %s, 0x%lx: %ld < %ld? ",
	    regname(rs1), regname(rs2), (unsigned long)imm, RS1sp, RS2sp);
	if (RS1s < RS2s) {
		dest = cpu->pc + imm;
		TR("yes! off to 0x%lx", (unsigned long)dest);
		if (FN(rx_jump)(cpu, dest) == 0) {
			TR("succeeded");
		}
		else {
			TR("exception");
		}
	}
	else {
		TR("no");
	}
}

static
void
FN(rx_do_bltu)(struct riscvcpu *cpu, unsigned rs1, unsigned rs2, uint32_t imm)
{
	uint32_t dest;

	TRL("bltu %s, %s, 0x%lx: %lu < %lu? ",
	    regname(rs1), regname(rs2), (unsigned long)imm, RS1up, RS2up);
	if (RS1u < RS2u) {
		dest = cpu->pc + imm;
		TR("yes! off to 0x%lx", (unsigned long)dest);
		if (FN(rx_jump)(cpu, dest) == 0) {
			TR("succeeded");
		}
		else {
			TR("exception");
		}
	}
	else {
		TR("no");
	}
}

static
void
FN(rx_do_bge)(struct riscvcpu *cpu, unsigned rs1, unsigned rs2, uint32_t imm)
{
	uint32_t dest;

	TRL("bge %s, %s, 0x%lx: %ld >= %ld? ",
	    regname(rs1), regname(rs2), (unsigned long)imm, RS1sp, RS2sp);
	if (RS1s >= RS2s) {
		dest = cpu->pc + imm;
		TR("yes! off to 0x%lx", (unsigned long)dest);
		if (FN(rx_jump)(cpu, dest) == 0) {
			TR("succeeded");
		}
		else {
			TR("exception");
		}
	}
	else {
		TR("no");
	}
}

static
void
FN(rx_do_bgeu)(struct riscvcpu *cpu, unsigned rs1, unsigned rs2, uint32_t imm)
{
	uint32_t dest;

	TRL("bgeu %s, %s, 0x%lx: %lu >= %lu? ",
	    regname(rs1), regname(rs2), (unsigned long)imm, RS1up, RS2up);
	if (RS1u >= RS2u) {
		dest = cpu->pc + imm;
		TR("yes! off to 0x%lx", (unsigned long)dest);
		if (FN(rx_jump)(cpu, dest) == 0) {
			TR("succeeded");
		}
		else {
			TR("exception");
		}
	}
	else {
		TR("no");
	}
}

static
void
FN(rx_do_lb)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, uint32_t imm)
{
	uint32_t addr;
	uint32_t tmp;

	addr = RS1u + imm;
	TRL("lb %s, %ld(%s): [0x%lx] -> ",
	    regname(rd), IMMsp, regname(rs1), (unsigned long)addr);
	if (FN(doload)(cpu, S_SBYTE, false, addr, &tmp) == 0) {
		if (rd != 0) {
			RDx = tmp;
			TR("%ld", RDsp);
		}
		else {
			TR("discard");
		}
	}
	else {
		TR("[exception]");
	}
}

static
void
FN(rx_do_lbu)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, uint32_t imm)
{
	uint32_t addr;
	uint32_t tmp;

	addr = RS1u + imm;
	TRL("lbu %s, %ld(%s): [0x%lx] -> ",
	    regname(rd), IMMsp, regname(rs1), (unsigned long)addr);
	if (FN(doload)(cpu, S_UBYTE, false, addr, &tmp) == 0) {
		if (rd != 0) {
			RDx = tmp;
			TR("%lu", RDup);
		}
		else {
			TR("discard");
		}
	}
	else {
		TR("[exception]");
	}
}

static
void
FN(rx_do_lh)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, uint32_t imm)
{
	uint32_t addr;
	uint32_t tmp;

	addr = RS1u + imm;
	TRL("lh %s, %ld(%s): [0x%lx] -> ",
	    regname(rd), IMMsp, regname(rs1), (unsigned long)addr);
	if (FN(doload)(cpu, S_SHALF, false, addr, &tmp) == 0) {
		if (rd != 0) {
			RDx = tmp;
			TR("%ld", RDsp);
		}
		else {
			TR("discard");
		}
	}
	else {
		TR("[exception]");
	}
}

static
void
FN(rx_do_lhu)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, uint32_t imm)
{
	uint32_t addr;
	uint32_t tmp;

	addr = RS1u + imm;
	TRL("lhu %s, %ld(%s): [0x%lx] -> ",
	    regname(rd), IMMsp, regname(rs1), (unsigned long)addr);
	if (FN(doload)(cpu, S_UHALF, false, addr, &tmp) == 0) {
		if (rd != 0) {
			RDx = tmp;
			TR("%lu", RDup);
		}
		else {
			TR("discard");
		}
	}
	else {
		TR("[exception]");
	}
}

static
void
FN(rx_do_lw)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, uint32_t imm)
{
	uint32_t addr;
	uint32_t tmp;

	addr = RS1u + imm;
	TRL("lw %s, %ld(%s): [0x%lx] -> ",
	    regname(rd), IMMsp, regname(rs1), (unsigned long)addr);
	if (FN(doload)(cpu, S_WORD, false, addr, &tmp) == 0) {
		if (rd != 0) {
			RDx = tmp;
			TR("%lu", RDup);
		}
		else {
			TR("discard");
		}
	}
	else {
		TR("[exception]");
	}
}

static
void
FN(rx_do_sb)(struct riscvcpu *cpu, unsigned rs2, unsigned rs1, uint32_t imm)
{
	uint32_t addr;

	addr = RS1u + imm;
	TR("sb %s, %ld(%s): %lu -> [0x%lx]",
	   regname(rs2), IMMsp, regname(rs1), RS2up & 0xff,
	   (unsigned long)addr);
	(void) FN(dostore)(cpu, S_UBYTE, addr, RS2u);
}

static
void
FN(rx_do_sh)(struct riscvcpu *cpu, unsigned rs2, unsigned rs1, uint32_t imm)
{
	uint32_t addr;

	addr = RS1u + imm;
	TR("sh %s, %ld(%s): %lu -> [0x%lx]",
	   regname(rs2), IMMsp, regname(rs1), RS2up & 0xffff,
	   (unsigned long)addr);
	(void) FN(dostore)(cpu, S_UHALF, addr, RS2u);
}

static
void
FN(rx_do_sw)(struct riscvcpu *cpu, unsigned rs2, unsigned rs1, uint32_t imm)
{
	uint32_t addr;

	addr = RS1u + imm;
	TR("sw %s, %ld(%s): %lu -> [0x%lx]",
	   regname(rs2), IMMsp, regname(rs1), RS2up, (unsigned long)addr);
	(void) FN(dostore)(cpu, S_WORD, addr, RS2u);
}

static
void
FN(rx_do_fencetso)(struct riscvcpu *cpu)
{
	TR("fence.tso");
	/* Don't need to actually do anything */
	(void)cpu;
}

static
void
FN(rx_do_fence)(struct riscvcpu *cpu, unsigned pred, unsigned succ)
{
	TR("fence %s%s%s%s, %s%s%s%s",
	   (pred & 8) ? "I" : "-",
	   (pred & 4) ? "O" : "-",
	   (pred & 2) ? "R" : "-",
	   (pred & 1) ? "W" : "-",
	   (succ & 8) ? "I" : "-",
	   (succ & 4) ? "O" : "-",
	   (succ & 2) ? "R" : "-",
	   (succ & 1) ? "W" : "-");
#ifndef USE_TRACE
	(void)pred;
	(void)succ;
#endif
	/* Don't need to actually do anything */
	(void)cpu;
}

static
void
FN(rx_do_ecall)(struct riscvcpu *cpu)
{
	TR("ecall");
	FN(exception)(cpu, EX_UCALL, 0, "");
}

static
void
FN(rx_do_ebreak)(struct riscvcpu *cpu)
{
	TR("ebreak");
	FN(exception)(cpu, EX_BREAKPOINT, 0, "");
}

static
void
FN(rx_do_fencei)(struct riscvcpu *cpu)
{
	TR("fence.i");
	/* don't actually need to do anything */
	(void)cpu;
}

static
void
FN(rx_do_mul)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, unsigned rs2)
{
	TRL("mul %s, %s, %s: %lu * %lu -> ",
	    regname(rd), regname(rs1), regname(rs2), RS1up, RS2up);
	if (rd != 0) {
		RDx = RS1u * RS2u;
		TR("%lu", RDup);
	}
	else {
		TR("discard");
	}
}

static
void
FN(rx_do_mulh)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, unsigned rs2)
{
	TRL("mulh %s, %s, %s: %ld * %ld -> ",
	    regname(rd), regname(rs1), regname(rs2), RS1sp, RS2sp);
	if (rd != 0) {
		RDx = ((uint64_t)((int64_t)RS1s * (int64_t)RS2s)) >> 32;
		TR("%ld", RDsp);
	}
	else {
		TR("discard");
	}
}

static
void
FN(rx_do_mulhu)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, unsigned rs2)
{
	TRL("mulhu %s, %s, %s: %lu * %lu -> ",
	    regname(rd), regname(rs1), regname(rs2), RS1up, RS2up);
	if (rd != 0) {
		RDx = ((uint64_t)RS1u * (uint64_t)RS2u) >> 32;
		TR("%lu", RDup);
	}
	else {
		TR("discard");
	}
}

static
void
FN(rx_do_mulhsu)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, unsigned rs2)
{
	bool neg;
	uint64_t rs1val, res;

	TRL("mulhsu %s, %s, %s: %ld * %lu -> ",
	    regname(rd), regname(rs1), regname(rs2), RS1sp, RS2up);
	/*
	 * This isn't an operation you can express in C...
	 */
	if (RS1s < 0) {
		neg = true;
		/* promote before negating to not choke on 0x80000000 */
		rs1val = (uint64_t)(-(int64_t)RS1s);
	}
	else {
		neg = false;
		rs1val = (uint64_t)RS1u;
	}
	res = rs1val * (uint64_t)RS2u;
	if (neg) {
		res = (uint64_t)(-(int64_t)res);
	}
	if (rd != 0) {
		RDx = res >> 32;
		TR("%ld", RDsp);
	}
	else {
		TR("discard");
	}
}

static
void
FN(rx_do_div)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, unsigned rs2)
{
	TRL("div %s, %s, %s: %ld / %ld -> ",
	    regname(rd), regname(rs1), regname(rs2), RS1sp, RS2sp);
	if (rd == 0) {
		TR("discard");
		return;
	}
	if (RS2s == 0) {
		RDx = 0xffffffff;
	}
	else if (RS1u == 0x80000000 && RS2s == -1) {
		RDx = 0x80000000;
	}
	else {
		RDx = RS1s / RS2s;
	}
	TR("%ld", RDsp);
}

static
void
FN(rx_do_divu)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, unsigned rs2)
{
	TRL("divu %s, %s, %s: %lu / %lu -> ",
	    regname(rd), regname(rs1), regname(rs2), RS1up, RS2up);
	if (rd == 0) {
		TR("discard");
		return;
	}
	if (RS2u == 0) {
		RDx = 0xffffffff;
	}
	else {
		RDx = RS1u / RS2u;
	}
	TR("%lu", RDup);
}

static
void
FN(rx_do_rem)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, unsigned rs2)
{
	TRL("rem %s, %s, %s: %ld %% %ld -> ",
	    regname(rd), regname(rs1), regname(rs2), RS1sp, RS2sp);
	if (rd == 0) {
		TR("discard");
		return;
	}
	if (RS2s == 0) {
		RDx = RS1s;
	}
	else if (RS1u == 0x80000000 && RS2s == -1) {
		RDx = 0;
	}
	else {
		RDx = RS1s % RS2s;
	}
	TR("%ld", RDsp);
}

static
void
FN(rx_do_remu)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, unsigned rs2)
{
	TRL("remu %s, %s, %s: %lu %% %lu -> ",
	    regname(rd), regname(rs1), regname(rs2), RS1up, RS2up);
	if (rd == 0) {
		TR("discard");
		return;
	}
	if (RS2u == 0) {
		RDx = RS1u;
	}
	else {
		RDx = RS1u % RS2u;
	}
	TR("%lu", RDup);
}

static
void
FN(rx_do_lr)(struct riscvcpu *cpu, unsigned rd, unsigned rs1,
	     bool aq, bool rl)
{
	uint32_t addr;
	uint32_t val;

	TRL("lr %s, %s%s%s: [0x%lx] -> ", 
	    regname(rd), regname(rs1),
	    aq ? " [aq]" : "",
	    rl ? " [rl]" : "",
	    RS1up);

	/* don't need to do anything with these */
	(void)aq;
	(void)rl;

	addr = RS1u;

	/* lr/sc are specifically not allowed to be unaligned */
	if ((addr & 0x3) != 0) {
		TR("[unaligned]");
		FN(exception)(cpu, EX_LALIGN, addr, ", unaligned LR");
		return;
	}

	if (FN(domem)(cpu, addr, &val, RWX_READ, RWX_READ)) {
		TR("[exception]");
		/* exception */
		return;
	}

	/* load reserved: just save what we did */
	cpu->lr_active = 1;
	cpu->lr_addr = addr;
	cpu->lr_value = val;

	g_stats.s_percpu[cpu->cpunum].sp_lls++;

	if (rd != 0) {
		RDx = val;
		TR("%ld", RDsp);
	}
	else {
		TR("discard");
	}
}

static
void
FN(rx_do_sc)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, unsigned rs2,
	     bool aq, bool rl)
{
	uint32_t addr, val, temp;

	addr = RS1u;
	val = RS2u;

	TRL("sc %s, %s, %s%s%s: %ld -> [0x%lx]: ", 
	    regname(rd), regname(rs1), regname(rs2),
	    aq ? " [aq]" : "",
	    rl ? " [rl]" : "",
	    (unsigned long)val, (unsigned long)addr);

	/* don't need to do anything with these */
	(void)aq;
	(void)rl;

	/* lr/sc are specifically not allowed to be unaligned */
	if ((addr & 0x3) != 0) {
		TR("[unaligned]");
		FN(exception)(cpu, EX_LALIGN, addr, ", unaligned LR");
		return;
	}

	/*
	 * Store conditional.
	 *
	 * Unlike MIPS other accesses to memory during an LR/SC
	 * sequence are allowed; they just invalidate the termination
	 * guarantee. However, because they invalidate the termination
	 * guarantee we can have SC always fail if other memory
	 * accesses have been made.
	 *
	 * Other than that, this implementation is the same as the
	 * MIPS one, where LR is just load and SC is a
	 * compare-and-swap. (See the comment in mipscore.h for
	 * further info.)
	 *
	 * SC fails if:
	 *
	 * 1. We don't have an active reservation.
	 * 2. The target vaddr is not the same as the one on file
	 *    from LR.
	 * 3. The result from reading the target vaddr is different
	 *    from the value on file from LR.
	 *
	 * If we take an exception on either the read or write of
	 * the target address, we just return, and cancel the
	 * current reservation.
	 *
	 * The reservation is cancelled at the following times:
	 *    - at the end of SC (regardless of success)
	 *    - at any ordinary load or store
	 * Executing an LL cancels the old reservation and takes out a
	 * new one.
	 *
	 * Note that (unlike on MIPS) traps don't cancel the
	 * reservation, though realities of kernel code mean that no
	 * reservation is likely to survive an interrupt or exception.
	 */

	if (!cpu->lr_active) {
		goto fail;
	}
	cpu->lr_active = false;

	if (cpu->lr_addr != addr) {
		goto fail;
	}
	if (FN(domem)(cpu, addr, &temp, RWX_READ, RWX_WRITE)) {
		/* exception */
		return;
	}
	if (temp != cpu->lr_value) {
		goto fail;
	}
	if (FN(domem)(cpu, addr, &val, RWX_WRITE, RWX_WRITE)) {
		/* exception */
		return;
	}
	/* success */
	if (rd != 0) {
		RDx = 1;
		TR("%lu (succeeded)", RDup);
	}
	else {
		TR("discard (succeeded)");
	}
	g_stats.s_percpu[cpu->cpunum].sp_okscs++;
	return;

 fail:
	/* failure */
	if (rd != 0) {
		RDx = 0;
		TR("%lu (failed)", RDup);
	}
	else {
		TR("discard (failed)");
	}
	g_stats.s_percpu[cpu->cpunum].sp_badscs++;
}

/*
 * Supervisor instructions
 */

static
void
FN(rx_do_sret)(struct riscvcpu *cpu)
{
	TRL("sret: ");

	cpu->status_sie = cpu->status_spie;
	cpu->status_spie = true;
	cpu->super = cpu->status_spp;

	if (!cpu->Cext) {
		cpu->pc = cpu->sepc & 0xfffffffc;
	}
	else {
		cpu->pc = cpu->sepc;
	}
	cpu->nextpc = cpu->pc;

	TR("pc 0x%lx mode %c sie %c", (unsigned long)cpu->pc,
	   cpu->super ? 'S' : 'U', cpu->status_sie ? '1' : '0');
	CPUTRACE(DOTRACE_IRQ, cpu->cpunum,
		 "after sret: spie %s sie %s",
		 cpu->status_spie ? "enabled" : "disabled",
		 cpu->status_sie ? "enabled" : "disabled");

	(void)FN(precompute_pc)(cpu);
}

static
void
FN(rx_do_wfi)(struct riscvcpu *cpu)
{
	bool eie, tie, sie;

	TRL("wfi: ");

	/*
	 * WFI is supposed to honor the bits in the IE register but
	 * not the master switch in the status register. Don't wait
	 * if something relevant is already pending.
	 *
	 * Note: currently we only come here if cpu->super is true,
	 * but that might chagne if we implement user interrupts.
	 */
	eie = cpu->super ? cpu->ie_seie : /*cpu->ie_ueie*/ true;
	tie = cpu->super ? cpu->ie_stie : /*cpu->ie_utie*/ true;
	sie = cpu->super ? cpu->ie_ssie : /*cpu->ie_usie*/ true;

	if ((cpu->irq_lamebus && eie) ||
	    (cpu->irq_timer && tie) ||
	    (cpu->irq_ipi && sie)) {
		TR("already pending, not idling");
		CPUTRACE(DOTRACE_IRQ, cpu->cpunum,
			 "wfi: already pending, not idling");
	}
	else {
		cpu->state = CPU_IDLE;
		RUNNING_MASK_OFF(cpu->cpunum);
		TR("idling");
		CPUTRACE(DOTRACE_IRQ, cpu->cpunum,
			 "wfi: idling");
	}
}

static
void
FN(rx_do_sfence_vma)(struct riscvcpu *cpu, unsigned rs1, unsigned rs2)
{
	TRL("sfence.vma %s, %s: va 0x%lx, asid 0x%lx, ptbase 0x%lx -> ",
	    regname(rs1), regname(rs2), RS1up, RS2up,
	    (unsigned long) cpu->mmu_ptbase_pa);

	if (FN(reload_pagetables)(cpu)) {
		return;
	}

	// While rs1 (if not x0) gives the vaddr of a page to flush,
	// and rs2 (if not x0) gives an asid to limit the effects to,
	// the cache we have is limited enough that it's more sensible
	// to just invalidate it unconditionally.
	(void)rs1;
	(void)rs2;

	// 0xffffffff is not a valid vpage so cannot match anything
	// (everything it's compared to will have 0 in the page offset bits)
	cpu->mmu_cached_vpage = 0xffffffff;

	TR("success");
}

static
void
FN(rx_do_amoswap)(struct riscvcpu *cpu, unsigned rd, unsigned rs1,unsigned rs2,
		  bool aq, bool rl)
{
	uint32_t memval, result;

	TRL("amoswap %s, %s, %s%s%s: 0x%lx -> [0x%lx] -> ",
	    regname(rd), regname(rs1), regname(rs2),
	    aq ? " [aq]" : "",
	    rl ? " [rl]" : "",
	    RS2sp, RS1up);

	/* don't need to do anything with these */
	(void)aq;
	(void)rl;

	if (FN(doload)(cpu, S_WORD, true, RS1u, &memval) == 0) {
		result = RS2u;
		if (FN(dostore)(cpu, S_WORD, RS1u, result) == 0) {
			if (rd != 0) {
				RDx = memval;
				TR("%ld", RDsp);
			}
			else {
				TR("discard %ld", (unsigned long)memval);
			}
		}
		else {
			TR("[store exception]");
		}
	}
	else {
		TR("[exception]");
	}
}

static
void
FN(rx_do_amoadd)(struct riscvcpu *cpu, unsigned rd, unsigned rs1,unsigned rs2,
		 bool aq, bool rl)
{
	uint32_t memval, result;

	TRL("amoadd %s, %s, %s%s%s: 0x%lx + [0x%lx] -> ",
	    regname(rd), regname(rs1), regname(rs2),
	    aq ? " [aq]" : "",
	    rl ? " [rl]" : "",
	    RS2sp, RS1up);

	/* don't need to do anything with these */
	(void)aq;
	(void)rl;

	if (FN(doload)(cpu, S_WORD, true, RS1u, &memval) == 0) {
		result = memval + RS2u;
		if (FN(dostore)(cpu, S_WORD, RS1u, result) == 0) {
			if (rd != 0) {
				RDx = memval;
				TR("%ld", RDsp);
			}
			else {
				TR("discard");
			}
		}
		else {
			TR("[store exception]");
		}
	}
	else {
		TR("[exception]");
	}
}

static
void
FN(rx_do_amoand)(struct riscvcpu *cpu, unsigned rd, unsigned rs1,unsigned rs2,
		 bool aq, bool rl)
{
	uint32_t memval, result;

	TRL("amoand %s, %s, %s%s%s: 0x%lu + [0x%lx] -> ",
	    regname(rd), regname(rs1), regname(rs2),
	    aq ? " [aq]" : "",
	    rl ? " [rl]" : "",
	    RS2up, RS1up);

	/* don't need to do anything with these */
	(void)aq;
	(void)rl;

	if (FN(doload)(cpu, S_WORD, true, RS1u, &memval) == 0) {
		result = memval & RS2u;
		if (FN(dostore)(cpu, S_WORD, RS1u, result) == 0) {
			if (rd != 0) {
				RDx = memval;
				TR("0x%lx", RDup);
			}
			else {
				TR("discard");
			}
		}
		else {
			TR("[store exception]");
		}
	}
	else {
		TR("[exception]");
	}
}

static
void
FN(rx_do_amoor)(struct riscvcpu *cpu, unsigned rd, unsigned rs1,unsigned rs2,
		 bool aq, bool rl)
{
	uint32_t memval, result;

	TRL("amoor %s, %s, %s%s%s: 0x%lu + [0x%lx] -> ",
	    regname(rd), regname(rs1), regname(rs2),
	    aq ? " [aq]" : "",
	    rl ? " [rl]" : "",
	    RS2up, RS1up);

	/* don't need to do anything with these */
	(void)aq;
	(void)rl;

	if (FN(doload)(cpu, S_WORD, true, RS1u, &memval) == 0) {
		result = memval | RS2u;
		if (FN(dostore)(cpu, S_WORD, RS1u, result) == 0) {
			if (rd != 0) {
				RDx = memval;
				TR("0x%lx", RDup);
			}
			else {
				TR("discard");
			}
		}
		else {
			TR("[store exception]");
		}
	}
	else {
		TR("[exception]");
	}
}

static
void
FN(rx_do_amoxor)(struct riscvcpu *cpu, unsigned rd, unsigned rs1,unsigned rs2,
		 bool aq, bool rl)
{
	uint32_t memval, result;

	TRL("amoxor %s, %s, %s%s%s: 0x%lu + [0x%lx] -> ",
	    regname(rd), regname(rs1), regname(rs2),
	    aq ? " [aq]" : "",
	    rl ? " [rl]" : "",
	    RS2up, RS1up);

	/* don't need to do anything with these */
	(void)aq;
	(void)rl;

	if (FN(doload)(cpu, S_WORD, true, RS1u, &memval) == 0) {
		result = memval ^ RS2u;
		if (FN(dostore)(cpu, S_WORD, RS1u, result) == 0) {
			if (rd != 0) {
				RDx = memval;
				TR("0x%lx", RDup);
			}
			else {
				TR("discard");
			}
		}
		else {
			TR("[store exception]");
		}
	}
	else {
		TR("[exception]");
	}
}

static
void
FN(rx_do_amomax)(struct riscvcpu *cpu, unsigned rd, unsigned rs1,unsigned rs2,
		 bool aq, bool rl)
{
	uint32_t memval, result;

	TRL("amomax %s, %s, %s%s%s: max 0x%lx, [0x%lx] -> ",
	    regname(rd), regname(rs1), regname(rs2),
	    aq ? " [aq]" : "",
	    rl ? " [rl]" : "",
	    RS2sp, RS1up);

	/* don't need to do anything with these */
	(void)aq;
	(void)rl;

	if (FN(doload)(cpu, S_WORD, true, RS1u, &memval) == 0) {
		result = (int32_t)memval > (int32_t)RS2u ? memval : RS2u;
		if (FN(dostore)(cpu, S_WORD, RS1u, result) == 0) {
			if (rd != 0) {
				RDx = memval;
				TR("%ld", RDsp);
			}
			else {
				TR("discard");
			}
		}
		else {
			TR("[store exception]");
		}
	}
	else {
		TR("[exception]");
	}
}

static
void
FN(rx_do_amomaxu)(struct riscvcpu *cpu, unsigned rd, unsigned rs1,unsigned rs2,
		 bool aq, bool rl)
{
	uint32_t memval, result;

	TRL("amomaxu %s, %s, %s%s%s: max 0x%lu, [0x%lx] -> ",
	    regname(rd), regname(rs1), regname(rs2),
	    aq ? " [aq]" : "",
	    rl ? " [rl]" : "",
	    RS2up, RS1up);

	/* don't need to do anything with these */
	(void)aq;
	(void)rl;

	if (FN(doload)(cpu, S_WORD, true, RS1u, &memval) == 0) {
		result = memval > RS2u ? memval : RS2u;
		if (FN(dostore)(cpu, S_WORD, RS1u, result) == 0) {
			if (rd != 0) {
				RDx = memval;
				TR("%lu", RDup);
			}
			else {
				TR("discard");
			}
		}
		else {
			TR("[store exception]");
		}
	}
	else {
		TR("[exception]");
	}
}

static
void
FN(rx_do_amomin)(struct riscvcpu *cpu, unsigned rd, unsigned rs1,unsigned rs2,
		 bool aq, bool rl)
{
	uint32_t memval, result;

	TRL("amomin %s, %s, %s%s%s: max 0x%lx, [0x%lx] -> ",
	    regname(rd), regname(rs1), regname(rs2),
	    aq ? " [aq]" : "",
	    rl ? " [rl]" : "",
	    RS2sp, RS1up);

	/* don't need to do anything with these */
	(void)aq;
	(void)rl;

	if (FN(doload)(cpu, S_WORD, true, RS1u, &memval) == 0) {
		result = (int32_t)memval < (int32_t)RS2u ? memval : RS2u;
		if (FN(dostore)(cpu, S_WORD, RS1u, result) == 0) {
			if (rd != 0) {
				RDx = memval;
				TR("%ld", RDsp);
			}
			else {
				TR("discard");
			}
		}
		else {
			TR("[store exception]");
		}
	}
	else {
		TR("[exception]");
	}
}

static
void
FN(rx_do_amominu)(struct riscvcpu *cpu, unsigned rd, unsigned rs1,unsigned rs2,
		 bool aq, bool rl)
{
	uint32_t memval, result;

	TRL("amomax %s, %s, %s%s%s: max 0x%lu, [0x%lx] -> ",
	    regname(rd), regname(rs1), regname(rs2),
	    aq ? " [aq]" : "",
	    rl ? " [rl]" : "",
	    RS2up, RS1up);

	/* don't need to do anything with these */
	(void)aq;
	(void)rl;

	if (FN(doload)(cpu, S_WORD, true, RS1u, &memval) == 0) {
		result = memval < RS2u ? memval : RS2u;
		if (FN(dostore)(cpu, S_WORD, RS1u, result) == 0) {
			if (rd != 0) {
				RDx = memval;
				TR("%lu", RDup);
			}
			else {
				TR("discard");
			}
		}
		else {
			TR("[store exception]");
		}
	}
	else {
		TR("[exception]");
	}
}

/*
 * Read values from CSRs.
 *
 * Note that this also does permission checks; it doesn't make sense
 * to do them earlier because of the complexity of whether reads and
 * writes actually happen.
 *
 * Returns nonzero if an exception happens. (Which means the csr number
 * was bad or we aren't allowed to access it.)
 */
static
int
FN(csrread)(struct riscvcpu *cpu, unsigned csr, uint32_t *val)
{
	/* it gets sign-extended but we don't want the upper bits */
	csr &= 0xfff;

	switch (csr) {
	    /* user traps: not supported */
	    case CSR_USTATUS:
	    case CSR_UIE:
	    case CSR_UTVEC:
	    case CSR_USCRATCH:
	    case CSR_UEPC:
	    case CSR_UCAUSE:
	    case CSR_UTVAL:
	    case CSR_UIP:
		break;
	    /* fpu: not supported (yet) */
	    case CSR_FFLAGS:
	    case CSR_FRM:
	    case CSR_FCSR:
		break;
	    /* supervisor */
	    case CSR_SSTATUS:
		if (!cpu->super) {
			break;
		}
		*val = read_csr_sstatus(cpu);
		return 0;
	    case CSR_SEDELEG:
	    case CSR_SIDELEG:
		/* part of user traps; not supported */
		break;
	    case CSR_SIE:
		if (!cpu->super) {
			break;
		}
		*val = read_csr_sie(cpu);
		return 0;
	    case CSR_STVEC:
		if (!cpu->super) {
			break;
		}
		*val = cpu->stvec;
		return 0;
	    case CSR_SCOUNTEREN:
		/* counters; not supported */
		break;
	    case CSR_SSCRATCH:
		if (!cpu->super) {
			break;
		}
		*val = cpu->sscratch;
		return 0;
	    case CSR_SEPC:
		if (!cpu->super) {
			break;
		}
		*val = read_csr_sepc(cpu);
		return 0;
	    case CSR_SCAUSE:
		if (!cpu->super) {
			break;
		}
		*val = read_csr_scause(cpu);
		return 0;
	    case CSR_STVAL:
		if (!cpu->super) {
			break;
		}
		*val = cpu->stval;
		return 0;
	    case CSR_SIP:
		if (!cpu->super) {
			break;
		}
		*val = read_csr_sip(cpu);
		return 0;
	    case CSR_SATP:
		if (!cpu->super) {
			break;
		}
		*val = read_csr_satp(cpu);
		return 0;
	    /* allow reading the model identity machine-mode registers */
	    case CSR_MVENDORID:
		if (!cpu->super) {
			break;
		}
		/* 0 means "noncommercial implementation" */
		*val = 0;
		return 0;
	    case CSR_MARCHID:
		if (!cpu->super) {
			break;
		}
		/*
		 * 0 means "not supported"; it is not clear whether
		 * there's a "software emulator" number to use or if
		 * in the long run we should get an official id for
		 * sys161. (XXX)
		 *
		 * upstream id table is here:
		 * https://github.com/riscv/riscv-isa-manual/blob/master/marchid.md
		 */
		*val = 0;
		return 0;
	    case CSR_MIMPID:
		if (!cpu->super) {
			break;
		}
		/*
		 * 0 means "not supported" but we should put the
		 * sys161 version in here.
		 */
		*val = 0;
		return 0;
	    case CSR_MHARTID:
		if (!cpu->super) {
			break;
		}
		*val = cpu->cpunum;
		return 0;
	    /* machine mode registers; prohibited */
	    case CSR_MSTATUS:
	    case CSR_MISA:
	    case CSR_MEDELEG:
	    case CSR_MIDELEG:
	    case CSR_MIE:
	    case CSR_MTVEC:
	    case CSR_MCOUNTEREN:
	    case CSR_MSCRATCH:
	    case CSR_MEPC:
	    case CSR_MCAUSE:
	    case CSR_MTVAL:
	    case CSR_MIP:
	    case CSR_PMPCFG0:
	    case CSR_PMPCFG1:
	    case CSR_PMPCFG2:
	    case CSR_PMPCFG3:
		break;
	    /* custom timer (XXX should go away) */
	    case CSR_SYS161_TIMER:
		*val = cpu->cycletrigger;
		return 0;
	    default:
		if (csr >= 0xc00 && csr < 0xc20) {
			/* performance counters: not supported (yet) */
			break;
		}
		if (csr >= 0xc80 && csr < 0xca0) {
			/* performance counters: not supported (yet) */
			break;
		}
		if (csr >= 0xb00 && csr < 0xb20) {
			/* M-mode performance counters: not supported */
			break;
		}
		if (csr >= 0xb80 && csr < 0xba0) {
			/* M-mode performance counters: not supported */
			break;
		}
		if (csr >= 0x320 && csr < 0x340) {
			/* M-mode performance counters: not supported */
			break;
		}
		if (csr >= 0x3b0 && csr < 0x3c0) {
			/* pmpaddr0-15; machine mode, prohibited */
			break;
		}
		if (csr >= 0x7a0 && csr < 0x7a4) {
			/* debug mode */
			break;
		}
		if (csr >= 0x7b0 && csr < 0x7b4) {
			/* debug mode */
			break;
		}
		/* no such register */
		break;
	}
	return -1;
}

/*
 * Write values to CSRs.
 *
 * Note that this also does permission checks; it doesn't make sense
 * to do them earlier because of the complexity of whether reads and
 * writes actually happen.
 *
 * Returns nonzero if an exception happens. (Which means the csr number
 * was bad or we aren't allowed to access it.)
 */
static
int
FN(csrwrite)(struct riscvcpu *cpu, unsigned csr, uint32_t val)
{
	/* it gets sign-extended but we don't want the upper bits */
	csr &= 0xfff;

	switch (csr) {
	    /* user traps: not supported */
	    case CSR_USTATUS:
	    case CSR_UIE:
	    case CSR_UTVEC:
	    case CSR_USCRATCH:
	    case CSR_UEPC:
	    case CSR_UCAUSE:
	    case CSR_UTVAL:
	    case CSR_UIP:
		break;
	    /* fpu: not supported (yet) */
	    case CSR_FFLAGS:
	    case CSR_FRM:
	    case CSR_FCSR:
		break;
	    /* supervisor */
	    case CSR_SSTATUS:
		if (!cpu->super) {
			break;
		}
		write_csr_sstatus(cpu, val);
		CPUTRACE(DOTRACE_IRQ, cpu->cpunum,
			 "writing SSTATUS: spie %s sie %s",
			 cpu->status_spie ? "enabled" : "disabled",
			 cpu->status_sie ? "enabled" : "disabled");
		return 0;
	    case CSR_SEDELEG:
	    case CSR_SIDELEG:
		/* part of user traps; not supported */
		break;
	    case CSR_SIE:
		if (!cpu->super) {
			break;
		}
		write_csr_sie(cpu, val);
		CPUTRACE(DOTRACE_IRQ, cpu->cpunum,
			 "writing SIE: LB %s TIMER %s IPI %s",
			 cpu->ie_seie ? "enabled" : "disabled",
			 cpu->ie_stie ? "enabled" : "disabled",
			 cpu->ie_ssie ? "enabled" : "disabled");
			 
		return 0;
	    case CSR_STVEC:
		if (!cpu->super) {
			break;
		}
		/* bottom two bits are mode, hardwired to 0 for us */
		cpu->stvec = val & 0xfffffffc;
		return 0;
	    case CSR_SCOUNTEREN:
		/* counters; not supported */
		break;
	    case CSR_SSCRATCH:
		if (!cpu->super) {
			break;
		}
		cpu->sscratch = val;
		return 0;
	    case CSR_SEPC:
		if (!cpu->super) {
			break;
		}
		write_csr_sepc(cpu, val);
		return 0;
	    case CSR_SCAUSE:
		if (!cpu->super) {
			break;
		}
		write_csr_scause(cpu, val);
		return 0;
	    case CSR_STVAL:
		if (!cpu->super) {
			break;
		}
		cpu->stval = val;
		return 0;
	    case CSR_SIP:
		if (!cpu->super) {
			break;
		}
		write_csr_sip(cpu, val);
		return 0;
	    case CSR_SATP:
		if (!cpu->super) {
			break;
		}
		write_csr_satp(cpu, val);
		return 0;
	    /* the model identity machine-mode registers are readonly */
	    case CSR_MVENDORID:
	    case CSR_MARCHID:
	    case CSR_MIMPID:
	    case CSR_MHARTID:
		break;
	    /* machine mode registers; prohibited */
	    case CSR_MSTATUS:
	    case CSR_MISA:
	    case CSR_MEDELEG:
	    case CSR_MIDELEG:
	    case CSR_MIE:
	    case CSR_MTVEC:
	    case CSR_MCOUNTEREN:
	    case CSR_MSCRATCH:
	    case CSR_MEPC:
	    case CSR_MCAUSE:
	    case CSR_MTVAL:
	    case CSR_MIP:
	    case CSR_PMPCFG0:
	    case CSR_PMPCFG1:
	    case CSR_PMPCFG2:
	    case CSR_PMPCFG3:
		break;
	    /* custom timer (XXX should go away) */
	    case CSR_SYS161_TIMER:
		if (!cpu->super) {
			break;
		}
		/* XXX this is gross */
		cpu->cycletrigger = cpu->cyclecount + val;
		cpu->irq_timer = false;
		return 0;
	    default:
		if (csr >= 0xc00 && csr < 0xc20) {
			/* performance counters: not supported (yet) */
			break;
		}
		if (csr >= 0xc80 && csr < 0xca0) {
			/* performance counters: not supported (yet) */
			break;
		}
		if (csr >= 0xb00 && csr < 0xb20) {
			/* M-mode performance counters: not supported */
			break;
		}
		if (csr >= 0xb80 && csr < 0xba0) {
			/* M-mode performance counters: not supported */
			break;
		}
		if (csr >= 0x320 && csr < 0x340) {
			/* M-mode performance counters: not supported */
			break;
		}
		if (csr >= 0x3b0 && csr < 0x3c0) {
			/* pmpaddr0-15; machine mode, prohibited */
			break;
		}
		if (csr >= 0x7a0 && csr < 0x7a4) {
			/* debug mode */
			break;
		}
		if (csr >= 0x7b0 && csr < 0x7b4) {
			/* debug mode */
			break;
		}
		/* no such register */
		break;
	}
	return -1;
}

/* read a csr */
static
int
FN(rx_do_csr_r)(struct riscvcpu *cpu, unsigned rd, unsigned csr)
{
	uint32_t val;

	if (FN(csrread)(cpu, csr, &val)) {
		TR("exception");
		return -1;
	}
	if (rd != 0) {
		RDx = val;
		TR("read 0x%lx", RDup);
	}
	else {
		TR("discard 0x%lx", (unsigned long)val);
	}
	return 0;
}

/* write a csr */
static
int
FN(rx_do_csr_w)(struct riscvcpu *cpu, uint32_t val, unsigned csr)
{
	TRL("write 0x%lx -> ", (unsigned long)val);
	if (FN(csrwrite)(cpu, csr, val)) {
		TR("exception");
		return -1;
	}
	TR("read nothing");
	return 0;
}

/* exchange with a csr */
static
int
FN(rx_do_csr_x)(struct riscvcpu *cpu, unsigned rd, uint32_t val, unsigned csr)
{
	uint32_t oldval;

	TRL("write 0x%lx -> ", (unsigned long)val);
	if (FN(csrread)(cpu, csr, &oldval)) {
		TR("exception");
		return -1;
	}
	if (FN(csrwrite)(cpu, csr, val)) {
		TR("exception");
		return -1;
	}
	if (rd != 0) {
		RDx = oldval;
		TR("read 0x%lx", RDup);
	}
	else {
		TR("discard 0x%lx", (unsigned long)oldval);
	}
	return 0;
}

/* read and change a csr */
static
int
FN(rx_do_csr_rc)(struct riscvcpu *cpu, unsigned rd, unsigned mask, bool doset,
		 unsigned csr)
{
	uint32_t oldval, newval;

	if (FN(csrread)(cpu, csr, &oldval)) {
		TR("exception");
		return -1;
	}
	newval = doset ? (oldval | mask) : (oldval & ~(uint32_t) mask);
	if (FN(csrwrite)(cpu, csr, newval)) {
		TR("exception");
		return -1;
	}
	if (rd != 0) {
		RDx = oldval;
		TR("read 0x%lx -> write 0x%lx",
		   (unsigned long)oldval, (unsigned long)newval);
	}
	else {
		TR("discard 0x%lx -> write 0x%lx",
		   (unsigned long)oldval, (unsigned long)newval);
	}
	return 0;
}

/* csrrw instruction: either exchange or write */
static
int
FN(rx_do_csrrw)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, unsigned csr)
{
	TRL("csrrw %s, %u, %s: ",
	    regname(rd), csr, regname(rs1));

	if (rd != 0) {
		return FN(rx_do_csr_x)(cpu, rd, RS1u, csr);
	}
	else {
		return FN(rx_do_csr_w)(cpu, RS1u, csr);
	}
}

/* csrrwi instruction: either exchange or write */
static
int
FN(rx_do_csrrwi)(struct riscvcpu *cpu, unsigned rd, unsigned imm, unsigned csr)
{
	TRL("csrrw %s, %u, %u: ",
	    regname(rd), csr, imm);

	if (rd != 0) {
		return FN(rx_do_csr_x)(cpu, rd, imm, csr);
	}
	else {
		return FN(rx_do_csr_w)(cpu, imm, csr);
	}
}

/* csrrs instruction: either read-and-change or read */
static
int
FN(rx_do_csrrs)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, unsigned csr)
{
	TRL("csrrs %s, %u, %s: ",
	    regname(rd), csr, regname(rs1));

	if (rs1 != 0) {
		return FN(rx_do_csr_rc)(cpu, rd, RS1u, true/*set*/, csr);
	}
	else {
		return FN(rx_do_csr_r)(cpu, rd, csr);
	}
}

/* csrrsi instruction: either read-and-change or read */
static
int
FN(rx_do_csrrsi)(struct riscvcpu *cpu, unsigned rd, unsigned imm, unsigned csr)
{
	TRL("csrrsi %s, %u, %u: ",
	    regname(rd), csr, imm);

	if (imm != 0) {
		return FN(rx_do_csr_rc)(cpu, rd, imm, true/*set*/, csr);
	}
	else {
		return FN(rx_do_csr_r)(cpu, rd, csr);
	}
}

/* csrrc instruction: either read-and-change or read */
static
int
FN(rx_do_csrrc)(struct riscvcpu *cpu, unsigned rd, unsigned rs1, unsigned csr)
{
	TRL("csrrc %s, %u, %s: ",
	    regname(rd), csr, regname(rs1));

	if (rs1 != 0) {
		return FN(rx_do_csr_rc)(cpu, rd, RS1u, false/*clear*/, csr);
	}
	else {
		return FN(rx_do_csr_r)(cpu, rd, csr);
	}
}

/* csrrci instruction: either read-and-change or read */
static
int
FN(rx_do_csrrci)(struct riscvcpu *cpu, unsigned rd, unsigned imm, unsigned csr)
{
	TRL("csrrci %s, %u, %u: ",
	    regname(rd), csr, imm);

	if (imm != 0) {
		return FN(rx_do_csr_rc)(cpu, rd, imm, false/*clear*/, csr);
	}
	else {
		return FN(rx_do_csr_r)(cpu, rd, csr);
	}
}


////////////////////////////////////////////////////////////
// decoding 32-bit instructions

#define NEED_RS1     uint32_t rs1 = (insn & 0x000f8000) >> 15	// register
#define NEED_RS2     uint32_t rs2 = (insn & 0x01f00000) >> 20	// register
#define NEED_RD      uint32_t rd  = (insn & 0x00000f80) >> 7    // register
#define NEED_AQ      bool aq = (insn & 0x04000000) != 0;
#define NEED_RL      bool rl = (insn & 0x02000000) != 0;
#define NEED_IMM     uint32_t imm

#define SIGNEXT(k)   ((insn & 0x80000000) ? (0xffffffff << (k)) : 0)
#define SET_I_IMM    (imm = SIGNEXT(12) | (insn >> 20))
#define SET_S_IMM    (imm = SIGNEXT(12) | \
		      ((insn & 0xfe000000) >> 20) | ((insn & 0x00000f80) >> 7))
#define SET_B_IMM    (imm = SIGNEXT(12) | \
		      ((insn & 0x7e000000) >> 20) | \
                      ((insn & 0x00000f00) >> 7) | \
                      ((insn & 0x00000080) << 4))
#define SET_U_IMM    (imm = insn & 0xfffff000)
#define SET_J_IMM    (imm = SIGNEXT(19) | \
                      (insn & 0x000ff000) | \
                      ((insn & 0x00100000) >> 9) | \
                      ((insn & 0x7fe00000) >> 20))
#define SET_SH_IMM   (SET_I_IMM, imm &= 31)


/*
 * Base instructions
 */
	
static
void
FN(rx_addi)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_IMM; SET_I_IMM;
	FN(rx_do_addi)(cpu, rd, rs1, imm);
}

static
void
FN(rx_slti)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_IMM; SET_I_IMM;
	FN(rx_do_slti)(cpu, rd, rs1, imm);
}

static
void
FN(rx_sltiu)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_IMM; SET_I_IMM;
	FN(rx_do_sltiu)(cpu, rd, rs1, imm);
}

static
void
FN(rx_andi)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_IMM; SET_I_IMM;
	FN(rx_do_andi)(cpu, rd, rs1, imm);
}

static
void
FN(rx_ori)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_IMM; SET_I_IMM;
	FN(rx_do_ori)(cpu, rd, rs1, imm);
}

static
void
FN(rx_xori)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_IMM; SET_I_IMM;
	FN(rx_do_xori)(cpu, rd, rs1, imm);
}

static
void
FN(rx_slli)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_IMM; SET_SH_IMM;
	FN(rx_do_slli)(cpu, rd, rs1, imm);
}

static
void
FN(rx_srli)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_IMM; SET_SH_IMM;
	FN(rx_do_srli)(cpu, rd, rs1, imm);
}

static
void
FN(rx_srai)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_IMM; SET_SH_IMM;
	FN(rx_do_srai)(cpu, rd, rs1, imm);
}

static
void
FN(rx_lui)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_IMM; SET_U_IMM;
	FN(rx_do_lui)(cpu, rd, imm);
}

static
void
FN(rx_auipc)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_IMM; SET_U_IMM;
	FN(rx_do_auipc)(cpu, rd, imm);
}

static
void
FN(rx_add)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2;
	FN(rx_do_add)(cpu, rd, rs1, rs2);
}

static
void
FN(rx_slt)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2;
	FN(rx_do_slt)(cpu, rd, rs1, rs2);
}

static
void
FN(rx_sltu)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2;
	FN(rx_do_sltu)(cpu, rd, rs1, rs2);
}

static
void
FN(rx_and)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2;
	FN(rx_do_and)(cpu, rd, rs1, rs2);
}

static
void
FN(rx_or)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2;
	FN(rx_do_or)(cpu, rd, rs1, rs2);
}

static
void
FN(rx_xor)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2;
	FN(rx_do_xor)(cpu, rd, rs1, rs2);
}

static
void
FN(rx_sll)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2;
	FN(rx_do_sll)(cpu, rd, rs1, rs2);
}

static
void
FN(rx_srl)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2;
	FN(rx_do_srl)(cpu, rd, rs1, rs2);
}

static
void
FN(rx_sra)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2;
	FN(rx_do_sra)(cpu, rd, rs1, rs2);
}

static
void
FN(rx_sub)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2;
	FN(rx_do_sub)(cpu, rd, rs1, rs2);
}

static
void
FN(rx_jal)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_IMM; SET_J_IMM;
	FN(rx_do_jal)(cpu, rd, imm, cpu->pc + 4);
}

static
void
FN(rx_jalr)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_IMM; SET_I_IMM;
	FN(rx_do_jalr)(cpu, rd, rs1, imm, cpu->pc + 4);
}

static
void
FN(rx_beq)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RS1; NEED_RS2; NEED_IMM; SET_B_IMM;
	FN(rx_do_beq)(cpu, rs1, rs2, imm);
}

static
void
FN(rx_bne)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RS1; NEED_RS2; NEED_IMM; SET_B_IMM;
	FN(rx_do_bne)(cpu, rs1, rs2, imm);
}

static
void
FN(rx_blt)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RS1; NEED_RS2; NEED_IMM; SET_B_IMM;
	FN(rx_do_blt)(cpu, rs1, rs2, imm);
}

static
void
FN(rx_bltu)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RS1; NEED_RS2; NEED_IMM; SET_B_IMM;
	FN(rx_do_bltu)(cpu, rs1, rs2, imm);
}

static
void
FN(rx_bge)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RS1; NEED_RS2; NEED_IMM; SET_B_IMM;
	FN(rx_do_bge)(cpu, rs1, rs2, imm);
}

static
void
FN(rx_bgeu)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RS1; NEED_RS2; NEED_IMM; SET_B_IMM;
	FN(rx_do_bgeu)(cpu, rs1, rs2, imm);
}

static
void
FN(rx_lb)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_IMM; SET_I_IMM;
	FN(rx_do_lb)(cpu, rd, rs1, imm);
}

static
void
FN(rx_lbu)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_IMM; SET_I_IMM;
	FN(rx_do_lbu)(cpu, rd, rs1, imm);
}

static
void
FN(rx_lh)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_IMM; SET_I_IMM;
	FN(rx_do_lh)(cpu, rd, rs1, imm);
}

static
void
FN(rx_lhu)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_IMM; SET_I_IMM;
	FN(rx_do_lhu)(cpu, rd, rs1, imm);
}

static
void
FN(rx_lw)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_IMM; SET_I_IMM;
	FN(rx_do_lw)(cpu, rd, rs1, imm);
}

static
void
FN(rx_sb)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RS2; NEED_RS1; NEED_IMM; SET_S_IMM;
	FN(rx_do_sb)(cpu, rs2, rs1, imm);
}

static
void
FN(rx_sh)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RS2; NEED_RS1; NEED_IMM; SET_S_IMM;
	FN(rx_do_sh)(cpu, rs2, rs1, imm);
}

static
void
FN(rx_sw)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RS2; NEED_RS1; NEED_IMM; SET_S_IMM;
	FN(rx_do_sw)(cpu, rs2, rs1, imm);
}

static
void
FN(rx_fence)(struct riscvcpu *cpu, uint32_t insn)
{
	unsigned fm, pred, succ;
	NEED_IMM; SET_I_IMM;

	fm = imm >> 8;
	pred = (imm >> 4) & 0xf;
	succ = imm & 0xf;

	if (fm == 8 && pred == 3 && succ == 3) {
		/* FENCE.TSO */
		FN(rx_do_fencetso)(cpu);
	}
	else {
		/* all reserved fences are supposed to be generic ones */
		FN(rx_do_fence)(cpu, pred, succ);
	}
}

static
void
FN(rx_ecall)(struct riscvcpu *cpu, uint32_t insn)
{
	(void)insn;
	FN(rx_do_ecall)(cpu);
}

static
void
FN(rx_ebreak)(struct riscvcpu *cpu, uint32_t insn)
{
	(void)insn;
	FN(rx_do_ebreak)(cpu);
}

/*
 * Supervisor instructions
 */

static
void
FN(rx_sret)(struct riscvcpu *cpu, uint32_t insn)
{
	(void)insn;
	FN(rx_do_sret)(cpu);
}

static
void
FN(rx_wfi)(struct riscvcpu *cpu, uint32_t insn)
{
	(void)insn;
	FN(rx_do_wfi)(cpu);
}

static
void
FN(rx_sfence_vma)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RS1; NEED_RS2;
	FN(rx_do_sfence_vma)(cpu, rs1, rs2);
}

/*
 * Zifencei extension (icache flush)
 */

static
void
FN(rx_fencei)(struct riscvcpu *cpu, uint32_t insn)
{
	(void)insn;
	FN(rx_do_fencei)(cpu);
}

/*
 * M extension (multiply/divide)
 */

static
void
FN(rx_mul)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2;
	FN(rx_do_mul)(cpu, rd, rs1, rs2);
}

static
void
FN(rx_mulh)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2;
	FN(rx_do_mulh)(cpu, rd, rs1, rs2);
}

static
void
FN(rx_mulhu)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2;
	FN(rx_do_mulhu)(cpu, rd, rs1, rs2);
}

static
void
FN(rx_mulhsu)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2;
	FN(rx_do_mulhsu)(cpu, rd, rs1, rs2);
}

static
void
FN(rx_div)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2;
	FN(rx_do_div)(cpu, rd, rs1, rs2);
}

static
void
FN(rx_divu)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2;
	FN(rx_do_divu)(cpu, rd, rs1, rs2);
}

static
void
FN(rx_rem)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2;
	FN(rx_do_rem)(cpu, rd, rs1, rs2);
}

static
void
FN(rx_remu)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2;
	FN(rx_do_remu)(cpu, rd, rs1, rs2);
}

/*
 * A extension (atomics)
 */

static
void
FN(rx_lr)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_AQ; NEED_RL;
	FN(rx_do_lr)(cpu, rd, rs1, aq, rl);
}

static
void
FN(rx_sc)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2; NEED_AQ; NEED_RL;
	FN(rx_do_sc)(cpu, rd, rs1, rs2, aq, rl);
}

static
void
FN(rx_amoswap)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2; NEED_AQ; NEED_RL;
	FN(rx_do_amoswap)(cpu, rd, rs1, rs2, aq, rl);
}

static
void
FN(rx_amoadd)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2; NEED_AQ; NEED_RL;
	FN(rx_do_amoadd)(cpu, rd, rs1, rs2, aq, rl);
}

static
void
FN(rx_amoand)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2; NEED_AQ; NEED_RL;
	FN(rx_do_amoand)(cpu, rd, rs1, rs2, aq, rl);
}

static
void
FN(rx_amoor)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2; NEED_AQ; NEED_RL;
	FN(rx_do_amoor)(cpu, rd, rs1, rs2, aq, rl);
}

static
void
FN(rx_amoxor)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2; NEED_AQ; NEED_RL;
	FN(rx_do_amoxor)(cpu, rd, rs1, rs2, aq, rl);
}

static
void
FN(rx_amomax)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2; NEED_AQ; NEED_RL;
	FN(rx_do_amomax)(cpu, rd, rs1, rs2, aq, rl);
}

static
void
FN(rx_amomaxu)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2; NEED_AQ; NEED_RL;
	FN(rx_do_amomaxu)(cpu, rd, rs1, rs2, aq, rl);
}

static
void
FN(rx_amomin)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2; NEED_AQ; NEED_RL;
	FN(rx_do_amomin)(cpu, rd, rs1, rs2, aq, rl);
}

static
void
FN(rx_amominu)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_RS2; NEED_AQ; NEED_RL;
	FN(rx_do_amominu)(cpu, rd, rs1, rs2, aq, rl);
}

/*
 * Zicsr extension (control registers)
 */

static
void
FN(rx_csrrw)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_IMM; SET_I_IMM;
	/* imm is the csr number */
	if (FN(rx_do_csrrw)(cpu, rd, rs1, imm)) {
		FN(exception)(cpu, EX_ILLINST, insn, ", illegal CSR access");
	}
}

static
void
FN(rx_csrrs)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_IMM; SET_I_IMM;
	/* imm is the csr number */
	if (FN(rx_do_csrrs)(cpu, rd, rs1, imm)) {
		FN(exception)(cpu, EX_ILLINST, insn, ", illegal CSR access");
	}
}

static
void
FN(rx_csrrc)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_IMM; SET_I_IMM;
	/* imm is the csr number */
	if (FN(rx_do_csrrc)(cpu, rd, rs1, imm)) {
		FN(exception)(cpu, EX_ILLINST, insn, ", illegal CSR access");
	}
}

static
void
FN(rx_csrrwi)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_IMM; SET_I_IMM;
	/* imm is the csr number; rs1 value is the actual immediate */
	if (FN(rx_do_csrrwi)(cpu, rd, rs1, imm)) {
		FN(exception)(cpu, EX_ILLINST, insn, ", illegal CSR access");
	}
}

static
void
FN(rx_csrrsi)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_IMM; SET_I_IMM;
	/* imm is the csr number; rs1 value is the actual immediate */
	if (FN(rx_do_csrrsi)(cpu, rd, rs1, imm)) {
		FN(exception)(cpu, EX_ILLINST, insn, ", illegal CSR access");
	}
}

static
void
FN(rx_csrrci)(struct riscvcpu *cpu, uint32_t insn)
{
	NEED_RD; NEED_RS1; NEED_IMM; SET_I_IMM;
	/* imm is the csr number; rs1 value is the actual immediate */
	if (FN(rx_do_csrrci)(cpu, rd, rs1, imm)) {
		FN(exception)(cpu, EX_ILLINST, insn, ", illegal CSR access");
	}
}

static
void
FN(rx_ill)(struct riscvcpu *cpu, uint32_t insn)
{
	TR("[illegal instruction %08lx]", (unsigned long) insn);
	FN(exception)(cpu, EX_ILLINST, insn, "");
}

/*
 * opcode dispatch
 */

static
int
FN(insn32)(struct riscvcpu *cpu, uint32_t insn)
{
	uint32_t op, funct3, funct7;

	/*
	 * Decode instruction.
	 */

	/* primary opcode, excluding size bits (already handled) */
	op = ((insn & 0x0000007c) >> 2);
	/* secondary opcode fields */
	funct3 = (insn & 0x00007000) >> 12;
	funct7 = (insn & 0xfe000000) >> 25;

	switch (op) {
	    case OP32_LOAD:
		switch (funct3) {
		    case OPLOAD_LB: FN(rx_lb)(cpu, insn); break;
		    case OPLOAD_LH: FN(rx_lh)(cpu, insn); break;
		    case OPLOAD_LW: FN(rx_lw)(cpu, insn); break;
		    case OPLOAD_LBU: FN(rx_lbu)(cpu, insn); break;
		    case OPLOAD_LHU: FN(rx_lhu)(cpu, insn); break;
		    default: FN(rx_ill)(cpu, insn); break;
		}
		break;
	    case OP32_LOADFP: FN(rx_ill)(cpu, insn); break;
	    case OP32_MISCMEM:
		switch (funct3) {
		    case OPMISCMEM_FENCE: FN(rx_fence)(cpu, insn); break;
		    case OPMISCMEM_FENCEI: FN(rx_fencei)(cpu, insn); break;
		    default: FN(rx_ill)(cpu, insn); break;
		}
		break;
	    case OP32_OPIMM:
		switch (funct3) {
		    case OPOPIMM_ADDI: FN(rx_addi)(cpu, insn); break;
		    case OPOPIMM_SLI:
			switch (funct7) {
			    case 0: FN(rx_slli)(cpu, insn); break;
			    default: FN(rx_ill)(cpu, insn); break;
			}
			break;
		    case OPOPIMM_SLTI: FN(rx_slti)(cpu, insn); break;
		    case OPOPIMM_SLTIU: FN(rx_sltiu)(cpu, insn); break;
		    case OPOPIMM_XORI: FN(rx_xori)(cpu, insn); break;
		    case OPOPIMM_SRI:
			switch (funct7) {
			    case 0: FN(rx_srli)(cpu, insn); break;
			    case 32: FN(rx_srai)(cpu, insn); break;
			    default: FN(rx_ill)(cpu, insn); break;
			}
			break;
		    case OPOPIMM_ORI: FN(rx_ori)(cpu, insn); break;
		    case OPOPIMM_ANDI: FN(rx_andi)(cpu, insn); break;
		}
		break;
	    case OP32_AUIPC: FN(rx_auipc)(cpu, insn); break;
	    case OP32_OPIMM32: FN(rx_ill)(cpu, insn); break;
	    case OP32_STORE:
		switch (funct3) {
		    case OPSTORE_SB: FN(rx_sb)(cpu, insn); break;
		    case OPSTORE_SH: FN(rx_sh)(cpu, insn); break;
		    case OPSTORE_SW: FN(rx_sw)(cpu, insn); break;
		    default: FN(rx_ill)(cpu, insn); break;
		}
		break;
	    case OP32_STOREFP: FN(rx_ill)(cpu, insn); break;
	    case OP32_AMO:
                if (funct3 != OPAMO_32) {
			FN(rx_ill)(cpu, insn);
		}
		else {
			switch (funct7 >> 2) {
			    case OPAMO_AMOADD: FN(rx_amoadd)(cpu, insn); break;
			    case OPAMO_AMOSWAP:FN(rx_amoswap)(cpu, insn);break;
			    case OPAMO_LR: FN(rx_lr)(cpu, insn); break;
			    case OPAMO_SC: FN(rx_sc)(cpu, insn); break;
			    case OPAMO_AMOXOR: FN(rx_amoxor)(cpu, insn); break;
			    case OPAMO_AMOAND: FN(rx_amoand)(cpu, insn); break;
			    case OPAMO_AMOOR:  FN(rx_amoor)(cpu, insn); break;
			    case OPAMO_AMOMIN: FN(rx_amomin)(cpu, insn); break;
			    case OPAMO_AMOMAX: FN(rx_amomax)(cpu, insn); break;
			    case OPAMO_AMOMINU:FN(rx_amominu)(cpu, insn);break;
			    case OPAMO_AMOMAXU:FN(rx_amomaxu)(cpu, insn);break;
			    default: FN(rx_ill)(cpu, insn); break;
			}
		}
		break;
	    case OP32_OP:
		switch (funct7) {
		    case OPOP_ARITH:
			switch (funct3) {
			    case OPARITH_ADD: FN(rx_add)(cpu, insn); break;
			    case OPARITH_SLL: FN(rx_sll)(cpu, insn); break;
			    case OPARITH_SLT: FN(rx_slt)(cpu, insn); break;
			    case OPARITH_SLTU: FN(rx_sltu)(cpu, insn); break;
			    case OPARITH_XOR: FN(rx_xor)(cpu, insn); break;
			    case OPARITH_SRL: FN(rx_srl)(cpu, insn); break;
			    case OPARITH_OR: FN(rx_or)(cpu, insn); break;
			    case OPARITH_AND: FN(rx_and)(cpu, insn); break;
			    default: FN(rx_ill)(cpu, insn); break;
			}
			break;
		    case OPOP_NARITH:
			switch (funct3) {
			    case OPNARITH_SUB: FN(rx_sub)(cpu, insn); break;
			    case OPNARITH_SRA: FN(rx_sra)(cpu, insn); break;
			    default: FN(rx_ill)(cpu, insn); break;
			}
			break;
		    case OPOP_MULDIV:
			switch (funct3) {
			    case OPMULDIV_MUL: FN(rx_mul)(cpu, insn); break;
			    case OPMULDIV_MULH: FN(rx_mulh)(cpu, insn); break;
			    case OPMULDIV_MULHSU:FN(rx_mulhsu)(cpu,insn);break;
			    case OPMULDIV_MULHU: FN(rx_mulhu)(cpu,insn); break;
			    case OPMULDIV_DIV: FN(rx_div)(cpu, insn); break;
			    case OPMULDIV_DIVU: FN(rx_divu)(cpu, insn); break;
			    case OPMULDIV_REM: FN(rx_rem)(cpu, insn); break;
			    case OPMULDIV_REMU: FN(rx_remu)(cpu, insn); break;
			    default: FN(rx_ill)(cpu, insn); break;
			}
			break;
		    default: FN(rx_ill)(cpu, insn); break;
		}
		break;
	    case OP32_LUI: FN(rx_lui)(cpu, insn); break;
	    case OP32_OP32: FN(rx_ill)(cpu, insn); break;
	    case OP32_MADD: FN(rx_ill)(cpu, insn); break;
	    case OP32_MSUB: FN(rx_ill)(cpu, insn); break;
	    case OP32_NMADD: FN(rx_ill)(cpu, insn); break;
	    case OP32_NMSUB: FN(rx_ill)(cpu, insn); break;
	    case OP32_OPFP: FN(rx_ill)(cpu, insn); break;
	    case OP32_BRANCH:
		switch (funct3) {
		    case OPBRANCH_BEQ: FN(rx_beq)(cpu, insn); break;
		    case OPBRANCH_BNE: FN(rx_bne)(cpu, insn); break;
		    case OPBRANCH_BLT: FN(rx_blt)(cpu, insn); break;
		    case OPBRANCH_BGE: FN(rx_bge)(cpu, insn); break;
		    case OPBRANCH_BLTU: FN(rx_bltu)(cpu, insn); break;
		    case OPBRANCH_BGEU: FN(rx_bgeu)(cpu, insn); break;
		    default: FN(rx_ill)(cpu, insn); break;
		}
		break;
	    case OP32_JALR:
		if (funct3 != 0) {
			FN(rx_ill)(cpu, insn);
		}
		else {
			FN(rx_jalr)(cpu, insn);
		}
		break;
	    case OP32_JAL: FN(rx_jal)(cpu, insn); break;
	    case OP32_SYSTEM:
		switch (funct3) {
		    case OPSYSTEM_PRIV:
			switch (funct7) {
			    case OPPRIV_USER:
				switch ((insn & 0x01f00000) >> 20) {
				    case OPUSER_ECALL:
					FN(rx_ecall)(cpu, insn);
					break;
				    case OPUSER_EBREAK:
					if (FN(try_breakpoint)(cpu)) {
						TR("debugger breakpoint");
						return 1;
					}
					FN(rx_ebreak)(cpu, insn);
					break;
				    case OPUSER_URET:
					FN(rx_ill)(cpu, insn);
					break;
				    default:
					FN(rx_ill)(cpu, insn);
					break;
				}
				break;
			    case OPPRIV_SYSTEM:
				switch ((insn & 0x01f00000) >> 20) {
				    case OPSYSTEM_SRET:
					if (cpu->super) {
						FN(rx_sret)(cpu, insn);
					}
					else {
						FN(rx_ill)(cpu, insn);
					}
					break;
				    case OPSYSTEM_WFI:
					if (cpu->super) {
						FN(rx_wfi)(cpu, insn);
					}
					else {
						FN(rx_ill)(cpu, insn);
					}
					break;
				    default:
					FN(rx_ill)(cpu, insn);
					break;
				}
				break;
			    case OPPRIV_SFENCE_VMA:
				if (cpu->super) {
					FN(rx_sfence_vma)(cpu, insn);
				}
				else {
					FN(rx_ill)(cpu, insn);
				}
				break;
			    default: FN(rx_ill)(cpu, insn); break;
			}
			break;
		    case OPSYSTEM_CSRRW: FN(rx_csrrw)(cpu, insn); break;
		    case OPSYSTEM_CSRRS: FN(rx_csrrs)(cpu, insn); break;
		    case OPSYSTEM_CSRRC: FN(rx_csrrc)(cpu, insn); break;
		    case OPSYSTEM_CSRRWI: FN(rx_csrrwi)(cpu, insn); break;
		    case OPSYSTEM_CSRRSI: FN(rx_csrrsi)(cpu, insn); break;
		    case OPSYSTEM_CSRRCI: FN(rx_csrrci)(cpu, insn); break;
		    default: FN(rx_ill)(cpu, insn); break;
		}
		break;
	}
	return 0;
}

////////////////////////////////////////////////////////////
// decoding 16-bit instructions

static
int
FN(insn16)(struct riscvcpu *cpu, uint32_t insn)
{
	unsigned op;
	unsigned imm;

	/*
	 * Note: the upper half word might contain the next
	 * instruction, so be sure to ignore it.
	 */

	op = (insn & 0xe000) >> 13;
	switch (insn & 3) {
	    case 0: {
		    /* Quadrant 0 */
		    unsigned r1, r2;

		    r1 = 0x8 | ((insn & 0x0380) > 7); /* take bits 7-9 */
		    r2 = 0x8 | ((insn & 0x001c) > 2); /* bits 2-4 */

		    switch (op) {
			case 0:
			    /* C.ADDI4SPN */
			    /*
			     * immediate is:
			     *    bit 6-9  from bit 7-10
			     *    bit 4-5  from bit 11-12
			     *    bit 3    from bit 5
			     *    bit 2    from bit 6
			     */
			    imm =
				    ((insn & 0x0780) >> 1) |
				    ((insn & 0x1800) >> 7) |
				    ((insn & 0x0020) >> 2) |
				    ((insn & 0x0040) >> 4);
			    if (imm == 0) {
				    /* if r2 field 0 illegal, else reserved*/
				    FN(rx_ill)(cpu, insn & 0xffff);
			    }
			    else {
				    FN(rx_do_addi)(cpu, r2, 2/*sp*/, imm);
			    }
			    break;
			case 1:
			    /* C.FLD (or C.LQ on RV128), not supported */
			    FN(rx_ill)(cpu, insn & 0xffff);
			    break;
			case 2:
			    /* C.LW */
			    /*
			     * immediate is:
			     *    bit 6    from bit 5
			     *    bit 3-5  from bit 10-12
			     *    bit 2    from bit 4
			     */
			    imm =
				    ((insn & 0x0020) << 1) |
				    ((insn & 0x1c00) >> 7) |
				    ((insn & 0x0010) >> 2);
			    FN(rx_do_lw)(cpu, r2, r1, imm);
			    break;
			case 3:
			    /* S.FLW (or C.LD on RV64/128), not supported */
			    FN(rx_ill)(cpu, insn & 0xffff);
			    break;
			case 4:
			    /* reserved */
			    FN(rx_ill)(cpu, insn & 0xffff);
			    break;
			case 5:
			    /* C.FSD (or C.SQ on RV128), not supported */
			    FN(rx_ill)(cpu, insn & 0xffff);
			    break;
			case 6:
			    /* C.SW */
			    /* immediate same as C.LW */
			    imm =
				    ((insn & 0x0020) << 1) |
				    ((insn & 0x1c00) >> 7) |
				    ((insn & 0x0010) >> 2);
			    FN(rx_do_sw)(cpu, r2, r1, imm);
			    break;
			case 7:
			    /* S.FSW (or C.SD on RV64/128), not supported */
			    FN(rx_ill)(cpu, insn & 0xffff);
			    break;
		    }
	    }
	    break;
	    case 1: {
		    /* Quadrant 1 */
		    unsigned r1, r2;

		    switch (op) {
			case 0:
			    /* C.ADDI */
			    /*
			     * explicit nop if rd field is 0
			     * (and a hint if imm is then not 0)
			     * and if not an explicit nop, a hint if imm is 0
			     * none of that matters though, can just do addi
			     */
			    /*
			     * immediate is:
			     *    bit 5    from bit 12
			     *    bit 0-4  from bit 2-6
			     *    ...sign-extended from bit 12
			     */
			    imm =
				    ((insn & 0x007c) >> 2) |
				    ((insn & 0x1000) >> 7);
			    if (insn & 0x1000) {
				    imm |= 0xffffffc0;
			    }
			    /* full rd field */
			    r1 = (insn & 0x0f80) >> 7;
			    FN(rx_do_addi)(cpu, r1, r1, imm);
			    break;
			case 1:
			    /* C.JAL */
			    /*
			     * immediate is:
			     *    bit 11    from bit 12
			     *    bit 10    from bit 8
			     *    bit 8-9   from bit 9-10
			     *    bit 7     from bit 6
			     *    bit 6     from bit 7
			     *    bit 5     from bit 2
			     *    bit 4     from bit 11
			     *    bit 1-3   from bit 3-5
			     *    ...sign-extended from bit 11.
			     * (sheesh!)
			     */
			    imm =
				    ((insn & 0x1000) >> 1) |
				    ((insn & 0x0100) << 2) |
				    ((insn & 0x0600) >> 1) |
				    ((insn & 0x0040) << 1) |
				    ((insn & 0x0080) >> 1) |
				    ((insn & 0x0004) << 3) |
				    ((insn & 0x0800) >> 7) |
				    ((insn & 0x0038) >> 2);
			    if (insn & 0x1000) {
				    imm |= 0xfffff000;
			    }
			    FN(rx_do_jal)(cpu, 1/*ra*/, imm, cpu->pc + 2);
			    break;
			case 2:
			    /* C.LI */
			    /* full rd field */
			    r1 = (insn & 0x0f80) >> 7;
			    if (r1 == 0) {
				    /* hint (ignore) */
				    break;
			    }
			    /* immediate is the same as C.ADDI */
			    imm =
				    ((insn & 0x007c) >> 2) |
				    ((insn & 0x1000) >> 7);
			    if (insn & 0x1000) {
				    imm |= 0xffffffc0;
			    }
			    FN(rx_do_addi)(cpu, r1, 0/*zero*/, imm);
			    break;
			case 3:
			    /* full rd field */
			    r1 = (insn & 0x0f80) >> 7;
			    if (r1 == 0) {
				    /* hint; do nothing */
				    break;
			    }
			    if (r1 == 2) {
				    /* C.ADDI16SP */
				    /*
				     * immediate is:
				     *    bit 9    from bit 12
				     *    bit 7-8  from bit 3-4
				     *    bit 6    from bit 5
				     *    bit 5    from bit 2
				     *    bit 4    from bit 6
				     *    ...sign-extended from bit 12.
				     */
				    imm =
					    ((insn & 0x1000) >> 3) |
					    ((insn & 0x0018) << 4) |
					    ((insn & 0x0020) << 1) |
					    ((insn & 0x0004) << 3) |
					    ((insn & 0x0040) >> 2);
				    if (insn & 0x1000) {
					    imm |= 0xfffffe00;
				    }
				    if (imm == 0) {
					    /* reserved */
					    FN(rx_ill)(cpu, insn & 0xffff);
				    }
				    else {
					    /* 2 is sp */
					    FN(rx_do_addi)(cpu, 2, 2, imm);
				    }
			    }
			    else {
				    /* C.LUI */
				    /*
				     * immediate is the same as C.ADDI
				     * but shifted left by 12
				     */
				    imm =
					    ((insn & 0x007c) << 10) |
					    ((insn & 0x1000) << 5);
				    if (insn & 0x1000) {
					    imm |= 0xfffc0000;
				    }
				    if (imm == 0) {
					    /* reserved */
					    FN(rx_ill)(cpu, insn & 0xffff);
				    }
				    else {
					    FN(rx_do_lui)(cpu, r1, imm);
				    }
			    }
			    break;
			case 4:
			    r1 = 0x8 | ((insn & 0x0380) > 7); /*take bits 7-9*/
			    r2 = 0x8 | ((insn & 0x001c) > 2); /* bits 2-4 */
			    /* immediate like C.ADDI but zero-extended */
			    imm =
				    ((insn & 0x007c) >> 2) |
				    ((insn & 0x1000) >> 7);
			    switch ((insn & 0x0c00) >> 10) {
				case 0:
				    /* C.SRLI */
				    if (insn & 0x1000) {
					    /* reserved */
					    FN(rx_ill)(cpu, insn & 0xffff);
					    break;
				    }
				    /* imm == 0 is a hint but whatever */
				    FN(rx_do_srli)(cpu, r1, r1, imm);
				    break;
				case 1:
				    /* C.SRAI */
				    if (insn & 0x1000) {
					    /* reserved */
					    FN(rx_ill)(cpu, insn & 0xffff);
					    break;
				    }
				    /* imm == 0 is a hint but whatever */
				    FN(rx_do_srai)(cpu, r1, r1, imm);
				    break;
				case 2:
				    /* C.ANDI */
				    /* sign-extend after all */
				    if (insn & 0x1000) {
					    imm |= 0xffffffc0;
				    }
				    FN(rx_do_andi)(cpu, r1, r1, imm);
				    break;
				case 3:
				    if (insn & 0x1000) {
					    /* reserved */
					    FN(rx_ill)(cpu, insn & 0xffff);
					    break;
				    }
				    switch ((insn & 0x0060) >> 5) {
					case 0: /* C.SUB */
					    FN(rx_do_sub)(cpu, r1, r1, r2);
					    break;
					case 1: /* C.XOR */
					    FN(rx_do_xor)(cpu, r1, r1, r2);
					    break;
					case 2: /* C.OR */
					    FN(rx_do_or)(cpu, r1, r1, r2);
					    break;
					case 3: /* C.AND */
					    FN(rx_do_and)(cpu, r1, r1, r2);
					    break;
				    }
				    break;
			    }
			    break;
			case 5:
			    /* C.J */
			    /* immediate is the same as C.JAL */
			    imm =
				    ((insn & 0x1000) >> 1) |
				    ((insn & 0x0100) << 2) |
				    ((insn & 0x0600) >> 1) |
				    ((insn & 0x0040) << 1) |
				    ((insn & 0x0080) >> 1) |
				    ((insn & 0x0004) << 3) |
				    ((insn & 0x0800) >> 7) |
				    ((insn & 0x0038) >> 2);
			    if (insn & 0x1000) {
				    imm |= 0xfffff000;
			    }
			    FN(rx_do_jal)(cpu, 0/*zero*/, imm, cpu->pc + 2);
			    break;
			case 6:
			case 7:
			    /* C.BEQZ and C.BNEZ */
			    r1 = 0x8 | ((insn & 0x0380) > 7); /*take bits 7-9*/
			    /*
			     * immediate is:
			     *    bit 8    from bit 12
			     *    bit 6-7  from bit 5-6
			     *    bit 5    from bit 2
			     *    bit 3-4  from bit 10-11
			     *    bit 1-2  from bit 3-4
			     *    ...sign-extended from bit 12
			     */
			    imm =
				    ((insn & 0x1000) >> 4) |
				    ((insn & 0x0060) << 1) |
				    ((insn & 0x0004) << 3) |
				    ((insn & 0x0c00) >> 7) |
				    ((insn & 0x0018) >> 2);
			    if (insn & 0x1000) {
				    imm |= 0xfffffe00;
			    }
			    if (insn & 0x2000) {
				    FN(rx_do_beq)(cpu, r1, 0/*zero*/, imm);
			    }
			    else {
				    FN(rx_do_bne)(cpu, r1, 0/*zero*/, imm);
			    }
			    break;
		    }
	    }
	    break;
	    case 2: {
		    /* Quadrant 2 */
		    unsigned rd, rs2, imm;

		    /* full register fields */
		    rd = (insn & 0x0f80) >> 7;
		    rs2 = (insn & 0x007c) >> 2;

		    switch (op) {
			case 0:
			    /* C.SLLI */
			    if (insn & 0x1000) {
				    /* reserved */
				    FN(rx_ill)(cpu, insn & 0xffff);
				    break;
			    }
			    imm = rs2;
			    FN(rx_do_sll)(cpu, rd, rd, imm);
			    break;
			case 1:
			    /* C.FLDSP, not supported */
			    FN(rx_ill)(cpu, insn & 0xffff);
			    break;
			case 2:
			    /* C.LWSP */
			    if (rd == 0) {
				    /* reserved */
				    FN(rx_ill)(cpu, insn & 0xffff);
				    break;
			    }
			    /*
			     * immediate is:
			     *    bit 5    from bit 12
			     *    bit 2-4  from bit 4-6
			     *    bit 6-7  from bit 2-3
			     */
			    imm =
				    ((insn & 0x1000) >> 7) |
				    ((insn & 0x0070) >> 2) |
				    ((insn & 0x000c) << 4);
			    FN(rx_do_lw)(cpu, rd, 2/*sp*/, imm);
			    break;
			case 3:
			    /* C.FLWSP, not supported */
			    FN(rx_ill)(cpu, insn & 0xffff);
			    break;
			case 4:
			    if (insn & 0x1000) {
				    if (rd == 0 && rs2 == 0) { /* C.EBREAK */
					    if (FN(try_breakpoint)(cpu)) {
						    return 1;
					    }
					    FN(rx_do_ebreak)(cpu);
				    }
				    else if (rs2 == 0) { /* C.JALR */
					    FN(rx_do_jalr)(cpu, 1/*ra*/, rd,
							   0/*imm*/,
							   cpu->pc+2);
				    }
				    else { /* C.ADD */
					    FN(rx_do_add)(cpu, rd, rd, rs2);
				    }
			    }
			    else if (rs2 == 0 && rd == 0) {
				    /* reserved */
				    FN(rx_ill)(cpu, insn & 0xffff);
			    }
			    else if (rs2 == 0) { /* C.JR */
				    FN(rx_do_jalr)(cpu, 0/*zero*/, rd,
						   0/*imm*/, 0);
			    }
			    else { /* C.MV */
				    FN(rx_do_add)(cpu, rd, 0/*zero*/, rs2);
			    }
			    break;
			case 5:
			    /* C.FSDSP, not supported */
			    FN(rx_ill)(cpu, insn & 0xffff);
			    break;
			case 6:
			    /* C.SWSP */
			    /*
			     * immediate is:
			     *    bit 6-7 from bit 7-8
			     *    bit 2-5 from bit 9-12
			     */
			    imm =
				    ((insn & 0x0180) >> 1) |
				    ((insn & 0x1e00) >> 7);
			    FN(rx_do_sw)(cpu, rs2, 2/*sp*/, imm);
			    break;
			case 7:
			    /* C.FSWSP, not supportd */
			    FN(rx_ill)(cpu, insn & 0xffff);
			    break;
		    }
	    }
	    break;
	    case 3:
		/* case 3 doesn't come to this function */
		Assert(false);
		break;
	}
	return 0;
}

////////////////////////////////////////////////////////////
// toplevel cycle

static
int
FN(cpu_cycle)(void)
{
	uint32_t insn;
	unsigned whichcpu;
	unsigned breakpoints = 0;
	unsigned retire_usermode;

	for (whichcpu=0; whichcpu < ncpus; whichcpu++) {
		struct riscvcpu *cpu = &mycpus[whichcpu];

		if (cpu->state != CPU_RUNNING) {
			// don't check this on the critical path
			//Assert((cpu_running_mask & thiscpumask) == 0);
			g_stats.s_percpu[cpu->cpunum].sp_icycles++;
			continue;
		}

		/*
		 * Check for interrupts.
		 */
		if (cpu->status_sie) {
			int lb = cpu->irq_lamebus && cpu->ie_seie;
			int timer = cpu->irq_timer && cpu->ie_stie;
			int ipi = cpu->irq_ipi && cpu->ie_ssie;

			if (lb || timer || ipi) {
				int code;
				const char *codestr;

				CPUTRACE(DOTRACE_IRQ, cpu->cpunum,
					 "Taking interrupt:%s%s%s",
					 lb ? " LAMEbus" : "",
					 ipi ? " IPI" : "",
					 timer ? " timer" : "");
				if (lb) {
					code = IRQ_SEXTERN;
					codestr = ", LAMEbus";
				}
				else if (timer) {
					code = IRQ_STIMER;
					codestr = ", timer";
				}
				else {
					code = IRQ_SSOFT;
					codestr = ", IPI";
				}
				FN(interrupt)(cpu, code, codestr);

				/*
				 * Start processing the interrupt this cycle.
				 *
				 * (Anything done before this point in this
				 * function should be redone; but there isn't
				 * anything of substance.)
				 */
			}
		}

		if (IS_USERMODE(cpu)) {
			g_stats.s_percpu[cpu->cpunum].sp_ucycles++;
#ifdef USE_TRACE
			tracehow = DOTRACE_UINSN;
#endif
		}
		else {
			g_stats.s_percpu[cpu->cpunum].sp_kcycles++;
#ifdef USE_TRACE
			tracehow = DOTRACE_KINSN;
#endif
		}

		/*
		 * If at the end of all the following logic, we
		 * haven't taken an exception, we've retired an
		 * instruction. We need to record whether it was user
		 * or kernel here, because otherwise if we switch
		 * modes it'll be credited incorrectly.
		 */
		cpu->trapped = false;
		retire_usermode = IS_USERMODE(cpu);

		TRL("at %08x: ", cpu->pc);
		cpu->hit_breakpoint = 0;
		

		/*
		 * Fetch instruction.
		 *
		 * We cache the page translation for the PC. Use the
		 * page part of the precomputed physpc and also the
		 * precomputed page pointer.
		 *
		 * Note that as a result of precomputing everything,
		 * exceptions related to PC mishaps occur at jump
		 * time, never during instruction fetch itself. They
		 * will nonetheless report the destination as the
		 * exception PC. I believe this is acceptable behavior
		 * to exhibit, although it may not be in strict
		 * conformance with the specification.
		 *
		 * On riscv instructions can span page boundaries, so
		 * we need to cope. The code here is supposed to work
		 * as follows:
		 *    - since memory fetching is by words, fetch the
		 *      word that the instruction begins on.
		 *    - if the PC is on a halfword boundary, because
		 *      riscv is little-endian the part of what we
		 *      fetched that we want is the upper bytes, so
		 *      shift by 16.
		 *    - then decode the instruction length (even if
		 *      we are missing the rest of the instruction,
		 *      we can do that safely)
		 *    - for a 2-byte instruction we can just go
		 *    - we don't support any 8-byte or up instructions
		 *    - for a 4-byte instruction we might need to
		 *      fetch the other half
		 *    - if it's on the next page, advance the PC by
		 *      2 in order to run precompute_pc. If that
		 *      hits a trap, clean up by subtracting 2 from
		 *      sepc. (One does not at that point need to
		 *      subtract 2 from the pc itself because it
		 *      will be pointing to the trap handler.)
		 *    - If that succeeds, set the PC back to what it
		 *      was so that branches and calls and traps other
		 *      than from instruction fetch will record the
		 *      correct address. Note that this will cause
		 *      pcoff to be negative and using it to access
		 *      pcpage will be UB, but this will not happen,
		 *      because either a branch/call or trap will
		 *      pick a different next instruction, call
		 *      precompute_pc, and change pcoff, or we'll
		 *      set pcoff to nextpcoff, which is 2.
		 *    - the portion of the next word we want is the
		 *      lower bytes, shifted into the upper bytes to
		 *      paste in.
		 */
		insn = bus_use_map(cpu->pcpage, cpu->pcoff & 0xfffffffc);
		if (cpu->pcoff & 0x2) {
			insn = insn >> 16;
		}
		if ((insn & 0x3) != 0x3) {
			cpu->nextpc = cpu->pc + 2;
			cpu->nextpcoff = cpu->pcoff + 2;
			TRL("insn 0x%04x: ", (unsigned)insn);
			breakpoints += FN(insn16)(cpu, insn);
		}
		else if ((insn & 0x1c) == 0x1c) {
			FN(rx_ill)(cpu, insn);
		}
		else if (cpu->pcoff & 0x2) {
			/* unaligned fetch of 32-bit insn */
			unsigned pcoff2, insn2;

			pcoff2 = cpu->pcoff + 2;
			if (pcoff2 == 0x1000) {
				/* crossed page boundary */
				cpu->pc += 2;
				if (FN(precompute_pc)(cpu)) {
					/* exception */
					/* correct sepc */
					cpu->sepc -= 2;
					/* on to next cpu */
					continue;
				}
				insn2 = bus_use_map(cpu->pcpage, cpu->pcoff);
				/* account for partial PC change above */
				cpu->pc -= 2;
				cpu->pcoff -= 2;
			}
			else {
				insn2 = bus_use_map(cpu->pcpage, pcoff2);
			}
			insn |= (insn2 & 0x0000ffff) << 16;
			cpu->nextpc = cpu->pc + 4;
			cpu->nextpcoff = cpu->pcoff + 4;
			TRL("insn 0x%04lx: ", (unsigned long)insn);
			breakpoints += FN(insn32)(cpu, insn);
		}
		else {
			cpu->nextpc = cpu->pc + 4;
			cpu->nextpcoff = cpu->pcoff + 4;
			TRL("insn 0x%04lx: ", (unsigned long)insn);
			breakpoints += FN(insn32)(cpu, insn);
		}

		// Update PC.
		cpu->pc = cpu->nextpc;
		if ((cpu->pc & 0xfff)==0) {
			/* crossed page boundary */
			if (FN(precompute_pc)(cpu)) {
				/* exception. on to next cpu. */
				continue;
			}
		}
		else {
			cpu->pcoff = cpu->nextpcoff;
		}

		/* Timer. Take interrupt on next cycle; call it a pipeline effect. */
		cpu->cyclecount++;
		if (cpu->cyclecount == cpu->cycletrigger) {
			cpu->irq_timer = 1;
			CPUTRACE(DOTRACE_IRQ, cpu->cpunum, "Timer irq ON");
		}

		/*
		 * If the PC (which is the instruction we're going to execute
		 * on the next cycle) is still what it was saved as above,
		 * meaning we aren't jumping to an exception vector, we've
		 * retired an instruction. If in user mode, we've made
		 * progress.
		 *
		 * Note that it's important to claim progress only when we've
		 * retired an instruction; just spending a cycle in user mode
		 * doesn't count as it's possible to set up livelock
		 * conditions where user-mode instructions are started
		 * regularly but never complete.
		 */
		if (!cpu->trapped) {
			if (retire_usermode) {
				g_stats.s_percpu[cpu->cpunum].sp_uretired++;
				progress = 1;
			}
			else {
				g_stats.s_percpu[cpu->cpunum].sp_kretired++;
			}
		}

	}

	if (breakpoints == 0) {
		return 1;
	}
	if (breakpoints == ncpus) {
		return 1;
	}

	/*
	 * Some CPUs took a cycle, but one or more hit a builtin
	 * breakpoint and didn't.
	 *
	 * Ideally we should roll back the ones that did, or account
	 * for the time properly in some other fashion. Or stop using
	 * a global clock.
	 *
	 * For the time being, slip the clock. This makes builtin
	 * breakpoints not quite noninvasive on multiprocessor
	 * configs. Sigh.
	 */
	return 0;
}

/*
 * Note that this is the same for each build but it's here so we
 * aren't going through the cpu type dispatch on every cycle.
 */
static
uint64_t
FN(cpu_cycles)(uint64_t maxcycles)
{
	uint64_t i;

	cpu_cycling = 1;
	i = 0;
	while (i < maxcycles && cpu_cycling) {
		if (FN(cpu_cycle)()) {
			i++;
			cpu_cycles_count = i;
		}
		if (cpu_running_mask == 0) {
			/* nothing occurs until we reach maxcycles */
			if (cpu_cycling) {
				g_stats.s_tot_icycles += maxcycles - i;
				i = maxcycles;
			}
		}
	}
	cpu_cycles_count = 0;
	return i;
}

