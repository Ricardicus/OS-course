#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <stdint.h>

#include "our_malloc.h"

//#define DEBUG_ALIGNMENT_OUR_MALLOC
//#define DEBUG_OUR_MALLOC
//#define WRITE_TO_FILE

//#define WRITE_LIFTED

#define ALIGNMENT_SIZE_FOR_MALLOC	8

#define UPPER_LIMIT_ORDER		23

#define NUMBER_OF_LEVELS		22

#define MINIMUM_BLOCK_SIZE		(1<<(UPPER_LIMIT_ORDER - NUMBER_OF_LEVELS + 1))
#define MAXIMUM_BLOCK_SIZE		(1<<(UPPER_LIMIT_ORDER))

typedef struct meta_info {
	// This is stored on each block
	size_t order;
	// free is used to check if this
	// is a left or right buddy and if it
	// is allocated or not.
	int free; 
	struct meta_info * succ; // successor in ths free list this block belongs to
	struct meta_info * pred; // predecessor in ths free list this block belongs to
} meta_info;

/*
* Here is the free lists stored in the data segment on the program. 
*/
static meta_info* free_lists[NUMBER_OF_LEVELS];

static void * top_of_the_heap = NULL;

#ifdef WRITE_LIFTED
void * start;
#endif

/*
* Initialize the free_lists
*/
static void buddy_system_init()
{
	// Align the base of the heap
	top_of_the_heap = sbrk(0);

	intptr_t offset = ((intptr_t) top_of_the_heap) % ALIGNMENT_SIZE_FOR_MALLOC;

	if ( offset != 0 ) {
		//printf("offset not zero.\n");
		void * status = sbrk(ALIGNMENT_SIZE_FOR_MALLOC - offset);
		assert ( status != (void*) - 1);
	}

	// Allocating the heap that is going to be used.
	// We are allocating a suffieciently large block at once only
	// in the buddy system.
	top_of_the_heap = sbrk( MAXIMUM_BLOCK_SIZE );
	assert ( top_of_the_heap != (void*) - 1);

#ifdef WRITE_LIFTED
	start = top_of_the_heap;
#endif 

	// The heap is allocated!
	meta_info * first_node = (meta_info*) top_of_the_heap;
	first_node->order = NUMBER_OF_LEVELS - 1;
	first_node->free = 1;
	first_node->succ = NULL;
	first_node->pred = NULL;

	// Initialize the free list
	free_lists[first_node->order] = first_node;
}

/*
* This function maps a gien size (requested)
* to an order that can be mapped tp the free_lists vector
*
* Examples: 
*		If we have the lowest level 0 mapped to 2^12
*		then all number <= 2^12 will be mapped to 0
*		and all numbers >= 2^13 will be mapped to a value > 0
*/
static size_t map_size_to_order(size_t size)
{
	if ( size < MINIMUM_BLOCK_SIZE ) {
		return 0;
	} else {
		size_t count = 0;
		size -= 1;
		while ( size >= MINIMUM_BLOCK_SIZE ) {
			count++;
			size >>= 1;
		}
		return count;
	}
}

static meta_info * find_buddy(meta_info * ptr) 
{
	if ( ptr == NULL )
		return NULL;

	uintptr_t address_to_buddy = (uintptr_t) top_of_the_heap + (((uintptr_t) ptr - (uintptr_t) top_of_the_heap) ^ (1 << ((UPPER_LIMIT_ORDER - NUMBER_OF_LEVELS + 1) + ptr->order)));
	return (void*) address_to_buddy;
}

static size_t align_this_size(size_t size) {
	return ( size + ALIGNMENT_SIZE_FOR_MALLOC - 1 ) & ~(ALIGNMENT_SIZE_FOR_MALLOC - 1);
}

static void add_to_free_list(meta_info * block)
{
	size_t order = block->order;
	block->pred = NULL;
	block->succ = free_lists[order];

	if ( free_lists[order] != NULL )
		free_lists[order]->pred = block;

	free_lists[order] = block;
}

static void remove_from_free_list(meta_info * block, size_t order)
{
	meta_info * walk;
	walk = free_lists[order];

	while ( walk != NULL ) {

		if ( walk == block ) {
			// this one should be removed
			meta_info * last = walk->pred;
			meta_info * next = walk->succ;

			if ( last != NULL )
				last->succ = next;

			if ( next != NULL )
				next->pred = last;

			if ( walk == free_lists[order] )
				free_lists[order] = next;

			walk->free = 0;
		}

		walk = walk->succ;
	}	
}

void * calloc(size_t count, size_t size)
{

#ifdef WRITE_LIFTED
	setvbuf(stderr, NULL, _IONBF, 0);
	fprintf(stderr, "lifted (so far): %d\n", sbrk(0) - start);
#endif

#ifdef DEBUG_OUR_MALLOC
	//printf("CALLOC CALLED\n");
#endif

	size_t total = count * size;
	size_t actual = align_this_size(total);
	void * request = malloc(actual);

	if ( request == NULL )
		return NULL;

	memset(request, 0, actual);
	return request;
}

