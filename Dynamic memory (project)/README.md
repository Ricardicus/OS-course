# dynamic memory implementation verified on gawk

The guru asked us to make our own implementation of 
malloc, realloc, free and calloc and test 
this by running the self tests of gawk.

The memory allocation techniques that were used were:

* The buddy system
* Linked list system
