/******************************************************************************
*  Vroom-DS Z80 CPU  (R800 / Turbo-R optimized ED table)
*
* Note: Most of this file is from the ColEm emulator core by Marat Fayzullin
*       but heavily modified for specific NDS use. If you want to use this
*       code, you are advised to seek out the much more portable fMSX core
*       and contact Marat.
*
******************************************************************************/

/** Z80: portable Z80 emulator *******************************/
/**                                                         **/
/**                         CodesED.h                       **/
/**                                                         **/
/** This file contains implementation for the ED table of   **/
/** Z80 commands. It is included from Z80.c.                **/
/**                                                         **/
/** Copyright (C) Marat Fayzullin 1994-2021                 **/
/**     You are not allowed to distribute this software     **/
/**     commercially. Please, notify me, if you make any    **/
/**     changes to this file.                               **/
/*************************************************************/

/** Optimization notes ***************************************/
/** - Repeat instructions (LDIR/LDDR/INIR/INDR/OTIR/OTDR/   **/
/**   CPIR/CPDR) no longer touch F on every iteration; flags **/
/**   are computed once, on the exit path. Exit values are   **/
/**   unchanged from the original.                           **/
/** - 16-bit immediate address fetch uses one PC update     **/
/**   instead of two (M_FETCHW below).                       **/
/** - Define R800_SIMPLE_IO_FLAGS to also drop the          **/
/**   undocumented INI/IND/OUTI/OUTD/OTIR/OTDR/INIR/INDR    **/
/**   flag work (N-from-data bit, HL.l+I carry). Off by      **/
/**   default because it changes visible F results.          **/
/*************************************************************/

/* Fetch a 16-bit immediate into J.W with a single PC update.
   (word) cast keeps the high-byte fetch wrapping at 0xFFFF. */
#undef  M_FETCHW
#define M_FETCHW(Jp) \
  Jp.W = (word)(OpZ80(CPU.PC.W) | (OpZ80((word)(CPU.PC.W+1)) << 8)); \
  CPU.PC.W += 2

/* I/O block flag helpers. Z is the Z-flag term to merge in. */
#undef  IO_IN_FLAGS
#undef  IO_OUT_FLAGS
#ifdef R800_SIMPLE_IO_FLAGS
  #define IO_IN_FLAGS(Z)   ((CPU.AF.B.l&C_FLAG)|N_FLAG|(Z))
  #define IO_OUT_FLAGS(Z)  ((CPU.AF.B.l&C_FLAG)|N_FLAG|(Z))
#else
  #define IO_IN_FLAGS(Z)   ((I&0x80 ? N_FLAG:0)|(Z))
  #define IO_OUT_FLAGS(Z)  ((CPU.AF.B.l&S_FLAG)|(I&0x80 ? N_FLAG:0)|(Z)| \
                            (CPU.HL.B.l+I>255 ? (C_FLAG|H_FLAG):0))
#endif

/** This is a special patch for emulating BIOS calls: ********/
case DB_FE:     PatchZ80(&CPU);break;
/*************************************************************/

case ADC_HL_BC: M_ADCW(BC);break;
case ADC_HL_DE: M_ADCW(DE);break;
case ADC_HL_HL: M_ADCW(HL);break;
case ADC_HL_SP: M_ADCW(SP);break;

case SBC_HL_BC: M_SBCW(BC);break;
case SBC_HL_DE: M_SBCW(DE);break;
case SBC_HL_HL: M_SBCW(HL);break;
case SBC_HL_SP: M_SBCW(SP);break;

case LD_xWORDe_HL:
  M_FETCHW(J);
  WrZ80(J.W++,CPU.HL.B.l);
  WrZ80(J.W,CPU.HL.B.h);
  break;
case LD_xWORDe_DE:
  M_FETCHW(J);
  WrZ80(J.W++,CPU.DE.B.l);
  WrZ80(J.W,CPU.DE.B.h);
  break;
