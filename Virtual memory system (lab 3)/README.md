# Pretty awesome lab of virtual memory

The guru at the institution asked us to implement virtual memory to used when parsing
his assembly programs. In this example it is a simple program tha computes factorials (12! in this case).
When the program has finished the result is displayed in register R3.

# pagefault handling

We replaced RAM pages with a simple fifo algo and a slightly better second change replace algorithm.
