#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#define MEMORY_SIZE 2097152 // 2^21 bytes = 2 MB as per the specification in the project document.
#define BLOCKS_NUM 64
#define BlOCK_SIZE 8
#define MEM_LATENCY 16 //***

typedef enum {
    BUS_NOOP = 0,
    BUS_RD = 1,   // Bus Read (for S or E states)
    BUS_RDX = 2,  // Bus Read Exclusive (for M state / Writing)
    BUS_FLUSH = 3 // Writing back to memory
} bus_cmd_t;

// Opcodes enum////////////////////////////
typedef enum {
    OP_ADD = 0,
    OP_SUB = 1,
    OP_AND = 2,
    OP_OR = 3,
    OP_XOR = 4,
    OP_MUL = 5,
    OP_SLL = 6,
    OP_SRA = 7,
    OP_SRL = 8,
    OP_BEQ = 9,
    OP_BNE = 10,
    OP_BLT = 11,
    OP_BGT = 12,
    OP_BLE = 13,
    OP_BGE = 14,
    OP_JAL = 15,
    OP_LW = 16,
    OP_SW = 17,
    /* 18,19 not used */
    OP_HALT = 20
} opcode_t;

typedef struct {
    opcode_t opcode;
    int rd;
    int rs;
    int rt;
    int immediate; // 16 -bit signed immediate (we need 12 but all the integer data structures is C are power of 2).
} inst_s;

typedef struct {
    int PC;
    int reg0;
    int reg1;
    int reg2;
    int reg3;
    int reg4;
    int reg5;
    int reg6;
    int reg7;
    int reg8;
    int reg9;
    int reg10;
    int reg11;
    int reg12;
    int reg13;
    int reg14;
    int reg15;
    inst_s f;
    inst_s d;
    inst_s ex;
    inst_s mem;
    inst_s wb;
} reg_s;
//////////////////////////////////////////
typedef struct {
    // EX->MEM
    uint32_t ex_alu;        // ALU result or computed address
    uint32_t ex_store_data; // for SW

    // MEM->WB
    uint32_t mem_load_data; // for LW
} stage_data_s;


// MESI struct
typedef enum {
    INVALID = '0',
    SHARED = '1',
    EXCLUSIVE = '2',
    MODIFIED = '3'
} mesi_state_t;

//Cache Structs
typedef struct {
    uint16_t tag;    // 12 bits 
    mesi_state_t state; // 2 bits 
} TSRAM_Line;

typedef struct {
    uint32_t data[BlOCK_SIZE]; //width block is 32 bit , with 8 words
} DSRAM_Block;

typedef struct {
    DSRAM_Block dsram[BLOCKS_NUM];
    TSRAM_Line tsram[BLOCKS_NUM];
} cache_s;

//bus struct
typedef struct {
    unsigned int bus_origid;
    bus_cmd_t bus_cmd;
    unsigned int bus_addr; // first 21 bits
    uint32_t bus_data; // 32 bit data
    bool bus_shared; // 1 when answering BusRd transac' if a core has the data in cache, otherwise 0.
} bus_s;

typedef struct {
    char* imem[4];
    char* memin;
    char* memout;
    char* regout[4];
    char* coretrace[4];
    char* bustrace;
    char* dsram[4];
    char* tsram[4];
    char* stats[4];
} sim_files_t;

static FILE* g_core_trace_fp[4] = { NULL, NULL, NULL, NULL };
static FILE* g_bustrace_fp = NULL;

//***
// Main Memory struct
typedef struct {
    uint32_t* data;           // memory data
    bool busy;                // if busy iwth other instruction
    int wait_cycles;          // respond in x cycles
    unsigned int request_addr; // address base in cache
    int word_offset;          // offset in block
    bus_cmd_t pending_cmd;    // Command type being serviced 
} main_memory_t;


cache_s cache_core[4];     //array of 4 caches
main_memory_t main_memory; //main memory instance

int last_bus_winner = 3;

