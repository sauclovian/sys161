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

/*
 * These are further down.
 */
static int FN(precompute_pc)(struct mipscpu *cpu);
static int FN(precompute_nextpc)(struct mipscpu *cpu);

static
inline
int
FN(findtlb)(const struct mipscpu *cpu, uint32_t vpage)
{
#ifdef USE_TLBMAP
	uint8_t tm;

	tm = cpu->tlbmap[vpage >> 12];
	if (tm == TM_NOPAGE) {
		return -1;
	}
	return tm;
#else
	int i;
	for (i=0; i<NTLB; i++) {
		const struct mipstlb *mt = &cpu->tlb[i];
		if (mt->mt_vpn!=vpage) continue;
		if (mt->mt_pid==cpu->tlbentry.mt_pid || mt->mt_global) {
			return i;
		}
	}

	return -1;
#endif
}

static
void
FN(probetlb)(struct mipscpu *cpu)
{
	uint32_t vpage;
	int ix;

	vpage = cpu->tlbentry.mt_vpn;
	ix = FN(findtlb)(cpu, vpage);

	CPUTRACEL(DOTRACE_TLB, cpu->cpunum, "tlbp:       ");
	TLBTRV(&cpu->tlbentry);

	if (ix<0) {
		CPUTRACE(DOTRACE_TLB, cpu->cpunum, "NOT FOUND");
		cpu->tlbpf = 1;
	}
	else {
		TLBTRP(&cpu->tlb[ix]);
		CPUTRACE(DOTRACE_TLB, cpu->cpunum, ": [%d]", ix);
		cpu->tlbindex = ix;
		cpu->tlbpf = 0;
	}
}

static
void
FN(writetlb)(struct mipscpu *cpu, int ix, const char *how)
{
	(void)how;
	CPUTRACEL(DOTRACE_TLB, cpu->cpunum, "%s: [%2d] ", how, ix);
	TLBTR(&cpu->tlb[ix]);
	CPUTRACEL(DOTRACE_TLB, cpu->cpunum, " ==> ");
	TLBTR(&cpu->tlbentry);
	CPUTRACE(DOTRACE_TLB, cpu->cpunum, " ");

#ifdef USE_TLBMAP
	cpu->tlbmap[cpu->tlb[ix].mt_vpn >> 12] = TM_NOPAGE;
#endif

	cpu->tlb[ix] = cpu->tlbentry;

#ifdef USE_TLBMAP
	cpu->tlbmap[cpu->tlb[ix].mt_vpn >> 12] = ix;
#endif

	check_tlb_dups(cpu, ix);

	/* 
	 * If the OS coder is a lunatic, the mapping for the pc might
	 * have changed. If this causes an exception, we don't need
	 * to do anything special, though.
	 */
	(void) FN(precompute_pc)(cpu);
	(void) FN(precompute_nextpc)(cpu);
}

static
void
FN(do_wait)(struct mipscpu *cpu)
{
	/* Only wait if no interrupts are already pending */
	if (!cpu->irq_lamebus && !cpu->irq_ipi && !cpu->irq_timer) {
		cpu->state = CPU_IDLE;
		RUNNING_MASK_OFF(cpu->cpunum);
	}
}

static
void
FN(do_rfe)(struct mipscpu *cpu)
{
	//uint32_t bits;

	if (IS_USERMODE(cpu)) {
		smoke("RFE in usermode not caught by instruction decoder");
	}

	cpu->current_usermode = cpu->prev_usermode;
	cpu->current_irqon = cpu->prev_irqon;
	cpu->prev_usermode = cpu->old_usermode;
	cpu->prev_irqon = cpu->old_irqon;
	CPUTRACE(DOTRACE_EXN, cpu->cpunum,
		 "Return from exception: %s mode, interrupts %s, sp %x",
		 (cpu->current_usermode) ? "user" : "kernel",
		 (cpu->current_irqon) ? "on" : "off",
		 cpu->r[29]);

	/*
	 * Re-lookup the translations for the pc, because we might have
	 * changed to usermode.
	 *
	 * Furthermore, hack the processor state so if there's an
	 * exception it happens with things pointing to the
	 * instruction happening after the rfe, not the rfe itself.
	 */
	
	cpu->in_jumpdelay = 0;
	cpu->expc = cpu->pc;

	FN(precompute_pc)(cpu);
	FN(precompute_nextpc)(cpu);
}

/*
 * This corrects the state of the processor if we're handling a
 * breakpoint with the remote gdb code. The remote gdb code should use
 * cpu->expc as the place execution stopped; in that case something
 * vaguely reasonable will happen if some nimrod puts a breakpoint in
 * a branch delay slot.
 *
 * This does not invalidate ll_active.
 */
static
void
FN(phony_exception)(struct mipscpu *cpu)
{
	cpu->jumping = 0;
	cpu->in_jumpdelay = 0;
	cpu->pc = cpu->expc;
	cpu->nextpc = cpu->pc + 4;

	/*
	 * These shouldn't fail because we were just executing with 
	 * the same values.
	 */
	if (FN(precompute_pc)(cpu)) {
		smoke("precompute_pc failed in phony_exception");
	}
	if (FN(precompute_nextpc)(cpu)) {
		smoke("precompute_nextpc failed in phony_exception");
	}
}

static
void
FN(exception)(struct mipscpu *cpu, int code, int cn_or_user, uint32_t vaddr,
	  const char *exception_name_supplement)
{
	//uint32_t bits;
	int boot = (cpu->status_bootvectors) != 0;

	CPUTRACE(DOTRACE_EXN, cpu->cpunum,
		 "exception: code %d (%s%s), expc %x, vaddr %x, sp %x", 
		 code, exception_name(code), exception_name_supplement,
		 cpu->expc, vaddr, cpu->r[29]);
#ifndef USE_TRACE
	(void)exception_name_supplement;
#endif

	if (code==EX_IRQ) {
		g_stats.s_irqs++;
	}
	else {
		g_stats.s_exns++;
	}

	cpu->cause_bd = cpu->in_jumpdelay;
	if (code==EX_CPU) {
		cpu->cause_ce = ((uint32_t)cn_or_user << 28);
	}
	else {
		cpu->cause_ce = 0;
	}
	cpu->cause_code = code << 2;

	cpu->jumping = 0;
	cpu->in_jumpdelay = 0;
	
	cpu->ll_active = 0;

	// roll the status bits
	cpu->old_usermode = cpu->prev_usermode;
	cpu->old_irqon = cpu->prev_irqon;
	cpu->prev_usermode = cpu->current_usermode;
	cpu->prev_irqon = cpu->current_irqon;
	cpu->current_usermode = 0;
	cpu->current_irqon = 0;

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

	/*
	 * These cannot fail. Furthermore, if for some reason they do,
	 * they'll likely recurse forever and not return, so checking
	 * the return value is fairly pointless.
	 */
	(void) FN(precompute_pc)(cpu);
	(void) FN(precompute_nextpc)(cpu);
}

/*
 * Warning: do not change this function without making corresponding
 * changes to debug_translatemem (below).
 */
