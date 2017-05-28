#include <assert.h>
#include <stdarg.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

#define NREG		(32)	
#define PAGESIZE_WIDTH	(2)	
#define PAGESIZE	(1<<PAGESIZE_WIDTH)	
#define NPAGES		(2048)
#define RAM_PAGES	(8)	
#define RAM_SIZE	(RAM_PAGES * PAGESIZE)
#define SWAP_PAGES	(128)	
#define SWAP_SIZE	(SWAP_PAGES * PAGESIZE)
#undef DEBUG	

#define ADD	(0)
#define ADDI	(1)
#define SUB	(2)
#define SUBI	(3)
#define SGE	(4)
#define SGT	(5)
#define SEQ	(6)
#define BT      (7)
#define BF      (8)
#define BA      (9)
#define ST      (10)
#define LD      (11)  
#define CALL	(12)
#define JMP	(13)
#define MUL	(14)
#define SEQI	(15)
#define HALT    (16)

/* MACROS ADDED BY US */
#define FIFO 			0
#define SECOND_CHANCE 	1
#define RANDOM 			2

#define CURRENT_REPLACE  FIFO

char*	mnemonics[] = { 
	[ADD] = "add",
	[ADDI] = "addi",
	[SUB] = "sub",
	[SUBI] = "subi",
	[SGE] = "sge",
	[SGT] = "sgt",
	[SEQ] = "seq",
	[SEQI] = "seqi",
	[BT] = "bt",
	[BF] = "bf",
	[BA] = "ba",
	[ST] = "st",
	[LD] = "ld",
	[CALL] = "call",
	[JMP] = "jmp",
	[MUL] = "mul",
	[HALT] = "halt",
};

typedef struct {
	unsigned	pc;		/* Program counter. */
	unsigned	reg[NREG];	/* Registers. */
} cpu_t;

typedef struct {
	unsigned int	page:26;	/* Swap or RAM page. */
	unsigned int	inmemory:1;	/* Page is in memory. */
	unsigned int	ondisk:1;	/* Page is on disk. */
	unsigned int	modified:1;	/* Page was modified while in memory. */
	unsigned int	referenced:1;	/* Page was referenced recently. */
	unsigned int	readonly:1;	/* Error if written to (not checked). */
} page_table_entry_t;

typedef struct {
	page_table_entry_t*	owner;	/* Owner of this phys page. */
	unsigned		page;	/* Swap page of page if assigned. */
} coremap_entry_t;

static unsigned long long	num_pagefault;		/* Statistics. */
static page_table_entry_t	page_table[NPAGES];	/* OS data structure. */
static coremap_entry_t		coremap[RAM_PAGES];	/* OS data structure. */
static unsigned			memory[RAM_SIZE];	/* Hardware: RAM. */
static unsigned			swap[SWAP_SIZE];	/* Hardware: disk. */
static unsigned			(*replace)(void);	/* Page repl. alg. */

static unsigned disk_writes = 0;


unsigned make_instr(unsigned opcode, unsigned dest, unsigned s1, unsigned s2)
{
	return (opcode << 26) | (dest << 21) | (s1 << 16) | (s2 & 0xffff);
}

unsigned extract_opcode(unsigned instr)
{
	return instr >> 26;
}

unsigned extract_dest(unsigned instr)
{
	return (instr >> 21) & 0x1f;
}

unsigned extract_source1(unsigned instr)
{
	return (instr >> 16) & 0x1f;
}

signed extract_constant(unsigned instr)
{
	return (short)(instr & 0xffff);
}

void error(char* fmt, ...)
{
	va_list		ap;
	char		buf[BUFSIZ];

	va_start(ap, fmt);
	vsprintf(buf, fmt, ap);
	fprintf(stderr, "error: %s\n", buf);
	exit(1);
}

static void read_page(unsigned phys_page, unsigned swap_page)
{
	memcpy(&memory[phys_page * PAGESIZE], 
		&swap[swap_page * PAGESIZE], 
		PAGESIZE * sizeof(unsigned));
}

