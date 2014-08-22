#ifndef CPU_H
#define CPU_H

/* nonzero if at least one cpu has work to do */
extern u_int32_t cpu_running_mask;

/* number of cycles into cpu_cycles() */
extern uint64_t cpu_cycles_count;

void cpu_init(unsigned numcpus);
uint64_t cpu_cycles(uint64_t maxcycles); /* returns cycles spent */
void cpu_stopcycling(void); /* stops cpu_cycles() */

void cpu_dumpstate(void);

unsigned cpu_numcpus(void);

/* Functions for enabling/disabling cpus */
void cpu_enable(unsigned cpunum);
void cpu_disable(unsigned cpunum);
int cpu_enabled(unsigned cpunum);

/* Functions used for address range translation by the kernel load code */
int cpu_get_load_paddr(u_int32_t vaddr, u_int32_t size, u_int32_t *paddr);
int cpu_get_load_vaddr(u_int32_t paddr, u_int32_t size, u_int32_t *vaddr);

/* Functions used to update the cpu state by the kernel load code */
void cpu_set_entrypoint(unsigned cpunum, u_int32_t addr);
void cpu_set_stack(unsigned cpunum, u_int32_t stackaddr, u_int32_t argument);

/* Function used for secondary cpu initialization */
u_int32_t cpu_get_secondary_start_stack(u_int32_t lboffset);

/* Function for IRQ propagation */
void cpu_set_irqs(unsigned cpunum, int lamebus, int ipi);

/* Functions used by the remote gdb support */
unsigned cpudebug_get_break_cpu(void);
int cpudebug_fetch_byte(unsigned cpunum, u_int32_t va, u_int8_t *byte);
int cpudebug_fetch_word(unsigned cpunum, u_int32_t va, u_int32_t *word);
int cpudebug_store_byte(unsigned cpunum, u_int32_t va, u_int8_t byte);
int cpudebug_store_word(unsigned cpunum, u_int32_t va, u_int32_t word);
void cpudebug_get_bp_region(u_int32_t *start, u_int32_t *end);
void cpudebug_getregs(unsigned cpunum, u_int32_t *regs, int maxregs,
		      int *nregs);

/* Functions used by the profiling code */
u_int32_t cpuprof_sample(void);

#endif /* CPU_H */
