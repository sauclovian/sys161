#ifndef BUS_H
#define BUS_H

/*
 * Addresses are relative to the start of RAM pretending it's contiguous,
 * or relative to the start of I/O space. We split things up this way
 * because the actual memory layout is machine-dependent.
 */

int bus_mem_fetch(u_int32_t addr, u_int32_t *);
int bus_mem_store(u_int32_t addr, u_int32_t);
int bus_io_fetch(u_int32_t addr, u_int32_t *);
int bus_io_store(u_int32_t addr, u_int32_t);

/*
 * Propagate bus interrupt state to cpu interrupt state.
 */
void bus_forward_interrupts(void);

/*
 * Set up bus and cards in bus.
 */
void bus_config(const char *configfile);

/*
 * Clean up bus in preparation for exit.
 */
void bus_cleanup(void);


/*
 * Load kernel. (boot.c)
 */
void load_kernel(const char *image, const char *argument);

#endif /* BUS_H */