static void write_page(unsigned phys_page, unsigned swap_page)
{
	disk_writes++;

	memcpy(&swap[swap_page * PAGESIZE], 
		&memory[phys_page * PAGESIZE], 
		PAGESIZE * sizeof(unsigned));
}

static unsigned new_swap_page()
{
	static int 		count;

	assert(count < SWAP_PAGES);

	return count++;
}

static unsigned fifo_page_replace()
{
	static unsigned index = 0;

	unsigned	page;

	page = index;

	assert(page < RAM_PAGES);

	index = (index + 1) % RAM_PAGES;

	return page;
}

static unsigned random_replace()
{
 	static unsigned first_time = 1;	
 	static unsigned first_time_count = 0;
 	static unsigned last_page = 0;
	unsigned page;

 	if ( first_time ) {

 		// The first RAM_PAGES times it is called
 		// the numbers will be 0,1,2,..., RAM_PAGES - 1

 		page = first_time_count;

		if ( first_time_count == RAM_PAGES - 1 )
			first_time = 0;		

		++first_time_count;
		printf("%s page: %u\n", __func__, page);
		return page;
 	}

	page = (unsigned) (rand() % RAM_PAGES);

	if ( last_page == page ) {
		page = ( page + 1 ) % RAM_PAGES;
		last_page = page;
	}

	printf("%s page: %u\n", __func__, page);
	last_page = page;
	return page;
}

static unsigned second_chance_replace()
{
	static unsigned index = 0;

	unsigned	page;

	while ( 1 ) {
		
		// Runs until return statement 
		page = index;

		if ( coremap[page].owner == NULL ) {
			// The RAM is not full yet, 
			// the page is simply returned;
			index = (index + 1) % RAM_PAGES;
			return page;
		} else {
			// Now we will look at the referenced bit
			// to determines whether or not this page
			// gets a second chance


			index = (index + 1) % RAM_PAGES; // Preparing for the next use

			if ( coremap[page].owner->referenced ) {
				coremap[page].owner->referenced = 0;
			} else {
				return page;
			}

		}

	}

	return 0;
}

static unsigned take_phys_page()
{
	unsigned		page;	/* Page to be replaced. */
	static unsigned ram_is_full = 0;
	static unsigned swap_counter = 0;

	page = (*replace)();

	if ( page == 0 && ! ram_is_full ) {
		/* 
		* This is the first time a page fault is invoked.
		* The core map is important and is filled with randomness at this moment.
		* We need to initialize it.
		*/

		/* Initializing the core map */
		int i = 0;
		for (; i<RAM_PAGES; ++i) {
			coremap[i].page = SWAP_SIZE; // if we see that this value is larger than SWAP_SIZE - 1 in the future, we know it is unassigned.
			coremap[i].owner = NULL;
		}

	}

	if ( ram_is_full ) {
		printf("Full ram!\n");
/* 
* 'page' is now pointing to valuable data in RAM that needs to be moved 
* to the swap in order for there not to be any data overwrites. 
*/		
		if ( coremap[page].page < SWAP_SIZE ) {
			/*
			* This virtual page associated with this RAM page has a reserved position in the swap.
			* This information can be used to reduce the number of write operations.
			*/
			printf("Write to swap!\n");
			// ========== in the future: check is the page has not been modified - in that case: don't move back to disk //

			// ========== for now: we swap regardless back to disk. 

			unsigned swap_page = coremap[page].page;

			if ( coremap[page].owner->modified )
				write_page(page, swap_page); // writes from ram to swap

			/* Updating our page table */
			coremap[page].owner->ondisk = 1;
			coremap[page].owner->inmemory = 0;
			coremap[page].owner->page = swap_page;
			coremap[page].owner->modified = 0;
			coremap[page].owner->referenced = 0;

		} else {
/*
* This page has no reserved place on the swap and is therefore placed at the top position of the swap
*/
			write_page(page, swap_counter); // Place this memory at the top of the swap

			assert(coremap[page].owner != NULL); // In fact: It _has_ to be non-NULL at this point, otherwise we have had a bad implementation.

			/* Updating our page table */
			coremap[page].owner->ondisk = 1;
			coremap[page].owner->inmemory = 0;
			coremap[page].owner->page = swap_counter;
			coremap[page].owner->modified = 0;
			coremap[page].owner->referenced = 0;

			swap_counter++;

			assert(swap_counter < SWAP_PAGES);

			printf("Swap counter: %u\n", swap_counter);

		}

	} else {
/* 	
*	There is no need to move memory from RAM to swap since this memory is 
*	used for the first time and nothing is overwritten at this point. 
*/
		if ( page == RAM_PAGES - 1 ) {
/* 
* From now on, we will need to swap memory to avoid overwrites 
*/
			ram_is_full = 1;
		}  

	}

	printf("%s returns page: %u\n",__func__, page);

	return page;
}