case LD_xWORDe_BC:
  M_FETCHW(J);
  WrZ80(J.W++,CPU.BC.B.l);
  WrZ80(J.W,CPU.BC.B.h);
  break;
case LD_xWORDe_SP:
  M_FETCHW(J);
  WrZ80(J.W++,CPU.SP.B.l);
  WrZ80(J.W,CPU.SP.B.h);
  break;

case LD_HL_xWORDe:
  M_FETCHW(J);
  CPU.HL.B.l=RdZ80(J.W++);
  CPU.HL.B.h=RdZ80(J.W);
  break;
case LD_DE_xWORDe:
  M_FETCHW(J);
  CPU.DE.B.l=RdZ80(J.W++);
  CPU.DE.B.h=RdZ80(J.W);
  break;
case LD_BC_xWORDe:
  M_FETCHW(J);
  CPU.BC.B.l=RdZ80(J.W++);
  CPU.BC.B.h=RdZ80(J.W);
  break;
case LD_SP_xWORDe:
  M_FETCHW(J);
  CPU.SP.B.l=RdZ80(J.W++);
  CPU.SP.B.h=RdZ80(J.W);
  break;

case RRD:
  I=RdZ80(CPU.HL.W);
  J.B.l=(I>>4)|(CPU.AF.B.h<<4);
  WrZ80(CPU.HL.W,J.B.l);
  CPU.AF.B.h=(I&0x0F)|(CPU.AF.B.h&0xF0);
  CPU.AF.B.l=PZSTable[CPU.AF.B.h]|(CPU.AF.B.l&C_FLAG);
  break;
case RLD:
  I=RdZ80(CPU.HL.W);
  J.B.l=(I<<4)|(CPU.AF.B.h&0x0F);
  WrZ80(CPU.HL.W,J.B.l);
  CPU.AF.B.h=(I>>4)|(CPU.AF.B.h&0xF0);
  CPU.AF.B.l=PZSTable[CPU.AF.B.h]|(CPU.AF.B.l&C_FLAG);
  break;

case LD_A_I:
  CPU.AF.B.h=CPU.I;
  CPU.AF.B.l=(CPU.AF.B.l&C_FLAG)|(CPU.IFF&IFF_2? P_FLAG:0)|ZSTable[CPU.AF.B.h];
  break;

case LD_A_R:
  CPU.R = (CPU.R & 0x80) + (CPU.ICount & 0x7F); CPU.AF.B.h=(CPU.R&0x7F) | CPU.R_HighBit;  // The R is a 7-bit refresh counter with a 'secret' flag at the high bit that a few odd games take advantage of
  CPU.AF.B.l=(CPU.AF.B.l&C_FLAG)|(CPU.IFF&IFF_2? P_FLAG:0)|ZSTable[CPU.AF.B.h];
  break;

case LD_I_A:   CPU.I=CPU.AF.B.h;break;
case LD_R_A:   CPU.R=CPU.AF.B.h;CPU.R_HighBit = (CPU.R & 0x80); break;

case IM_0:     CPU.IFF&=~(IFF_IM1|IFF_IM2);break;
case IM_1:     CPU.IFF=(CPU.IFF&~IFF_IM2)|IFF_IM1;break;
case IM_2:     CPU.IFF=(CPU.IFF&~IFF_IM1)|IFF_IM2;break;

case RETI:
case RETN:     if(CPU.IFF&IFF_2) CPU.IFF|=IFF_1; else CPU.IFF&=~IFF_1;
               M_RET;break;

case NEG:      I=CPU.AF.B.h;CPU.AF.B.h=0;M_SUB(I);break;

case IN_B_xC:  M_IN(CPU.BC.B.h);break;
case IN_C_xC:  M_IN(CPU.BC.B.l);break;
case IN_D_xC:  M_IN(CPU.DE.B.h);break;
case IN_E_xC:  M_IN(CPU.DE.B.l);break;
case IN_H_xC:  M_IN(CPU.HL.B.h);break;
case IN_L_xC:  M_IN(CPU.HL.B.l);break;
case IN_A_xC:  M_IN(CPU.AF.B.h);break;
case IN_F_xC:  M_IN(J.B.l);break;

