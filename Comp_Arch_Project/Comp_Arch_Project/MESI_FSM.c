#define BLOCKS_NUM 64
#define BLOCK_SIZE 8
#include <stdbool.h>
// bus_origid 0-3 (cores) 4. Main memory
// bus_cmd 0 - noop, 1 - BusRd, 2 - BusRdX, 3 - Flush
typedef struct {
	unsigned int bus_origid;
	unsigned int bus_cmd;
	unsigned int bus_addr; // first 21 bits
	unsigned int bus_data; // 32 bit data
	bool bus_shared; // 1 when answering BusRd transac' if a core has the data in cache, otherwise 0.
} bus_s;

typedef enum {
	INVALID = '0',
	SHARED = '1',
	EXCLUSIVE = '2',
	MODIFIED = '3'
} mesi_state_t;



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
    if (caches[core_id].tsram[idx].tag == tag && caches[core_id].tsram[idx].state != INVALID) {

        switch (bus->bus_cmd) {
        case 1: // BusRd (Someone else wants to read)
            bus->bus_shared = true; // Tell the requester: "I have this data"

            if (caches[core_id].tsram[idx].state == MODIFIED) {
                // Flush: Provide my dirty data to the bus before changing state
                bus->bus_data = caches[core_id].dsram[idx];
                caches[core_id].tsram[idx].state = SHARED;
            }
            else if (caches[core_id].tsram[idx].state == EXCLUSIVE) {
                caches[core_id].tsram[idx].state = SHARED;
            }
            break;

        case 2: // BusRdX (Someone else wants to write/modify)
            if (caches[core_id].tsram[idx].state == MODIFIED) {
                // I have the most recent data, must provide it before I invalidate
                bus->bus_data = caches[core_id].dsram[idx];
            }
            // Everyone else must invalidate on a BusRdX
            caches[core_id].tsram[idx].state = INVALID;
            break;

        case 3: // Flush (Data being written to memory)
            // Usually no state change needed for a flush snooped from another core
            break;
        }
    }
}
