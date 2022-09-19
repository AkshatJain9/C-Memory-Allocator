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

// Dummy value for size used in Fence-Posts
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

/*
* Initialise free block for all space as a single free block
*/
MetaBlock* initialise(int m) {
  // Allocating Initial Memory
  MetaBlock* init = mmap(NULL, m*ARENA_SIZE, 
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);

  if (init == NULL) {
    return NULL;
  }

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

/* 
* Return the index of the freeList array representing which free list a block
* will go in. Assumes a max size of 8 (index 7)
*/
int getIndex(size_t size) {
  // First bin is for <64 bytes, store initial index for this
  int out = 0;
  size_t pow = 64;

  // Each bin is determined by powers of two, so double and incr. Limit to 4096
  while (out < 7 && pow <= size) {
    pow *= 2;
    out++;
  }

  return out;
}

/*
* Given a size, allocates memory using mmap and returns starting address
*/
void *my_malloc(size_t size)
{ 
  // Checking is size is valid
  if (size == 0 || size > kMaxAllocationSize) {
    return NULL;
  }

  // Aligning and providing minimum for size
  size = round_up(size + kMetaBlockSize, kAlignment);
  if (size < kPointerBlockSize) {
    size = kPointerBlockSize;
  }

  int idx = getIndex(size);

  // If the relevant free list doesn't exist, initialise it. x2+ if idx=7
  if (freeListArray[idx] == NULL) {
    int multiple = size/ARENA_SIZE + 1;
    freeListArray[idx] = initialise(multiple);
    if (freeListArray[idx] == NULL) {
      errno = ENOMEM;
      exit(1);
    }
  }

  MetaBlock* curr = freeListArray[idx];

  // Keep going to next until big enough block is found
  while (curr != NULL && curr->size < size) {
    PointerBlock* ptrs = getPointers(curr);
    curr = ptrs->next;
  }

  // Traversed list, nothing found
  if (curr == NULL) {
    // Request additional memory from OS if we have no room
    int multiple = size/ARENA_SIZE + 1;
    MetaBlock* toInsert = initialise(multiple);

    // Get pointer of current head a newly allocated blocks
    PointerBlock* currentFreeListPtrs = getPointers(freeListArray[idx]);
    PointerBlock* newlyAllocatedPtrs = getPointers(toInsert);

    // Map relationship, making newly mapped area new root
    currentFreeListPtrs->prev = toInsert;
    newlyAllocatedPtrs->next = freeListArray[idx];

    // Store in FreeList array and continue with my_malloc
    freeListArray[idx] = toInsert;
    curr = toInsert;
  }

  MetaBlock* secondBlock = NULL;

  // If needed, the split block will become new root
  if (curr->size >= size + kPointerBlockSize + kMinAllocationSize) {
    secondBlock = splitBlock(curr, size);
  }

  // Calculating new root and/or updating freelist pointers
  if (curr == freeListArray[idx]) {
    // If the first block in free list was deemed appropriate, we need new root
    PointerBlock* currPointers = getPointers(curr);
    MetaBlock* newRoot = currPointers->next;
    PointerBlock* newRootPointers = getPointers(newRoot);
    // If there is a split block, we can just make that the root
    if (secondBlock != NULL) {
      if (newRootPointers != NULL) {
        newRootPointers->prev = secondBlock;
      }
      freeListArray[idx] = secondBlock;
    } else {
      // Otherwise we need to find another block
      if (newRoot == NULL) {
        // If there is no "next" element, then we need to make one
        // If idx is 7, we need at least 2*4096 for any allocation otherwise, 1x
        if (idx == 7) {
          newRoot = initialise(2);
        } else {
          newRoot = initialise(1);
        }
      } else {
        // Otherwise, simply point the next block's pointers to NULL
        newRootPointers->prev = NULL;
      }
      // And in either case, that becomes the new root
      freeListArray[idx] = newRoot;
    }
  } else {
    // In the case that the found block isnt the root, get the next and prev blocks
    PointerBlock* currPointers = getPointers(curr);
    MetaBlock* nextNode = currPointers->next;
    MetaBlock* prevNode = currPointers->prev;

    // Get the pointers of the next and prev blocks
    PointerBlock* nextNodePointers = getPointers(nextNode);
    PointerBlock* prevNodePointers = getPointers(prevNode);

    // If block was split, we update pointers to point to this
    if (secondBlock != NULL) {
      if (nextNodePointers != NULL) {
        nextNodePointers->prev = secondBlock;
      }
      if (prevNodePointers != NULL) {
        prevNodePointers->next = secondBlock;
      }
    } else {
      // Otherwise, the next and prev now point to each other
      if (nextNodePointers != NULL) {
        nextNodePointers->prev = prevNode;
      }
      if (prevNodePointers != NULL) {
        prevNodePointers->next = nextNode;
      }
    }
  }

  // Set size, so we can go to right block properly
  curr->size = size;

  // Set allocated bit on both header and footer boundary tags
  MetaBlock* currRight = getRightMetaBlock(curr);
  curr->size = size + 1;
  currRight->size = size + 1;

  MetaBlock* out = (MetaBlock*) (((size_t) curr) + sizeof(MetaBlock));

  memset(out, 0, size - kMetaBlockSize);

  return out;
}

/*
* Assumes pointer represents start of block, checks if allocation bit is set
*/
bool isAllocated(void* ptr) {
  MetaBlock* toRemove = (MetaBlock*) (((size_t) ptr) - sizeof(MetaBlock));
  return (toRemove->size % 2 == 1);
}

/*
* Coalese Function, takes in address of Central MetaBlock, then 
* checks left & right neighbours for combination
* Updates the pointers a free-list as well if needed
*/
void coalesce(MetaBlock* curr, int idx) {
  // Obtaining start address of adjacent MetaBlocks
  MetaBlock* leftNeighbour = (MetaBlock*) (((size_t) curr) - sizeof(MetaBlock));
  MetaBlock* rightNeighbour = (MetaBlock*) (((size_t) curr) + curr->size);

  // Storing new root and size, may be unchanged
  MetaBlock* root = curr;
  size_t newSize = curr->size;

  // Keeping track of pointers in case they need to be shuffled around
  PointerBlock* leftPointers = NULL;
  PointerBlock* rightPointers = NULL;

  // Checking if NOT fence post and unallocated
  if (rightNeighbour->size < kMemorySize && !(rightNeighbour->size & 1)) {
    newSize += rightNeighbour->size;
    rightPointers = getPointers(rightNeighbour);
  }

  // Reassigning left root if left block is free
  if (leftNeighbour->size < kMemorySize && !(leftNeighbour->size & 1)) {
    newSize += leftNeighbour->size;
    MetaBlock* leftNeighbourHeader = (MetaBlock*) (((size_t) leftNeighbour) - leftNeighbour->size + sizeof(MetaBlock));
    leftPointers = getPointers(leftNeighbourHeader);

    root = leftNeighbourHeader;
  }

  // Now handling the 4 cases we have in coalescing
  root->size = newSize;
  MetaBlock* coalescedRight = getRightMetaBlock(root);
  coalescedRight->size = newSize;
  
  // If both left and right were allocated, insert block at the start of freelist
  if ((!leftPointers && !rightPointers)) {
    // Update pointers to match format of freelist root
    PointerBlock* rootPointers = getPointers(root);
    rootPointers->prev = NULL;
    rootPointers->next = freeListArray[idx];

    // Update freeList, so toRemove is now root
    PointerBlock* freeListPointers = getPointers(freeListArray[idx]);
    freeListPointers->prev = root;

    freeListArray[idx] = root;
    return;
  }

  // If left block ONLY was free, simply copy over its pointers to current block
  if (leftPointers && !rightPointers) {
    // Copy over pointers to root
    PointerBlock* rootPointers = getPointers(root);
    rootPointers->prev = leftPointers->prev;
    rootPointers->next = leftPointers->next;

    return;
  }

  // If right block ONLY was free, copy over pointers AND update adjacent pointers
  if (!leftPointers && rightPointers) {
    // Copy over pointes to root
    PointerBlock* rootPointers = getPointers(root);
    rootPointers->prev = rightPointers->prev;
    rootPointers->next = rightPointers->next;

    // Get the pointer blocks of next and prev in free list
    PointerBlock* rightPrevPointers = getPointers(rightPointers->prev);
    PointerBlock* rightNextPointers = getPointers(rightPointers->next);

    // Make sure they point to the new root (which is more left) now
    if (rightPrevPointers) {
      rightPrevPointers->next = root;
    }

    if (rightNextPointers) {
      rightNextPointers->prev = root;
    }

    // If this block was the freelist, we also need to update that
    if (rightNeighbour == freeListArray[idx]) {
      freeListArray[idx] = root;
    }

    return;
  }

  // Now in the case that both right and left blocks were free

  // We decide to copy over left block pointers
  PointerBlock* rootPointers = getPointers(root);
  rootPointers->prev = leftPointers->prev;
  rootPointers->next = leftPointers->next;

  // Get pointers of right block
  MetaBlock* rightPrev = rightPointers->prev;
  MetaBlock* rightNext = rightPointers->next;

  // We DELETE the right block from the Free-List, since we will already have it
  if (rightPrev != NULL) {
    PointerBlock* temp = getPointers(rightPrev);
    temp->next = rightNext;
  }

  if (rightNext) {
    PointerBlock* temp2 = getPointers(rightNext);
    temp2->prev = rightPrev;
  }

  // Again, if this deleted node was the root, we update the root
  if (rightNeighbour == freeListArray[idx]) {
      freeListArray[idx] = root;
  }

}


/*
* Checks if any allocations have actually been done, for free to continue
*/
bool isInitialised() {
  for (int i = 0; i < 8; i++) {
    if (freeListArray[i]) {
      return true;
    }
  }
  return false;
}


/*
* Given a pointer, assumed to be start of allocated block, frees that block
* and re-inserts it into the relevant free-list
*/
void my_free(void *ptr)
{
  // If pointer is NULL/allocated, or nothing has been allocated, throw error
  if (ptr == NULL || !isInitialised() || !isAllocated(ptr)) {
    errno = EINVAL;
    fprintf(stderr, "my_free: %s\n", strerror(errno));
    exit(1);
  }

  // Block to be removed if criteria is met
  MetaBlock* toRemove = (MetaBlock*) (((size_t) ptr) - sizeof(MetaBlock));

  // Clear out last bit to properly get index and right block
  toRemove->size = toRemove->size - 1;

  // Get index of freeList and Right Block, update right block to be unallocated
  int idx = getIndex(toRemove->size);
  MetaBlock* toRemoveRight = getRightMetaBlock(toRemove);
  toRemoveRight->size = toRemoveRight->size - 1;

  // Coalesce, updating new root among 3 coninuous blocks
  coalesce(toRemove, idx);
}