#include <stdio.h>
#include <stdint.h>
#include "config.h"

uint32_t DbgPC; // last PC returned by VMstep

uint32_t DbgGroup (uint32_t op0, uint32_t op1,
                   uint32_t op2, uint32_t op3, uint32_t op4);

uint32_t FetchSP (void);                                    // Get stack pointer
uint32_t PopNum (void);                                    // Pop from the stack
void PushNum (uint32_t N);                                  // Push to the stack
uint32_t FetchCell (uint32_t addr);                      // Read from RAM or ROM
void StoreCell (uint32_t N, uint32_t addr);                      // Write to RAM
uint8_t FetchByte (uint32_t addr);                       // Read from RAM or ROM
uint16_t FetchHalf (uint32_t addr);                      // Read from RAM or ROM
void StoreByte (uint8_t N, uint32_t addr);                       // Write to RAM
void EraseROM (void);                                      // Erase internal ROM
void StoreROM (uint32_t N, uint32_t addr);         // Store cell to internal ROM
void vmRAMfetchStr(char *s, unsigned int address, uint8_t length); // get string
void vmRAMstoreStr(char *s, unsigned int address);     // Store unbounded string

#define dbgSP0  ((ROMsize+32)*4)
#define dbgRP0  ((ROMsize+64)*4)
