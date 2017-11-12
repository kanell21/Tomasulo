// -------------------------------------------------------------------
// Simple simulator
// This source code is provided for the coursework of CS
//
// Author: Aristides Efthymiou, efthym@cs.uoi.gr
//         Computer Science and Engineering, University of Ioannina
// -------------------------------------------------------------------


/*BEGIN_LEGAL 
Intel Open Source License 

Copyright (c) 2002-2007 Intel Corporation 
All rights reserved. 
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.  Redistributions
in binary form must reproduce the above copyright notice, this list of
conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.  Neither the name of
the Intel Corporation nor the names of its contributors may be used to
endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE INTEL OR
ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
END_LEGAL */
//
//


#include <stdio.h>
#include <iostream>
#include <fstream>
#include <string>

#include <set>
#include <vector>
#include "pin.H"

#include "sim.h"



KNOB<bool>   Knob_dissasemble(KNOB_MODE_WRITEONCE, "pintool", "diss", "0",            "enable dissasembly of x86 and micro-instructions");
KNOB<string> KnobOutputFile(  KNOB_MODE_WRITEONCE, "pintool", "o",    "tomasulo.out", "specify output file name");

std::ofstream TraceFile;

using std::cout;


string opcode2String(CPU_OPCODE_enum opcode)
{
    switch(opcode) {
      case MEMOP:  return "MEMOP";
      case LOAD:  return "LOAD";
      case STORE: return "STORE";
      case IALU:  return "IALU";
      case IMUL:  return "IMUL";
      case IDIV:  return "IDIV";
      case FALU:  return "FALU";
      case FMUL:  return "FMUL";
      case FDIV:  return "FDIV";
      default:    return "INVALID";
    }
}


extern void sim_init();
extern void print_stats();
extern void sim_uop (CPU_OPCODE_enum opCode,
                     UINT32 src1,
                     UINT32 src2,
                     UINT32 src3,
                     UINT32 dst);



extern UINT64 g_instructions_dispatched, g_instructions_wb;

LOCALFUN VOID Fini(int code, VOID * v)
{
    /*
    // Run a few more dummy instructions to make sure all proper instructions have exitted the pipe.
    UINT64 targetInst = g_instructions_dispatched;
    while (g_instructions_wb < targetInst) {
        sim_uop(IALU, REG_INST_G0, REG_INST_G0, REG_INST_G0, REG_INST_G1);
        g_instructions_dispatched--;
    }
   */
   print_stats();
   TraceFile.close();
}