//////////////////////////////////////////////////////////////////////////////
// ---- Branch delay slot support (per-core) ----
// If branch_pending[c] is true, then after the delay-slot we set PC = branch_target[c]
stage_data_s stage_data[4];   // one EX/MEM/WB latch per core
static bool     branch_pending[4] = { false, false, false, false };
static uint16_t branch_target[4] = { 0, 0, 0, 0 };

//////////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////////
// ---- Immediate sign extension (12-bit) ----
static inline int32_t sign_extend_12(uint32_t imm12)
{
    imm12 &= 0xFFFu;
    if (imm12 & 0x800u) {                 // if sign bit (bit 11) is 1
        imm12 |= 0xFFFFF000u;             // extend to 32-bit negative
    }
    return (int32_t)imm12;
}

// ---- Decode 32-bit instruction word into your inst_s ----
// Format: [31:24]=opcode, [23:20]=rd, [19:16]=rs, [15:12]=rt, [11:0]=imm12
static void decode_word(uint32_t instr_word, inst_s* out)
{
    out->opcode = (int)((instr_word >> 24) & 0xFF);
    out->rd = (int)((instr_word >> 20) & 0xF);
    out->rs = (int)((instr_word >> 16) & 0xF);
    out->rt = (int)((instr_word >> 12) & 0xF);

    // 12-bit immediate (signed)
    out->immediate = (int)sign_extend_12(instr_word & 0xFFFu);
}

//////////////////////////////////////////////////////////////////////////////

static int reg_read(reg_s* core, int reg_num)
{
    switch (reg_num) {
    case 0:  return core->reg0;
    case 1:  return core->reg1;
    case 2:  return core->reg2;
    case 3:  return core->reg3;
    case 4:  return core->reg4;
    case 5:  return core->reg5;
    case 6:  return core->reg6;
    case 7:  return core->reg7;
    case 8:  return core->reg8;
    case 9:  return core->reg9;
    case 10: return core->reg10;
    case 11: return core->reg11;
    case 12: return core->reg12;
    case 13: return core->reg13;
    case 14: return core->reg14;
    case 15: return core->reg15;
    default: return 0;
    }
}

static void reg_write(reg_s* core, int reg_num, int value)
{
    // PDF rule: R0 is always 0, R1 is immediate → not writable
    if (reg_num == 0 || reg_num == 1) return;

    switch (reg_num) {
    case 2:  core->reg2 = value; break;
    case 3:  core->reg3 = value; break;
    case 4:  core->reg4 = value; break;
    case 5:  core->reg5 = value; break;
    case 6:  core->reg6 = value; break;
    case 7:  core->reg7 = value; break;
    case 8:  core->reg8 = value; break;
    case 9:  core->reg9 = value; break;
    case 10: core->reg10 = value; break;
    case 11: core->reg11 = value; break;
    case 12: core->reg12 = value; break;
    case 13: core->reg13 = value; break;
    case 14: core->reg14 = value; break;
    case 15: core->reg15 = value; break;
    default: break;
    }

    core->reg0 = 0; // enforce hardwired zero
}
//****
// initialize main memory from memin.txt in MAINNNN
void init_main_memory() {
    // Allocate memory on heap
    main_memory.data = (uint32_t*)calloc(MEMORY_SIZE, sizeof(uint32_t));
    if (main_memory.data == NULL) {
        printf("Error: Failed to allocate main memory\n");
        exit(1);
    }

    main_memory.busy = false;
    main_memory.wait_cycles = 0;
    main_memory.request_addr = 0;
    main_memory.word_offset = 0;
    main_memory.pending_cmd = BUS_NOOP;

    FILE* file = fopen("memin.txt", "r");
    if (file == NULL) {
        printf("Warning: Could not open memin.txt, memory initialized to zero\n");
        return;
    }

    char line[64];
    int index = 0;

    while (fgets(line, sizeof(line), file) != NULL && index < MEMORY_SIZE) {
        uint32_t value;
        if (sscanf(line, "%x", &value) == 1) {
            main_memory.data[index] = value;
            index++;
        }
    }

    fclose(file);
    printf("Loaded %d words from memin.txt\n", index);
}

