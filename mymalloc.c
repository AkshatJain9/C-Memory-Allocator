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
  // printf("PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP\n");

  // printf("Finding Pointers for: %zu\n", (size_t) block);

  size_t bsize = block->size;
  assert(bsize > 0);

  // printf("BlockSize: %zu\n", (size_t) bsize);

  // Go to end of block, take away MetaBlock and Pointer to get to start of Pointer.
  PointerBlock* outpp = (PointerBlock*) (((size_t) block) + (block->size - sizeof(MetaBlock) - sizeof(PointerBlock)));

  // printf("Returning pointers at: %zu\n", (size_t) outpp);
  // printf("PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP\n");

  return outpp;
}

// Starting address of our heap, root
static MetaBlock* freeList = NULL;

static MetaBlock* lp = NULL;

// Initialise free block for all space as a single free block
MetaBlock* initialise() {
  // printf("IIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIII\n");
  // Allocating Initial Memory
  MetaBlock* init = mmap(NULL, kMemorySize, 
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);


  lp = init;
  // printf("Initialised Left FP at: %zu\n", (size_t) lp);
  // Init is left most, indicated by kMemorySize
  init->size = kMemorySize;

  // Go to the end, subtract Metablock for right_fp
  MetaBlock* right_fp = (MetaBlock*) (((size_t) init) + kMemorySize - sizeof(MetaBlock));
  right_fp->size = kMemorySize;

  // Freelist starts after left_fp
  init = (MetaBlock*) (((size_t) init) + sizeof(MetaBlock));
  init->size = kMemorySize - 2*sizeof(MetaBlock);
  // printf("Initialised FREELIST: %zu\n", (size_t) init);

  // Specify pointers, since it is free, both to NULL
  PointerBlock* init_ptr = getPointers(init);
  init_ptr->prev = NULL;
  init_ptr->next = NULL;


  // Add footer tag
  MetaBlock* freeListEnd = (MetaBlock*) (((size_t) init) + init->size - sizeof(MetaBlock));
  freeListEnd->size = kMemorySize - 2*sizeof(MetaBlock);

  // printf("IIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIII\n");
  return init;
}

MetaBlock* getRightPointer(MetaBlock* block) {
  if (block == NULL) {
    printf("The left boundary was null, so returning NULL\n");
    return NULL;
  }
  // printf("RRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRR\n");
  // printf("The block for which we are finding the right boundary: %zu\n", (size_t) block);

  size_t s = block->size;

  // printf("The size of this block: %zu\n", s);

  MetaBlock* new = (MetaBlock*) (((size_t) block) + s - sizeof(MetaBlock));

  // printf("The Right Boundary: %zu\n", (size_t) new);
  // printf("The current size of the right block: %zu\n", new->size);
  // printf("RRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRR\n");
  return new;
}

// Splits blocks into 2, updating size tag OF SECOND BLOCK ONLY
MetaBlock* splitBlock(MetaBlock* curr, size_t size) {
  // printf("SSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSS\n");
  // printf("Spliting block at %zu for size %zu\n", (size_t) curr, size);
  size_t size_before = curr->size;

  MetaBlock* newBlock = (MetaBlock*) (((size_t) curr) + size);

  newBlock->size = size_before - size;
  MetaBlock* newBlockRight = getRightPointer(newBlock);
  newBlockRight->size = size_before - size;

  // printf("SSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSS\n");

  return newBlock;
}



