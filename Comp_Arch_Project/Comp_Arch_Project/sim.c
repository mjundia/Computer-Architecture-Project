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

typedef struct {
	int opcode;
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
	DSRAM_Block bus_data; // 32 bit data
	bool bus_shared; // 1 when answering BusRd transac' if a core has the data in cache, otherwise 0.
	bool bus_stall; // Added for memory/cache latency synchronization.
} bus_s;


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

//****
// initialize main memory from memin.txt
void init_main_memory() {
	// Allocate memory on heap
	main_memory.data = (uint32_t*)calloc(MEMORY_SIZE, sizeof(uint32_t)); // REMEMBER TO FREE THE ALLOCATED MEMORY IN THE END!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
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

	char line[256]; // 256 is a bit redundant we theoreticall need 32, but for now we keep it this way.
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
	if (bus->bus_cmd == BUS_FLUSH) {
		unsigned int block_base = (bus->bus_addr >> 3) << 3; // Align to block boundary
		for (int i = 0; i < BlOCK_SIZE; i++) {
			unsigned int addr = block_base + i;
			if (addr < MEMORY_SIZE) {
				main_memory.data[addr] = bus->bus_data.data[i];
			}
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
			bus->bus_addr = main_memory.request_addr;
			bus->bus_data.data[main_memory.word_offset] = main_memory.data[addr];

			main_memory.word_offset++;

			// Check if entire block has been sent
			if (main_memory.word_offset >= BlOCK_SIZE) { // Maybe add modulo of the block size instead, this can be an edge case, think of it.
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
			main_memory.wait_cycles = MEM_LATENCY - 1;
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

int get_bus_arbitration(bool core_requests[4]) {
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
int get_index(unsigned int addr) {
	return (addr >> 5) % BLOCKS_NUM;
}

// Helper function to get the tag (remaining bits)
unsigned int get_tag(unsigned int addr) {
	return (addr >> 5) / BLOCKS_NUM;
}
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
				// Flush: Provide my dirty data to the bus before changing state
				bus->bus_data = cache_core[core_id].dsram[idx];
				cache_core[core_id].tsram[idx].state = SHARED;
			}
			else if (cache_core[core_id].tsram[idx].state == EXCLUSIVE) {
				cache_core[core_id].tsram[idx].state = SHARED;
			}
			break;

		case BUS_RDX: // BusRdX (Someone else wants to write/modify)
			if (cache_core[core_id].tsram[idx].state == MODIFIED) {
				// I have the most recent data, must provide it before I invalidate
				bus->bus_data = cache_core[core_id].dsram[idx];
			}
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