case OUT_xC_B: OutZ80(CPU.BC.W,CPU.BC.B.h);break;
case OUT_xC_C: OutZ80(CPU.BC.W,CPU.BC.B.l);break;
case OUT_xC_D: OutZ80(CPU.BC.W,CPU.DE.B.h);break;
case OUT_xC_E: OutZ80(CPU.BC.W,CPU.DE.B.l);break;
case OUT_xC_H: OutZ80(CPU.BC.W,CPU.HL.B.h);break;
case OUT_xC_L: OutZ80(CPU.BC.W,CPU.HL.B.l);break;
case OUT_xC_A: OutZ80(CPU.BC.W,CPU.AF.B.h);break;
case OUT_xC_F: OutZ80(CPU.BC.W,0);break;

/** Block I/O: loop iterations leave F alone, exit path sets it **/
case INI:
  I = InZ80(CPU.BC.W);
  WrZ80(CPU.HL.W++,I);
  --CPU.BC.B.h;
  CPU.AF.B.l=IO_IN_FLAGS(CPU.BC.B.h? 0:Z_FLAG);
  break;

case INIR:
  I = InZ80(CPU.BC.W);
  WrZ80(CPU.HL.W++,I);
  if(--CPU.BC.B.h) CPU.PC.W-=2;
  else { CPU.AF.B.l=IO_IN_FLAGS(Z_FLAG); CPU.ICount+=CYCLES_REPEAT_EXIT; }
  break;

case IND:
  I = InZ80(CPU.BC.W);
  WrZ80(CPU.HL.W--,I);
  --CPU.BC.B.h;
  CPU.AF.B.l=IO_IN_FLAGS(CPU.BC.B.h? 0:Z_FLAG);
  break;

case INDR:
  I = InZ80(CPU.BC.W);
  WrZ80(CPU.HL.W--,I);
  if(--CPU.BC.B.h) CPU.PC.W-=2;
  else { CPU.AF.B.l=IO_IN_FLAGS(Z_FLAG); CPU.ICount+=CYCLES_REPEAT_EXIT; }
  break;

case OUTI:
  --CPU.BC.B.h;
  I=RdZ80(CPU.HL.W++);
  OutZ80(CPU.BC.W,I);
  CPU.AF.B.l=IO_OUT_FLAGS(CPU.BC.B.h? 0:Z_FLAG);
  break;

case OTIR:
  --CPU.BC.B.h;
  I=RdZ80(CPU.HL.W++);
  OutZ80(CPU.BC.W,I);
  if(CPU.BC.B.h) CPU.PC.W-=2;
  else { CPU.AF.B.l=IO_OUT_FLAGS(Z_FLAG); CPU.ICount+=CYCLES_REPEAT_EXIT; }
  break;

case OUTD:
  --CPU.BC.B.h;
  I=RdZ80(CPU.HL.W--);
  OutZ80(CPU.BC.W,I);
  CPU.AF.B.l=IO_OUT_FLAGS(CPU.BC.B.h? 0:Z_FLAG);
  break;

case OTDR:
  --CPU.BC.B.h;
  I=RdZ80(CPU.HL.W--);
  OutZ80(CPU.BC.W,I);
  if(CPU.BC.B.h) CPU.PC.W-=2;
  else { CPU.AF.B.l=IO_OUT_FLAGS(Z_FLAG); CPU.ICount+=CYCLES_REPEAT_EXIT; }
  break;

/** Block transfer ********************************************/
case LDI:
  WrZ80(CPU.DE.W++,RdZ80(CPU.HL.W++));
  --CPU.BC.W;
  CPU.AF.B.l=(CPU.AF.B.l&~(N_FLAG|H_FLAG|P_FLAG))|(CPU.BC.W? P_FLAG:0);
  break;

