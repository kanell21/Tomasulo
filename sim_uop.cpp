#include "pin.H"
#include <stdio.h>
#include <iostream>
#include <fstream>
#include <string>

#include <queue>
#include <list>
#include <vector>
#include <new> 
#include "sim.h"


// -------------------- Reservation Station -----------------------------------
class ReservationStation {
	public:
		CPU_OPCODE_enum opCode;     // The operation of this instruction (IALU, etc)
		UINT32          dstReg;     // The destination register
		ReservationStation *src1;   // Pointer to a source RS which will produce a result. Null, if this source is ready
		ReservationStation *src2;   // ditto
		ReservationStation *src3;   // ditto
		bool to_be_executed;
		// ------------------------------------------------------------------------
		// Add any other variables you need here
		void set_dst(UINT32 dst1){
			dstReg=dst1;
		}
		void set_src1(ReservationStation *_src1){
			src1 = _src1;
		}
		void set_src2(ReservationStation *_src2){
			src2 = _src2;
		}
		void set_src3(ReservationStation *_src3){
			src3 = _src3;
		}    

		// Constructor method.
		ReservationStation(CPU_OPCODE_enum _opCode,
				UINT32          _dstReg,
				ReservationStation *_src1,
				ReservationStation *_src2,
				ReservationStation *_src3)
		{
			opCode = _opCode;
			dstReg = _dstReg;
			src1 = _src1;
			src2 = _src2;
			src3 = _src3;
			to_be_executed=false;
			// ----------------------------------------------------------------------
			// Add code to initialize other object variables here
		}

		// ----------------------------------------------------------------------
		// Add any other methods/functions here
		//  e.g. to track, remove dependents
};



// --------------------- FUs and RS pool combo -----------------------------------
// There will be an object of this class for each **type** of functional unit
// It holds info about the pipeline depth, initiation interval and latency of this
//  type of unit.
// There may be more than 1 actual units of this type, as determined by variable num_fus
// All the units share a reservation station pool: a list of POINTERS to reservation stations for
//   instructions which wait to be executed by an FU of this type. The list should be
//   ordered by program order, so use rs_pool.push_back to insert pointers to ReservationStation
//   objects. The number of items in the list must be up to num_rs
class ResStationFuncUnit{
	public:
		CPU_OPCODE_enum fu_type;     // Type of the FU: essentially the instruction opCode (MEMOP for loads, stores)
		UINT32  pipe_depth;          // Number of pipeline stages
		UINT32  initiation_interval; // Number of cycles between subsequent executions (for each unit)
		UINT32  latency;             // Latency of execution (cycles required for the result to be produced)

		UINT32  num_fus; // Number of FUs of this type 
		std::vector<UINT64> last_init;       // Last execution initiation time, per unit
		std::vector<UINT32> ops_in_progress; // Number of operations in progress, per unit

		UINT32  num_rs;  // Number of reservation stations shared by all FUs of this type
		std::list<ReservationStation *> rs_pool;  // The reservation station pool, common to all FUs of this object

		// Constructor
		ResStationFuncUnit(CPU_OPCODE_enum _fu_type,
				UINT32 _num_fus,
				UINT32 _num_rs,
				UINT32 _pipe_depth,
				UINT32 _initiation_interval,
				UINT32 _latency)
		{
			fu_type = _fu_type;
			num_fus = _num_fus;
			num_rs  = _num_rs;
			pipe_depth          = _pipe_depth;
			initiation_interval = _initiation_interval;
			latency             = _latency;
			// Set the number of entries of these 2 vectors. All entries contain 0
			ops_in_progress.resize(num_fus, 0);
			last_init.resize(num_fus, 0);
		}
};

// Array of POINTERS to ResStationFuncUnit. One for each type of FU.
//  They are created once at simulation initialization.
ResStationFuncUnit *rs_fu[LAST_FU];


// -------------------------- register Status ---------------------------------
// These are ReservationStation pointers so they can hold the current "tag" of a register,
//   i.e. point to the RS which will be producing the result they expect.
// This is NULL if the register has a valid value.
std::vector<ReservationStation *> registerStatus(REG_LAST, NULL);



