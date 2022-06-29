/*
 * Opcodes in the primary opcode field.
 */

/*
 * (A) For 32-bit instructions
 * (shifted right by 2 to drop the size bits)
 */

#define OP32_LOAD	0
#define OP32_LOADFP	1
/* custom-0             2 */
#define OP32_MISCMEM	3
#define OP32_OPIMM	4
#define OP32_AUIPC	5
#define OP32_OPIMM32	6
/* larger instructions  7 */
#define OP32_STORE	8
#define OP32_STOREFP	9
/* custom-1             10 */
#define OP32_AMO	11
#define OP32_OP		12
#define OP32_LUI	13
#define OP32_OP32	14
/* larger instructions  15 */
#define OP32_MADD	16
#define OP32_MSUB	17
#define OP32_NMSUB	18
#define OP32_NMADD	19
#define OP32_OPFP	20
/* reserved             21 */
/* custom-2 / rv128     22 */
/* larger instructions  23 */
#define OP32_BRANCH	24
#define OP32_JALR	25
/* reserved             26 */
#define OP32_JAL	27
#define OP32_SYSTEM	28
/* reserved             29 */
/* custom-3 / rv128     30 */
/* larger instructions  31 */

/*
 * (B) for 16-bit instructions
 */

/*
 * Secondary opcodes in the 7-bit funct7 field (shifted right by 25)
 * for various primary opcodes.
 */

#define OPOP_ARITH	0
#define OPOP_MULDIV	1
#define OPOP_NARITH	32

#define OPFP_FADD_S	0
#define OPFP_FADD_D	1
#define OPFP_FADD_Q	3
#define OPFP_FSUB_S	4
#define OPFP_FSUB_D	5
#define OPFP_FSUB_Q	7
#define OPFP_FMUL_S	8
#define OPFP_FMUL_D	9
#define OPFP_FMUL_Q	11
#define OPFP_FDIV_S	12
#define OPFP_FDIV_D	13
#define OPFP_FDIV_Q	15
#define OPFP_FSGN_S	16
#define OPFP_FSGN_D	17
#define OPFP_FSGN_Q	19
#define OPFP_FMINMAX_S	20
#define OPFP_FMINMAX_D	21
#define OPFP_FMINMAX_Q	23
#define OPFP_FCVT_SD_SQ	32
#define OPFP_FCVT_DS_DQ	33
#define OPFP_FCVT_QS_QD	35
#define OPFP_FSQRT_S	44
#define OPFP_FSQRT_D	45
#define OPFP_FSQRT_Q	47
#define OPFP_FCVT_WS	96
#define OPFP_FCVT_WD	97
#define OPFP_FCVT_WQ	99
#define OPFP_FCMP_S	80
#define OPFP_FCMP_D	81
#define OPFP_FCMP_Q	83
#define OPFP_FCVT_SW	104
#define OPFP_FCVT_DW	105
#define OPFP_FCVT_QW	107
#define OPFP_FMV_XW_FCLASS_S 112
#define OPFP_FMV_XD_FCLASS_D 113
#define OPFP_FCLASS_Q	115
#define OPFP_FMV_WX	120
#define OPFP_FMV_DX	121

/*
 * Secondary opcodes in the top 5 bits of the 7-bit funct7 field
 * (shifted right by 27) for the AMO primary opcode. (The bottom two
 * bits are ACQUIRE and RELEASE flags.)
 */

#define OPAMO_AMOADD	0
#define OPAMO_AMOSWAP	1
#define OPAMO_LR	2
#define OPAMO_SC	3
#define OPAMO_AMOXOR	4
#define OPAMO_AMOAND	8
#define OPAMO_AMOOR	12
#define OPAMO_AMOMIN	16
#define OPAMO_AMOMAX	20
#define OPAMO_AMOMINU	24
#define OPAMO_AMOMAXU	28

/*
 * Secondary/tertiary opcodes in the 3-bit funct3 field (shifted right by 12)
 * for various primary or secondary opcodes.
 */

/* LOAD: primary == 0 */
#define OPLOAD_LB	0
#define OPLOAD_LH	1
#define OPLOAD_LW	2
/*      OPLOAD_LD       3  (RV64 only) */
#define OPLOAD_LBU	4
#define OPLOAD_LHU	5
/*      OPLOAD_LWU      6  (RV64 only) */

/* LOADFP: primary == 1 */
#define OPLOADFP_FLW	2
#define OPLOADFP_FLD	3
#define OPLOADFP_FLQ	4

/* MISCMEM: primary == 3 */
#define OPMISCMEM_FENCE	 0
#define OPMISCMEM_FENCEI 1

