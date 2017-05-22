#include <unistd.h>
#include <stdio.h>
//#include <stdlib.h>
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

typedef struct meta_info {
	size_t size;
	int free;
	struct meta_info * next;
	struct meta_info * prev;

	//int magic_number;

} meta_info;

#define ALIGNMENT_SIZE_FOR_MALLOC	8

#ifdef WRITE_LIFTED
static void * start = 0;
#endif


meta_info * global_base = NULL;

/*
* This function returns a pointer to a free block of memory
* that is large enough for 'size'. And if it fails to find
* one, 'last' is set to point to the uppermost block. 
*/
static meta_info * find_next_free_block(meta_info ** last, size_t size){

	meta_info * walk = global_base;

	if ( walk == NULL ){
		// there is no global base
		return NULL;
	}

	while ( walk != NULL ) {
		*last = walk;

		if ( walk->size >= size && walk->free == 1 ) {
//			*last = NULL; // last is only legit if this function returns NULL
			return walk;
		}

		walk = walk->next;
	}

	return NULL; // now last is set to the uppermost block
}

void free(void * ptr)
{
	if ( ptr == NULL ) {
		return;
	}

	// Undefined behavior warning: If ptr does not belong to the data
	// previously allocated in 'malloc', the behavior is undefined. 
	meta_info * block = (meta_info*) ((char*)ptr - sizeof(meta_info));
	meta_info * next;
	meta_info * prev;

	block->free = 1;

#ifdef DEBUG_OUR_MALLOC
	//printf("free CALLED, size: %zu, magic_number: %d\n", block->size, block->magic_number);
#endif
	// check if merging is relevant.
	next = block->next;
	prev = block->prev;

	if ( next == NULL && prev == NULL ){
		// Nothing to do at this point.
		return;
	} 

	if ( next != NULL ) {

		if ( next->free == 1 ) {
			// merge!
			block->size += next->size + sizeof(meta_info);
			block->next = next->next;

			if ( next->next != NULL )
				next->next->prev = block;

		}

	}

	if ( prev != NULL ) {

		if ( prev->free == 1 ) {
			// merge!
			prev->size += block->size + sizeof(meta_info);
			prev->next = block->next;

			if ( block->next != NULL ) 
				block->next->prev = prev;

		}
	}

}

static size_t align_this_size(size_t size) {
	return ( size + ALIGNMENT_SIZE_FOR_MALLOC - 1 ) & ~(ALIGNMENT_SIZE_FOR_MALLOC - 1);
}

void * malloc(size_t size_requested)
{

#ifdef WRITE_LIFTED

/*	struct rlimit values;

	int status = getrlimit(RLIMIT_DATA, &values);

	fprintf(stderr, "status of getrlimit: %d\n", status);

	if ( values.rlim_max == RLIM_INFINITY ) {
		fprintf(stderr, "IT IS INFINITE...\n");
	}

	fprintf(stderr, "LIMITS soft (%u), max (%u)\n", values.rlim_cur, values.rlim_max); */

	static int first_time = 1;
	if ( first_time ) {
		start = sbrk(0);
		first_time = 0;
	}
#endif

#ifdef DEBUG_OUR_MALLOC
	static int count;
#endif	

	size_t size = align_this_size(size_requested);

	if ( global_base == NULL ) {
		// First time malloc is called
		// Initialization: move the base of the heap to an aligned place
		char * top_of_the_heap = sbrk(0);
		//printf("initial break: %d\n", top_of_the_heap);

		intptr_t offset = ((intptr_t) top_of_the_heap) % ALIGNMENT_SIZE_FOR_MALLOC;

		if ( offset != 0 ) {
			//printf("offset not zero.\n");
			sbrk(ALIGNMENT_SIZE_FOR_MALLOC - offset);
			//assert ( status != (void*) - 1);
		}

		//top_of_the_heap = sbrk(0);

		//assert( ((intptr_t) top_of_the_heap) % ALIGNMENT_SIZE_FOR_MALLOC == 0 );
		// The top of the heap is fixed!

		// Handling the malloc request
		void * request = sbrk(sizeof(meta_info) + size);

		if ( (void*)request == (void*) -1 ) {
			errno = ENOMEM;
			return NULL;
		}


/*#ifdef WRITE_TO_FILE
		count += sizeof(meta_info) + size;
		fprintf(debug_fp, "count: %llu\n", count);
		fclose(debug_fp);
#endif*/

		// The global base is not set
		// We will set it.
		global_base = (meta_info*) request;

#ifdef DEBUG_OUR_MALLOC
		global_base->magic_number = count;
#endif	

		global_base->next = NULL; // this marks the end of the list
		global_base->prev = NULL; // this is unique for the base.
		global_base->size = size;
		global_base->free = 0;

		return ((void*)global_base) + sizeof(meta_info);

	} 

	// else: Global base exists. 

	meta_info * next_free_block;

	meta_info * last = NULL;

	next_free_block = find_next_free_block(&last, size);

	if ( next_free_block == NULL ) {
		// There was no free block
		void * request = sbrk(size + sizeof(meta_info));

#ifdef DEBUG_OUR_MALLOC
		count++;
#endif	

		// when testing, when running return NULL here instead. 
		if ( request == (void*) -1 ) {
			// there was an error, errno is set and null is returned.
			errno = ENOMEM;
			return NULL;
		}

/*#ifdef WRITE_TO_FILE
		count += sizeof(meta_info) + size;
		fprintf(debug_fp, "count: %llu\n", count);
		fclose(debug_fp);
#endif*/

		// The request was successful!
		// 'last' is now available for us to use
		// since we iterated through the entire list in 'find_next_free_block'
		meta_info * the_new_block = (meta_info*) request;

		the_new_block->next = NULL;
		the_new_block->prev = last;
		the_new_block->free = 0;
		the_new_block->size = size;

		last->next = the_new_block;

#ifdef DEBUG_OUR_MALLOC

		the_new_block->magic_number = count;

#endif


		////printf("%d\n", ((char*)request) + sizeof(meta_info));
		return ((void*)the_new_block) + sizeof(meta_info);

	} else {
		// There exists a free block

		// Check if we can squeez
		if ( next_free_block->size > size + sizeof(meta_info) + 8 ) {
			// The squeezed block should have at least 8 bytes of data.
			meta_info * new_squeezed_block = ((void*)next_free_block) + sizeof(meta_info) + size;

			new_squeezed_block->size = next_free_block->size - sizeof(meta_info) - size;
			new_squeezed_block->prev = next_free_block;
			new_squeezed_block->next = next_free_block->next;
			new_squeezed_block->free = 1;

			if ( next_free_block->next != NULL ){
				next_free_block->next->prev = new_squeezed_block;
			}

			next_free_block->next = new_squeezed_block;

			next_free_block->size = size;

		}


#ifdef DEBUG_OUR_MALLOC
		//printf("MALLOC RETURNS OLD BLOCK, size: %zu, magic_number: %d\n",next_free_block->size, next_free_block->magic_number);
#endif

		next_free_block->free = 0; // this one is no longer free.

		////printf("%d\n", ((char*)next_free_block) + sizeof(meta_info));
		return (void*)next_free_block + sizeof(meta_info);

	}

}