static
inline
int
FN(translatemem)(struct mipscpu *cpu, uint32_t vaddr, int iswrite, uint32_t *ret)
{
	uint32_t seg;
	uint32_t paddr;

	// MIPS hardwired memory layout:
	//    0xc0000000 - 0xffffffff   kseg2 (kernel, tlb-mapped)
	//    0xa0000000 - 0xbfffffff   kseg1 (kernel, unmapped, uncached)
	//    0x80000000 - 0x9fffffff   kseg0 (kernel, unmapped, cached)
	//    0x00000000 - 0x7fffffff   kuseg (user, tlb-mapped)
	//
	// Since we don't implement cache, we can consider kseg0 and kseg1
	// equivalent (except remember that the base of each maps to paddr 0.)

	/*
	 * On intel at least it's noticeably faster this way:
	 * compute seg first, but don't use it when checking for address
	 * error, only for whether we're in a direct-mapped segment.
	 *
	 * My guess is that gcc's instruction scheduler isn't good enough to
	 * handle this on its own and we get pipeline stalls with a more
	 * sensible organization.
	 *
	 * XXX retest this with modern gcc
	 */
	seg = vaddr >> 30;

	if ((vaddr >= 0x80000000 && IS_USERMODE(cpu)) || (vaddr & 0x3)!=0) {
		FN(exception)(cpu, iswrite ? EX_ADES : EX_ADEL, 0, vaddr, "");
		return -1;
	}

	if (seg==2) {
		paddr = vaddr & 0x1fffffff;
	}
	else {
		uint32_t vpage;
		uint32_t off;
		uint32_t ppage;
		int ix;

		vpage = vaddr & 0xfffff000;
		off   = vaddr & 0x00000fff;

		CPUTRACEL(DOTRACE_TLB, cpu->cpunum,
			  "tlblookup:  %05x/%03x -> ", 
			  vpage >> 12, cpu->tlbentry.mt_pid);

		cpu->tlbentry.mt_vpn = vpage;
		ix = FN(findtlb)(cpu, vpage);

		if (ix<0) {
			int exc = iswrite ? EX_TLBS : EX_TLBL;
			int isuseraddr = vaddr < 0x80000000;
			CPUTRACE(DOTRACE_TLB, cpu->cpunum, "no match");
			FN(exception)(cpu, exc, isuseraddr, vaddr, ", miss");
			return -1;
		}
		TLBTRP(&cpu->tlb[ix]);
		CPUTRACEL(DOTRACE_TLB, cpu->cpunum, ": [%d]", ix);

		if (!cpu->tlb[ix].mt_valid) {
			int exc = iswrite ? EX_TLBS : EX_TLBL;
			CPUTRACE(DOTRACE_TLB, cpu->cpunum, " - INVALID");
			FN(exception)(cpu, exc, 0, vaddr, ", invalid");
			return -1;
		}
		if (iswrite && !cpu->tlb[ix].mt_dirty) {
			CPUTRACE(DOTRACE_TLB, cpu->cpunum, " - READONLY");
			FN(exception)(cpu, EX_MOD, 0, vaddr, "");
			return -1;
		}
		CPUTRACE(DOTRACE_TLB, cpu->cpunum, " - OK");
		ppage = cpu->tlb[ix].mt_pfn;
		paddr = ppage|off;
	}

	*ret = paddr;
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
FN(debug_translatemem)(const struct mipscpu *cpu, uint32_t vaddr, 
		   int iswrite, uint32_t *ret)
{
	uint32_t paddr;

	if ((vaddr & 0x3)!=0) {
		return -1;
	}

	if ((vaddr >> 30)==2) {
		paddr = vaddr & 0x1fffffff;
	}
	else {
		uint32_t vpage;
		uint32_t off;
		uint32_t ppage;
		int ix;

		vpage = vaddr & 0xfffff000;
		off   = vaddr & 0x00000fff;

		CPUTRACEL(DOTRACE_TLB, cpu->cpunum,
			  "tlblookup (debugger):  %05x/%03x -> ", 
			  vpage >> 12, cpu->tlbentry.mt_pid);

		ix = FN(findtlb)(cpu, vpage);

		if (ix<0) {
			CPUTRACE(DOTRACE_TLB, cpu->cpunum, "MISS");
			return -1;
		}
		TLBTRP(&cpu->tlb[ix]);
		CPUTRACEL(DOTRACE_TLB, cpu->cpunum, ": [%d]", ix);

		if (!cpu->tlb[ix].mt_valid) {
			CPUTRACE(DOTRACE_TLB, cpu->cpunum, " - INVALID");
			return -1;
		}
		if (iswrite && !cpu->tlb[ix].mt_dirty) {
			CPUTRACE(DOTRACE_TLB, cpu->cpunum, " - READONLY");
			return -1;
		}
		CPUTRACE(DOTRACE_TLB, cpu->cpunum, " - OK");
		ppage = cpu->tlb[ix].mt_pfn;
		paddr = ppage|off;
	}

	*ret = paddr;
	return 0;

}

static
inline
int
FN(accessmem)(struct mipscpu *cpu, uint32_t paddr, int iswrite, uint32_t *val)
{
	int buserr;

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
			buserr = bootrom_fetch(paddr - 0x1fc00000, val);
		}
	}
	else if (paddr < 0x20000000) {
		if (iswrite) {
			buserr = bus_io_store(cpu->cpunum,
					      paddr-0x1fe00000, *val);
		}
		else {
			buserr = bus_io_fetch(cpu->cpunum,
					      paddr-0x1fe00000, val);
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
		FN(exception)(cpu, EX_DBE, 0, 0, "");
		return -1;
	}
	
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
	 *    0x00000000 - 0x1fbfffff     RAM
	 *    0x1fc00000 - 0x1fdfffff     Boot ROM
	 *    0x1fe00000 - 0x1fffffff     LAMEbus mapped I/O
	 *    0x20000000 - 0xffffffff     RAM
	 */
	paddr &= 0xfffff000;

	if (paddr < 0x1fc00000) {
		return bus_mem_map(paddr);
	}

	if (paddr < 0x1fe00000) {
		return bootrom_map(paddr - 0x1fc00000);
	}

	if (paddr < 0x20000000) {
		/* don't allow executing from I/O registers */
		return NULL;
	}

	return bus_mem_map(paddr-0x00400000);
}

/*
 * iswrite should be true if *this* domem operation is a write.
 *
 * willbewrite should be true if any domem operation on this
 * cycle is (or will be) a write, even if *this* one isn't.
 * (This is for preventing silly exception behavior when writing
 * a sub-word quantity.)
 */
static
int
FN(domem)(struct mipscpu *cpu, uint32_t vaddr, uint32_t *val, 
      int iswrite, int willbewrite)
{
	uint32_t paddr;
	
	if (FN(translatemem)(cpu, vaddr, willbewrite, &paddr)) {
		return -1;
	}

	return FN(accessmem)(cpu, paddr, iswrite, val);
}

static
int
FN(precompute_pc)(struct mipscpu *cpu)
{
	uint32_t physpc;
	if (FN(translatemem)(cpu, cpu->pc, 0, &physpc)) {
		return -1;
	}
	cpu->pcpage = FN(mapmem)(physpc);
	if (cpu->pcpage == NULL) {
		FN(exception)(cpu, EX_IBE, 0, 0, "");
		if (cpu->pcpage == NULL) {
			smoke("Bus error invoking exception handler");
		}
		return -1;
	}
	cpu->pcoff = physpc & 0xfff;
	return 0;
}

static
int
FN(precompute_nextpc)(struct mipscpu *cpu)
{
	uint32_t physnext;
	if (FN(translatemem)(cpu, cpu->nextpc, 0, &physnext)) {
		return -1;
	}
	cpu->nextpcpage = FN(mapmem)(physnext);
	if (cpu->nextpcpage == NULL) {
		FN(exception)(cpu, EX_IBE, 0, 0, "");
		if (cpu->nextpcpage == NULL) {
			smoke("Bus error invoking exception handler");
		}
		return -1;
	}
	cpu->nextpcoff = physnext & 0xfff;
	return 0;
}


