#ifndef BRK_H
#define BRK_H

#include <mach/mach.h>		/* for vm_allocate, vm_offset_t */
#include <mach/vm_statistics.h>
#include <errno.h>

void *sbrk(int);
void *brk(void *);

#endif