// --------------------------- Event Queue ------------------------------------
class EventQ_Item
{
	public:
		UINT64             dueCycle;  // The cycle when the result is produced
		// -------------------------------------------------------------------
		// Add any other variables you need here
		ResStationFuncUnit *rsfu;
		UINT32 res_station;
		UINT32 fu_num;
		// Constructor
		EventQ_Item(UINT64 _dueCycle,
			    ResStationFuncUnit *_rs_fu,
			    UINT32 _res_station,
			    UINT32 funum
				// ------------------------------------------------
				// Add any other parameters you need here
				//  to initialize the added variables of this class
			   )
		{
			dueCycle    = _dueCycle;
			rsfu = _rs_fu;
			res_station = _res_station;
			fu_num = funum;
			// --------------------------------------
			// Add code to initialize other variables
		}

};

// Special class to create an ordered list by dueCycle 
class EventQ_cmp {
	public:
		bool operator()(EventQ_Item * lhs, EventQ_Item * rhs)
		{
			return (lhs->dueCycle > rhs->dueCycle);
		}
};

// The event queue, automatically sorted by dueCycle
std::priority_queue<EventQ_Item *, std::vector<EventQ_Item *>, EventQ_cmp> g_eventQ;



// ---------------------------------------------------------------------------
// --------------------------------- KNOBS -----------------------------------
// ---------------------------------------------------------------------------
// -------------------
// Simulation control:
// -------------------
// Debug verbosity:
// 0 - no messages
// 1 - keep-alive messages (every 100 million cycles)
// 2 - dispatch stage messages
// 3 - execute stage messages
// 4 - write-result stage messages
KNOB<UINT32> Knob_verbose      (KNOB_MODE_WRITEONCE, "pintool", "verb",              "-1", "enable detailed messages for debugging");
// Number of fast-forwarding instructions:
KNOB<UINT64> Knob_num_ff       (KNOB_MODE_WRITEONCE, "pintool", "ffwd",              "0", "number of instructions for fast-forward simulation");
// Number of warm-up cycles to simulate (before starting to take measurements):
KNOB<UINT64> Knob_num_warmUp   (KNOB_MODE_WRITEONCE, "pintool", "warmUp",      "1000000", "number of cycles for warm-up");
// Number of detailed cycles to simulate (including warm-up):
KNOB<UINT64> Knob_num_detailed (KNOB_MODE_WRITEONCE, "pintool", "detailed", "1001000000", "number of cycles for detailed simulation, including warm-up");
// NOTE: the last 2 knobs count cycles not instructions. For a wide processor the number of cycles are approximately
//   (number of instructions) / (dispatch width)

// ------------------------
// Processor configuration
// ------------------------
// Number of instructions that can be dispatched in 1 cycle:
KNOB<UINT32> Knob_disp_width (KNOB_MODE_WRITEONCE, "pintool", "dispatch_width", "1", "dispatch width of the processor");
// Number of Common Data Busses (CDB), which carry results from functional units to all reservation stations and the register file:
KNOB<UINT32> Knob_cdb_width  (KNOB_MODE_WRITEONCE, "pintool", "cdb_width",      "1", "number of Common Data Busses (CDB)");

//------------------------------------
// Number of functional units per type
//  must have at least 1 per type.
//------------------------------------

// Number of memory ports/units:
KNOB<UINT32> Knob_num_mem   (KNOB_MODE_WRITEONCE, "pintool", "num_mem",   "1", "number of memory ports");
// Number of Integer ALUs:
KNOB<UINT32> Knob_num_ialus (KNOB_MODE_WRITEONCE, "pintool", "num_ialus", "1", "number of integer ALUs");
// Number of Integer multipliers:
KNOB<UINT32> Knob_num_imuls (KNOB_MODE_WRITEONCE, "pintool", "num_imuls", "1", "number of integer multipliers");
// Number of Integer divisors:
KNOB<UINT32> Knob_num_idivs (KNOB_MODE_WRITEONCE, "pintool", "num_idivs", "1", "number of integer dividers");
// Number of floating point (FP) ALUs:
KNOB<UINT32> Knob_num_falus (KNOB_MODE_WRITEONCE, "pintool", "num_falus", "1", "number of FP ALUs");
// Number of FP multipliers:
KNOB<UINT32> Knob_num_fmuls (KNOB_MODE_WRITEONCE, "pintool", "num_fmuls", "1", "number of FP multipliers");
// Number of FP divisors:
KNOB<UINT32> Knob_num_fdivs (KNOB_MODE_WRITEONCE, "pintool", "num_fdivs", "1", "number of FP dividers");