static void pagefault(unsigned virt_page)
{
	assert( !page_table[virt_page].inmemory );
	unsigned		page;

	num_pagefault += 1;

	printf("num_pagefault: %llu\n", num_pagefault);

	page = take_phys_page(); // A physical adress in RAM (all the swapping has already been taken care of)

	if ( page_table[virt_page].ondisk ) {

		coremap[page].page = page_table[virt_page].page; // <-- Om denna kommenteras bort funkar random algoritmen.. Varför?

		read_page(page, page_table[virt_page].page );

	}

	/* Preparing the new page table entry at this stage (we see it, the 'take_phys_page' function does not) */
	page_table[virt_page].page = page;
	page_table[virt_page].inmemory = 1;
	page_table[virt_page].modified = 0;

	coremap[page].owner = &page_table[virt_page];
}

static void translate(unsigned virt_addr, unsigned* phys_addr, bool write)
{
	unsigned	virt_page;
	unsigned	offset;

	virt_page = virt_addr / PAGESIZE;
	offset = virt_addr & (PAGESIZE - 1);

	if (!page_table[virt_page].inmemory)
		pagefault(virt_page);

	page_table[virt_page].referenced = 1;

	if (write)
		page_table[virt_page].modified = 1;

	*phys_addr = page_table[virt_page].page * PAGESIZE + offset;
}

static unsigned read_memory(unsigned* memory, unsigned addr)
{
	unsigned	phys_addr;

	translate(addr, &phys_addr, false);

	return memory[phys_addr];
}

static void write_memory(unsigned* memory, unsigned addr, unsigned data)
{
	unsigned	phys_addr;

	translate(addr, &phys_addr, true);

	memory[phys_addr] = data;
}

void read_program(char* file, unsigned memory[], int* ninstr)
{
	FILE*		in;
	int		opcode;
	int		a, b, c;
	int		i;
	char		buf[BUFSIZ];
	char		text[BUFSIZ];
	int		n;
	int		line;

	/* Find out the number of mnemonics. */
	n = sizeof mnemonics / sizeof mnemonics[0];

	in = fopen(file, "r");

	if (in == NULL)
		error("cannot open file");

	line = 0;

	while (fgets(buf, sizeof buf, in) != NULL) {
		if (buf[0] == ';')
			continue;

		if (sscanf(buf, "%s %d,%d,%d", text, &a, &b, &c) != 4)
			error("syntax error near: \"%s\"", buf);

		opcode = -1;

		for (i = 0; i < n; ++i) {
			if (strcmp(text, mnemonics[i]) == 0) {
				opcode = i;
				break;
			}
		}

		if (opcode < 0)
			error("syntax error near: \"%s\"", text);

		write_memory(memory, line, make_instr(opcode, a, b, c));

		line += 1;
	} 

	*ninstr = line;
}

