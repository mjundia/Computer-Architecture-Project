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
	unsigned int bus_cmd;
	unsigned int bus_addr; // first 21 bits
	DSRAM_Block bus_data; // 32 bit data
	bool bus_shared; // 1 when answering BusRd transac' if a core has the data in cache, otherwise 0.
} bus_s;

cache_s cache_core[4];     //array of 4 caches


int last_bus_winner = 3;

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
	return addr % BLOCKS_NUM;
}

// Helper function to get the tag (remaining bits)
unsigned int get_tag(unsigned int addr) {
	return addr / BLOCKS_NUM;
}
void snoop_bus_transaction(bus_s* bus, int core_id) {
	// A core does not snoop its own request
	if (bus->bus_origid == core_id) return;

	int idx = get_index(bus->bus_addr);
	unsigned int tag = get_tag(bus->bus_addr);

	// Check if the tag matches and the line is not INVALID
	if (cache_core[core_id].tsram[idx].tag == tag && cache_core[core_id].tsram[idx].state != INVALID) {

		switch (bus->bus_cmd) {
		case 1: // BusRd (Someone else wants to read)
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

		case 2: // BusRdX (Someone else wants to write/modify)
			if (cache_core[core_id].tsram[idx].state == MODIFIED) {
				// I have the most recent data, must provide it before I invalidate
				bus->bus_data = cache_core[core_id].dsram[idx];
			}
			// Everyone else must invalidate on a BusRdX
			cache_core[core_id].tsram[idx].state = INVALID;
			break;

		case 3: // Flush (Data being written to memory)
			// Usually no state change needed for a flush snooped from another core
			break;
		}
	}
}