void *my_malloc(size_t size)
{ 
  // printf("-------------------------ENTERING MY MALLOC------------------\n");

  if (freeList == NULL) {
    freeList = initialise();
  }

  if (size == 0 || size > kMaxAllocationSize) {
    return NULL;
  }

  size = round_up(size + kMetaBlockSize, kAlignment);
  if (size < 40) {
    size = 40;
  }

  MetaBlock* curr = freeList;  

  // printf("FreeList is at: %zu\n", (size_t) freeList);
  // printf("Searching for size: %zu\n", size);

  // Keep going to next until big enough block is found
  while (curr->size < size) {
    PointerBlock* ptrs = getPointers(curr);

    // Traversed list, nothing found
    if (ptrs->next == NULL) {

      return NULL;
    }

    curr = ptrs->next;
  }
  // printf("Found block at: %zu\n", (size_t) curr);

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
      if (freeList == NULL) {
        printf("We ran out of room!\n");
      }
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
  // printf("FreeList is now: %zu\n", (size_t) freeList);
  // printf("FreeListSize: %zu\n", freeList->size);


  curr->size = size;
  MetaBlock* currRight = getRightPointer(curr);
  // printf("The Right Boundary for Allocated block: %zu\n", (size_t) currRight);
  curr->size = size + 1;
  currRight->size = size + 1; //
  MetaBlock* out = (MetaBlock*) (((size_t) curr) + sizeof(MetaBlock));
  // printf("Returning : %zu\n", (size_t) out);
  // printf("-------------------------EXITING MYMALLOC------------------\n");
  return out;

}

bool isAllocated(void* ptr) {
  MetaBlock* toRemove = (MetaBlock*) (((size_t) ptr) - sizeof(MetaBlock));
  return (toRemove->size % 2 == 1);
}


MetaBlock* coalesce(MetaBlock* curr) {
  MetaBlock* leftNeighbour = (MetaBlock*) (((size_t) curr) - sizeof(MetaBlock));
  MetaBlock* rightNeighbour = (MetaBlock*) (((size_t) curr) + curr->size);

  MetaBlock* root = curr;
  size_t newSize = curr->size;

  if (rightNeighbour->size < kMemorySize && !(rightNeighbour->size & 1)) {
    newSize += rightNeighbour->size;
  }

  if (leftNeighbour->size < kMemorySize && !(leftNeighbour->size & 1)) {
    newSize += leftNeighbour->size;
    root = (MetaBlock*) (((size_t) leftNeighbour) - leftNeighbour->size + sizeof(MetaBlock));
  }

  root->size = newSize;
  MetaBlock* coalescedRight = getRightPointer(root);
  coalescedRight->size = newSize;

  return root;
}


void my_free(void *ptr)
{
  // printf("**************************ENTERING MYFREE**********************\n");
  // printf("Seeking to free: %zu\n", (size_t) ptr);
  if (ptr == NULL || freeList == NULL || 
      ((size_t) ptr) < ((size_t) lp + sizeof(MetaBlock)) || 
      ((size_t) ptr) > (((size_t) lp) + kMemorySize - 4*sizeof(MetaBlock))
      || !isAllocated(ptr)) {
    errno = EINVAL;
    fprintf(stderr, "my_free: %s\n", strerror(errno));
    return;
  }

  MetaBlock* toRemove = (MetaBlock*) (((size_t) ptr) - sizeof(MetaBlock));
  
  // assert(toRemove != freeList);

  // printf("The block being cleared: %zu\n", (size_t) toRemove);
  // printf("The block being cleared's size: %zu\n", toRemove->size);

  // clear out last bit
  toRemove->size = toRemove->size - 1;
  MetaBlock* toRemoveRight = getRightPointer(toRemove);
  toRemoveRight->size = toRemoveRight->size - 1;

  toRemove = coalesce(toRemove);

  PointerBlock* toRemovePointers = getPointers(toRemove);
  toRemovePointers->prev = NULL;
  toRemovePointers->next = freeList;

  // printf("FreeList: %zu\n", (size_t) freeList);
  PointerBlock* freeListPointers = getPointers(freeList);
  // printf("FreeListPointers: %zu\n", (size_t) freeListPointers);

  freeListPointers->prev = toRemove;

  freeList = toRemove;
  // printf("FreeListSize After Freeing: %zu\n", freeList->size);
  // printf("**************************EXITING MYFREE**********************\n");
}