// write final memory to memout.txt
void write_memout() {
    FILE* file = fopen("memout.txt", "w");
    if (file == NULL) {
        printf("Error: Could not open memout.txt for writing\n");
        return;
    }

    for (int i = 0; i < MEMORY_SIZE; i++) {
        fprintf(file, "%08x\n", main_memory.data[i]);
    }

    fclose(file);
    printf("Wrote memory contents to memout.txt\n");
}

// handle memory operations per cycle
void main_memory_cycle(bus_s* bus) {
    //check flush and handle writeback immediately
    if (bus->bus_cmd == BUS_FLUSH && bus->bus_origid != 4) {
        if (bus->bus_addr < MEMORY_SIZE) {
            main_memory.data[bus->bus_addr] = bus->bus_data;
        }
        return;
    }


    // check if memory is busy
    if (main_memory.busy) {
        if (main_memory.wait_cycles > 0) {
            // countdown
            main_memory.wait_cycles--;
        }
        else {
            // wainting finished
            unsigned int addr = main_memory.request_addr + main_memory.word_offset;

            // prepare flush to send data back
            bus->bus_origid = 4; // Memory ID
            bus->bus_cmd = BUS_FLUSH;
            bus->bus_addr = addr;
            bus->bus_data = main_memory.data[addr];

            main_memory.word_offset++;

            // Check if entire block has been sent
            if (main_memory.word_offset >= BlOCK_SIZE) {
                main_memory.busy = false;
                main_memory.word_offset = 0;
            }
        }
    }
    else {
        // memory is idle, check for new read requests
        if ((bus->bus_cmd == BUS_RD || bus->bus_cmd == BUS_RDX) && bus->bus_origid != 4) {
            // start servicing the request
            main_memory.busy = true;
            main_memory.wait_cycles = MEM_LATENCY;
            main_memory.request_addr = (bus->bus_addr >> 3) << 3; // align to block boundary
            main_memory.word_offset = 0;
            main_memory.pending_cmd = bus->bus_cmd;
        }
    }
}

// clean main memory (maybe not needed)
void cleanup_main_memory() {
    if (main_memory.data != NULL) {
        free(main_memory.data);
        main_memory.data = NULL;
    }
}

//***

int get_bus_arbitration(bool core_requests[4]) {  //Call in Main ()
    for (int i = 1; i <= 4; i++) {
        int core_idx = (last_bus_winner + i) % 4;
        if (core_requests[core_idx]) {
            last_bus_winner = core_idx;
            return core_idx;
        }
    }
    return -1; // No one requested the bus
}

// Helper function to get the index (0-63) from the 21-bit address
int get_index(unsigned int addr) { return (addr >> 3) & 0x3F; }   // 6 bits

// Helper function to get the tag (remaining bits)
unsigned int get_tag(unsigned int addr) { return addr >> 9; }     // remaining bits