/* OPIMM: primary == 4 */
#define OPOPIMM_ADDI	0
#define OPOPIMM_SLI	1
#define OPOPIMM_SLTI	2
#define OPOPIMM_SLTIU	3
#define OPOPIMM_XORI	4
#define OPOPIMM_SRI	5
#define OPOPIMM_ORI	6
#define OPOPIMM_ANDI	7

/* OPIMM32: primary == 6 */
/*      OPOPIMM32_ADDIW	0  (RV64 only) */
/*	OPOPIMM32_SLIW	1  (RV64 only) */
/*	OPOPIMM32_SRIW	5  (RV64 only) */

/* STORE: primary == 8 */
#define OPSTORE_SB	0
#define OPSTORE_SH	1
#define OPSTORE_SW	2
/*      OPSTORE_SD      3  (RV64 only) */

/* STOREFP: primary == 9 */
#define OPSTOREFP_FSW	2
#define OPSTOREFP_FSD	3
#define OPSTOREFP_FSQ	4

/* AMO: primary == 11, funct7 == whatever */
#define OPAMO_32	2
/*      OPAMO_64	3  (RV64 only) */

/* ARITH: primary == 12 (OP), funct7 == 0 (ARITH) */
#define OPARITH_ADD	0
#define OPARITH_SLL	1
#define OPARITH_SLT	2
#define OPARITH_SLTU	3
#define OPARITH_XOR	4
#define OPARITH_SRL	5
#define OPARITH_OR	6
#define OPARITH_AND	7

/* NARITH: primary == 12 (OP), funct7 == 32 (NARITH) */
#define OPNARITH_SUB	0
#define OPNARITH_SRA	5

/* MULDIV: primary == 12 (OP), funct7 == 1 (MULDIV) */
#define OPMULDIV_MUL	0
#define OPMULDIV_MULH	1
#define OPMULDIV_MULHSU	2
#define OPMULDIV_MULHU	3
#define OPMULDIV_DIV	4
#define OPMULDIV_DIVU	5
#define OPMULDIV_REM	6
#define OPMULDIV_REMU	7

/* ARITHW: primary == 14 (OP32), funct7 == 0 (ARITH) */
/*	OPARITHW_ADDW	0  (RV64 only) */
/*	OPARITHW_SLLW	1  (RV64 only) */
/*	OPARITHW_SRLW	5  (RV64 only) */

/* NARITHW: primary == 14 (OP32), funct7 == 32 (NARITH) */
/*	OPNARITHW_SUBW	0  (RV64 only) */
/*	OPNARITHW_SRAW	5  (RV64 only) */

/* MULDIVW: primary == 14 (OP32), funct7 == 1 (MULDIV) */
/*      OPMULDIV_MULW	0  (RV64 only) */
/*      OPMULDIV_DIVW	4  (RV64 only) */
/*      OPMULDIV_DIVUW	5  (RV64 only) */
/*      OPMULDIV_REMW	6  (RV64 only) */
/*      OPMULDIV_REMUW	7  (RV64 only) */

/* BRANCH: primary == 24 (BRANCH) */
#define OPBRANCH_BEQ	0
#define OPBRANCH_BNE	1
#define OPBRANCH_BLT	4
#define OPBRANCH_BGE	5
#define OPBRANCH_BLTU	6
#define OPBRANCH_BGEU	7

/* SYSTEM: primary == 28 (SYSTEM) */
#define OPSYSTEM_PRIV	0
#define OPSYSTEM_CSRRW	1
#define OPSYSTEM_CSRRS	2
#define OPSYSTEM_CSRRC	3
#define OPSYSTEM_CSRRWI	5
#define OPSYSTEM_CSRRSI	6
#define OPSYSTEM_CSRRCI	7

/*
 * Tertiary opcodes in funct7 field (top 7 bits)
 */

/* PRIV: primary == 28 (SYSTEM), funct3 == 0 (PRIV) */
#define OPPRIV_USER	  0
#define OPPRIV_SYSTEM	  8
#define OPPRIV_SFENCE_VMA 9
#define OPPRIV_HFENCE_BVMA 17
#define OPPRIV_MACHINE	  24
#define OPPRIV_HFENCE_GVMA 81

/*
 * Quaternary opcodes in rs2 field (bits 20-24), shifted right by 20
 */

/* USER: primary == 28 (SYSTEM), funct3 == 0 (PRIV), funct7 == 0 (USER) */
#define OPUSER_ECALL	0
#define OPUSER_EBREAK	1
#define OPUSER_URET	2

/* SYSTEM: primary == 28 (SYSTEM), funct3 == 0 (PRIV), funct7 == 8 (SYSTEM) */
#define OPSYSTEM_SRET	2
#define OPSYSTEM_WFI	5

/* MACHINE: primary == 28 (SYSTEM), funct3 == 0 (PRIV),funct7 == 24 (MACHINE)*/
#define OPMACHINE_MRET	2