case LDIR:
  WrZ80(CPU.DE.W++,RdZ80(CPU.HL.W++));
  if(--CPU.BC.W) CPU.PC.W-=2;
  else { CPU.AF.B.l&=~(N_FLAG|H_FLAG|P_FLAG); CPU.ICount+=CYCLES_BLOCK_EXIT; }
  break;

case LDD:
  WrZ80(CPU.DE.W--,RdZ80(CPU.HL.W--));
  --CPU.BC.W;
  CPU.AF.B.l=(CPU.AF.B.l&~(N_FLAG|H_FLAG|P_FLAG))|(CPU.BC.W? P_FLAG:0);
  break;

case LDDR:
  WrZ80(CPU.DE.W--,RdZ80(CPU.HL.W--));
  if(--CPU.BC.W) CPU.PC.W-=2;
  else { CPU.AF.B.l&=~(N_FLAG|H_FLAG|P_FLAG); CPU.ICount+=CYCLES_BLOCK_EXIT; }
  break;

/** Block compare: flags computed only when the loop exits ****/
case CPI:
  I=RdZ80(CPU.HL.W++);
  J.B.l=CPU.AF.B.h-I;
  --CPU.BC.W;
  CPU.AF.B.l =
    N_FLAG|(CPU.AF.B.l&C_FLAG)|ZSTable[J.B.l]|
    ((CPU.AF.B.h^I^J.B.l)&H_FLAG)|(CPU.BC.W? P_FLAG:0);
  break;

case CPIR:
  I=RdZ80(CPU.HL.W++);
  J.B.l=CPU.AF.B.h-I;
  if(--CPU.BC.W && J.B.l) CPU.PC.W-=2;
  else
  {
    CPU.AF.B.l =
      N_FLAG|(CPU.AF.B.l&C_FLAG)|ZSTable[J.B.l]|
      ((CPU.AF.B.h^I^J.B.l)&H_FLAG)|(CPU.BC.W? P_FLAG:0);
    CPU.ICount+=CYCLES_REPEAT_EXIT;
  }
  break;

case CPD:
  I=RdZ80(CPU.HL.W--);
  J.B.l=CPU.AF.B.h-I;
  --CPU.BC.W;
  CPU.AF.B.l =
    N_FLAG|(CPU.AF.B.l&C_FLAG)|ZSTable[J.B.l]|
    ((CPU.AF.B.h^I^J.B.l)&H_FLAG)|(CPU.BC.W? P_FLAG:0);
  break;

case CPDR:
  I=RdZ80(CPU.HL.W--);
  J.B.l=CPU.AF.B.h-I;
  if(--CPU.BC.W && J.B.l) CPU.PC.W-=2;
  else
  {
    CPU.AF.B.l =
      N_FLAG|(CPU.AF.B.l&C_FLAG)|ZSTable[J.B.l]|
      ((CPU.AF.B.h^I^J.B.l)&H_FLAG)|(CPU.BC.W? P_FLAG:0);
    CPU.ICount+=CYCLES_REPEAT_EXIT;
  }
  break;

/** R800 multiply instructions (MSX Turbo R) *****************/
/** MULUB A,r : HL = A * r        (8x8  -> 16 bit)          **/
/** MULUW HL,rr: DE:HL = HL * rr  (16x16 -> 32 bit)         **/
case MULUB_A_B: M_MULUB(CPU.BC.B.h);break;
case MULUB_A_C: M_MULUB(CPU.BC.B.l);break;
case MULUB_A_D: M_MULUB(CPU.DE.B.h);break;
case MULUB_A_E: M_MULUB(CPU.DE.B.l);break;

case MULUW_HL_BC: M_MULUW(CPU.BC.W);break;
case MULUW_HL_SP: M_MULUW(CPU.SP.W);break;
