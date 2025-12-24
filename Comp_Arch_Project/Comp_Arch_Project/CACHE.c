#include <stdio.h>
#include <stdint.h>

#define BLOCKS_NUM 64
#define BlOCK_SIZE 8

typedef enum {
    INVALID = 0,
    SHARED = 1,
    EXCLUSIVE = 2,
    MODIFIED = 3
} MesiState;

typedef struct {
    uint16_t tag;    // 12 bits 
    MesiState state; // 2 bits 
} TSRAM_Line;

typedef struct {
    uint32_t data[BlOCK_SIZE]; //width block is 32 bit , with 8 words
} DSRAM_Block;

typedef struct {
    DSRAM_Block dsram[BLOCKS_NUM];
	TSRAM_Line tsram[BLOCKS_NUM]; 
} cache_s;

cache_s cache_core[4];     //array of 4 caches

void init_cache(cache_s* cache) {
    for (int i = 0; i < 4; i++) {
		for (int j = 0; j < BLOCKS_NUM; j++) {
			cache[i].tsram[j].state = INVALID;
			cache[i].tsram[j].tag = 0;
			cache[i].dsram[j] = 0;  
		}
    }
}

