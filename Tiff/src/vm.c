#include <stdio.h>
#include <stdlib.h>
#include "config.h"
#include "vm.h"
#include <string.h>

#define IMM    (IR & ~(-1<<slot))

/// Virtual Machine for 32-bit MachineForth.

/// The VM registers are defined here but generally not accessible outside this
/// module except through VMstep. The VM could be at the end of a cable, so we
/// don't want direct access to its innards.

/// These functions are always exported: VMpor, VMstep, SetDbgReg, GetDbgReg.
/// If TRACEABLE, you get more exported: UnTrace, VMreg[],
/// while importing the Trace function. This offers a way to break the
/// "no direct access" rule in a PC environment, for testing and VM debugging.

/// Flash writing is handled by streaming data to AXI space, through VMstep and
/// friends. ROM writing uses a WriteROM function exported to Tiff but not used
/// in a target system.

/// The optional Trace function tracks state changes using these parameters:
/// Type of state change: 0 = unmarked, 1 = new opcode, 2 or 3 = new group;
/// Register ID: Complement of register number if register, memory if other;
/// Old value: 32-bit.
/// New value: 32-bit.

#ifdef TRACEABLE
    #define T VMreg[0]
    #define N VMreg[1]
    #define R VMreg[2]
    #define A VMreg[3]
    #define B VMreg[4]
    #define RP VMreg[5]
    #define SP VMreg[6]
    #define UP VMreg[7]
    #define PC VMreg[8]
    #define DebugReg VMreg[9]
    #define RidT  (-1)
    #define RidN  (-2)
    #define RidR  (-3)
    #define RidA  (-4)
    #define RidB  (-5)
    #define RidRP (-6)
    #define RidSP (-7)
    #define RidUP (-8)
    #define RidPC (-9)
    #define VMregs 10

    uint32_t VMreg[VMregs];     // registers with undo capability
    uint32_t OpCounter[64];     // opcode counter
    unsigned long cyclecount;

    static int New; // New trace type, used to mark new sections of trace
    static uint32_t RAM[RAMsize];
    static uint32_t ROM[ROMsize];
    static uint32_t AXI[AXIsize];

    static void SDUP(void)  {
        Trace(New,RidSP,SP,SP-1); New=0;
                     --SP;
        Trace(0,SP & (RAMsize-1),RAM[SP & (RAMsize-1)],  N);
                                 RAM[SP & (RAMsize-1)] = N;
        Trace(0, RidN, N,  T);
                       N = T; }
    static void SDROP(void) {
        Trace(New,RidT,T,  N); New=0;
                       T = N;
        Trace(0, RidN, N,  RAM[SP & (RAMsize-1)]);
                       N = RAM[SP & (RAMsize-1)];
        Trace(0,RidSP,SP,SP+1);
                   SP++; }
    static void SNIP(void)  {
        Trace(New,RidN,N,  RAM[SP & (RAMsize-1)]);  New=0;
                       N = RAM[SP & (RAMsize-1)];
        Trace(0,RidSP, SP,SP+1);
                       SP++; }
    static void RDUP(void)  {
        Trace(New,RidRP,RP,RP-1); New=0;
                       --RP;
        Trace(0,RP & (RAMsize-1),RAM[RP & (RAMsize-1)],  R);
                                 RAM[RP & (RAMsize-1)] = R; }
    static void RDROP(void) {
        Trace(New,RidR,R,  RAM[RP & (RAMsize-1)]);  New=0;
                       R = RAM[RP & (RAMsize-1)];
        Trace(0,RidRP, RP,RP+1);
                       RP++; }

#else
    static uint32_t T;	 static uint32_t RP = 64;
    static uint32_t N;	 static uint32_t SP = 32;
    static uint32_t R;	 static uint32_t UP = 64;
    static uint32_t A;   static uint32_t DebugReg;
    static uint32_t B;	 static uint32_t PC;

    static uint32_t RAM[RAMsize];
    static uint32_t ROM[ROMsize];
    static uint32_t AXI[AXIsize];

    static void SDUP(void)  { RAM[--SP & (RAMsize-1)] = N;  N = T; }
    static void SDROP(void) { T = N;  N = RAM[SP++ & (RAMsize-1)]; }
    static void SNIP(void)  { N = RAM[SP++ & (RAMsize-1)]; }
    static void RDUP(void)  { RAM[--RP & (RAMsize-1)] = R; }
    static void RDROP(void) { R = RAM[RP++ & (RAMsize-1)]; }