// -------------------------------------------------------
// Number of reservation stations (RS) buffers per functional unit type
// -------------------------------------------------------
// Number of RS for memory ports/units:
KNOB<UINT32> Knob_num_rs_mem  (KNOB_MODE_WRITEONCE, "pintool", "num_rs_mem",  "1", "number of reservation stations for memory");
// Number of RS for IALU units:
KNOB<UINT32> Knob_num_rs_ialu (KNOB_MODE_WRITEONCE, "pintool", "num_rs_ialu", "1", "number of reservation stations for integer ALUs");
// Number of RS for IMUL units:
KNOB<UINT32> Knob_num_rs_imul (KNOB_MODE_WRITEONCE, "pintool", "num_rs_imul", "1", "number of reservation stations for integer multipliers");
// Number of RS for IDIV units:
KNOB<UINT32> Knob_num_rs_idiv (KNOB_MODE_WRITEONCE, "pintool", "num_rs_idiv", "1", "number of reservation stations for integer dividers");
// Number of RS for FP ALU units:
KNOB<UINT32> Knob_num_rs_falu (KNOB_MODE_WRITEONCE, "pintool", "num_rs_falu", "1", "number of reservation stations for FP ALUs");
// Number of RS for FP MUL units:
KNOB<UINT32> Knob_num_rs_fmul (KNOB_MODE_WRITEONCE, "pintool", "num_rs_fmul", "1", "number of reservation stations for FP multipliers");
// Number of RS for FP DIV units:
KNOB<UINT32> Knob_num_rs_fdiv (KNOB_MODE_WRITEONCE, "pintool", "num_rs_fdiv", "1", "number of reservation stations for FP dividers");

// ------------------------------
// Functional unit pipeline depth
// ------------------------------
// Pipeline depth of memory ports/units:
KNOB<UINT32> Knob_mem_pdepth  (KNOB_MODE_WRITEONCE, "pintool", "mem_pdepth",  "1", "memory port pipeline depth");
// Pipeline depth of IALU units:
KNOB<UINT32> Knob_ialu_pdepth (KNOB_MODE_WRITEONCE, "pintool", "ialu_pdepth", "1", "integer ALU pipeline depth");
// Pipeline depth of IMUL units:
KNOB<UINT32> Knob_imul_pdepth (KNOB_MODE_WRITEONCE, "pintool", "imul_pdepth", "1", "integer multiplier pipeline depth");
// Pipeline depth of IDIV units:
KNOB<UINT32> Knob_idiv_pdepth (KNOB_MODE_WRITEONCE, "pintool", "idiv_pdepth", "1", "integer divider pipeline depth");
// Pipeline depth of FALU units:
KNOB<UINT32> Knob_falu_pdepth (KNOB_MODE_WRITEONCE, "pintool", "falu_pdepth", "1", "FP ALU pipeline depth");
// Pipeline depth of FMUL units:
KNOB<UINT32> Knob_fmul_pdepth (KNOB_MODE_WRITEONCE, "pintool", "fmul_pdepth", "1", "FP multiplier pipeline depth");
// Pipeline depth of FDIV units:
KNOB<UINT32> Knob_fdiv_pdepth (KNOB_MODE_WRITEONCE, "pintool", "fdiv_pdepth", "1", "FP divider pipeline depth");

// -------------------------
// Functional unit latencies
// -------------------------
// Memory address-generation latency:
KNOB<UINT32> Knob_mem_add_lat (KNOB_MODE_WRITEONCE, "pintool", "mem_add_lat", "1", "memory address-generation latency");
// Memory access latency:
KNOB<UINT32> Knob_mem_acc_lat (KNOB_MODE_WRITEONCE, "pintool", "mem_acc_lat", "1", "memory access latency");
// IALU latency:
KNOB<UINT32> Knob_ialu_lat    (KNOB_MODE_WRITEONCE, "pintool", "ialu_lat",    "1", "integer ALU latency");
// IMUL latency:
KNOB<UINT32> Knob_imul_lat    (KNOB_MODE_WRITEONCE, "pintool", "imul_lat",    "4", "integer multiplier latency");
// IDIV latency:
KNOB<UINT32> Knob_idiv_lat    (KNOB_MODE_WRITEONCE, "pintool", "idiv_lat",    "8", "integer divider latency");
// FALU latency:
KNOB<UINT32> Knob_falu_lat    (KNOB_MODE_WRITEONCE, "pintool", "falu_lat",    "4", "FP ALU latency");
// FMUL latency:
KNOB<UINT32> Knob_fmul_lat    (KNOB_MODE_WRITEONCE, "pintool", "fmul_lat",    "8", "FP multiplier latency");
// FDIV latency:
KNOB<UINT32> Knob_fdiv_lat    (KNOB_MODE_WRITEONCE, "pintool", "fdiv_lat",   "10", "FP divider latency");

