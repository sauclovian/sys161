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
