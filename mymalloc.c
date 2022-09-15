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

// Starting address of our heap, root
static MetaBlock* freeListArray[8];

// Align sizes for faster operations
inline static size_t round_up(size_t size, size_t alignment) {
  const size_t mask = alignment - 1;
  return (size + mask) & ~mask;
}

/*
* Given a (presumed) free block's LEFT metadata block, finds the pointer block
*/
PointerBlock* getPointers(MetaBlock* block) {
  if (block == NULL) {
    return NULL;
  }

  // Go to end of block, take away MetaBlock and Pointer to get to start of Pointer.
  PointerBlock* outpp = (PointerBlock*) (((size_t) block) + (block->size - sizeof(MetaBlock) - sizeof(PointerBlock)));

  return outpp;
}



// Initialise free block for all space as a single free block
MetaBlock* initialise(int m) {
  // printf("IIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIII\n");
  // Allocating Initial Memory
  MetaBlock* init = mmap(NULL, m*ARENA_SIZE, 
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);


  // Init is left most, indicated by kMemorySize
  init->size = kMemorySize;

  // Go to the end, subtract Metablock for right_fp
  MetaBlock* right_fp = (MetaBlock*) (((size_t) init) + m*ARENA_SIZE - sizeof(MetaBlock));
  right_fp->size = kMemorySize;

  // Freelist starts after left_fp
  init = (MetaBlock*) (((size_t) init) + sizeof(MetaBlock));
  init->size = m*ARENA_SIZE - 2*sizeof(MetaBlock);

  // Specify pointers, since it is free, both to NULL
  PointerBlock* init_ptr = getPointers(init);
  init_ptr->prev = NULL;
  init_ptr->next = NULL;


  // Add footer tag
  MetaBlock* freeListEnd = (MetaBlock*) (((size_t) init) + init->size - sizeof(MetaBlock));
  freeListEnd->size = m*ARENA_SIZE - 2*sizeof(MetaBlock);

  // printf("IIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIII\n");
  return init;
}

/*
* Given the left metadata block, returns the right metadata block
* Assumes that the block is unallocated
*/
MetaBlock* getRightMetaBlock(MetaBlock* block) {
  if (block == NULL) {
    return NULL;
  }

  // Adding size to go to end, then taking away MetaBlock size
  MetaBlock* new = (MetaBlock*) (((size_t) block) + block->size - sizeof(MetaBlock));
  return new;
}

/*
* Splits a large block "curr" into 2 blocks, where "size" is the size of the
* first block. Only updates the tags of the second block
*/
MetaBlock* splitBlock(MetaBlock* curr, size_t size) {
  size_t size_before = curr->size;

  // Finding second block
  MetaBlock* newBlock = (MetaBlock*) (((size_t) curr) + size);

  // Updating boundary tags for second block only
  newBlock->size = size_before - size;
  MetaBlock* newBlockRight = getRightMetaBlock(newBlock);
  newBlockRight->size = size_before - size;

  return newBlock;
}

int getIndex(size_t size) {
  int out = 0;
  size_t pow = 64;

  while (pow <= size) {
    pow *= 2;
    out++;
  }

  if (out > 7) {
    return 7;
  }

  return out;
}


