/***************************************************************************
 *   Copyright (C) 2007 Ryan Schultz, PCSX-df Team, PCSX team              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02111-1307 USA.           *
 ***************************************************************************/

/*
* R3000A CPU functions.
*/

#include "r3000a.h"
#include "psxhw.h"
#include "psxdma.h"
#include "cdrom.h"
#include "mdec.h"
#include "psxinterpreter.h"
#include "Gamecube/wiiSXconfig.h"
#include "Gamecube/DEBUG.h"

R3000Acpu *psxCpu;
psxRegisters psxRegs;
extern bool needInitCpu;

int psxInit() {

	if (Config.Cpu == DYNACORE_INTERPRETER) {
		psxCpu = &psxInt;
	}
#if defined(__x86_64__) || defined(__i386__) || defined(__sh__) || defined(__ppc__) || defined(HW_RVL) || defined(HW_DOL)
	if (Config.Cpu == DYNACORE_DYNAREC)
	{
		psxCpu = &psxLightrec;
	}
	if (Config.Cpu == DYNACORE_DYNAREC_OLD)
	{
		psxCpu = &psxRec;
	}
#endif
	Log=0;

	int memInitResult = psxMemInit();
	if (memInitResult != 0) return memInitResult;

	if (needInitCpu)
	{
		return psxCpu->Init();
	}
	else
	{
		return 0;
	}
}

void psxReset() {
	bool introBypassed = FALSE;
	psxMemReset();

	memset(&psxRegs, 0, sizeof(psxRegs));

	psxRegs.pc = 0xbfc00000; // Start in bootstrap

	psxRegs.CP0.n.SR   = 0x10600000; // COP0 enabled | BEV = 1 | TS = 1
	psxRegs.CP0.n.PRid = 0x00000002; // PRevID = Revision ID, same as R3000A
	if (Config.HLE) {
		psxRegs.CP0.n.SR |= 1u << 30;    // COP2 enabled
		psxRegs.CP0.n.SR &= ~(1u << 22); // RAM exception vector
	}

	psxCpu->ApplyConfig();
	psxCpu->Reset();

	psxHwReset();
	psxBiosInit();

	// The automatic correction of pR3000Fix may have an impact on the execution of Bios,
	// so we will restore it first
	long tmpVal = Config.pR3000Fix;

	if (!Config.HLE) {
		psxExecuteBios();
		if (psxRegs.pc == 0x80030000 && LoadCdBios == BOOTTHRUBIOS_NO) {
			introBypassed = BiosBootBypass();
		}
	}
	if (Config.HLE || introBypassed)
		psxBiosSetupBootState();

    // Set the value of pR3000Fix after the completion of BIOS execution
    Config.pR3000Fix = tmpVal;

#ifdef EMU_LOG
	EMU_LOG("*BIOS END*\n");
#endif
	Log = 0;
}

void psxShutdown() {
	psxBiosShutdown();

	psxCpu->Shutdown();

	psxMemShutdown();
}

// cp0 is passed separately for lightrec to be less messy
void psxException(u32 cause, enum R3000Abdt bdt, psxCP0Regs *cp0) {
	u32 opcode = intFakeFetch(psxRegs.pc);

	if (unlikely(!Config.HLE && (opcode >> 25) == 0x25)) {
		// "hokuto no ken" / "Crash Bandicot 2" ...
		// BIOS does not allow to return to GTE instructions
		// (just skips it, supposedly because it's scheduled already)
		// so we execute it here
		psxCP2Regs *cp2 = (psxCP2Regs *)(cp0 + 1);
		psxRegs.code = opcode;
		extern void (*psxCP2[64])(struct psxCP2Regs *regs);
		psxCP2[opcode & 0x3f](cp2);
	}

	// Set the Cause
	cp0->n.Cause = (bdt << 30) | (cp0->n.Cause & 0x700) | cause;

	// Set the EPC & PC
	cp0->n.EPC = bdt ? psxRegs.pc - 4 : psxRegs.pc;

	if (cp0->n.SR & 0x400000)
		psxRegs.pc = 0xbfc00180;
	else
		psxRegs.pc = 0x80000080;

	// Set the SR
	cp0->n.SR = (cp0->n.SR & ~0x3f) | ((cp0->n.SR & 0x0f) << 2);
}

extern u32 psxNextCounter, psxNextsCounter;
extern void irq_test(psxCP0Regs *cp0);

void psxBranchTest() {

	if ((psxRegs.cycle - psxNextsCounter) >= psxNextCounter)
		psxRcntUpdate();

	irq_test(&psxRegs.CP0);
}

void psxJumpTest() {
	if (!Config.HLE && Config.PsxOut) {
		u32 call = psxRegs.GPR.n.t1 & 0xff;
		switch (psxRegs.pc & 0x1fffff) {
			case 0xa0:
#ifdef PSXBIOS_LOG
				if (call != 0x28 && call != 0xe) {
					PSXBIOS_LOG("Bios call a0: %s (%x) %x,%x,%x,%x\n", biosA0n[call], call, psxRegs.GPR.n.a0, psxRegs.GPR.n.a1, psxRegs.GPR.n.a2, psxRegs.GPR.n.a3); }
#endif
				if (biosA0[call])
					biosA0[call]();
				break;
			case 0xb0:
#ifdef PSXBIOS_LOG
				if (call != 0x17 && call != 0xb) {
					PSXBIOS_LOG("Bios call b0: %s (%x) %x,%x,%x,%x\n", biosB0n[call], call, psxRegs.GPR.n.a0, psxRegs.GPR.n.a1, psxRegs.GPR.n.a2, psxRegs.GPR.n.a3); }
#endif
				if (biosB0[call])
					biosB0[call]();
				break;
			case 0xc0:
#ifdef PSXBIOS_LOG
				PSXBIOS_LOG("Bios call c0: %s (%x) %x,%x,%x,%x\n", biosC0n[call], call, psxRegs.GPR.n.a0, psxRegs.GPR.n.a1, psxRegs.GPR.n.a2, psxRegs.GPR.n.a3);
#endif
				if (biosC0[call])
					biosC0[call]();
				break;
		}
	}
}

void psxExecuteBios() {
	int i;
	for (i = 0; i < 5000000; i++) {
		psxCpu->ExecuteBlock(EXEC_CALLER_BOOT);
		if ((psxRegs.pc & 0xff800000) == 0x80000000)
			break;
	}
	if (psxRegs.pc != 0x80030000)
		SysPrintf("non-standard BIOS detected (%d, %08x)\n", i, psxRegs.pc);
}