#ifdef DEBUG_OUR_MALLOC
static void print_pointer_not_freed(){
	meta_info * walk = global_base;
	while ( walk != NULL ){
		if ( walk->free == 0 ){
			//printf("[%d]: %s", walk->magic_number, ((char*)walk) + sizeof(meta_info));
		} else {
			//printf("[%d] found free block here of size: %zu\n", walk->magic_number, walk->size);
		}


		walk = walk->next;
	}
}
#endif	


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
		return malloc(100); // lets say 10 is our minimum size object.
	}
	void * tmp = malloc(size);
	if ( tmp != ptr )
		memcpy(tmp, ptr, ((meta_info*)(ptr-sizeof(meta_info)))->size < size ? ((meta_info*)(ptr-sizeof(meta_info)))->size : size);
	free(ptr);



	return tmp;
}

void * realloc_old(void* ptr, size_t size){
	meta_info * block;
	size_t old_block_size;

#ifdef DEBUG_OUR_MALLOC
	//printf("REALLOC CALLED\n");
#endif
	if ( ptr == NULL ) {
		// realloc(NULL, size) should be identical to malloc(size)
		return malloc(size);
	} else if ( size == 0 ) {
		// If size is zero and ptr is not NULL, a new, minimum sized object is
    	// allocated and the original object is freed.
		free(ptr);
		return malloc(16); // lets say 10 is our minimum size object.
	} 

	block = (meta_info*) ((char*)ptr - sizeof(meta_info)); // might be undefined, if used badly!

	old_block_size = block->size;

/*	if ( block->size >= size ) {
		return ptr;
	} else {
		// We look at our neighbour upstais and see if its free

		meta_info * upper_block = block->next;

		if ( upper_block != NULL ) {

			if ( upper_block->free == 1 && upper_block->size + block->size + sizeof(meta_info) > size ) {

				block->next = block->next->next;

				if ( block->next->next != NULL )
					block->next->next->prev = block;

				return ptr;
			}

		} 

		// We found no help from our neighbour upstairs. We need to allocate and the memcpy.

		void * new_data = malloc(size);

		memcpy(new_data, ptr, block->size);

		free(ptr);

		return new_data;
	}
*/

	free(ptr);
	void * new_mem = malloc(size);

	if ( new_mem == NULL )
		return NULL;

	if ( ptr != new_mem )
		memcpy(new_mem, ptr, size < old_block_size ? size : old_block_size );

	return new_mem;

}


#ifdef DEBUG_OUR_MALLOC
/*
int main(int argc, char * argv[])
{
	// Experiments: Allocate and free strings..
	//printf("sizeof int: %zu\n", sizeof(int));
	//printf("sizeof meta_info: %zu\n", sizeof(meta_info));

	char * msg = malloc(100);

	sn//printf(msg, 100, "hejsan svejsan! :) \n");

	//printf("%s", msg);

	free(msg);

	char * msg2 = malloc(200);

	sn//printf(msg2, 200, "hejsan 1 :)\n");

	char * msg3 = malloc(200);

	sn//printf(msg3, 200, "hejsan 2 :)\n");

	free(msg2);

	char * msg4 = malloc(50);

	sn//printf(msg4, 50, "hej\n");

	char * msg5 = malloc(50);

	sn//printf(msg5, 50, "hej\n");

	print_pointer_not_freed();

	return 0;
}*/
#endif	