// ------------------------------------
// Functional unit initiation intervals
// ------------------------------------
// Memory unit initiation interval:
KNOB<UINT32> Knob_mem_ivl  (KNOB_MODE_WRITEONCE, "pintool", "mem_interval",  "1", "memory initiation interval");
// IALU initiation interval:
KNOB<UINT32> Knob_ialu_ivl (KNOB_MODE_WRITEONCE, "pintool", "ialu_interval", "1", "integer ALU initiation interval");
// IMUL initiation interval:
KNOB<UINT32> Knob_imul_ivl (KNOB_MODE_WRITEONCE, "pintool", "imul_interval", "1", "integer multiplier initiation interval");
// IDIV initiation interval:
KNOB<UINT32> Knob_idiv_ivl (KNOB_MODE_WRITEONCE, "pintool", "idiv_interval", "4", "integer divider initiation interval");
// FALU initiation interval:
KNOB<UINT32> Knob_falu_ivl (KNOB_MODE_WRITEONCE, "pintool", "falu_interval", "1", "FP ALU initiation interval");
// FMUL initiation interval:
KNOB<UINT32> Knob_fmul_ivl (KNOB_MODE_WRITEONCE, "pintool", "fmul_interval", "2", "FP multiplier initiation interval");
// FDIV initiation interval:
KNOB<UINT32> Knob_fdiv_ivl (KNOB_MODE_WRITEONCE, "pintool", "fdiv_interval", "5", "FP divider initiation interval");

// ---------------------------------------------------------------------------
// --------------------------------- GLOBALS -----------------------------------
// ---------------------------------------------------------------------------
UINT64 g_cycle;         // Cycle counter
UINT64 g_cycle_start;   // the cycle when we start measuring
UINT64 g_last;          // for printing current cycle - to verify liveness,

UINT32 g_dispatch_count;  // instructions dispatched per cycle
bool   g_is_new_cycle;    // determines when a new clock cycle starts.
//     It is global because it must be preserved across calls to sim_uop()

UINT64 g_fastFwdSim,  //Number of remaining instructions to fast-forward
       g_warmUpSim,   // Remaining warm-up clock cycles
       g_detailedSim; // Total number of detailed simulation cycles

// ---------------------------------------------------------
// ---------------------------------------------------------
// Add any other globals needed to count interesting events,
//  e.g. instructions written back (to calculate CPI)
// ---------------------------------------------------------
// ---------------------------------------------------------


// -------------------------------------------------------------------------------
// --------------------------------- FUNCTIONS -----------------------------------
// -------------------------------------------------------------------------------
void run_Execute_stage();
void run_WriteResult_stage();