void * realloc(void* ptr, size_t size) {

	if ( ptr == NULL ) {
		// realloc(NULL, size) should be identical to malloc(size)
		return malloc(size);
	} else if ( size == 0 ) {
		// If size is zero and ptr is not NULL, a new, minimum sized object is
    	// allocated and the original object is freed.
		free(ptr);
		return malloc(10); // lets say 10 is our minimum size object.
	}
	void * tmp = malloc(size);
	size_t old_size = (1 << ( (UPPER_LIMIT_ORDER - NUMBER_OF_LEVELS + 1) + ((meta_info*)(ptr-sizeof(meta_info)))->order) );  

	if ( tmp != ptr )
		memcpy(tmp, ptr, old_size < size ? old_size : size );
	free(ptr);

	return tmp;
}

void * malloc(size_t requested_size)
{
	size_t size = align_this_size(requested_size) + sizeof(meta_info);

	if ( size >= MAXIMUM_BLOCK_SIZE ) {
		errno = ENOMEM;
		return NULL;
	}

	if ( top_of_the_heap == NULL ) {
		// Top of the heap is set
		// And the free_lists is initialized
		// with one huge block of memory
		buddy_system_init();
	}

	size_t order = map_size_to_order(size);

	meta_info * aspirant = free_lists[order];

	if ( aspirant == NULL ){
		// There is no block of a suitable size available.

		// We have to create one.
		size_t next_available_order = order + 1;

		while ( next_available_order < NUMBER_OF_LEVELS && free_lists[next_available_order] == NULL ){
			++next_available_order;
		}

		if ( next_available_order == NUMBER_OF_LEVELS ) {
			// We found no free blocks in any list. The system has reached a state of full capacity
			errno = ENOMEM;
			return NULL;
		}

		// next_available order represents a number in the free_lists where we can find a free block! 
		// It is of a higher order so we now have to split it accordingly.

		while ( next_available_order >= order ) {
		//	printf("next_available_order: %zu\n", next_available_order);
			
			meta_info * block = free_lists[next_available_order]; // guaranteed to be found

			if ( next_available_order == order ) {
				remove_from_free_list(block, block->order);
				return (void*)block + sizeof(meta_info);
			} else {
				// Splitting is required
	//			meta_info * old_list_entry_next = block->succ;
				remove_from_free_list(block, next_available_order);

				block->order = next_available_order - 1;
				block->free = 1;

				meta_info * new_buddy = find_buddy(block);

				new_buddy->free = 1;
				new_buddy->order = block->order;

				add_to_free_list(block);
				add_to_free_list(new_buddy);

				next_available_order--;
			}

		}

		// We could not split the memory until we found a block
		errno = ENOMEM;
		return NULL;

	} else {
		// One exists!
		//fprintf(stderr,"En funnen\n");
		remove_from_free_list(aspirant, aspirant->order);
		return (void*)aspirant + sizeof(meta_info);
	}

}

void free(void * ptr)
{
	meta_info * block;
	meta_info * buddy;
	size_t order;
	intptr_t diff;

	if ( ptr == NULL )
		return;

	block = (meta_info*) (ptr - sizeof(meta_info));
	buddy = find_buddy(block);
	order = block->order;
	block->free = 1;

	remove_from_free_list(block, block->order);

	while ( buddy->free && (buddy->order == block->order) ) {
		// Merge the blocks recursively

		if ( buddy < block ) {
			meta_info * tmp = buddy;
			buddy = block;
			block = tmp;
		}

		remove_from_free_list(buddy, buddy->order);
		remove_from_free_list(block, block->order);
		
		block->order += 1;
		block->free = 1;

		add_to_free_list(block);
		buddy = find_buddy(block);
	} 

} 


/*void print_all_free_blocks(){
	size_t order = 0;
	while ( order  < NUMBER_OF_LEVELS ){
		if ( free_lists[order] != NULL ){
			meta_info * walk = free_lists[order];

			while ( walk != NULL )
			{
				printf("[%d] order %zu free: %d\n", walk, walk->order, walk->free);
				walk = walk->succ;
			}

		}
		order++;
	}
}

int main(int argc, char * argv[])
{

//	buddy_system_init();
//	print_all_free_blocks();
	char * msg = malloc(1000);
	if ( msg == NULL ){
		printf("ERROR\n");
		return 0;
	}
	sprintf(msg, "%s\n", "hejsan! :)");

	//printf("%s",msg);

	//char * msg2 = omalloc(32);

	//omalloc(32);
	//omalloc(32);
	//omalloc(32);
//	omalloc(32);

	print_all_free_blocks();

	printf("===== After free =====\n");
	free(msg);

	print_all_free_blocks();

	return 0;
}*/