int run(int argc, char** argv)
{
	char*		file;
	cpu_t		cpu;
	int		i;
	int		j;
	int		ninstr;
	unsigned	instr;
	unsigned	opcode;
	unsigned	source_reg1;
	int		constant;
	unsigned	dest_reg;
	int		source1;
	int		source2;
	int		dest;
	unsigned	data;
	bool		proceed;
	bool		increment_pc;
	bool		writeback;

	if (argc > 1)
		file = argv[1];	
	else
		file = "a.s";

	// ======================== ADDED BY US ======================== //
	 memset(page_table, 0, sizeof page_table);

	read_program(file, memory, &ninstr);

	/* First instruction to execute is at address 0. */
	cpu.pc = 0;
	cpu.reg[0] = 0;

	proceed = true;

	while (proceed) {

		/* Fetch next instruction to execute. */
		instr = read_memory(memory, cpu.pc);

		/* Decode the instruction. */
		opcode = extract_opcode(instr);
		source_reg1 = extract_source1(instr);
		constant = extract_constant(instr);
		dest_reg = extract_dest(instr);

		/* Fetch operands. */
		source1 = cpu.reg[source_reg1];
		source2 = cpu.reg[constant & (NREG-1)];

		increment_pc = true;
		writeback = true;

//		printf("pc = %3d: \n", cpu.pc);

		switch (opcode) {
		case ADD:
	//		puts("ADD");
			dest = source1 + source2;
			break;
			
		case ADDI:
	//		puts("ADDI");
			dest = source1 + constant;
			break;
			
		case SUB:
	//		puts("SUB");
			dest = source1 - source2;
			break;
			
		case SUBI:
	//		puts("SUBI");
			dest = source1 - constant;
			break;
			
		case MUL:
	//		puts("MUL");
			dest = source1 * source2;
			break;
			
		case SGE:
//			puts("SGE");
			dest = source1 >= source2;
			break;
			
		case SGT:
//			puts("SGT");
			dest = source1 > source2;
			break;
			
		case SEQ:
//			puts("SEQ");
			dest = source1 == source2;
			break;
			
		case SEQI:
//			puts("SEQI");
			dest = source1 == constant;
			break;
			
		case BT:
//			puts("BT");
			writeback = false;
			if (source1 != 0) {
				cpu.pc = constant;
				increment_pc = false;
			}
			break;
				
		case BF:
//			puts("BF");
			writeback = false;
			if (source1 == 0) {
				cpu.pc = constant;
				increment_pc = false;
			}
			break;
				
		case BA:
//			puts("BA");
			writeback = false;
			increment_pc = false;
			cpu.pc = constant;
			break;

		case LD:
///			puts("LD");
			data = read_memory(memory, source1 + constant);
			dest = data;
			break;

		case ST:
	//		puts("ST");
			data = cpu.reg[dest_reg];
			write_memory(memory, source1 + constant, data);
			writeback = false;
			break;

		case CALL:
//			puts("CALL");
			increment_pc = false;
			dest = cpu.pc + 1;
			dest_reg = 31;
			cpu.pc = constant;
			break;

		case JMP:
//			puts("JMP");
			increment_pc = false;
			writeback = false;
			cpu.pc = source1;
			break;

		case HALT:
//			puts("HALT");
			increment_pc = false;
			writeback = false;
			proceed = false;
			break;
		
		default:
			error("illegal instruction at pc = %d: opcode = %d\n", 
				cpu.pc, opcode);
		}
			
		if (writeback && dest_reg != 0)
			cpu.reg[dest_reg] = dest;

		if (increment_pc)
			cpu.pc += 1;

#ifdef DEBUG
		i = 0;
		while (i < NREG) {
			for (j = 0; j < 4; ++j, ++i) {
				if (j > 0)
					printf("| ");
				printf("R%02d = %-12u", i, cpu.reg[i]);
			}
			printf("\n");
		}
#endif
	}

	i = 0;
	while (i < NREG) {
		for (j = 0; j < 4; ++j, ++i) {
			if (j > 0)
				printf("| ");
			printf("R%02d = %-12u", i, cpu.reg[i]);
		}
		printf("\n");
	

	}
	return 0;
}

int main(int argc, char** argv)
{
	time_t t;

	switch ( CURRENT_REPLACE ) {
	case FIFO:
		replace = fifo_page_replace;
		break;
	case SECOND_CHANCE:
		replace = second_chance_replace;
		break;
	case RANDOM:
		srand((unsigned) time(&t));
		replace = random_replace;
		break;
	default:
		replace = fifo_page_replace;
	}

	run(argc, argv);

	printf("======= STATISTICS =======\n");
	printf("page faults: %llu\n", num_pagefault);
	printf("disk writes: %u\n", disk_writes);

}