#endif // TRACEABLE

/// Tiff's ROM write functions for populating internal ROM.
/// A copy may be stored to SPI flash for targets that boot from SPI.
/// An MCU-based system will have ROM in actual ROM.

int WriteROM(uint32_t data, uint32_t address) { // EXPORTED
    uint32_t addr = address / 4;
    if (address & 3) return -23;        // alignment problem
    if (addr >= ROMsize) return -9;     // out of range
    ROM[addr] = data;
    return 0;
}

int EraseAXI4K(uint32_t address) { // EXPORTED
    uint32_t addr = address / 4;
    int i;
    if (address & 3) return -23;        // alignment problem
    if (addr >= (AXIsize-1024)) return -9;   // out of range
    for (i=0; i<1024; i++) {            // erase 4KB sector
        AXI[addr+i] = 0xFFFFFFFF;
    }
    return 0;
}

/// The VM's RAM and ROM are internal to this module.
/// They are both little endian regardless of the target machine.
/// RAM and ROM are accessed only through execution of an instruction
/// group. Writing to ROM is a special case, depending on a USER function
/// with the restriction that you can't turn '0's into '1's.


// Send a stream of RAM words to the AXI bus.
// The only thing on the AXI bus here is SPI flash.
// An external function could be added in the future for other stuff.
// dest is a cell address, length is 0 to 255 meaning 1 to 256 words.
static void SendAXI(unsigned int dest, unsigned int length) {
    int32_t src = (A / 4) - ROMsize;
    uint32_t old, data;     int i;
    if (src < 0) goto bogus;            // below RAM address
    if (src >= (RAMsize-length)) goto bogus;
    if (dest >= (AXIsize-length)) goto bogus;
    for (i=0; i<=length; i++) {
        old = AXI[dest];		        // existing flash data
        data = RAM[src++];
        if (~(old|data)) {
            tiffIOR = -60;              // not erased
            return;
        }
        AXI[dest++] = old & data;
    } return;
bogus: tiffIOR = -9;                    // out of range
}

// Receive a stream of RAM words from the AXI bus.
// The only thing on the AXI bus here is SPI flash.
// An external function could be added in the future for other stuff.
// src is a cell address, length is 0 to 255 meaning 1 to 256 words.
static void ReceiveAXI(unsigned int src, unsigned int length) {
    int32_t dest = (A / 4) - ROMsize;
    if (dest < 0) goto bogus;            // below RAM address
    if (dest >= (RAMsize-length)) goto bogus;
    if (src >= (AXIsize-length)) goto bogus;
    memmove(&RAM[src], &AXI[dest], length+1);
    return;
bogus: tiffIOR = -9;                    // out of range
}

// Generic fetch from ROM or RAM: ROM is at the bottom, RAM wraps.
static void FetchX (int32_t addr, int shift, int mask) {
    SDUP();                     // push memory location onto stack
#ifdef TRACEABLE
    int32_t temp;
    if (addr < ROMsize) {
        temp = (ROM[addr] >> shift) & mask;
    } else {
        if (addr < (ROMsize + RAMsize)) {
            temp = (RAM[addr-ROMsize] >> shift) & mask;
        } else if (addr < AXIsize) {
            temp = (AXI[addr] >> shift) & mask;
        } else temp = 0;
    }
    Trace(0, RidT, T, temp);
    T = temp;
#else
    if (addr < ROMsize) {
        T = (ROM[addr] >> shift) & mask;
    } else {
        if (addr < (ROMsize + RAMsize)) {
            T = (RAM[addr-ROMsize] >> shift) & mask;
        } else if (addr < AXIsize) {
            T = (AXI[addr] >> shift) & mask;
        } else T = 0;
    }
#endif // TRACEABLE
}

// Generic store to RAM: ROM is at the bottom, RAM wraps.
static void StoreX (uint32_t addr, uint32_t data, int shift, int mask) {
    uint32_t temp;
    addr = addr & (RAMsize-1);
    temp = RAM[addr] & (~(mask << shift));
#ifdef TRACEABLE
    temp = ((data & mask) << shift) | temp;
    Trace(New, addr, RAM[addr], temp);  New=0;
    RAM[addr] = temp;
#else
    RAM[addr] = ((data & mask) << shift) | temp;
#endif // TRACEABLE
    SDROP();
}