void snoop_bus_transaction(bus_s* bus, int core_id) {
    // A core does not snoop its own request
    if (bus->bus_origid == core_id) return;

    int idx = get_index(bus->bus_addr);
    unsigned int tag = get_tag(bus->bus_addr);

    // Check if the tag matches and the line is not INVALID
    if (cache_core[core_id].tsram[idx].tag == tag && cache_core[core_id].tsram[idx].state != INVALID) {

        switch (bus->bus_cmd) {
        case BUS_RD: // BusRd (Someone else wants to read)
            bus->bus_shared = true; // Tell the requester: "I have this data"

            if (cache_core[core_id].tsram[idx].state == MODIFIED) {
                cache_core[core_id].tsram[idx].state = SHARED;
            }
            else if (cache_core[core_id].tsram[idx].state == EXCLUSIVE) {
                cache_core[core_id].tsram[idx].state = SHARED;
            }
            break;

        case BUS_RDX: // BusRdX (Someone else wants to write/modify)
            // Everyone else must invalidate on a BusRdX
            cache_core[core_id].tsram[idx].state = INVALID;
            break;

        case BUS_FLUSH: // Flush (Data being written to memory)
            // Usually no state change needed for a flush snooped from another core
            break;

        case BUS_NOOP:
            break;
        }
    }
}
//////////////////////////////////////////////////////////////////////////////
// Executing opcodes
static void execute_ex_stage(reg_s* core, stage_data_s* sd)
{
    inst_s* in = &core->ex;

    int op = in->opcode;
    int rd = in->rd;
    int rs = in->rs;
    int rt = in->rt;

    int sVal = reg_read(core, rs);
    int tVal = reg_read(core, rt);

    switch (op) {
    case OP_ADD:
        sd->ex_alu = (uint32_t)(sVal + tVal);
        break;
    case OP_SUB:
        sd->ex_alu = (uint32_t)(sVal - tVal);
        break;
    case OP_AND:
        sd->ex_alu = (uint32_t)(sVal & tVal);
        break;
    case OP_OR:
        sd->ex_alu = (uint32_t)(sVal | tVal);
        break;
    case OP_XOR:
        sd->ex_alu = (uint32_t)(sVal ^ tVal);
        break;
    case OP_MUL:
        sd->ex_alu = (uint32_t)(sVal * tVal);
        break;
    case OP_SLL:
        sd->ex_alu = (uint32_t)((uint32_t)sVal << (tVal & 31));
        break;
    case OP_SRA:
        sd->ex_alu = (uint32_t)(sVal >> (tVal & 31));
        break;
    case OP_SRL:
        sd->ex_alu = (uint32_t)((uint32_t)sVal >> (tVal & 31));
        break;

    case OP_LW:
    case OP_SW: {
        // PDF: effective address is R[rs] + R[rt] (word address), no bytes :contentReference[oaicite:7]{index=7}
        uint32_t addr = (uint32_t)(sVal + tVal);
        sd->ex_alu = addr;                 // pass address to MEM
        sd->ex_store_data = (uint32_t)reg_read(core, rd); // SW stores R[rd]
        break;
    }

              // Branches are resolved in Decode per PDF, so EX does nothing for them :contentReference[oaicite:8]{index=8}
    case OP_BEQ: case OP_BNE: case OP_BLT: case OP_BGT: case OP_BLE: case OP_BGE:
    case OP_JAL:
        break;

    case OP_HALT:
        // You usually mark "halt seen" in WB or earlier,
        // but EX doesn't need to compute anything.
        break;

    default:
        break;
    }
}

static void writeback_stage(reg_s* core, stage_data_s* sd)
{
    inst_s* in = &core->wb;
    int op = in->opcode;

    switch (op) {
    case OP_ADD: case OP_SUB: case OP_AND: case OP_OR: case OP_XOR:
    case OP_MUL: case OP_SLL: case OP_SRA: case OP_SRL:
        reg_write(core, in->rd, (int)sd->ex_alu);
        break;

    case OP_LW:
        reg_write(core, in->rd, (int)sd->mem_load_data);
        break;

    case OP_JAL:
        // PDF: R15 = next instruction address, pc = R[rd][9:0] :contentReference[oaicite:9]{index=9}
        // Usually the PC redirection is handled in Decode with delay slot,
        // but writing R15 belongs in WB (or Decode if your design says so).
        reg_write(core, 15, core->PC + 1); // adjust if your PC bookkeeping differs
        break;

    default:
        break;
    }

    // Enforce R0 always 0 (safety)
    core->reg0 = 0;
}

static bool opcode_writes_reg(int opcode)
{
    if (opcode == OP_SW) return false;
    if (opcode >= OP_BEQ && opcode <= OP_BGE) return false;
    if (opcode == OP_HALT) return false;
    // JAL writes R15 (link)
    return true;
}

static int opcode_dest_reg(inst_s* ins)
{
    if (ins->opcode == OP_JAL) return 15;
    if (!opcode_writes_reg(ins->opcode)) return -1;
    return ins->rd;
}

