#include <stdio.h>
#define BLOCKS_NUM 64
#define BlOCK_SIZE 8


typedef struct {
	int ds_ram[BLOCK_SOZE*BLOCKS_NUM];
	int ts_ram[BLOCKS_NUM];
} cache_t;