static
void
FN(doload)(struct mipscpu *cpu, memstyles ms, uint32_t addr, uint32_t *res)
{
	switch (ms) {
	    case S_SBYTE:
	    case S_UBYTE:
	    {
		uint32_t val;
		uint8_t bval = 0;
		if (FN(domem)(cpu, addr & 0xfffffffc, &val, 0, 0)) {
			return;
		}
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
		uint32_t val;
		uint16_t hval = 0;
		if (FN(domem)(cpu, addr & 0xfffffffd, &val, 0, 0)) {
			return;
		}
		switch (addr & 2) {
			case 0: hval = (val & 0xffff0000)>>16; break;
			case 2: hval = val & 0x0000ffff; break;
		}
		if (ms==S_SHALF) *res = (int32_t)(int16_t)hval;
		else *res = hval;
	    }
	    break;
     
	    case S_WORDL:
	    {
		uint32_t val;
		uint32_t mask = 0;
		int shift = 0;
		if (FN(domem)(cpu, addr & 0xfffffffc, &val, 0, 0)) {
			return;
		}
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
		uint32_t val;
		uint32_t mask = 0;
		int shift = 0;
		if (FN(domem)(cpu, addr & 0xfffffffc, &val, 0, 0)) {
			return;
		}
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
FN(dostore)(struct mipscpu *cpu, memstyles ms, uint32_t addr, uint32_t val)
{
	switch (ms) {
	    case S_UBYTE:
	    {
		uint32_t wval;
		uint32_t mask = 0;
		int shift = 0;
		switch (addr & 3) {
		    case 0: mask = 0xff000000; shift=24; break;
		    case 1: mask = 0x00ff0000; shift=16; break;
		    case 2: mask = 0x0000ff00; shift=8; break;
		    case 3: mask = 0x000000ff; shift=0; break;
		}
		if (FN(domem)(cpu, addr & 0xfffffffc, &wval, 0, 1)) {
			return;
		}
		wval = (wval & ~mask) | ((val&0xff) << shift);
		if (FN(domem)(cpu, addr & 0xfffffffc, &wval, 1, 1)) {
			return;
		}
	    }
	    break;

	    case S_UHALF:
	    {
		uint32_t wval;
		uint32_t mask = 0;
		int shift = 0;
		switch (addr & 2) {
			case 0: mask = 0xffff0000; shift=16; break;
			case 2: mask = 0x0000ffff; shift=0; break;
		}
		if (FN(domem)(cpu, addr & 0xfffffffd, &wval, 0, 1)) {
			return;
		}
		wval = (wval & ~mask) | ((val&0xffff) << shift);
		if (FN(domem)(cpu, addr & 0xfffffffd, &wval, 1, 1)) {
			return;
		}
	    }
	    break;
	
	    case S_WORDL:
	    {
		uint32_t wval;
		uint32_t mask = 0;
		int shift = 0;
		if (FN(domem)(cpu, addr & 0xfffffffc, &wval, 0, 1)) {
			return;
		}
		switch (addr & 0x3) {
			case 0: mask = 0xffffffff; shift=0; break;
			case 1: mask = 0x00ffffff; shift=8; break;
			case 2: mask = 0x0000ffff; shift=16; break;
			case 3: mask = 0x000000ff; shift=24; break;
		}
		val >>= shift;
		wval = (wval & ~mask) | (val & mask);

		if (FN(domem)(cpu, addr & 0xfffffffc, &wval, 1, 1)) {
			return;
		}
	    }
	    break;
	    case S_WORDR:
	    {
		uint32_t wval;
		uint32_t mask = 0;
		int shift = 0;
		if (FN(domem)(cpu, addr & 0xfffffffc, &wval, 0, 1)) {
			return;
		}
		switch (addr & 0x3) {
			case 0: mask = 0xff000000; shift=24; break;
			case 1: mask = 0xffff0000; shift=16; break;
			case 2: mask = 0xffffff00; shift=8; break;
			case 3: mask = 0xffffffff; shift=0; break;
		}
		val <<= shift;
		wval = (wval & ~mask) | (val & mask);

		if (FN(domem)(cpu, addr & 0xfffffffc, &wval, 1, 1)) {
			return;
		}
	    }
	    break;

	    default:
		smoke("dostore: Illegal addressing mode");
	}
}

static 
void
FN(abranch)(struct mipscpu *cpu, uint32_t addr)
{
	CPUTRACE(DOTRACE_JUMP, cpu->cpunum, 
		 "jump: %x -> %x", cpu->nextpc-8, addr);

	if ((addr & 0x3) != 0) {
		FN(exception)(cpu, EX_ADEL, 0, addr, ", branch");
		return;
	}

	// Branches update nextpc (which points to the insn after 
	// the delay slot).

	cpu->nextpc = addr;
	cpu->jumping = 1;

	/*
	 * If the instruction in the delay slot is RFE, don't call
	 * precompute_nextpc. Instead, clear the precomputed nextpc
	 * stuff and call precompute_pc once the RFE has done its
	 * thing. This is important to make sure the new PC is fetched
	 * in user mode if the RFE is switching thereto.
	 */
	if (bus_use_map(cpu->pcpage, cpu->pcoff) == FULLOP_RFE) {
		cpu->nextpcpage = NULL;
		cpu->nextpcoff = 0;
	}
	else {
		/* if this fails, no special action is required */
		FN(precompute_nextpc)(cpu);
	}
}

static
void
FN(ibranch)(struct mipscpu *cpu, uint32_t imm)
{
	// The mips book is helpfully not specific about whether the
	// address to take the upper bits of is the address of the
	// jump or the delay slot or what. it just says "the current
	// program counter", which I shall interpret as the address of
	// the delay slot. Fortunately, one isn't likely to ever be
	// executing a jump that lies across a boundary where it would
	// matter.
	//
	// (Note that cpu->pc aims at the delay slot by the time we
	// get here.)
   
	uint32_t addr = (cpu->pc & 0xf0000000) | imm;
	FN(abranch)(cpu, addr);
}

static
void
FN(rbranch)(struct mipscpu *cpu, int32_t rel)
{
	uint32_t addr = cpu->pc + rel;  // relative to addr of delay slot
	FN(abranch)(cpu, addr);
}

static
uint32_t
FN(getstatus)(struct mipscpu *cpu)
{
	uint32_t val;

	val = cpu->status_copenable;
	/* STATUS_LOWPOWER always reads 0 */
	/* STATUS_XFPU64 always reads 0 */
	/* STATUS_REVENDIAN always reads 0 */
	/* STATUS_MDMX64 always reads 0 */
	/* STATUS_MODE64 always reads 0 */
	val |= cpu->status_bootvectors;
	/* STATUS_ERRORCAUSES always reads 0 */
	/* STATUS_CACHEPARITY always reads 0 */
	/* STATUS_R3KCACHE always reads 0 */
	val |= cpu->status_hardmask_timer;
	val |= cpu->status_hardmask_void;
	val |= cpu->status_hardmask_fpu;
	val |= cpu->status_hardmask_ipi;
	val |= cpu->status_hardmask_lb;
	val |= cpu->status_softmask;

	if (cpu->old_usermode) val |= STATUS_KUo;
	if (cpu->old_irqon) val |= STATUS_IEo;
	if (cpu->prev_usermode) val |= STATUS_KUp;
	if (cpu->prev_irqon) val |= STATUS_IEp;
	if (cpu->current_usermode) val |= STATUS_KUc;
	if (cpu->current_irqon) val |= STATUS_IEc;

	return val;
}

static
void
FN(setstatus)(struct mipscpu *cpu, uint32_t val)
{
	cpu->status_copenable = val & STATUS_COPENABLE;
	/* STATUS_LOWPOWER is ignored (for now) */
	/* STATUS_XFPU64 is ignored because we aren't 64-bit */
	/* STATUS_REVENDIAN is ignored (for now) */
	/* STATUS_MDMX64 is ignored because we aren't 64-bit */
	/* STATUS_MODE64 is ignored because we aren't 64-bit */
	cpu->status_bootvectors = val & STATUS_BOOTVECTORS;
	if (val & STATUS_ERRORCAUSES) {
		/*
		 * Writing to these bits can turn them off but not on.
		 * And because for the moment we don't implement the
		 * conditions that would turn them on (instead we drop
		 * to the debugger for such things) we can just ignore
		 * the write.
		 */
	}
	/* STATUS_CACHEPARITY is ignored */
	if (val & STATUS_R3KCACHE) {
		hang("Status register write attempted to use "
		     "r2000/r3000 cache control");
	}
	cpu->status_hardmask_timer = val & STATUS_HARDMASK_TIMER;
	cpu->status_hardmask_void = val & 
		(STATUS_HARDMASK_UNUSED2 | STATUS_HARDMASK_UNUSED4);
	cpu->status_hardmask_fpu = val & STATUS_HARDMASK_FPU;
	cpu->status_hardmask_ipi = val & STATUS_HARDMASK_IPI;
	cpu->status_hardmask_lb = val & STATUS_HARDMASK_LB;
	cpu->status_softmask = val & STATUS_SOFTMASK;

	cpu->old_usermode = val & STATUS_KUo;
	cpu->old_irqon = val & STATUS_IEo;
	cpu->prev_usermode = val & STATUS_KUp;
	cpu->prev_irqon = val & STATUS_IEp;
	cpu->current_usermode = val & STATUS_KUc;
	cpu->current_irqon = val & STATUS_IEc;
}


static
uint32_t
FN(getcause)(struct mipscpu *cpu)
{
	uint32_t val;
	val = cpu->cause_ce | cpu->cause_softirq | cpu->cause_code;

	if (cpu->cause_bd) {
		val |= CAUSE_BD;
	}

	if (cpu->irq_lamebus) {
		val |= CAUSE_HARDIRQ_LB;
	}
	if (cpu->irq_ipi) {
		val |= CAUSE_HARDIRQ_IPI;
	}
	if (cpu->irq_timer) {
		val |= CAUSE_HARDIRQ_TIMER;
	}

	return val;
}

static
void
FN(setcause)(struct mipscpu *cpu, uint32_t val)
{
	/* c0_cause is read-only except for the soft irq bits */
	cpu->cause_softirq = val & CAUSE_SOFTIRQ;
}

static
uint32_t
FN(getindex)(struct mipscpu *cpu)
{
	uint32_t val = cpu->tlbindex << 8;
	if (cpu->tlbpf) {
		val |= 0x80000000;
	}
	return val;
}

static
void
FN(setindex)(struct mipscpu *cpu, uint32_t val)
{
	cpu->tlbindex = (val >> 8) & 63;
	cpu->tlbpf = val & 0x80000000;
}

static
uint32_t
FN(getrandom)(struct mipscpu *cpu)
{
	cpu->tlbrandom %= RANDREG_MAX;
	return (cpu->tlbrandom+RANDREG_OFFSET) << 8;
}

/*************************************************************/

#define LINK2(rg)  (cpu->r[rg] = cpu->nextpc)
#define LINK LINK2(31)

/* registers as lvalues */
#define RTx  (cpu->r[rt])
#define RSx  (cpu->r[rs])
#define RDx  (cpu->r[rd])

/* registers as signed 32-bit rvalues */
#define RTs  ((int32_t)RTx)
#define RSs  ((int32_t)RSx)
#define RDs  ((int32_t)RDx)

/* registers as unsigned 32-bit rvalues */
#define RTu  ((uint32_t)RTx)
#define RSu  ((uint32_t)RSx)
#define RDu  ((uint32_t)RDx)

/* registers as printf-able signed values */
#define RTsp  ((long)RTs)
#define RSsp  ((long)RSs)
#define RDsp  ((long)RDs)

/* registers as printf-able unsigned values */
#define RTup  ((unsigned long)RTu)
#define RSup  ((unsigned long)RSu)
#define RDup  ((unsigned long)RDu)

/* XXX why does this call phony_exception...? */
#define STALL { FN(phony_exception)(cpu); }
#define WHILO {if (cpu->hiwait>0 || cpu->lowait>0) { STALL; return; }}
#define WHI   {if (cpu->hiwait>0) { STALL; return; }}
#define WLO   {if (cpu->lowait>0) { STALL; return; }}
#define SETHILO(n) (cpu->hiwait = cpu->lowait = (n))
#define SETHI(n)   (cpu->hiwait = (n))
#define SETLO(n)   (cpu->lowait = (n))

#define OVF	  { FN(exception)(cpu, EX_OVF, 0, 0, ""); }
#define CHKOVF(v) {if (((int64_t)(int32_t)(v))!=(v)) { OVF; return; }}

#define TRL(...)  CPUTRACEL(tracehow, cpu->cpunum, __VA_ARGS__)
#define TR(...)   CPUTRACE(tracehow, cpu->cpunum, __VA_ARGS__)

#define NEEDRS	 uint32_t rs = (insn & 0x03e00000) >> 21	// register
#define NEEDRT	 uint32_t rt = (insn & 0x001f0000) >> 16	// register
#define NEEDRD	 uint32_t rd = (insn & 0x0000f800) >> 11	// register
#define NEEDTARG uint32_t targ=(insn & 0x03ffffff)           // target of jump
#define NEEDSH	 uint32_t sh = (insn & 0x000007c0) >> 6	// shift count
#define NEEDCN	 uint32_t cn = (insn & 0x0c000000) >> 26	// coproc. no.
#define NEEDSEL	 uint32_t sel= (insn & 0x00000007)	     // register select
#define NEEDIMM	 uint32_t imm= (insn & 0x0000ffff)	     // immediate value
#define NEEDSMM	 NEEDIMM; int32_t smm = (int32_t)(int16_t)imm 
					       // sign-extended immediate value
#define NEEDADDR NEEDRS; NEEDSMM; uint32_t addr = RSu + (uint32_t)smm
                                                     // register+offset address


static
void
FN(domf)(struct mipscpu *cpu, int cn, unsigned reg, unsigned sel, int32_t *greg)
{
	unsigned regsel;
	
	if (cn!=0 || IS_USERMODE(cpu)) {
		FN(exception)(cpu, EX_CPU, cn, 0, ", mfc instruction");
		return;
	}

	regsel = REGSEL(reg, sel);
	switch (regsel) {
	    case C0_INDEX:   *greg = FN(getindex)(cpu); break;
	    case C0_RANDOM:  *greg = FN(getrandom)(cpu); break;
	    case C0_TLBLO:   *greg = tlbgetlo(&cpu->tlbentry); break;
	    case C0_CONTEXT: *greg = cpu->ex_context; break;
	    case C0_VADDR:   *greg = cpu->ex_vaddr; break;
	    case C0_COUNT:   *greg = cpu->ex_count; break;
	    case C0_TLBHI:   *greg = tlbgethi(&cpu->tlbentry); break;
	    case C0_COMPARE: *greg = cpu->ex_compare; break;
	    case C0_STATUS:  *greg = FN(getstatus)(cpu); break;
	    case C0_CAUSE:   *greg = FN(getcause)(cpu); break;
	    case C0_EPC:     *greg = cpu->ex_epc; break;
	    case C0_PRID:    *greg = cpu->ex_prid; break;
	    case C0_CFEAT:   *greg = cpu->ex_cfeat; break;
	    case C0_IFEAT:   *greg = cpu->ex_ifeat; break;
	    case C0_CONFIG0: *greg = cpu->ex_config0; break;
	    case C0_CONFIG1: *greg = cpu->ex_config1; break;
#if 0 /* not yet */
	    case C0_CONFIG2: *greg = cpu->ex_config2; break;
	    case C0_CONFIG3: *greg = cpu->ex_config3; break;
	    case C0_CONFIG4: *greg = cpu->ex_config4; break;
	    case C0_CONFIG5: *greg = cpu->ex_config5; break;
	    case C0_CONFIG6: *greg = cpu->ex_config6; break;
	    case C0_CONFIG7: *greg = cpu->ex_config7; break;
#endif
	    default:
		FN(exception)(cpu, EX_RI, cn, 0, ", invalid cop0 register");
		break;
	}
}

static
void
FN(domt)(struct mipscpu *cpu, int cn, int reg, int sel, int32_t greg)
{
	unsigned regsel;

	if (cn!=0 || IS_USERMODE(cpu)) {
		FN(exception)(cpu, EX_CPU, cn, 0, ", mtc instruction");
		return;
	}

	regsel = REGSEL(reg, sel);
	switch (regsel) {
	    case C0_INDEX:   FN(setindex)(cpu, greg); break;
	    case C0_RANDOM:  /* read-only register */ break;
	    case C0_TLBLO:   tlbsetlo(&cpu->tlbentry, greg); break;
	    case C0_CONTEXT: cpu->ex_context = greg; break;
	    case C0_VADDR:   cpu->ex_vaddr = greg; break;
	    case C0_COUNT:   cpu->ex_count = greg; break;
	    case C0_TLBHI:   tlbsethi(&cpu->tlbentry, greg); break;
	    case C0_COMPARE:
		cpu->ex_compare = greg;
		cpu->ex_compare_used = 1;
		if (cpu->ex_count > cpu->ex_compare) {
			/* XXX is this right? */
			cpu->ex_count = 0;
		}
		if (cpu->irq_timer) {
			CPUTRACE(DOTRACE_IRQ, cpu->cpunum, "Timer irq OFF");
		}
		cpu->irq_timer = 0;
		break;
	    case C0_STATUS:  FN(setstatus)(cpu, greg); break;
	    case C0_CAUSE:   FN(setcause)(cpu, greg); break;
	    case C0_EPC:     /* read-only register */ break;
	    case C0_PRID:    /* read-only register */ break;
	    case C0_CFEAT:   /* read-only register */ break;
	    case C0_IFEAT:   /* read-only register */ break;
	    case C0_CONFIG0:
		/*
		 * Silently ignore. Note however that the
		 * CONFIG0_KSEG0_COHERE field to set the coherence
		 * type for kseg0 is theoretically supposed to be
		 * read/write.
		 */
		break;
	    case C0_CONFIG1: /* read-only register */ break;
	    case C0_CONFIG2: /* read-only register */ break;
	    case C0_CONFIG3: /* read-only register */ break;
	    case C0_CONFIG4: /* read-only register */ break;
	    case C0_CONFIG5: /* read-only register */ break;
	    case C0_CONFIG6: /* read-only register */ break;
	    case C0_CONFIG7: /* read-only register */ break;
	    default:
		FN(exception)(cpu, EX_RI, cn, 0, ", invalid cop0 register");
		break;
	}
}

static
void
FN(dolwc)(struct mipscpu *cpu, int cn, uint32_t addr, int reg)
{
	(void)addr;
	(void)reg;
	FN(exception)(cpu, EX_CPU, cn, 0, ", lwc instruction");
}

static
void
FN(doswc)(struct mipscpu *cpu, int cn, uint32_t addr, int reg)
{
	(void)addr;
	(void)reg;
	FN(exception)(cpu, EX_CPU, cn, 0, ", swc instruction");
}

static
inline
void
FN(mx_add)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDRD;
	int64_t t64;

	TRL("add %s, %s, %s: %ld + %ld -> ",
	    regname(rd), regname(rs), regname(rt), RSsp, RTsp);
	t64 = (int64_t)RSs + (int64_t)RTs;
	CHKOVF(t64);
	RDx = (int32_t)t64;
	TR("%ld", RDsp);
}

static
inline
void
FN(mx_addi)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDSMM;
	int64_t t64;

	TRL("addi %s, %s, %lu: %ld + %ld -> ",
	    regname(rt), regname(rs), (unsigned long)imm, RSsp, (long)smm);
	t64 = (int64_t)RSs + smm;
	CHKOVF(t64);
	RTx = (int32_t)t64;
	TR("%ld", RTsp);
}

static
inline
void
FN(mx_addiu)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDRS; NEEDSMM;
	TRL("addiu %s, %s, %lu: %ld + %ld -> ", 
	    regname(rt), regname(rs), (unsigned long)imm, RSsp, (long)smm);

	/* must add as unsigned, or overflow behavior is not defined */
	RTx = RSu + (uint32_t)smm;

	TR("%ld", RTsp);
}