// returns: true = stall decode, false = no stall
static bool decode_stage(int core_id, reg_s* core, uint32_t instr_word)
{
    // 1) Decode raw word into the core->d pipeline register
    decode_word(instr_word, &core->d);

    // 2) Update special immediate register R1
    core->reg1 = core->d.immediate;


    // 3) If a redirect from a previously taken branch is pending,
    //    apply it now (THIS is what enforces the delay slot)
    if (branch_pending[core_id]) {
        core->PC = (int)branch_target[core_id];      // PC becomes 10-bit target
        branch_pending[core_id] = false;
    }

    // 4) Resolve branch / jal in Decode (decision now, redirect next cycle)
    {
        int op = core->d.opcode;
        int rs = core->d.rs;
        int rt = core->d.rt;
        int rd = core->d.rd;

        int32_t vrs = (int32_t)reg_read(core, rs);
        int32_t vrt = (int32_t)reg_read(core, rt);

        bool taken = false;

        switch (op) {
        case OP_BEQ: taken = (vrs == vrt); break;
        case OP_BNE: taken = (vrs != vrt); break;
        case OP_BLT: taken = (vrs < vrt); break;
        case OP_BGT: taken = (vrs > vrt); break;
        case OP_BLE: taken = (vrs <= vrt); break;
        case OP_BGE: taken = (vrs >= vrt); break;
        case OP_JAL:
            taken = true;
            break;
        default:
            break;
        }

        if (taken) {
            // target is in R[rd] low 10 bits
            uint16_t tgt = (uint16_t)(reg_read(core, rd) & 0x3FF);
            branch_pending[core_id] = true;
            branch_target[core_id] = tgt;
        }
    }

    // 5) Data hazard detection (no forwarding).
    //    Stall if decode reads a reg that will be written by EX or MEM or WB.
    //    (This is the safe version for the PDF “no half cycle” behavior.)
    {
        int src1 = core->d.rs;
        int src2 = core->d.rt;

        int ex_dst = opcode_dest_reg(&core->ex);
        int mem_dst = opcode_dest_reg(&core->mem);
        int wb_dst = opcode_dest_reg(&core->wb);

        bool hazard = false;

        if (ex_dst > 1 && (ex_dst == src1 || ex_dst == src2)) hazard = true;
        if (mem_dst > 1 && (mem_dst == src1 || mem_dst == src2)) hazard = true;
        if (wb_dst > 1 && (wb_dst == src1 || wb_dst == src2)) hazard = true;

        return hazard; // true = stall
    }
}

void init_sim_files(int argc, char* argv[], sim_files_t* files)
{
    if (argc != 27) {
        fprintf(stderr,
            "ERROR: Invalid number of arguments (%d)\n"
            "Usage:\n"
            "sim.exe imem0 imem1 imem2 imem3 memin memout "
            "regout0 regout1 regout2 regout3 "
            "core0trace core1trace core2trace core3trace "
            "bustrace "
            "dsram0 dsram1 dsram2 dsram3 "
            "tsram0 tsram1 tsram2 tsram3 "
            "stats0 stats1 stats2 stats3\n",
            argc);
        exit(1);
    }

    int i = 1;

    // Instruction memories
    for (int c = 0; c < 4; c++)
        files->imem[c] = argv[i++];

    // Main memory
    files->memin = argv[i++];
    files->memout = argv[i++];

    // Register outputs
    for (int c = 0; c < 4; c++)
        files->regout[c] = argv[i++];

    // Core traces
    for (int c = 0; c < 4; c++)
        files->coretrace[c] = argv[i++];

    // Bus trace
    files->bustrace = argv[i++];

    // DSRAM dumps
    for (int c = 0; c < 4; c++)
        files->dsram[c] = argv[i++];

    // TSRAM dumps
    for (int c = 0; c < 4; c++)
        files->tsram[c] = argv[i++];

    // Stats
    for (int c = 0; c < 4; c++)
        files->stats[c] = argv[i++];
}

int main(int argc, char* argv[])
{
    sim_files_t files;
    init_sim_files(argc, argv, &files);
    return 0;
}