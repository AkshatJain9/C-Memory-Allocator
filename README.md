# C Dynamic Memory Allocator

The porject implements the C functions `malloc()` and `free()` from direct Unix System calls and Manual Memory Management. The code for this is found in `mymalloc.c` and can be tested and benchmarked using the `test.py` and `bench.py` files respectively.


The program stores memory blocks in an Explicit Free List data structure. In addition, it has the following features;
- Constant Time Coalescing
- Reduced Meta-Data storage using Bit Manipulations
- Dynamic `mmap` additions for large memory blocks
- Custom error handling

This must be run on a Unix system or on Windows using WSL. To use this in a program, simply import mymalloc.c and call the functions `my_malloc()` and `my_free()`.