#ifndef CPU_H
#define CPU_H

void cpu_init(void);
int cpu_cycle(void);		/* returns nonzero if builtin breakpoint */

extern int cpu_irq_line;              /* nonzero if IRQ is active */
extern int cpu_nmi_line;              /* nonzero if NMI is active */

/* Functions used for address range translation by the kernel load code */
int cpu_get_load_paddr(u_int32_t vaddr, u_int32_t size, u_int32_t *paddr);
int cpu_get_load_vaddr(u_int32_t paddr, u_int32_t size, u_int32_t *vaddr);

/* Functions used to update the cpu state by the kernel load code */
void cpu_set_entrypoint(u_int32_t addr);
void cpu_set_stack(u_int32_t stackaddr, u_int32_t argument);

/* Functions used by the remote gdb support */
void cpudebug_get_bp_region(u_int32_t *start, u_int32_t *end);
int cpudebug_translate_address(u_int32_t va, u_int32_t *pa_ret);
void cpudebug_getregs(u_int32_t *regs, int maxregs, int *nregs);

#endif /* CPU_H */
