/*
 * Tell mainloop to begin poweroff.
 * (poweroff takes a few ms; see speed.h)
 */
void main_poweroff(void);


/*
 * Tell mainloop it should suspend execution.
 */
void main_stop(void);

/*
 * Tell mainloop it should resume execution.
 */
void main_continue(void);

/*
 * Have the mainloop code run a single processor cycle.
 * Returns nonzero if we hit a breakpoint instruction in that cycle.
 */
int onecycle(void);


/*
 * Hardware counters reported at simulator exit.
 */

struct stats {
	u_int64_t s_ucycles;  // user mode cycles
	u_int64_t s_kcycles;  // kernel mode cycles
	u_int64_t s_icycles;  // idle cycles
	u_int32_t s_irqs;     // total interrupts
	u_int32_t s_exns;     // total exceptions
	u_int32_t s_rsects;   // disk sectors read
	u_int32_t s_wsects;   // disk sectors written
	u_int32_t s_rchars;   // console chars read
	u_int32_t s_wchars;   // console chars written
	//u_int32_t s_rpkts;    // network packets read
	//u_int32_t s_wpkts;    // network packets written
};

extern struct stats g_stats;