void *my_malloc(size_t size)
{ 
  // Checking is size is valid
  if (size == 0 || size > kMaxAllocationSize) {
    return NULL;
  }

  // Aligning and providing minimum for size
  size = round_up(size + kMetaBlockSize, kAlignment);
  if (size < 32) {
    size = 32;
  }

  int idx = getIndex(size);


  if (freeListArray[idx] == NULL) {
    int multiple = size/ARENA_SIZE + 1;
    freeListArray[idx] = initialise(multiple);
  }

  MetaBlock* curr = freeListArray[idx];


  // Keep going to next until big enough block is found
  while (curr != NULL && curr->size < size) {
    PointerBlock* ptrs = getPointers(curr);
    curr = ptrs->next;
  }

  // Traversed list, nothing found
  if (curr == NULL) {
    int multiple = size/ARENA_SIZE + 1;
    MetaBlock* toInsert = initialise(multiple);

    PointerBlock* currentFreeListPtrs = getPointers(freeListArray[idx]);
    PointerBlock* newlyAllocatedPtrs = getPointers(toInsert);
    currentFreeListPtrs->prev = toInsert;
    newlyAllocatedPtrs->next = freeListArray[idx];

    freeListArray[idx] = toInsert;
    curr = toInsert;
  }


  MetaBlock* secondBlock = NULL;
  // If needed, the split block will become new root
  if (curr->size >= size + kPointerBlockSize + kMinAllocationSize) {
    secondBlock = splitBlock(curr, size);
  }

  if (curr == freeListArray[idx]) {
    PointerBlock* currPointers = getPointers(curr);
    MetaBlock* newRoot = currPointers->next;
    PointerBlock* newRootPointers = getPointers(newRoot);
    if (secondBlock != NULL) {
      if (newRootPointers != NULL) {
        newRootPointers->prev = secondBlock;
      }
      freeListArray[idx] = secondBlock;
    } else {
      if (newRoot == NULL) {
        if (idx == 7) {
          newRoot = initialise(2);
        } else {
          newRoot = initialise(1);
        }
      } else {
        newRootPointers->prev = NULL;
      }
      freeListArray[idx] = newRoot;
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


  curr->size = size;
  MetaBlock* currRight = getRightMetaBlock(curr);

  curr->size = size + 1;
  currRight->size = size + 1; //
  MetaBlock* out = (MetaBlock*) (((size_t) curr) + sizeof(MetaBlock));


  return out;

}

bool isAllocated(void* ptr) {
  MetaBlock* toRemove = (MetaBlock*) (((size_t) ptr) - sizeof(MetaBlock));
  return (toRemove->size % 2 == 1);
}

/*
* Coalese Function, takes in address of Central MetaBlock, then 
* checks left & right neighbours for combination
* Does NOT update the pointers, only header/footer tags
*/
MetaBlock* coalesce(MetaBlock* curr) {
  // Obtaining start address of adjacent MetaBlocks
  MetaBlock* leftNeighbour = (MetaBlock*) (((size_t) curr) - sizeof(MetaBlock));
  MetaBlock* rightNeighbour = (MetaBlock*) (((size_t) curr) + curr->size);

  // Storing new root and size, may be unchanged
  MetaBlock* root = curr;
  size_t newSize = curr->size;

  // Checking if NOT fence post and unallocated
  if (rightNeighbour->size < kMemorySize && !(rightNeighbour->size & 1)) {
    newSize += rightNeighbour->size;
  }

  // Reassigning left root if left block is free
  if (leftNeighbour->size < kMemorySize && !(leftNeighbour->size & 1)) {
    newSize += leftNeighbour->size;
    root = (MetaBlock*) (((size_t) leftNeighbour) - leftNeighbour->size + sizeof(MetaBlock));
  }

  // Updating Boundary Tags
  root->size = newSize;
  MetaBlock* coalescedRight = getRightMetaBlock(root);
  coalescedRight->size = newSize;

  return root;
}

bool isInitialised() {
  for (int i = 0; i < 8; i++) {
    if (freeListArray[i]) {
      return true;
    }
  }
  return false;
}


void my_free(void *ptr)
{
  if (ptr == NULL || !isInitialised() || !isAllocated(ptr)) {
    errno = EINVAL;
    fprintf(stderr, "my_free: %s\n", strerror(errno));
    return;
  }

  MetaBlock* toRemove = (MetaBlock*) (((size_t) ptr) - sizeof(MetaBlock));

  // clear out last bit
  toRemove->size = toRemove->size - 1;
  int idx = getIndex(toRemove->size);
  MetaBlock* toRemoveRight = getRightMetaBlock(toRemove);
  toRemoveRight->size = toRemoveRight->size - 1;

  toRemove = coalesce(toRemove);

  PointerBlock* toRemovePointers = getPointers(toRemove);
  toRemovePointers->prev = NULL;
  toRemovePointers->next = freeListArray[idx];

  PointerBlock* freeListPointers = getPointers(freeListArray[idx]);

  freeListPointers->prev = toRemove;

  freeListArray[idx] = toRemove;
}