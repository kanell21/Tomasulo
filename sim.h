#ifndef SIM_H 
#define SIM_H

enum CPU_OPCODE_enum {
  MEMOP = 1,  // Not a real opcode; bundles LOAD, STORE together.
  IALU,
  IMUL,
  IDIV,
  FALU,
  FMUL,
  FDIV,
  LAST_FU,  // marks the number of "real" functional units
  LOAD,   // extra "codes" to differentiate MEMOP when required
  STORE,
};

extern string opcode2String(CPU_OPCODE_enum opcode);

extern std::ofstream TraceFile;

#endif