// Pin instrumentation function. 
//
LOCALFUN VOID Instruction(INS ins, VOID *v)
{
    // Exclude any weird instructions. 
    // Flow control instructions just honour their dependencies, they do not change the flow
    //    perfect branch prediction
    if ( INS_IsSyscall(ins)  // Ignore syscalls and returns
    || INS_IsSysret(ins)
    || INS_IsPrefetch(ins)   // Ignore prefetch instructions
    || INS_IsHalt(ins)       // and other
    || INS_IsNop(ins)
    )
        return;

    // There can only be one of these, so get them once and use them as many times as needed.
    REG baseReg = REG_FullRegName(INS_MemoryBaseReg(ins));
    REG indexReg = REG_FullRegName(INS_MemoryIndexReg(ins));

    if (Knob_dissasemble.Value())   // print the instruction for debugging
        cout << INS_Disassemble(ins) << endl;

    bool is_fp = false;    // Not a floating point instruction
	bool foundMemRead = false;
    CPU_OPCODE_enum opcode;
    // ------------------- Loads ---------------------------------- 
    // If there is a load, it must be the first micro-op.
    //   If the macro-instr contains more loads, only the first is used.
    for (UINT32 memOpIdx = 0; memOpIdx < INS_MemoryOperandCount(ins) && !foundMemRead; memOpIdx++) {
        if (INS_MemoryOperandIsRead(ins, memOpIdx)) {
            // Assume all loads use both base and index registers
            //    even if they are not needed by a specific load, it will be needed by the (macro) instruction
            //      so no harm is done.
            foundMemRead = true;
            INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR) sim_uop,
                           IARG_UINT32, LOAD,
                           IARG_UINT32, baseReg,
                           IARG_UINT32, indexReg,
                           IARG_UINT32, REG_INVALID(),
                           // Use dummy register to return loaded value to main uOp
                           IARG_UINT32, REG_INST_G0, IARG_END);
            if (Knob_dissasemble.Value())
                cout << " -> LOAD " << REG_StringShort(REG_INST_G0) << " = *( "
                     << REG_StringShort(baseReg) << " + " << REG_StringShort(indexReg) << " )" << endl;
        }
    }

    // ------------------- Main uOp here ---------------------------
    std::vector<REG> src, dst;

    if (foundMemRead == true)   // Add implicit source - from the load uOp
        src.push_back(REG_INST_G0);

    for (UINT32 opIdx = 0; opIdx < INS_OperandCount(ins); opIdx++) {
        if (!INS_OperandIsMemory(ins, opIdx)) { 
            REG reg = REG_FullRegName(INS_OperandReg(ins, opIdx));
            if (REG_is_fr(reg))
                is_fp = true;

            if (INS_OperandRead(ins, opIdx)
            && REG_valid(reg)
            && (reg != baseReg)
            && (reg != indexReg))
                src.push_back(reg);
            if (INS_OperandWritten(ins, opIdx) && REG_valid(reg))
                dst.push_back(reg);
        }
        // Special case for load effective address
        if (INS_OperandIsAddressGenerator(ins, opIdx)) {
            if (REG_valid(indexReg))
                src.push_back(indexReg);
            if (REG_valid(baseReg))
                src.push_back(baseReg);
        }
    }
    if (INS_IsMemoryWrite(ins))
        dst.push_back(REG_INST_G1);  // Implicit destination to be picked up by the store uOp

    // Fill the 3 first elements of src with REG_INVALID, if any are empty
    src.insert(src.end(), (src.size() >= 3)? 0:3-src.size() , REG_INVALID());

    // Decode main opcode
    if (OPCODE_StringShort(INS_Opcode(ins)).find("MUL") != string::npos) {
        opcode = is_fp? FMUL : IMUL;
    } else if (OPCODE_StringShort(INS_Opcode(ins)).find("DIV") != string::npos) {
        opcode = is_fp? FDIV : IDIV;
    } else {
        opcode = is_fp? FALU : IALU;
    }
    // There can be many destinations
    //    e.g. stack POP instructions return the data on the stack and update the stack pointer register
    for (std::vector<REG>::iterator it=dst.begin(); it != dst.end(); it++)  {
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR) sim_uop,
                       IARG_UINT32, opcode,
                       IARG_UINT32, src[0],
                       IARG_UINT32, src[1],
                       IARG_UINT32, src[2],
                       IARG_UINT32, *it, IARG_END);
        if (Knob_dissasemble.Value())
            cout << " -> " << opcode2String(opcode)   << " " << REG_StringShort(*it)
                 << " = "  << REG_StringShort(src[0]) << "|" << REG_StringShort(src[1])
                 << "|  "  << REG_StringShort(src[2]) << endl;
    }
  
    // ------------------- Stores ---------------------------------- 
    for (UINT32 memOpIdx = 0; memOpIdx < INS_MemoryOperandCount(ins); memOpIdx++) {
        if (INS_MemoryOperandIsWritten(ins, memOpIdx)) {
            // Assume all stores use both source registers (base, index)
            INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR) sim_uop,
                           IARG_UINT32, STORE,
                           IARG_UINT32, REG_INST_G1,
                           IARG_UINT32, baseReg,
                           IARG_UINT32, indexReg,
                           IARG_UINT32, REG_INVALID(),
                           IARG_END);
            if (Knob_dissasemble.Value())
              cout << " -> STORE *( " <<  REG_StringShort(baseReg) << " + "
                   << REG_StringShort(indexReg) << ") =" << REG_StringShort(REG_INST_G1) << endl;
            break;  // There should only be at most 1 store uop
        }
    }
}





int main(int argc, char *argv[])
{

    PIN_Init(argc, argv);

    // Write to a file since cout and cerr maybe closed by the application
    TraceFile.open(KnobOutputFile.Value().c_str());

    INS_AddInstrumentFunction(Instruction, 0);
    PIN_AddFiniFunction(Fini, 0);


    sim_init();  // Initialise simulator globals and data structures
    PIN_StartProgram();
    // Never returns  

    cout <<"SIM: Simulation is ended " << endl; 
    return 0; // make compiler happy
}