void sim_init()
{
	// --------------------------------------------------------------------------
	// Initialise data structures and counters here.
	// --------------------------------------------------------------------------
	rs_fu[MEMOP] = new ResStationFuncUnit(MEMOP, Knob_num_mem.Value(), Knob_num_rs_mem.Value(),
			Knob_mem_pdepth.Value(), Knob_mem_ivl.Value(),
			Knob_mem_add_lat.Value());
	rs_fu[IALU]  = new ResStationFuncUnit(IALU, Knob_num_ialus.Value(), Knob_num_rs_ialu.Value(),
			Knob_ialu_pdepth.Value(), Knob_ialu_ivl.Value(),
			Knob_ialu_lat.Value());
	rs_fu[IMUL]  = new ResStationFuncUnit(IMUL, Knob_num_imuls.Value(), Knob_num_rs_imul.Value(),
			Knob_ialu_pdepth.Value(), Knob_imul_ivl.Value(),
			Knob_imul_lat.Value());
	rs_fu[IDIV]  = new ResStationFuncUnit(IDIV, Knob_num_idivs.Value(), Knob_num_rs_idiv.Value(),
			Knob_ialu_pdepth.Value(), Knob_idiv_ivl.Value(),
			Knob_idiv_lat.Value());
	rs_fu[FALU]  = new ResStationFuncUnit(FALU, Knob_num_falus.Value(), Knob_num_rs_falu.Value(),
			Knob_ialu_pdepth.Value(), Knob_falu_ivl.Value(),
			Knob_falu_lat.Value());
	rs_fu[FMUL]  = new ResStationFuncUnit(FMUL, Knob_num_fmuls.Value(), Knob_num_rs_fmul.Value(),
			Knob_ialu_pdepth.Value(), Knob_fmul_ivl.Value(),
			Knob_fmul_lat.Value());
	rs_fu[FDIV]  = new ResStationFuncUnit(FDIV, Knob_num_fdivs.Value(), Knob_num_rs_fdiv.Value(),
			Knob_ialu_pdepth.Value(), Knob_fdiv_ivl.Value(),
			Knob_fdiv_lat.Value());
//	cout << "REG_LAST: " << REG_LAST  << endl ;
	g_cycle = 0;
	g_dispatch_count = 0;
	g_is_new_cycle = false;
	g_last = 0;
	g_fastFwdSim  = Knob_num_ff.Value();        // Number of instructions to fast-forward
	g_cycle_start = 0;
	g_detailedSim = Knob_num_detailed.Value();  // Number of detailed simulation cycles (including warm-up)
	g_warmUpSim   = Knob_num_warmUp.Value();    // Number of warmup cycles
	if (g_detailedSim < g_warmUpSim)
		g_detailedSim = g_warmUpSim;

	// ---------------------------------------------------------
	// ---------------------------------------------------------
	// Initialize any other globals needed for counting interesting events,
	// ---------------------------------------------------------
	// ---------------------------------------------------------
	return;
}


void print_stats()
{
	TraceFile << "Fast-forwarded instructions: "                << Knob_num_ff.Value() << endl;
	TraceFile << "Detailed simulation cycles (incl. warm-up): " << Knob_num_detailed.Value() << endl;
	TraceFile << "Warm-up cycles: "                             << Knob_num_warmUp.Value() << endl;
	TraceFile << "Number of (measure) cycles: "                 << g_cycle - g_cycle_start << endl;
	// ---------------------------------------------------------
	// ---------------------------------------------------------
	// Add instructions to write the information you collect
	//   during the simulation
	// ---------------------------------------------------------
	// ---------------------------------------------------------
}



void debug_reservation_stations(){
	cout << "########### RESERVATION STATIONS ########### " << endl; 
	cout << "Cycle : " << g_cycle << endl; 
	for (int i = MEMOP; i < LAST_FU; i++) {
		cout << "Type: " << opcode2String((CPU_OPCODE_enum) i);
		cout << endl;
		for (std::list<ReservationStation*>::iterator it = rs_fu[i]->rs_pool.begin(); it != rs_fu[i]->rs_pool.end(); it++) {
			ReservationStation *rs_p = *it;
			cout << "dst: " << rs_p->dstReg << " src1: " << rs_p->src1 << " src2: " << rs_p->src2;
			cout << endl;
		}
	}
	cout << "############################################ " << endl; 

	return;
}

void debug_queue(std::priority_queue<EventQ_Item *, std::vector<EventQ_Item *>, EventQ_cmp> queue) {
		
		cout << "########### PRIORITY QUEUE ########### " << endl;
		cout << "Cycle : " << g_cycle << endl;
		cout << "Queue size: " << g_eventQ.size() <<  endl ;

		while (!queue.empty()){
			EventQ_Item *ev_item;
			ev_item = queue.top();
			
			cout << "Top item found: " << endl;
					
			ReservationStation *dres;
			list<ReservationStation*>::iterator itPool = ev_item->rsfu->rs_pool.begin();
			advance(itPool,ev_item->res_station);
			dres = *(itPool);

			cout << "Res Found: " << endl ; 	
			cout << "dst: " << dres->dstReg << " src1: " << dres->src1 << " src2: " << dres->src2 << " at station: " << ev_item->res_station << " Cycle: " << ev_item->dueCycle;
			cout << endl;
			queue.pop();	
		}
		cout << "###################################### " << endl;
		return;
}

