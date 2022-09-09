#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/mman.h>

#include "mymalloc.h"


// Header and Footer Block for size, last bit is allocation
typedef struct MetaBlock {
  size_t size;
} MetaBlock;

// Pointer Block to link next/prev free block
typedef struct PointerBlock {
  MetaBlock* prev;
  MetaBlock* next;
} PointerBlock;


// Size of meta-data per free block
const size_t kPointerBlockSize = 2*sizeof(MetaBlock) + sizeof(PointerBlock);

// Size of meta-data per allocated block
const size_t kMetaBlockSize = kPointerBlockSize - sizeof(PointerBlock);

// Maximum allocation size (16 MB)
const size_t kMaxAllocationSize = (16ull << 20) - kMetaBlockSize;

// Memory size that is mmapped (64 MB)
const size_t kMemorySize = (16ull << 22);

// Align sizes for faster operations
inline static size_t round_up(size_t size, size_t alignment) {
  const size_t mask = alignment - 1;
  return (size + mask) & ~mask;
}

PointerBlock* getPointers(MetaBlock* block) {
  if (block == NULL) {
    return NULL;
  }
  // Go to end of block, take away MetaBlock and Pointer to get to start of Pointer.
  // printf("The current block is: %zu\n", (size_t) block);
  // printf("The current block's size is: %zu\n", block->size);
  size_t p = ((size_t) block) + (block->size - sizeof(MetaBlock) - sizeof(PointerBlock));
  // printf("The pointer for this block is: %zu\n", p);
  return (PointerBlock*) p;
}

// Starting address of our heap
static MetaBlock* freeList = NULL;

// Initialise free block for all space as a single free block
MetaBlock* initialise() {
  // Allocating Initial Memory
  MetaBlock* init = mmap(NULL, kMemorySize, 
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
  
  // Init is left most, indicated by kMemorySize
  init->size = kMemorySize;

  // Go to the end, subtract Metablock for right_fp
  MetaBlock* right_fp = (MetaBlock*) (((size_t) init) + kMemorySize - sizeof(MetaBlock));
  right_fp->size = kMemorySize;

  // Freelist starts after left_fp
  init = (MetaBlock*) (((size_t) init) + sizeof(MetaBlock));
  init->size = kMemorySize - 2*sizeof(MetaBlock);

  // Specify pointers, since it is free, both to NULL
  PointerBlock* init_ptr = getPointers(init);
  init_ptr->prev = NULL;
  init_ptr->next = NULL;

  // Add footer tag
  MetaBlock* freeListEnd = (MetaBlock*) (((size_t) init) + init->size - sizeof(MetaBlock));
  freeListEnd->size = kMemorySize - 2*sizeof(MetaBlock);

  return init;
}

MetaBlock* getRightPointer(MetaBlock* block) {
  if (block == NULL) {
    return NULL;
  }
  size_t s = block->size;

  MetaBlock* new = (MetaBlock*) (((size_t) block) + s - sizeof(MetaBlock));
  return new;
}

// Splits blocks into 2, updating size tags, NOT updating pointers, assumes first to be allocated
MetaBlock* splitBlock(MetaBlock* curr, size_t size) {
  size_t size_before = curr->size;

  MetaBlock* newBlock = (MetaBlock*) (((size_t) curr) + size);

  newBlock->size = size_before - size;
  curr->size = size | 0b1;

  MetaBlock* currRight = getRightPointer(curr);
  MetaBlock* newBlockRight = getRightPointer(newBlock);

  newBlockRight->size = size_before - size;
  currRight->size = size | 0b1;

  return newBlock;
}



void *my_malloc(size_t size)
{
  if (freeList == NULL) {
    freeList = initialise();
  }

  if (size == 0 || size > kMaxAllocationSize) {
    printf("Not a valid size!");
    return NULL;
  }

  size = round_up(size + kMetaBlockSize, kAlignment);

  MetaBlock* curr = freeList;

  // Keep going to next until big enough block is found
  while (curr->size < size) {
    PointerBlock* ptrs = getPointers(curr);

    // Traversed list, nothing found
    if (ptrs->next == NULL) {
      return NULL;
    }

    curr = ptrs->next;
  }

  MetaBlock* secondBlock = NULL;
  // If needed, the split block will become new root
  if (curr->size >= size + kPointerBlockSize + kMinAllocationSize) {
    secondBlock = splitBlock(curr, size);
  }

  if (curr == freeList) {
    PointerBlock* currPointers = getPointers(curr);
    MetaBlock* newRoot = currPointers->next;
    PointerBlock* newRootPointers = getPointers(newRoot);
    if (secondBlock != NULL) {
      if (newRootPointers != NULL) {
        newRootPointers->prev = secondBlock;
      }
      freeList = secondBlock;
    } else {
      if (newRootPointers != NULL) {
        newRootPointers->prev = NULL;
      }
      freeList = newRoot;
    }
  } else {
    PointerBlock* currPointers = getPointers(curr);
    MetaBlock* nextNode = currPointers->next;
    MetaBlock* prevNode = currPointers->prev;

    PointerBlock* nextNodePointers = getPointers(nextNode);
    PointerBlock* prevNodePointers = getPointers(prevNode);

    if (secondBlock != NULL) {
      if (nextNodePointers != NULL) {
        nextNodePointers->prev = secondBlock;
      }
      if (prevNodePointers != NULL) {
        prevNodePointers->next = secondBlock;
      }
    } else {
      if (nextNodePointers != NULL) {
        nextNodePointers->prev = prevNode;
      }
      if (prevNodePointers != NULL) {
        prevNodePointers->next = nextNode;
      }
    }
  }
  MetaBlock* out = (MetaBlock*) (((size_t) curr) + sizeof(MetaBlock));
  return out;
}

void my_free(void *ptr)
{
  if (ptr == NULL || freeList == NULL || 
      (size_t) ptr < (size_t) freeList || 
      (size_t) ptr >= ((size_t) freeList + kMemorySize - 2*sizeof(MetaBlock))) {
    errno = EINVAL;
    fprintf(stderr, "my_free: %s\n", strerror(errno));
    return;
  }

  MetaBlock* toRemove = (MetaBlock*) (((size_t)ptr) - sizeof(MetaBlock));

  PointerBlock* toRemovePointers = getPointers(toRemove);

  toRemovePointers->prev = NULL;
  toRemovePointers->next = freeList;

  PointerBlock* freeListPointers = getPointers(freeList);
  freeListPointers->prev = toRemove;

  freeList = toRemove;

}