static
inline
void
FN(mx_addu)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDRD;
	TRL("addu %s, %s, %s: %ld + %ld -> ",
	    regname(rd), regname(rs), regname(rt), RSsp, RTsp);
	RDx = RSu + RTu;
	TR("%ld", RDsp);
}

static
inline
void
FN(mx_and)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDRD;
	TRL("and %s, %s, %s: 0x%lx & 0x%lx -> ", 
	    regname(rd), regname(rs), regname(rt), RSup, RTup);
	RDx = RSu & RTu;
	TR("0x%lx", RDup);
}

static
inline
void
FN(mx_andi)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDIMM;
	TRL("andi %s, %s, %lu: 0x%lx & 0x%lx -> ", 
	    regname(rt), regname(rs), (unsigned long) imm, RSup, 
	    (unsigned long) imm);
	RTx = RSu & imm;
	TR("0x%lx", RTup);
}

static
inline
void
FN(mx_bcf)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDSMM; NEEDCN;
	(void)smm;
	TR("bc%df %ld", cn, (long)smm);
	FN(exception)(cpu, EX_CPU, cn, 0, ", bcf instruction");
}

static
inline
void
FN(mx_bct)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDSMM; NEEDCN;
	(void)smm;
	TR("bc%dt %ld", cn, (long)smm);
	FN(exception)(cpu, EX_CPU, cn, 0, ", bct instruction");
}

static
inline
void
FN(mx_beq)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDRS; NEEDSMM;
	TRL("beq %s, %s, %ld: %lu==%lu? ", 
	    regname(rs), regname(rt), (long)smm, RSup, RTup);
	if (RSu==RTu) {
		TR("yes");
		FN(rbranch)(cpu, smm<<2);
	}
	else {
		TR("no");
	}
}

static
inline
void
FN(mx_bgezal)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDSMM;
	TRL("bgezal %s, %ld: %ld>=0? ", regname(rs), (long)smm, RSsp);
	LINK;
	if (RSs>=0) {
		TR("yes");
		FN(rbranch)(cpu, smm<<2); 
	}
	else {
		TR("no");
	}
}

static
inline
void
FN(mx_bgez)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDSMM;
	TRL("bgez %s, %ld: %ld>=0? ", regname(rs), (long)smm, RSsp);
	if (RSs>=0) {
		TR("yes");
		FN(rbranch)(cpu, smm<<2);
	}
	else {
		TR("no");
	}
}

static
inline
void
FN(mx_bltzal)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDSMM;
	TRL("bltzal %s, %ld: %ld<0? ", regname(rs), (long)smm, RSsp);
	LINK;
	if (RSs<0) {
		TR("yes");
		FN(rbranch)(cpu, smm<<2);
	}
	else {
		TR("no");
	}
}

static
inline
void
FN(mx_bltz)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDSMM;
	TRL("bltz %s, %ld: %ld<0? ", regname(rs), (long)smm, RSsp);
	if (RSs<0) {
		TR("yes");
		FN(rbranch)(cpu, smm<<2);
	}
	else {
		TR("no");
	}
}

static
inline
void
FN(mx_bgtz)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDSMM;
	TRL("bgtz %s, %ld: %ld>0? ", regname(rs), (long)smm, RSsp);
	if (RSs>0) {
		TR("yes");
		FN(rbranch)(cpu, smm<<2);
	}
	else {
		TR("no");
	}
}

static
inline
void
FN(mx_blez)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDSMM;
	TRL("blez %s, %ld: %ld<=0? ", regname(rs), (long)smm, RSsp);
	if (RSs<=0) {
		TR("yes");
		FN(rbranch)(cpu, smm<<2);
	}
	else {
		TR("no");
	}
}

static
inline
void
FN(mx_bne)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDSMM;
	TRL("bne %s, %s, %ld: %lu!=%lu? ", 
	    regname(rs), regname(rt), (long)smm, RSup, RTup);
	if (RSu!=RTu) {
		TR("yes");
		FN(rbranch)(cpu, smm<<2);
	}
	else {
		TR("no");
	}
}

/*
 * Cache control. This actually does nothing, but that's adequate for
 * the time being.
 */