void sim_uop (CPU_OPCODE_enum opCode,  // The instruction opcode
		UINT32 src1,             // source register 1
		UINT32 src2,             // source register 2
		UINT32 src3,             // source register 3
		UINT32 dst)              // destination register
{
	if (g_fastFwdSim > 0) {  // We are in fast-forward simulation, update f-fwd instruction count only
		g_fastFwdSim--;
		if (g_fastFwdSim == 0)
			std::cout <<"SIM: ------- Fast-forward phase ended --------" << std::endl; 
		return; // Don't run detailed simulation. Leave this function
	}
	bool instruction_can_dispatch;
	do {
		
		instruction_can_dispatch = true;
		/* ------------------------ This is the DISPATCH stage ----------------------- */
		UINT32 fu_type;
		if ((opCode == LOAD) || (opCode == STORE))
			fu_type = MEMOP; // bundle LOAD/STORE instructions to the same functional unit (MEMOP)
		else
			fu_type = opCode;
		
		if(rs_fu[fu_type]->rs_pool.size() ==  rs_fu[fu_type]->num_rs){
			instruction_can_dispatch = false;
		}


		// For debugging:
		if (Knob_verbose.Value() == 1) {
			std::cout << "At: " << g_cycle
				<< " Dispatching instruction: " << opcode2String(opCode)
				<< " dst:"  << REG_StringShort( (REG) dst)  << " " << dst
				<< " src1:" << REG_StringShort( (REG) src1) << " " << src1
				<< " src2:" << REG_StringShort( (REG) src2) << " " << src2
				<< " src3:" << REG_StringShort( (REG) src3) << " " << src3
				<< " Can dispatch " << instruction_can_dispatch
				<< std::endl;
		}
		// --------------------------------------------------------------------------
		// --------------------------------------------------------------------------
		// Write code to check if instruction can dispatch.
		//   if it cannot dispatch, set variable instruction_can_dispatch = false
		// --------------------------------------------------------------------------
		// --------------------------------------------------------------------------

		// End of "can dispatch" code
		// --------------------------------------------------------------------------
		// For debugging:
		if (Knob_verbose.Value() >= 2) {
			std::cout << " rsPoolsz: " << rs_fu[fu_type]->rs_pool.size() 
				<< " numRs: "    << rs_fu[fu_type]->num_rs
				<< " dispatch "  << instruction_can_dispatch
				<< std::endl;
		}
		if (instruction_can_dispatch) {
			// ------------------------------------------------------------------------
			// ------------------------------------------------------------------------
			// Dispatch the instruction------------------------------------------------
			// ------------------------------------------------------------------------
			// ------------------------------------------------------------------------
			// v
				if (Knob_verbose.Value() == 1) {
//					cout << "----------------Before Dispatch----------------- " << endl;
//					debug_reservation_stations();
//					debug_queue(g_eventQ);
				}
		
			ReservationStation *res = new ReservationStation(opCode,dst,NULL,NULL,NULL);

			switch(opCode){

				case STORE: // In case of store there is no dst register!!
					{
						if(src1 != 0){
							if(registerStatus[src1]!=NULL){
								res->set_src1(registerStatus[src1]);				  	
							}
						}
						if(src2 != 0){ 
							if(registerStatus[src2]!=NULL){
								res->set_src2(registerStatus[src2]);				  	
							}
						}
						if(src3 != 0){
							if(registerStatus[src3]!=NULL){
								res->set_src3(registerStatus[src3]);				  	
							}
						}
						//cout << "STORE Instruction" << endl;
					}

				default:
					{

						if(src1 != 0){
							if(registerStatus[src1]!=NULL){
								res->set_src1(registerStatus[src1]);				  	
							}
						}
						if(src2 != 0){
							if(registerStatus[src2]!=NULL){
								res->set_src2(registerStatus[src2]);				  	
							}
						}
						registerStatus[dst] = res; // Update the registerStatus[dst] -- it points to the last ReservationStation
					}

			}
			rs_fu[fu_type]->rs_pool.push_back(res);
			
			if (Knob_verbose.Value() == 0) {
//					cout << "----------------After Dispatch----------------- " << endl;
//					debug_reservation_stations();
//					debug_queue(g_eventQ);
				}
		

			// End of dispatch

			// Count number of instructions dispatched in this clock cycle
			g_dispatch_count++;
			if (g_dispatch_count == Knob_disp_width.Value()) {
				g_is_new_cycle = true;
				g_dispatch_count = 0;
			}
		} else { // Issue is stalled. Move on to the next cycle
			g_is_new_cycle = true;
			g_dispatch_count = 0;
		}

		if (g_is_new_cycle) {
			g_is_new_cycle = false;
			g_cycle++;  // count the cycle
			if (g_warmUpSim > 0) {  // Keep track of warm-up cycles
				g_warmUpSim--;
				if (g_warmUpSim == 0) {
					g_cycle_start = g_cycle;   // Keep the cycle when warm-up finishes.
					///////////////////////////////////////////////////////////////////////////////////////////
					// IMPORTANT: (g_cycle-g_cycle_start) is the total number of cycles for calculating IPC etc.
					///////////////////////////////////////////////////////////////////////////////////////////
					g_last = g_cycle;  // for keep-alive print-outs, if enabled
					std::cout <<"SIM: ------- Warm-up phase ended --------" << std::endl; 
				}
			}
			// Run pipe stages in reverse order
			//  so as not to propagate an instruction through all stages in a single cycle!
			// Result forwarding works because the WriteResult stage "wakes-up" dependent instructions
			//  which will start execution in the same cycle
			run_WriteResult_stage();
			if (Knob_verbose.Value() == 1) {
//			cout << "--------Before Execute-------- " << endl;
//			debug_queue(g_eventQ);
			}
			run_Execute_stage();
			if (Knob_verbose.Value() == 1) {
//			cout << "--------After Execute--------- " <<endl;
//			debug_queue(g_eventQ);
			}
			if (g_detailedSim < (g_cycle - g_cycle_start)) { // Check for end of simulation
				print_stats();
				TraceFile.close();
				PIN_ExitProcess(0);   // end the simulation
			}
			if (Knob_verbose.Value() >= 1) {
				// Print something to show simulation is alive
				if (g_cycle - g_last == 100000000) {
					std::cout <<"SIM: cycle: " << g_cycle << std::endl; 
					g_last = g_cycle;
				}
			}
		}

	} while (!instruction_can_dispatch);
}

