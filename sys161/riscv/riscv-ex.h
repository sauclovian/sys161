/*
 * exceptions
 */

#define EX_IALIGN	0	/* instruction fetch alignment error */
#define EX_IACCESS	1	/* instruction fetch permission fault */
#define EX_ILLINST	2	/* illegal instruction */
#define EX_BREAKPOINT	3	/* breakpoint instruction */
#define EX_LALIGN	4	/* load alignment error */
#define EX_LACCESS	5	/* load permission fault */
#define EX_SALIGN	6	/* store alignment error */
#define EX_SACCESS	7	/* store permission fault */
#define EX_UCALL	8	/* ECALL from user to supervisor */
#define EX_SCALL	9	/* ECALL from supervisor to hypervisor*/
/* reserved     	10 */
/* reserved             11 */
#define EX_IPAGE	12	/* instruction fetch page fault */
#define EX_LPAGE	13	/* load page fault */
/* reserved             14 */
#define EX_SPAGE	15	/* store page fault */


/*
 * interrupts
 */

#define IRQ_USOFT	0
#define IRQ_SSOFT	1
/* reserved             2 */
/* reserved             3 */
#define IRQ_UTIMER	4
#define IRQ_STIMER	5
/* reserved             6 */
/* reserved             7 */
#define IRQ_UEXTERN	8
#define IRQ_SEXTERN	9
/* reserved             10-15 */