#ifdef TRACEABLE
    // Untrace undoes a state change by restoring old data
    void UnTrace(int32_t ID, uint32_t old) {  // EXPORTED
        int idx = ~ID;
        if (ID<0) {
            if (idx < VMregs) {
                VMreg[idx] = old;
            }
        } else {
            StoreX(ID, old, 0, 0xFFFFFFFF);
        }
    }
#endif // TRACEABLE

////////////////////////////////////////////////////////////////////////////////
/// Access to the VM is through four functions:
///    VMstep       // Execute an instruction group
///    VMpor        // Power-on reset
///    SetDbgReg    // write to the debug mailbox
///    GetDbgReg    // read from the debug mailbox
/// IR is the instruction group.
/// Paused is 0 when PC post-increments, other when not.

void VMpor(void) {  // EXPORTED
#ifdef TRACEABLE
    memset(OpCounter, 0, 64);           // clear opcode profile counters
    cyclecount = 0;                     // cycles since POR
#endif // TRACEABLE
    PC = 0;  RP = 64;  SP = 32;  UP = 64;
    T=0;  N=0;  R=0;  A=0;  B=0;  DebugReg = 0;
    memset(RAM, 0, RAMsize);            // clear RAM
}

uint32_t VMstep(uint32_t IR, int Paused) {  // EXPORTED
	uint32_t M;  int slot;
	unsigned int opcode, addr;
// The PC is incremented at the same time the IR is loaded. Slot0 is next clock.
// The instruction group returned from memory will be latched in after the final
// slot executes. In the VM, that is simulated by a return from this function.
// Some slots may alter the PC. The last slot may alter the PC, so if it modifies
// the PC extra cycles are taken after the instruction group to resolve the
// instruction flow. If the PC has been steady long enough for the instruction
// to show up, it's latched into IR. Otherwise, there will be some delay while
// memory returns the instruction.

    if (!Paused) {
#ifdef TRACEABLE
        Trace(3, RidPC, PC, PC + 1);
#endif // TRACEABLE
        PC = PC + 1;
    }

	slot = 26;
	while (slot>=0) {
		opcode = (IR >> slot) & 0x3F;	// slot = 26, 20, 14, 8, 2
#ifdef TRACEABLE
LastOp: if (OpCounter[opcode] != 0xFFFFFFFF) OpCounter[opcode]++;
        New = 1;  // first state change in an opcode
        if (!Paused) cyclecount += 1;
#else
LastOp:
#endif // TRACEABLE
        switch (opcode) {
			case 000:									break;	// NOP
			case 001: SDUP();							break;	// DUP
			case 002:
#ifdef TRACEABLE
                Trace(New, RidPC, PC, R>>2);  New=0;
                if (!Paused) cyclecount += 3;  // PC change flushes pipeline
#endif // TRACEABLE
                // PC is a cell address. The return stack works in bytes.
                PC = R>>2;  RDROP();  slot = 0;         break;	// ;
			case 003:
#ifdef TRACEABLE
                Trace(New, RidT, T, T + N);  New=0;
#endif // TRACEABLE
                T = T + N;  SNIP();	                    break;	// +
			case 004: slot = 0;						    break;	// NO:
			case 005: SDUP();
#ifdef TRACEABLE
                Trace(New, RidT, T, R);  New=0;
#endif // TRACEABLE
                T = R;					                break;	// R@
			case 006:
#ifdef TRACEABLE
                Trace(New, RidPC, PC, R);  New=0;
                if (!Paused) cyclecount += 3;  // PC change flushes pipeline
#endif // TRACEABLE
                PC = R>>2;	 RDROP();      			    break;	// ;|
			case 007:
#ifdef TRACEABLE
                Trace(New, RidT, T, T & N);  New=0;
#endif // TRACEABLE
                T = T & N;  SNIP();	                    break;	// AND
			case 010: if (T!=0) slot = 0;				break;	// NIF:
			case 011: M = N;  SDUP();
#ifdef TRACEABLE
                Trace(0, RidT, T, M);
#endif // TRACEABLE
                T = M;				                    break;	// OVER
			case 012: SDUP();
#ifdef TRACEABLE
                Trace(0, RidT, T, R);
#endif // TRACEABLE
			    T = R;  RDROP();				        break;	// R>
			case 013:
#ifdef TRACEABLE
                Trace(New, RidT, T, T ^ N);  New=0;
#endif // TRACEABLE
                T = T ^ N;  SNIP();	                    break;	// XOR
			case 014: if (T==0) slot = 0;				break;	// IF|
			case 015: SDUP();
#ifdef TRACEABLE
                Trace(0, RidT, T, A);
#endif // TRACEABLE
                T = A;						            break;	// A
			case 016: RDROP();				            break;	// RDROP

			case 020: if (T & 0x80000000) slot = 0;		break;	// +IF:
			case 021: M = N & 0xFF;
                SendAXI(T/4, M);    // T is address, N is length-1
#ifdef TRACEABLE
                Trace(New, RidA, A, A + 4 * (M + 1));  New=0;
                Trace(0, RidA, T, T + 4 * (M + 1));
#endif // TRACEABLE
				A += 4 * (M + 1);
				T += 4 * (M + 1);			    		break;	// !AS
			case 022: FetchX(A>>2, 0, 0xFFFFFFFF); 		break;  // @A
			case 023: 									break;
			case 024: if ((T&0x80000000)==0) slot = 0;  break;  // -IF:
			case 025:
#ifdef TRACEABLE
                Trace(New, RidT, T, T*2);  New=0;
#endif // TRACEABLE
                T = T*2;	        				    break;	// 2*
			case 026: FetchX(A>>2, 0, 0xFFFFFFFF);
#ifdef TRACEABLE
                Trace(0, RidA, A, A+4);
#endif // TRACEABLE
			    A += 4;                                 break;  // @A+

			case 030: if (R & 0x10000) slot = 0;
#ifdef TRACEABLE
                Trace(New, RidR, R, R-1);  New=0;
#endif // TRACEABLE
                R--;		                            break;	// NEXT
			case 031:
#ifdef TRACEABLE
                Trace(New, RidT, T, (unsigned) T / 2);  New=0;
#endif // TRACEABLE
			    T = T / 2;                              break;	// U2/
			case 032: FetchX(A>>2, (A&2) * 8, 0xFFFF);  break;  // W@A
			case 033:
#ifdef TRACEABLE
                Trace(New, RidA, A, T);  New=0;
#endif // TRACEABLE
			    A = T;	SDROP();		    			break;	// A!
			case 034: if ((R&0x10000)==0) slot = 26;
#ifdef TRACEABLE
                Trace(New, RidR, R, R-1);  New=0;
#endif // TRACEABLE
                R--;		                            break;	// REPT
			case 035:
#ifdef TRACEABLE
                Trace(New, RidT, T, T / 2);  New=0;
#endif // TRACEABLE
			    T = (signed)T / 2;                      break;	// 2/
			case 036: FetchX(A>>2, (A&3) * 8, 0xFF);    break;  // C@A
			case 037:
#ifdef TRACEABLE
                Trace(New, RidB, B, T);  New=0;
#endif // TRACEABLE
			    B = T;	SDROP();		    			break;	// B!

			case 040: M = (IMM + SP + ROMsize)*4;               // SP
GetPointer:
#ifdef TRACEABLE
                Trace(New, RidA, A, M);  New=0;
#endif // TRACEABLE
			    A = M;  return PC;
			case 041:
#ifdef TRACEABLE
                Trace(New, RidT, T, ~T);  New=0;
#endif // TRACEABLE
			    T = ~T;                                 break;	// COM
			case 042: StoreX(A>>2, T, 0, 0xFFFFFFFF);   break;  // !A
			case 043:
#ifdef TRACEABLE
                Trace(New, RidRP, RP, T>>2);  New=0;
#endif // TRACEABLE
			    RP = (uint8_t) (T>>2);  SDROP();    	break;	// RP!
			case 044: M = (IMM + RP + ROMsize)*4;               // RP
                goto GetPointer;
			case 045: M = T; T=DebugReg; DebugReg=M;	break;	// PORT
			case 046: StoreX(B>>2, T, 0, 0xFFFFFFFF); 		    // !B+
#ifdef TRACEABLE
                Trace(0, RidB, B, B + 4);
#endif // TRACEABLE
				B += 4;                                 break;
			case 047:
#ifdef TRACEABLE
                Trace(New, RidSP, SP, T>>2);  New=0;
#endif // TRACEABLE
                // SP! does not post-drop
			    SP = (uint8_t) (T>>2);          	    break;	// SP!

			case 050: M = (IMM + UP + ROMsize)*4;  	            // UP
                goto GetPointer;

			case 052: StoreX(A>>2, T, (A&2)*8, 0xFFFF); break;  // W!A
			case 053:
#ifdef TRACEABLE
                Trace(New, RidUP, UP, T>>2);  New=0;
#endif // TRACEABLE
			    UP = (uint8_t) (T>>2);  SDROP();	    break;	// UP!
            case 054:
#ifdef TRACEABLE
                Trace(New, RidT, T, (T<<24) | (IMM & 0xFFFFFF));  New=0;
#endif // TRACEABLE
                T = (T<<24) | (IMM & 0xFFFFFF);
                return PC;                                      // SH24
			case 056: StoreX(A>>2, T, (A&3)*8, 0xFF);   break;  // C!A

			case 060: M = UserFunction (T, N, IMM);             // USER
#ifdef TRACEABLE
                Trace(New, RidT, T, M);  New=0;
#endif // TRACEABLE
                T = M;  return PC;

			case 063: SNIP();							break;	// NIP
			case 064:
#ifdef TRACEABLE
                Trace(New, ~9, PC, IMM);  New=0;
                if (!Paused) cyclecount += 3;  // PC change flushes pipeline
#endif // TRACEABLE
                // Jumps and calls use cell addressing
			    PC = IMM;  return PC;                           // JUMP

			case 066: M = N & 0xFF;
                ReceiveAXI(T/4, M);
#ifdef TRACEABLE
                Trace(New, RidA, A, A + 4 * (M + 1));  New=0;
                Trace(0, RidA, T, T + 4 * (M + 1));
#endif // TRACEABLE
				A += 4 * (M + 1);
				T += 4 * (M + 1);			    		break;	// @AS
			case 070: SDUP();
#ifdef TRACEABLE
                Trace(0, RidT, T, IMM);
#endif // TRACEABLE
                T = IMM;  return PC;                            // LIT

			case 072: SDROP();					        break;	// DROP
			case 073: addr = SP & (RAMsize-1);  M = RAM[addr];  // ROT
#ifdef TRACEABLE
                Trace(New, addr, RAM[addr], N);  RAM[addr]=N;
                Trace(0, RidN, N, T);    N = T;
                Trace(0, RidT, T, M);    T = M;  New=0; break;
#else
                RAM[addr]=N;  N=T;  T=M;  break;
#endif // TRACEABLE
			case 074: RDUP();   slot=-1;                	    // CALL
#ifdef TRACEABLE
                Trace(0, RidR, R, PC);    R = PC;
                Trace(0, RidPC, PC, IMM);  PC = IMM;  return PC;
                if (!Paused) cyclecount += 3;  // PC change flushes pipeline
#else
                R = PC<<2;  PC = IMM;  return PC;
#endif // TRACEABLE
			case 075:
#ifdef TRACEABLE
                Trace(New, RidT, T, T + 1);  New=0;
#endif // TRACEABLE
			    T = T + 1;                              break;	// 1+
			case 076: RDUP();	                                // R>
#ifdef TRACEABLE
                Trace(0, RidR, R, T);
#endif // TRACEABLE
                R = T;
                SDROP();      	                        break;
			case 077: M = N;  	                                // SWAP
#ifdef TRACEABLE
                Trace(New, RidN, N, T);  N = T;  New=0;
                Trace(0, RidT, T, M);    T = M;         break;
#else
                N = T;  T = M;  break;
#endif // TRACEABLE
			default:                           		    break;	//
		}
		slot -= 6;
	}
    if (slot == -4) {
        opcode = IR & 3;
        goto LastOp;
    }
    return PC;
}

// write to the debug mailbox
void SetDbgReg(uint32_t n) {  // EXPORTED
    DebugReg = n;
}

// read from the debug mailbox
uint32_t GetDbgReg(void) {  // EXPORTED
    return DebugReg;
}