void run_Execute_stage()
{

	// ----------------------- This is the EXECUTE stage -------------------------------------------------------------
	// NOTES for memory operations:
	// 1.  Memory operations must be handled in FIFO order. That's why there is one MEMOP unit for loads and stores.
	//     MUST not execute newer mem operations if an older one has unresolved memory references.
	// 2. Loads must wait for both address generation and memory access to produce a result
	// 3. Stores must wait only for address generation. The actual store happens at write back, but we assume there is
	//    a store-buffer so we don't have to wait for the memory access.
	// ---------------------------------------------------------------------------------------------------------------
	bool execute = true;
	for (int i = MEMOP; i < LAST_FU; i++) {  // For all types of FUs
		for (UINT32 ii = 0; ii < rs_fu[i]->num_fus; ii++) {  // For each FU of type i
			// For debugging:
			execute = true;

			if((g_cycle - rs_fu[i]->last_init[ii]) < rs_fu[i]->initiation_interval){
				execute = false;	
			}

			if((rs_fu[i]->ops_in_progress[ii]) == rs_fu[i]->pipe_depth){
				execute = false;
			}
		
			if (Knob_verbose.Value() >= 3) {
				std::cout << "At: "                  << g_cycle
					<< " FU type: "            << opcode2String((CPU_OPCODE_enum) i) 
					<< " FU num: " << ii
					<< " FU last initiation: " << rs_fu[i]->last_init[ii]
					<< " Cannot initiate: "    << ((g_cycle - rs_fu[i]->last_init[ii]) < rs_fu[i]->initiation_interval)
					<< " Pipe full: "          << ((rs_fu[i]->ops_in_progress[ii]) >= rs_fu[i]->pipe_depth)
					<< " Execute: "		   << execute
					<< std::endl;
			}
			// -----------------------------------------------------------
			// -----------------------------------------------------------
			// Check if this unit can execute an instruction at this cycle
			// -----------------------------------------------------------
			// -----------------------------------------------------------
	

			// End of "unit can execute" code
			// -----------------------------------------------------------
			if(execute){
				int res_from_pool = 0; // pass info to EventQ
				for (std::list<ReservationStation*>::iterator it = rs_fu[i]->rs_pool.begin(); it != rs_fu[i]->rs_pool.end(); it++) {
				// -------------------------------------------------------------
				// Look from oldest to newest entries in the reservation station
				//  for instructions ready to execute
				// When an instruction is selected, calculate when the result will be ready (cycle number)
				//   and use the event Queue to keep track of the time when results are produced:
				//   g_eventQ.push(new EventQ_Item(CYCLE_DONE, ANY OTHER INFO YOU NEED AT WRITE_RESULT STAGE))
				// NOTES:
				// 1. Once an instruction (RS-entry) is scheduled, it must not be allowed to be selected for execution again!
				// 2. A functional unit can only execute 1 instruction at a time.
				// -------------------------------------------------------------
					ReservationStation *rs_p = *it;
				// For debugging:
					if (Knob_verbose.Value() >= 3) {
						std::cout << "At: " << g_cycle
							<< " FUtype: " << opcode2String((CPU_OPCODE_enum) i) << " FUnum:" << ii
							<< " sources: " << rs_p->src1 << ", " << rs_p->src2 << ", " << rs_p->src3;
					}
				// -----------------------------------------------------------------------------
				// -----------------------------------------------------------------------------
				// Write code to check for ready instructions and schedule their result due time
				// -----------------------------------------------------------------------------
				// -----------------------------------------------------------------------------
				 	//if(i == MEMOP && rs_p != *(rs_fu[i]->rs_pool.begin())) break;
					if( rs_p->src1 == NULL && rs_p->src2 == NULL  && rs_p->src3 == NULL && rs_p->to_be_executed == false ){
//						cout << "Inserted in Queue " << " Pool: " <<  res_from_pool << " fu_number: " << ii <<  endl; 
						rs_fu[i]->ops_in_progress[ii]++;
						rs_fu[i]->last_init[ii] = g_cycle;
						g_eventQ.push(new EventQ_Item(g_cycle+rs_fu[i]->latency,rs_fu[i],res_from_pool,ii));
					//		debug_queue(g_eventQ);
						rs_p->to_be_executed = true;
						break;
					}
				// End of code for execution initiation
				// -----------------------------------------------------------------------------
					res_from_pool++;
				}  // endforeach reservation station
			}
		} // endforeach FU
	} // endforeach FU-type
	return;
}