static
inline
void
FN(mx_cache)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDADDR; NEEDRT;
	unsigned cachecode, op;
	enum { L1i, L1d, L2, L3 } cache;
	unsigned cacheway, cacheindex, cacheoffset;
	uint32_t waymask, indexmask, offsetmask;
	unsigned wayshift, indexshift;

	/*
	 * Some documentation says that this instruction is kernel-
	 * only. Other documentation does not say (and even fails to
	 * state that either EX_CPU or EX_RI can be triggered) but
	 * it's clear that at least some of the cache ops cannot be
	 * allowed to unprivileged code. So, better to be safe.
	 */
	if (IS_USERMODE(cpu)) {
		FN(exception)(cpu, EX_CPU, 0 /*cop0*/, 0, ", cache instruction");
		return;
	}

	/*
	 * The caches we reported in the config1 register are 4K,
	 * 4-way, with 16-byte cache lines; that is, 64 sets.
	 * Therefore, the index is 6 bits, the way when we need to
	 * specify it explicitly is 2 bits, and the offset is 4 bits.
	 *
	 * XXX we should have the whole thing parameterized by
	 * preprocessor macros, or even configured at runtime.
	 *
	 * XXX also this should come after we pick the cache, inasmuch
	 * as the parameters ought to be different for L2 and L3. If
	 * we have L2 and L3 - a mips of the vintage we're kinda still
	 * pretending to be wouldn't.
	 */
	waymask = 0x00000c00;
	indexmask = 0x000003f0;
	offsetmask = 0x0000000f;
	wayshift = 10;
	indexshift = 4;

	/* the RT field here is a constant rather than a register number */
	cachecode = rt >> 3;
	op = rt & 7;

	/* XXX should have symbolic constants for this */
	switch (cachecode) {
	    case 0: cache = L1i; break; /* L1 icache */
	    case 1: cache = L1d; break; /* L1 dcache or unified L1 */
	    case 2: cache = L3; break; /* L3 */
	    case 3: cache = L2; break; /* L2 */
	}

	switch (op) {
	    case 0:
	    case 1:
	    case 2:
	    case 3:
		/* address the cache by index */
		cacheway = (addr & waymask) >> wayshift;
		cacheindex = (addr & indexmask) >> indexshift;
		cacheoffset = (addr & offsetmask);
		break;
	    case 4:
	    case 5:
	    case 6:
	    case 7:
		/* address the cache by address */
		if (FN(translatemem)(cpu, addr, 0, &addr) < 0) {
			return;
		}
		cacheway = 0; /* make compiler happy; need to check all ways */
		cacheindex = (addr & indexmask) >> indexshift;
		cacheoffset = (addr & offsetmask);
		break;
	}

	switch (op) {
	    case 0: /* index writeback & invalidate */
		/*
		 * Look at cache[cacheindex].ways[cacheway]; write it back
		 * if needed, then invalidate it. (For the icache, just
		 * invalidate it.)
		 */
		(void)cache;
		(void)cacheway;
		(void)cacheindex;
		break;
	    case 1: /* index load tag */
		/*
		 * Look at cache[cacheindex].ways[cacheway]; read the
		 * tag into the TagLo and TagHi registers; use the
		 * offset to read the data out of the cache line into
		 * the DataLo and DataHi registers. Note: we don't
		 * have any of these registers, and DataLo/DataHi are
		 * optional.
		 *
		 * The sub-word bits of the offset, if any, should be
		 * ignored rather than kvetched about.
		 */
		(void)cache;
		(void)cacheindex;
		(void)cacheway;
		(void)cacheoffset;
		break;
	    case 2: /* index store tag */
		/*
		 * Look at cache[cacheindex].ways[cacheway]; write the
		 * tag from the TagLo and TagHi registers.
		 */
		(void)cache;
		(void)cacheindex;
		(void)cacheway;
		break;
	    case 3: /* implementation-defined operation */
		break;
	    case 4: /* hit invalidate */
		/*
		 * Look at cache[cacheindex]; check all ways for a
		 * matching tag, and if found invalidate it without
		 * writing back.
		 */
		(void)cache;
		(void)cacheindex;
		break;
	    case 5: /* hit writeback & invalidate */
		if (cache == L1i) {
			/*
			 * For the L1 cache this op is "fill" (!)
			 * Load one of the ways at cache[cacheindex]
			 * from memory using the specified address.
			 */
			(void)cache;
			(void)cacheindex;
			(void)addr;
		}
		else {
			/*
			 * Look at cache[cacheindex]; check all ways for
			 * a matching tag, and if found, write it back if
			 * necessary and then invalidate it.
			 */
			(void)cache;
			(void)cacheindex;
		}
		break;
	    case 6: /* hit writeback */
		/*
		 * Look at cache[cacheindex]; check all ways for a
		 * matching tag, and if found, write it back if
		 * necessary. Leave it valid. A nop for the icache, I
		 * guess.
		 */
		(void)cache;
		(void)cacheindex;
		break;
	    case 7: /* fetch and lock */
		/*
		 * Look at cache[cacheindex]; load one of the ways
		 * from memory using the specified address, and lock
		 * it in. It then won't be kicked out unless
		 * explicitly invalidated, or revoked from another
		 * processor. (Or if the lock bit is cleared by
		 * mucking directly with the tag.)
		 */
		(void)cache;
		(void)cacheindex;
		(void)addr;
		break;
	}
}

static
inline
void
FN(mx_cf)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDRD; NEEDCN;
	(void)rt;
	(void)rd;
	TR("cfc%d %s, $%u", cn, regname(rt), rd);
	FN(exception)(cpu, EX_CPU, cn, 0, ", cfc instruction");
}

static
inline
void
FN(mx_ct)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDRD; NEEDCN;
	(void)rt;
	(void)rd;
	TR("ctc%d %s, $%u", cn, regname(rt), rd);
	FN(exception)(cpu, EX_CPU, cn, 0, ", ctc instruction");
}

static
inline
void
FN(mx_j)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDTARG;
	TR("j 0x%lx", (unsigned long)(targ<<2));
	FN(ibranch)(cpu, targ<<2);
}

static
inline
void
FN(mx_jal)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDTARG;
	TR("jal 0x%lx", (unsigned long)(targ<<2));
	LINK;
	FN(ibranch)(cpu, targ<<2);
	prof_call(cpu->pc, cpu->nextpc);
}

static
inline
void
FN(mx_lb)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR;
	TRL("lb %s, %ld(%s): [0x%lx] -> ", 
	    regname(rt), (long)smm, regname(rs), (unsigned long)addr);
	FN(doload)(cpu, S_SBYTE, addr, (uint32_t *) &RTx);
	TR("%ld", RTsp);
}

static
inline
void
FN(mx_lbu)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR;
	TRL("lbu %s, %ld(%s): [0x%lx] -> ",
	    regname(rt), (long)smm, regname(rs), (unsigned long)addr);
	FN(doload)(cpu, S_UBYTE, addr, (uint32_t *) &RTx);
	TR("%ld", RTsp);
}

static
inline
void
FN(mx_lh)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR;
	TRL("lh %s, %ld(%s): [0x%lx] -> ", 
	    regname(rt), (long)smm, regname(rs), (unsigned long)addr);
	FN(doload)(cpu, S_SHALF, addr, (uint32_t *) &RTx);
	TR("%ld", RTsp);
}

static
inline
void
FN(mx_lhu)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR;
	TRL("lhu %s, %ld(%s): [0x%lx] -> ", 
	    regname(rt), (long)smm, regname(rs), (unsigned long)addr);
	FN(doload)(cpu, S_UHALF, addr, (uint32_t *) &RTx);
	TR("%ld", RTsp);
}

static
inline
void
FN(mx_ll)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR;
	TRL("ll %s, %ld(%s): [0x%lx] -> ", 
	    regname(rt), (long)smm, regname(rs), (unsigned long)addr);
	if (FN(domem)(cpu, addr, (uint32_t *) &RTx, 0, 0)) {
		/* exception */
		return;
	}

	/* load linked: just save what we did */
	cpu->ll_active = 1;
	cpu->ll_addr = addr;
	cpu->ll_value = RTs;

	g_stats.s_percpu[cpu->cpunum].sp_lls++;

	TR("%ld", RTsp);
}

static
inline
void
FN(mx_lui)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDIMM;
	TR("lui %s, 0x%x", regname(rt), imm);
	RTx = imm << 16;
}

static
inline
void
FN(mx_lw)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR;
	TRL("lw %s, %ld(%s): [0x%lx] -> ", 
	    regname(rt), (long)smm, regname(rs), (unsigned long)addr);
	FN(domem)(cpu, addr, (uint32_t *) &RTx, 0, 0);
	TR("%ld", RTsp);
}

static
inline
void
FN(mx_lwc)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR; NEEDCN;
	TR("lwc%d $%u, %ld(%s)", cn, rt, (long)smm, regname(rs));
	FN(dolwc)(cpu, cn, addr, rt);
}

static
inline
void
FN(mx_lwl)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR;
	TRL("lwl %s, %ld(%s): [0x%lx] -> ", 
	    regname(rt), (long)smm, regname(rs), (unsigned long)addr);
	FN(doload)(cpu, S_WORDL, addr, (uint32_t *) &RTx);
	TR("0x%lx", RTup);
}

static
inline
void
FN(mx_lwr)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR;
	TRL("lwr %s, %ld(%s): [0x%lx] -> ", 
	    regname(rt), (long)smm, regname(rs), (unsigned long)addr);
	FN(doload)(cpu, S_WORDR, addr, (uint32_t *) &RTx);
	TR("0x%lx", RTup);
}

static
inline
void
FN(mx_sb)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR;
	TR("sb %s, %ld(%s): %d -> [0x%lx]", 
	   regname(rt), (long)smm, regname(rs),
	   (int)(RTu&0xff), (unsigned long)addr);
	FN(dostore)(cpu, S_UBYTE, addr, RTu);
}

