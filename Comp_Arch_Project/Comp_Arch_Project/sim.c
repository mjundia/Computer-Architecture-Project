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

//Cache Structs
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