void run_WriteResult_stage()
{
	/* --------------------- This is the WRITE_RESULT stage ------------------- */
	for (UINT32 cdb_count = 0; cdb_count < Knob_cdb_width.Value(); cdb_count++) {   // For each common data bus (result bus
		// Check if a result is due on this cycle.
		//   e.g. use g_eventQ.empty(), g_eventQ.top()
		// If there is:
		// 1.  Wake-up the dependents: Look for instructions which have this RS as a source and mark that source as ready
		// 2.  Remove the event from the event queue, delete the event object,
		//       remove the RS object from the rs_pool of the appropriate ResStationFuncUnit and delete the RS object.

		// For debugging:
		if (Knob_verbose.Value() >= 4) {
			std::cout << "At: " << g_cycle
				<< " WB: " ;
		}
		if( g_eventQ.empty()) break;

		EventQ_Item *ev_item;
		ev_item = g_eventQ.top();

//		cout <<  " Cycle: " << g_cycle << " Due: " << ev_item->dueCycle << endl;
		if(g_cycle < ev_item->dueCycle){
//			cout <<  "Not yet" << endl;
			break;
		}
		ReservationStation *dres;

		list<ReservationStation*>::iterator itPool = ev_item->rsfu->rs_pool.begin();
		advance(itPool,ev_item->res_station);
		dres = *(itPool);
		ev_item->rsfu->ops_in_progress[ev_item->fu_num]--;

		//cout << ev_item ->rsfu->fu_type << endl; 
		for (int i = MEMOP; i < LAST_FU; i++) { 
			for (std::list<ReservationStation*>::iterator it = rs_fu[i]->rs_pool.begin(); it != rs_fu[i]->rs_pool.end(); it++){
					ReservationStation *rs_p = *it;
					if(rs_p->src1 == dres){ 
						//cout << "Solved Dependency" << endl;  
						rs_p->src1 = NULL; }
					if(rs_p->src2 == dres){ 
						//cout << "Solved Dependency" << endl; 
						rs_p->src2 = NULL;}
					if(rs_p->src3 == dres){ 
						//cout << "Solved Dependency" << endl; 
						rs_p->src3 = NULL;}
					
			}
	
		}
		
		for(int i=0; i< REG_LAST ; i++){
			if( registerStatus[i] == dres ){
//				cout << "Dependency of register: " << i << endl ; 
				registerStatus[i]=NULL;
			}
		}
//		cout << "Going to delete" << endl;
		ev_item->rsfu->rs_pool.erase(itPool);
//		cout<< "Deleted" << endl;
		g_eventQ.pop();
		free(ev_item);
		free(dres);
		// End of result write handling
		// -------------------------------------------------------------
	} // endfor cdb_count
	return;
}
