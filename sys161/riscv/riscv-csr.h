#define CSR_USTATUS		0x000
#define CSR_UIE			0x004
#define CSR_UTVEC		0x005

#define CSR_USCRATCH		0x040
#define CSR_UEPC		0x041
#define CSR_UCAUSE		0x042
#define CSR_UTVAL		0x043
#define CSR_UIP			0x044

#define CSR_FFLAGS		0x001
#define CSR_FRM			0x002
#define CSR_FCSR		0x003

#define CSR_CYCLE		0xc00
#define CSR_TIME		0xc01
#define CSR_INSTRET		0xc02
#define CSR_HPMCOUNTER3		0xc03
/*      CSR_HPMCOUNTER4-30	0xc04-0xc1e */
#define CSR_HPMCOUNTER31	0xc1f
#define CSR_CYCLEH		0xc80
#define CSR_TIMEH		0xc81
#define CSR_INSTRETH		0xc82
#define CSR_HPMCOUNTER3H	0xc83
/*      CSR_HPMCOUNTER4H-30H	0xc84-0xc9e */
#define CSR_HPMCOUNTER31H	0xc9f

#define CSR_SSTATUS		0x100
#define CSR_SEDELEG		0x102
#define CSR_SIDELEG		0x103
#define CSR_SIE			0x104
#define CSR_STVEC		0x105
#define CSR_SCOUNTEREN		0x106

#define CSR_SSCRATCH		0x140
#define CSR_SEPC		0x141
#define CSR_SCAUSE		0x142
#define CSR_STVAL		0x143
#define CSR_SIP			0x144

#define CSR_SATP		0x180

#define CSR_MVENDORID		0xf11
#define CSR_MARCHID		0xf12
#define CSR_MIMPID		0xf13
#define CSR_MHARTID		0xf14

#define CSR_MSTATUS		0x300
#define CSR_MISA		0x301
#define CSR_MEDELEG		0x302
#define CSR_MIDELEG		0x303
#define CSR_MIE			0x304
#define CSR_MTVEC		0x305
#define CSR_MCOUNTEREN		0x306

#define CSR_MSCRATCH		0x340
#define CSR_MEPC		0x341
#define CSR_MCAUSE		0x342
#define CSR_MTVAL		0x343
#define CSR_MIP			0x344

#define CSR_PMPCFG0		0x3a0
#define CSR_PMPCFG1		0x3a1
#define CSR_PMPCFG2		0x3a2
#define CSR_PMPCFG3		0x3a3
#define CSR_PMPADDR0		0x3b0
/*      CSR_PMPADDR1-14		0x3b1-0x3be */
#define CSR_PMPADDR15		0x3bf

#define CSR_MCYCLE		0xb00
#define CSR_MINSTRET		0xb02
#define CSR_MHPMCOUNTER3	0xb03
/*      CSR_MHPMCOUNTER4-30	0xb04-0xb1e */
#define CSR_MHPMCOUNTER31	0xb1f
#define CSR_MCYCLEH		0xb80
#define CSR_MINSTRETH		0xb82
#define CSR_MPHMCOUNTER3H	0xb83
/*      CSR_MPHMCOUNTER4H-30H	0xb84-0xb9e */
#define CSR_MPHMCOUNTER31H	0xb9f

#define CSR_MCOUNTINHIBIT	0x320
#define CSR_MHPMEVENT3		0x323
/*      CSR_MHPMEVENT4-30	0x324-0x33e */
#define CSR_MHPMEVENT31		0x33f

#define CSR_TSELECT		0x7a0
#define CSR_TDATA1		0x7a1
#define CSR_TDATA2		0x7a2
#define CSR_TDATA3		0x7a3

#define CSR_DCSR		0x7b0
#define CSR_DPC			0x7b1
#define CSR_DSCRATCH0		0xb72
#define CSR_DSCRATCH1		0xb73

/* XXX should go away */
#define CSR_SYS161_TIMER	0x9c0