static
inline
void
FN(mx_sc)(struct mipscpu *cpu, uint32_t insn)
{
	uint32_t temp;
	NEEDRT; NEEDADDR;
	TR("sc %s, %ld(%s): %ld -> [0x%lx]", 
	   regname(rt), (long)smm, regname(rs), RTsp, (unsigned long)addr);

	/*
	 * Store conditional.
	 *
	 * This implementation uses the observation that if the target
	 * memory address contains the same value we loaded earlier
	 * with LL, then execution is completely equivalent to
	 * performing an atomic operation that reads it now. This is
	 * true even if the target memory has been written to in
	 * between, even though allowing that is formally against the
	 * spec. Other operations that might allow distinguishing
	 * such a case, such as reading other memory regions with or
	 * without using LL, lead to the behavior of the SC being
	 * formally unpredictable or undefined.
	 *
	 * (Since a number of people have missed or misunderstood this
	 * point: it is NOT ALLOWED to do other memory accesses
	 * between the LL and SC. If you do so, the SC no longer
	 * guarantees you an atomic operation; it might fail if
	 * another CPU has accessed the memory you LL'd, but it might
	 * not. Or it might always fail. This is how LL/SC is defined,
	 * on MIPS and other architectures, not a "bug" in System/161.
	 * If you want to make multiple memory accesses atomic, you
	 * need a transactional memory system; good luck with that.)
	 *
	 * The address of the SC must be the same as the LL. That's
	 * supposed to mean both vaddr and paddr, as well as caching
	 * mode and any memory bus consistency mode. If they don't
	 * match, the behavior of the SC is undefined. We check the
	 * vaddr and fail if it doesn't match; if someone does
	 * something reckless to make the vaddr but not the paddr
	 * match, they deserve the consequences. (This can only happen
	 * in kernel mode; changing MMU mappings from user mode
	 * requires a trap, which invalidates the LL.)
	 *
	 * So:
	 *
	 * 1. If we don't have an LL active, SC fails.
	 * 2. If the target vaddr is not the same as the one on file
	 *    from LL, SC fails.
	 * 3. We reread from the address; if the read fails, we
	 *    have taken an exception, so just return.
	 * 4. If the result value is different from the one on file
	 *    from LL, SC fails.
	 * 5. We write to the address; if the write fails, we have
	 *    taken an exception, so just return.
	 * 6. SC succeeds.
	 */

	if (!cpu->ll_active) {
		goto fail;
	}
	if (cpu->ll_addr != addr) {
		goto fail;
	}
	if (FN(domem)(cpu, addr, &temp, 0, 1)) {
		/* exception */
		return;
	}
	if (temp != cpu->ll_value) {
		goto fail;
	}
	if (FN(domem)(cpu, addr, (uint32_t *) &RTx, 1, 1)) {
		/* exception */
		return;
	}
	/* success */
	RTx = 1;
	g_stats.s_percpu[cpu->cpunum].sp_okscs++;
	return;

 fail:
	/* failure */
	RTx = 0;
	g_stats.s_percpu[cpu->cpunum].sp_badscs++;
}

static
inline
void
FN(mx_sh)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR;
	TR("sh %s, %ld(%s): %d -> [0x%lx]", 
	   regname(rt), (long)smm, regname(rs),
	   (int)(RTu&0xffff), (unsigned long)addr);
	FN(dostore)(cpu, S_UHALF, addr, RTu);
}

static
inline
void
FN(mx_sw)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR;
	TR("sw %s, %ld(%s): %ld -> [0x%lx]", 
	   regname(rt), (long)smm, regname(rs), RTsp, (unsigned long)addr);
	FN(domem)(cpu, addr, (uint32_t *) &RTx, 1, 1);
}

static
inline
void
FN(mx_swc)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR; NEEDCN;
	TR("swc%d $%u, %ld(%s)", cn, rt, (long)smm, regname(rs));
	FN(doswc)(cpu, cn, addr, rt);
}

static
inline
void
FN(mx_swl)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR;
	TR("swl %s, %ld(%s): 0x%lx -> [0x%lx]", 
	   regname(rt), (long)smm, regname(rs), RTup, (unsigned long)addr);
	FN(dostore)(cpu, S_WORDL, addr, RTu);
}

static
inline
void
FN(mx_swr)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDADDR;
	TR("swr %s, %ld(%s): 0x%lx -> [0x%lx]", 
	   regname(rt), (long)smm, regname(rs), RTup, (unsigned long)addr);
	FN(dostore)(cpu, S_WORDR, addr, RTu);
}

static
inline
void
FN(mx_break)(struct mipscpu *cpu, uint32_t insn)
{
	(void)insn;
	TR("break");
	FN(exception)(cpu, EX_BP, 0, 0, "");
}

static
inline
void
FN(mx_div)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT;
	TRL("div %s %s: %ld / %ld -> ", 
	    regname(rs), regname(rt), RSsp, RTsp);

	WHILO;
	if (RTs==0) {
		/*
		 * On divide-by-zero the mips doesn't trap.
		 * Instead, the assembler emits an integer check for
		 * zero that (on the real chip) runs in parallel with
		 * the divide unit.
		 *
		 * I don't know what the right values to load in the
		 * result are, if there are any that are specified,
		 * but I'm going to make up what seems like a good
		 * excuse for machine infinity.
		 */
		if (RSs < 0) {
			cpu->lo = 0xffffffff;
		}
		else {
			cpu->lo = 0x7fffffff;
		}
		cpu->hi = 0;
		TR("ERR");
	}
	else {
		cpu->lo = RSs/RTs;
		cpu->hi = RSs%RTs;
		TR("%ld, remainder %ld", 
		   (long)(int32_t)cpu->lo, (long)(int32_t)cpu->hi);
	}
	SETHILO(2);
}

static
inline
void
FN(mx_divu)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT;
	TRL("divu %s %s: %lu / %lu -> ", 
	    regname(rs), regname(rt), RSup, RTup);

	WHILO;
	if (RTu==0) {
		/*
		 * See notes under signed divide above.
		 */
		cpu->lo = 0xffffffff;
		cpu->hi = 0;
		TR("ERR");
	}
	else {
		cpu->lo=RSu/RTu;
		cpu->hi=RSu%RTu;
		TR("%lu, remainder %lu", 
		   (unsigned long)(uint32_t)cpu->lo, 
		   (unsigned long)(uint32_t)cpu->hi);
	}
	SETHILO(2);
}

static
inline
void
FN(mx_jr)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS;
	TR("jr %s: 0x%lx", regname(rs), RSup);
	FN(abranch)(cpu, RSu);
}

static
inline
void
FN(mx_jalr)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRD;
	TR("jalr %s, %s: 0x%lx", regname(rd), regname(rs), RSup);
	LINK2(rd);
	FN(abranch)(cpu, RSu);
#ifdef USE_TRACE
	prof_call(cpu->pc, cpu->nextpc);
#endif
}

static
inline
void
FN(mx_mf)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDRD; NEEDCN; NEEDSEL;
	if (sel) {
		TRL("mfc%d %s, $%u.%u: ... -> ", cn, regname(rt), rd, sel);
	}
	else {
		TRL("mfc%d %s, $%u: ... -> ", cn, regname(rt), rd);
	}
	FN(domf)(cpu, cn, rd, sel, &RTx);
	TR("0x%lx", RTup);
}

static
inline
void
FN(mx_mfhi)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRD;
	TRL("mfhi %s: ... -> ", regname(rd));
	WHI;
	RDx = cpu->hi;
	SETHI(2);
	TR("0x%lx", RDup);
}

static
inline
void
FN(mx_mflo)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRD;
	TRL("mflo %s: ... -> ", regname(rd));
	WLO;
	RDx = cpu->lo;
	SETLO(2);
	TR("0x%lx", RDup);
}

static
inline
void
FN(mx_mt)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRT; NEEDRD; NEEDCN; NEEDSEL;
	if (sel) {
		TR("mtc%d %s, $%u.%u: 0x%lx -> ...", cn, regname(rt), rd, sel,
		   RTup);
	}
	else {		
		TR("mtc%d %s, $%u: 0x%lx -> ...", cn, regname(rt), rd, RTup);
	}
	FN(domt)(cpu, cn, rd, sel, RTs);
}

static
inline
void
FN(mx_mthi)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS;
	TR("mthi %s: 0x%lx -> ...", regname(rs), RSup);
	WHI;
	cpu->hi = RSu;
	SETHI(2);
}

static
inline
void
FN(mx_mtlo)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS;
	TR("mtlo %s: 0x%lx -> ...", regname(rs), RSup);
	WLO;
	cpu->lo = RSu;
	SETLO(2);
}

static
inline
void
FN(mx_mult)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT;
	int64_t t64;
	TRL("mult %s, %s: %ld * %ld -> ", 
	    regname(rs), regname(rt), RSsp, RTsp);
	WHILO;
	t64=(int64_t)RSs*(int64_t)RTs;
	cpu->hi = (((uint64_t)t64)&0xffffffff00000000ULL) >> 32;
	cpu->lo = (uint32_t)(((uint64_t)t64)&0x00000000ffffffffULL);
	SETHILO(2);
	TR("%ld %ld", (long)(int32_t)cpu->hi, (long)(int32_t)cpu->lo);
}

static
inline
void
FN(mx_multu)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT;
	uint64_t t64;
	TRL("multu %s, %s: %lu * %lu -> ", 
	    regname(rs), regname(rt), RSup, RTup);
	WHILO;
	t64=(uint64_t)RSu*(uint64_t)RTu;
	cpu->hi = (t64&0xffffffff00000000ULL) >> 32;
	cpu->lo = (uint32_t)(t64&0x00000000ffffffffULL);
	SETHILO(2);
	TR("%lu %lu",
	   (unsigned long)(uint32_t)cpu->hi,
	   (unsigned long)(uint32_t)cpu->lo);
}

static
inline
void
FN(mx_nor)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDRD;
	TRL("nor %s, %s, %s: ~(0x%lx | 0x%lx) -> ",
	    regname(rd), regname(rs), regname(rt), RSup, RTup);
	RDx = ~(RSu | RTu);
	TR("0x%lx", RDup);
}

static
inline
void
FN(mx_or)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDRD;
	TRL("or %s, %s, %s: 0x%lx | 0x%lx -> ", 
	    regname(rd), regname(rs), regname(rt), RSup, RTup);
	RDx = RSu | RTu;
	TR("0x%lx", RDup);
}

static
inline
void
FN(mx_ori)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDIMM;
	TRL("ori %s, %s, %lu: 0x%lx | 0x%lx -> ", 
	    regname(rt), regname(rs), (unsigned long)imm,
	    RSup, (unsigned long)imm);
	RTx = RSu | imm;
	TR("0x%lx", RTup);
}

static
inline
void
FN(mx_rfe)(struct mipscpu *cpu, uint32_t insn)
{
	(void)insn;
	TR("rfe");
	FN(do_rfe)(cpu);
}

static
inline
void
FN(mx_sll)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRD; NEEDRT; NEEDSH;
	TRL("sll %s, %s, %u: 0x%lx << %u -> ", 
	    regname(rd), regname(rt), (unsigned)sh, RTup, (unsigned)sh);
	RDx = RTu << sh;
	TR("0x%lx", RDup);
}

static
inline
void
FN(mx_sllv)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRD; NEEDRT; NEEDRS;
	unsigned vsh = (RSu&31);
	TRL("sllv %s, %s, %s: 0x%lx << %u -> ", 
	    regname(rd), regname(rt), regname(rs), RTup, vsh);
	RDx = RTu << vsh;
	TR("0x%lx", RDup);
}

static
inline
void
FN(mx_slt)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDRD;
	TRL("slt %s, %s, %s: %ld < %ld -> ", 
	    regname(rd), regname(rs), regname(rt), RSsp, RTsp);
	RDx = RSs < RTs;
	TR("%ld", RDsp);
}

static
inline
void
FN(mx_slti)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDSMM;
	TRL("slti %s, %s, %ld: %ld < %ld -> ", 
	    regname(rt), regname(rs), (long)smm, RSsp, (long)smm);
	RTx = RSs < smm;
	TR("%ld", RTsp);
}

static
inline
void
FN(mx_sltiu)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDSMM;
	TRL("sltiu %s, %s, %lu: %lu < %lu -> ", 
	    regname(rt), regname(rs), (unsigned long)imm, RSup, 
	    (unsigned long)(uint32_t)smm);
	// Yes, the immediate is sign-extended then treated as
	// unsigned, according to my mips book. Blech.
	RTx = RSu < (uint32_t) smm;
	TR("%ld", RTsp);
}

static
inline
void
FN(mx_sltu)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDRD;
	TRL("sltu %s, %s, %s: %lu < %lu -> ", 
	    regname(rd), regname(rs), regname(rt), RSup, RTup);
	RDx = RSu < RTu;
	TR("%ld", RDsp);
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

static
inline
void
FN(mx_sra)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRD; NEEDRT; NEEDSH;
	TRL("sra %s, %s, %u: 0x%lx >> %u -> ", 
	    regname(rd), regname(rt), (unsigned)sh, RTup, (unsigned)sh);
	RDx = FN(signedshift)(RTu, sh);
	TR("0x%lx", RDup);
}

static
inline
void
FN(mx_srav)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDRD;
	unsigned vsh = (RSu&31);
	TRL("srav %s, %s, %s: 0x%lx >> %u -> ", 
	    regname(rd), regname(rt), regname(rs), RTup, vsh);
	RDx = FN(signedshift)(RTu, vsh);
	TR("0x%lx", RDup);
}

static
inline
void
FN(mx_srl)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRD; NEEDRT; NEEDSH;
	TRL("srl %s, %s, %u: 0x%lx >> %u -> ", 
	    regname(rd), regname(rt), (unsigned)sh, RTup, (unsigned)sh);
	RDx = RTu >> sh;
	TR("0x%lx", RDup);
}

static
inline
void
FN(mx_srlv)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDRD;
	unsigned vsh = (RSu&31);
	TRL("srlv %s, %s, %s: 0x%lx >> %u -> ", 
	    regname(rd), regname(rt), regname(rs), RTup, vsh);
	RDx = RTu >> vsh;
	TR("0x%lx", RDup);
}

static
inline
void
FN(mx_sub)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDRD;
	int64_t t64;
	TRL("sub %s, %s, %s: %ld - %ld -> ", 
	    regname(rd), regname(rs), regname(rt), RSsp, RTsp);
	t64 = (int64_t)RSs - (int64_t)RTs;
	CHKOVF(t64);
	RDx = (int32_t)t64;
	TR("%ld", RDsp);
}

static
inline
void
FN(mx_subu)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDRD;
	TRL("subu %s, %s, %s: %ld - %ld -> ", 
	    regname(rd), regname(rs), regname(rt), RSsp, RTsp);
	RDx = RSu - RTu;
	TR("%ld", RDsp);
}

static
inline
void
FN(mx_sync)(struct mipscpu *cpu, uint32_t insn)
{
	/* flush pending memory accesses; for now nothing needed */
	(void)cpu;
	(void)insn;
	TR("sync");
	g_stats.s_percpu[cpu->cpunum].sp_syncs++;
}

static
inline
void
FN(mx_syscall)(struct mipscpu *cpu, uint32_t insn)
{
	(void)insn;
	TR("syscall");
	FN(exception)(cpu, EX_SYS, 0, 0, "");
}

static
inline
void
FN(mx_tlbp)(struct mipscpu *cpu, uint32_t insn)
{
	(void)insn;
	TR("tlbp");
	FN(probetlb)(cpu);
}

static
inline
void
FN(mx_tlbr)(struct mipscpu *cpu, uint32_t insn)
{
	(void)insn;
	TR("tlbr");
	cpu->tlbentry = cpu->tlb[cpu->tlbindex];
	CPUTRACEL(DOTRACE_TLB, cpu->cpunum, "tlbr:  [%2d] ", cpu->tlbindex);
	TLBTR(&cpu->tlbentry);
	CPUTRACE(DOTRACE_TLB, cpu->cpunum, " ");
}

static
inline
void
FN(mx_tlbwi)(struct mipscpu *cpu, uint32_t insn)
{
	(void)insn;
	TR("tlbwi");
	FN(writetlb)(cpu, cpu->tlbindex, "tlbwi");
}

static
inline
void
FN(mx_tlbwr)(struct mipscpu *cpu, uint32_t insn)
{
	(void)insn;
	TR("tlbwr");
	cpu->tlbrandom %= RANDREG_MAX;
	FN(writetlb)(cpu, cpu->tlbrandom+RANDREG_OFFSET, "tlbwr");
}

static
inline
void
FN(mx_wait)(struct mipscpu *cpu, uint32_t insn)
{
	(void)insn;
	TR("wait");
	FN(do_wait)(cpu);
}

static
inline
void
FN(mx_xor)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDRD;
	TRL("xor %s, %s, %s: 0x%lx ^ 0x%lx -> ",
	    regname(rd), regname(rs), regname(rt), RSup, RTup);
	RDx = RSu ^ RTu;
	TR("0x%lx", RDup);
}

static
inline
void
FN(mx_xori)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDRS; NEEDRT; NEEDIMM;
	TRL("xori %s, %s, %lu: 0x%lx ^ 0x%lx -> ",
	    regname(rt), regname(rs), (unsigned long)imm, 
	    RSup, (unsigned long)imm);
	RTx = RSu ^ imm;
	TR("0x%lx", RTup);
}

static
inline
void
FN(mx_ill)(struct mipscpu *cpu, uint32_t insn)
{
	(void)insn;
	TR("[illegal instruction %08lx]", (unsigned long) insn);
	FN(exception)(cpu, EX_RI, 0, 0, "");
}

/*
 * Note: OP_WAIT is not defined for mips r2000/r3000 - it's from later
 * MIPS versions. However, we support it here anyway because spinning
 * in an idle loop is just plain stupid.
 */
static
inline
void
FN(mx_copz)(struct mipscpu *cpu, uint32_t insn)
{
	NEEDCN;
	uint32_t copop;

	if (cn!=0) {
		FN(exception)(cpu, EX_CPU, cn, 0, ", copz instruction");
		return;
	}
	if (IS_USERMODE(cpu)) {
		FN(exception)(cpu, EX_CPU, cn, 0, ", copz instruction");
		return;
	}

	copop = (insn & 0x03e00000) >> 21;	// coprocessor opcode

	if (copop & 0x10) {
		copop = (insn & 0x01ffffff);	// real coprocessor opcode
		switch (copop) {
		    case 1: FN(mx_tlbr)(cpu, insn); break;
		    case 2: FN(mx_tlbwi)(cpu, insn); break;
		    case 6: FN(mx_tlbwr)(cpu, insn); break;
		    case 8: FN(mx_tlbp)(cpu, insn); break;
		    case 16: FN(mx_rfe)(cpu, insn); break;
		    case 32: FN(mx_wait)(cpu, insn); break;
		    default: FN(mx_ill)(cpu, insn); break;
		}
	}
	else switch (copop) {
	    case 0: FN(mx_mf)(cpu, insn); break;
	    case 2: FN(mx_cf)(cpu, insn); break;
	    case 4: FN(mx_mt)(cpu, insn); break;
	    case 6: FN(mx_ct)(cpu, insn); break;
	    case 8:
	    case 12:
		if (insn & 0x00010000) {
			FN(mx_bcf)(cpu, insn);
		}
		else {
			FN(mx_bct)(cpu, insn);
		}
		break;
	    default: FN(mx_ill)(cpu, insn);
	}
}

static
int
FN(cpu_cycle)(void)
{
	uint32_t insn;
	uint32_t op;
	unsigned whichcpu;
	unsigned breakpoints = 0;
	uint32_t retire_pc;
	unsigned retire_usermode;

	for (whichcpu=0; whichcpu < ncpus; whichcpu++) {
		struct mipscpu *cpu = &mycpus[whichcpu];

		if (cpu->state != CPU_RUNNING) {
			// don't check this on the critical path
			//Assert((cpu_running_mask & thiscpumask) == 0);
			g_stats.s_percpu[cpu->cpunum].sp_icycles++;
			continue;
		}

	/* INDENT HORROR BEGIN */

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
	if (cpu->current_irqon) {
		uint32_t soft = cpu->status_softmask & cpu->cause_softirq;
		int lb = cpu->irq_lamebus && cpu->status_hardmask_lb;
		int ipi = cpu->irq_ipi && cpu->status_hardmask_ipi;
		int timer = cpu->irq_timer && cpu->status_hardmask_timer;

		if (lb || ipi || timer || soft) {
			CPUTRACE(DOTRACE_IRQ, cpu->cpunum,
				 "Taking interrupt:%s%s%s%s",
				 lb ? " LAMEbus" : "",
				 ipi ? " IPI" : "",
				 timer ? " timer" : "",
				 soft ? " soft" : "");
			FN(exception)(cpu, EX_IRQ, 0, 0,
				  lb ? ", LAMEbus" :
				  ipi ? ", IPI" :
				  timer ? ", timer" :
				  soft ? ", softirq" : "");
			/*
			 * Start processing the interrupt this cycle.
			 *
			 * That is, repeat the code above us in this
			 * function.
			 *
			 * At this point we're executing the first 
			 * instruction of the exception handler, which
			 * cannot be a jump delay slot.
			 *
			 * Thus, just set expc.
			 */
			cpu->expc = cpu->pc;
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
	 * If at the end of all the following logic, the PC (which
	 * will hold the address of the next instruction to execute)
	 * is still what's currently nextpc, we haven't taken an
	 * exception and that means we've retired an instruction. We
	 * need to record whether it was user or kernel here, because
	 * otherwise if we switch modes (e.g. with RFE) it'll be
	 * credited incorrectly.
	 */
	retire_pc = cpu->nextpc;
	retire_usermode = IS_USERMODE(cpu);

	/*
	 * Fetch instruction.
	 *
	 * We cache the page translation for the PC.
	 * Use the page part of the precomputed physpc and also the
	 * precomputed page pointer.
	 *
	 * Note that as a result of precomputing everything, exceptions
	 * related to PC mishaps occur at jump time, or possibly when
	 * *nextpc* crosses a page boundary (below) or whatnot, never 
	 * during instruction fetch itself. I believe this is acceptable
	 * behavior to exhibit.
	 */
	insn = bus_use_map(cpu->pcpage, cpu->pcoff);

	// Update PC. 
	cpu->pc = cpu->nextpc;
	cpu->pcoff = cpu->nextpcoff;
	cpu->pcpage = cpu->nextpcpage;
	cpu->nextpc += 4;
	if ((cpu->nextpc & 0xfff)==0) {
		/* crossed page boundary */
		if (insn == FULLOP_RFE) {
			/* defer precompute_nextpc() */
			cpu->nextpcpage = NULL;
			cpu->nextpcoff = 0;
		}
		else if (FN(precompute_nextpc)(cpu)) {
			/* exception. on to next cpu. */
			continue;
		}
	}
	else {
		cpu->nextpcoff += 4;
	}

	TRL("at %08x: ", cpu->expc);
	
	/*
	 * Decode instruction.
	 */

	cpu->hit_breakpoint = 0;
	
	op = (insn & 0xfc000000) >> 26;   // opcode

	switch (op) {
	    case OPM_SPECIAL:
		// use function field
		switch (insn & 0x3f) {
		    case OPS_SLL: FN(mx_sll)(cpu, insn); break;
		    case OPS_SRL: FN(mx_srl)(cpu, insn); break;
		    case OPS_SRA: FN(mx_sra)(cpu, insn); break;
		    case OPS_SLLV: FN(mx_sllv)(cpu, insn); break;
		    case OPS_SRLV: FN(mx_srlv)(cpu, insn); break;
		    case OPS_SRAV: FN(mx_srav)(cpu, insn); break;
		    case OPS_JR: FN(mx_jr)(cpu, insn); break;
		    case OPS_JALR: FN(mx_jalr)(cpu, insn); break;
		    case OPS_SYSCALL: FN(mx_syscall)(cpu, insn); break;
		    case OPS_BREAK: 
			/*
			 * If we're in the range that we can debug in (that
			 * is, not the TLB-mapped segments), activate the
			 * kernel debugging hooks.
			 */
			if (gdb_canhandle(cpu->expc)) {
				FN(phony_exception)(cpu);
				cpu_stopcycling();
				main_enter_debugger(0 /* not lethal */);
				/*
				 * Don't bill time for hitting the breakpoint.
				 */
				breakpoints++;
				cpu->ex_count--;
				cpu->hit_breakpoint = 1;
				continue;
			}
			FN(mx_break)(cpu, insn);
			break;
		    case OPS_SYNC: FN(mx_sync)(cpu, insn); break;
		    case OPS_MFHI: FN(mx_mfhi)(cpu, insn); break;
		    case OPS_MTHI: FN(mx_mthi)(cpu, insn); break;
		    case OPS_MFLO: FN(mx_mflo)(cpu, insn); break;
		    case OPS_MTLO: FN(mx_mtlo)(cpu, insn); break;
		    case OPS_MULT: FN(mx_mult)(cpu, insn); break;
		    case OPS_MULTU: FN(mx_multu)(cpu, insn); break;
		    case OPS_DIV: FN(mx_div)(cpu, insn); break;
		    case OPS_DIVU: FN(mx_divu)(cpu, insn); break;
		    case OPS_ADD: FN(mx_add)(cpu, insn); break;
		    case OPS_ADDU: FN(mx_addu)(cpu, insn); break;
		    case OPS_SUB: FN(mx_sub)(cpu, insn); break;
		    case OPS_SUBU: FN(mx_subu)(cpu, insn); break;
		    case OPS_AND: FN(mx_and)(cpu, insn); break;
		    case OPS_OR: FN(mx_or)(cpu, insn); break;
		    case OPS_XOR: FN(mx_xor)(cpu, insn); break;
		    case OPS_NOR: FN(mx_nor)(cpu, insn); break;
		    case OPS_SLT: FN(mx_slt)(cpu, insn); break;
		    case OPS_SLTU: FN(mx_sltu)(cpu, insn); break;
		    default: FN(mx_ill)(cpu, insn); break;
		}
		break;
	    case OPM_BCOND:
		// use rt field
		switch ((insn & 0x001f0000) >> 16) {
		    case 0: FN(mx_bltz)(cpu, insn); break;
		    case 1: FN(mx_bgez)(cpu, insn); break;
		    case 16: FN(mx_bltzal)(cpu, insn); break;
		    case 17: FN(mx_bgezal)(cpu, insn); break;
		    default: FN(mx_ill)(cpu, insn); break;
		}
		break;
	    case OPM_J: FN(mx_j)(cpu, insn); break;
	    case OPM_JAL: FN(mx_jal)(cpu, insn); break;
	    case OPM_BEQ: FN(mx_beq)(cpu, insn); break;
	    case OPM_BNE: FN(mx_bne)(cpu, insn); break;
	    case OPM_BLEZ: FN(mx_blez)(cpu, insn); break;
	    case OPM_BGTZ: FN(mx_bgtz)(cpu, insn); break;
	    case OPM_ADDI: FN(mx_addi)(cpu, insn); break;
	    case OPM_ADDIU: FN(mx_addiu)(cpu, insn); break;
	    case OPM_SLTI: FN(mx_slti)(cpu, insn); break;
	    case OPM_SLTIU: FN(mx_sltiu)(cpu, insn); break;
	    case OPM_ANDI: FN(mx_andi)(cpu, insn); break;
	    case OPM_ORI: FN(mx_ori)(cpu, insn); break;
	    case OPM_XORI: FN(mx_xori)(cpu, insn); break;
	    case OPM_LUI: FN(mx_lui)(cpu, insn); break;
	    case OPM_COP0:
	    case OPM_COP1:
	    case OPM_COP2:
	    case OPM_COP3: FN(mx_copz)(cpu, insn); break;
	    case OPM_LB: FN(mx_lb)(cpu, insn); break;
	    case OPM_LH: FN(mx_lh)(cpu, insn); break;
	    case OPM_LWL: FN(mx_lwl)(cpu, insn); break;
	    case OPM_LW: FN(mx_lw)(cpu, insn); break;
	    case OPM_LBU: FN(mx_lbu)(cpu, insn); break;
	    case OPM_LHU: FN(mx_lhu)(cpu, insn); break;
	    case OPM_LWR: FN(mx_lwr)(cpu, insn); break;
	    case OPM_SB: FN(mx_sb)(cpu, insn); break;
	    case OPM_SH: FN(mx_sh)(cpu, insn); break;
	    case OPM_SWL: FN(mx_swl)(cpu, insn); break;
	    case OPM_SW: FN(mx_sw)(cpu, insn); break;
	    case OPM_SWR: FN(mx_swr)(cpu, insn); break;
	    case OPM_CACHE: FN(mx_cache)(cpu, insn); break;
	    case OPM_LWC0: /* LWC0 == LL */ FN(mx_ll)(cpu, insn); break;
	    case OPM_LWC1:
	    case OPM_LWC2:
	    case OPM_LWC3: FN(mx_lwc)(cpu, insn); break;
	    case OPM_SWC0: /* SWC0 == SC */ FN(mx_sc)(cpu, insn); break;
	    case OPM_SWC1:
	    case OPM_SWC2:
	    case OPM_SWC3: FN(mx_swc)(cpu, insn); break;
	    default: FN(mx_ill)(cpu, insn); break;
	}

	/* Timer. Take interrupt on next cycle; call it a pipeline effect. */
	cpu->ex_count++;
	if (cpu->ex_compare_used && cpu->ex_count == cpu->ex_compare) {
		cpu->ex_count = 0; /* XXX is this right? */
		cpu->irq_timer = 1;
		CPUTRACE(DOTRACE_IRQ, cpu->cpunum, "Timer irq ON");
	}

	if (cpu->lowait > 0) {
		cpu->lowait--;
	}
	if (cpu->hiwait > 0) {
		cpu->hiwait--;
	}

	cpu->in_jumpdelay = 0;
	
	cpu->tlbrandom++;

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
	if (cpu->pc == retire_pc) {
		if (retire_usermode) {
			g_stats.s_percpu[cpu->cpunum].sp_uretired++;
			progress = 1;
		}
		else {
			g_stats.s_percpu[cpu->cpunum].sp_kretired++;
		}
	}

	/* INDENT HORROR END */

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

