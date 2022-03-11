/*
 * Explicit Free List Memory Allocator
 *
 * This custom memory allocator creates and destroys blocks of memory for use by an outside
 * program. This is achieved using a block system that holds the size of the memory within the
 * block and whether or not it is allocated. This infomration is then used by the allocator
 * to find areas into which new data can be held.
 *
 * This allocator utilizes an Exlpicit Free List, meaning, it has a pointer to the most recently
 * available free block of memory. That block will also hold a pointer to the next (temporaly, not
 * spatially) free block and so on. This allows us to quickly jump between blocks to find one large
 * enough for the new data. This is called a "first-fit" algorithm.
 *
 * When memory is no longer needed, the outside program can free the space and this allocator will
 * mark the blocks as unallocated and coalesce them into unallocated neighbors to ensure the greatest
 * utilization of space. This applies for the Free List members as well. When the blocks are coalesced,
 * the pointers of the Free List are updated to keep a current list of availablity.
 *
 * When the allocator needs more memory, it requests a number of pages from the system and
 * initializes them as a new unit of the heap it is allocating. This ratio is carefully balanced as
 * too large will cause too much unused memory, but too small will cause constant calls to retrieve new
 * system pages. When a page (on our heap) is no longer in use the allocator will release it back to
 * the system.
 *
 * Author: Brighton Fox (u0981544)
 * Made with code from: Mu Zhang, Randal E. Bryant, and David R. O’Hallaron
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>

#include "mm.h"
#include "memlib.h"

/* always use 16-byte alignment */
#define ALIGNMENT 16

/* rounds up to the nearest multiple of ALIGNMENT */
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

/* rounds up to the nearest multiple of mem_pagesize() */
#define PAGEALIGN(size) (((size) + (mem_pagesize() - 1)) & ~(mem_pagesize() - 1))

// Header and Footer of each block need to be large enough to hold size (alongside the allocation bit)
typedef size_t blockHeader;
typedef size_t blockFooter;

// Each block will need to have room for the size of the size and allocation markers
#define OVERHEAD (sizeof(blockHeader) + sizeof(blockFooter))

// Given a payload pointer, get the header or footer pointer
#define HDRP(bp) ((char *)(bp) - sizeof(blockHeader))
#define FTRP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)) - OVERHEAD)

// Given a payload pointer, get the next or previous payload pointer
#define NEXT_BLKP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)))
#define PREV_BLKP(bp) ((char *)(bp)-GET_SIZE((char *)(bp)-OVERHEAD))

// Explicit Free List node struct, each should hold a pointer to the previous and next free nodes in the list
typedef struct eflNode
{
  struct eflNode *prev;
  struct eflNode *next;
} eflNode;

// each block must be big enough to hold the free list pointers to be useful as a free block
#define MINSIZE (sizeof(eflNode))

// Given a pointer to a header, get or set its value
#define GET(p) (*(size_t *)(p))
#define PUT(p, val) (*(size_t *)(p) = (val))

// Combine a size and alloc bit
#define PACK(size, alloc) ((size) | (alloc))

// Given a header pointer, get the alloc or size
#define GET_ALLOC(p) (GET(p) & 0x1)
#define GET_SIZE(p) (GET(p) & ~0xF)

// // Given a header pointer, get the alloc or size
// #define GET_ALLOC(p) ((block_header *)(p))->allocated
// #define GET_SIZE(p) ((block_header *)(p))->size

/* Set a block to allocated
 * Update block headers/footers as needed
 * Update free list if applicable
 * Split block if applicable
 */
static void setAllocated(void *b, size_t size);

/* Request more memory by calling mem_map
 * Initialize the new chunk of memory as applicable
 * Update free list if applicable
 */
static void extend(size_t s);

/* Coalesce a free block if applicable
 * Returns pointer to new coalesced block
 */
static void *coalesce(void *bp);

/* Add the newly freed block as a free node to the Explicit Free List
 */
static void addEFLNode(void *newFree);

/* Remove the newly allocated block as a free node from the Explicit Free List
 * Combine with its neighbors as neccessary
 */
static void removeEFLNode(void *oldFree);

// Pagesize cannot change while program is running, so store it here for easy use
#define PAGESIZE (mem_pagesize())

// Each Page will start with two sets of blockHeaders and blockFooters
#define PAGEOVERHEAD ((2 * sizeof(blockHeader)) + (2 * sizeof(blockFooter)))

// How many pages our allocator pages are actually mapped to (used to decrease the number of times real pages need to be fetched or freed)
#define PAGERATIO 10

// Used to always have a referece to the base page of memory
void *basePage;

// The address of the next free block of memory (most recently freed)
eflNode *nextFree;

/* Initialize the memory allocator by requesting from mem_map
 */
int mm_init(void)
{
  nextFree = NULL;
  basePage = NULL;
  extend(PAGESIZE);
  return 0;
}

/* Allocate the next available space in the heap that can fit the data (size) and return a pointer to that location
 * If no spaces large enough are avaialable, request more memory and try again
 */
void *mm_malloc(size_t size)
{
  // ignore spurious requests
  if (size == 0)
    return NULL;

  // must allocate at least enough for the free list pointers
  if (size <= MINSIZE)
    size = MINSIZE;

  size_t sizeAlign = ALIGN(size + OVERHEAD);

  // starting from nextFree, go down the list of free nodes until one that is large enough is found (first-fit)
  eflNode *avail = nextFree;
  while (avail != NULL)
  {
    if (GET_SIZE(HDRP(avail)) >= sizeAlign)
    {
      setAllocated(avail, sizeAlign);
      return avail;
    }
    avail = avail->next;
  }

  // no free block was large enough, we need to extend and try again with the newly created memory
  extend(sizeAlign);
  return mm_malloc(size);
}

/* Deallocate the block of memory in the heap at location *bp
 * Coalesce that block with tougching free blocks and add the whole as a new free node in the Free List
 * Release pages of memor that no longer hold any data
 */
void mm_free(void *bp)
{
  size_t size = GET_SIZE(HDRP(bp));

  // assert the the block is no longer allocated and allow the block to be coalesced
  PUT(HDRP(bp), PACK(size, 0));
  PUT(FTRP(bp), PACK(size, 0));
  bp = coalesce(bp);

  // size may have changed due to the coalescing
  size = GET_SIZE(HDRP(bp));

  // after unallocating, if the previous block only contains the Overhead information (page prolouge),
  // and the next block is size 0 (page epilouge), then the page is empty and can be unmapped
  if (GET_SIZE(HDRP(PREV_BLKP(bp))) == OVERHEAD && GET_SIZE(HDRP(NEXT_BLKP(bp))) == 0)
  {
    // get address of head of the page
    void *start = (void *)((unsigned long)bp - 2 * OVERHEAD);

    // unless it is our basePage
    if (start == basePage)
      return;

    // this memory is no longer available and should not be in the Free List
    removeEFLNode(bp);

    mem_unmap(start, size + PAGEOVERHEAD);
  }
}

/* Request more memory by calling mem_map
 * Initialize the new chunk of memory as applicable
 * Update free list if applicable
 */
static void extend(size_t size)
{
  // adjust size to a multiple of PAGESIZE and apply our ratio
  if (size % PAGESIZE != 0)
    size = PAGEALIGN(size);
  size *= PAGERATIO;

  // request size worth of bytes of pages from memory
  void *pageAdr = mem_map(size);

  // if no more memory is available, return
  if (pageAdr == NULL)
    return;

  // initialize our page
  // page prolouge (started half aligned into page to allow for alignemnt if needed)
  PUT(pageAdr + ALIGNMENT / 2, PACK(OVERHEAD, 1));
  PUT(pageAdr + ALIGNMENT / 2 + sizeof(blockHeader), PACK(OVERHEAD, 1));

  // page epilouge
  PUT(pageAdr + size - sizeof(blockFooter), PACK(0, 1));

  // initialize the rest of the page as a free block and add it to the free list
  PUT(pageAdr + ALIGNMENT / 2 + OVERHEAD, PACK(size - (PAGEOVERHEAD), 0));
  PUT(FTRP(pageAdr + OVERHEAD + sizeof(blockHeader)), PACK(size - (PAGEOVERHEAD), 0));
  addEFLNode(pageAdr + PAGEOVERHEAD);

  // first extend special case, assign basePage
  if (basePage == NULL)
    basePage = pageAdr;

  return;
}

/* Set a block to allocated
 * Update block headers/footers as needed
 * Update free list if applicable
 * Split block if applicable
 */
static void setAllocated(void *bp, size_t size)
{
  // verify that we have enough room in the page to split the free block
  size_t available = GET_SIZE(HDRP(bp));
  if (available - size >= PAGEOVERHEAD)
  {
    // assign the block as allocated and remove it as a free node
    PUT(HDRP(bp), PACK(size, 1));
    PUT(FTRP(bp), PACK(size, 1));
    removeEFLNode(bp);

    // move past the newly allocated memory and reassign what is left as a free node
    bp = NEXT_BLKP(bp);
    PUT(HDRP(bp), PACK(available - size, 0));
    PUT(FTRP(bp), PACK(available - size, 0));
    addEFLNode(bp);
  }

  // the split free block would be too small, so no need to add it to the list
  else
  {
    // assign the block as allocated and remove it as a free node
    PUT(HDRP(bp), PACK(available, 1));
    PUT(FTRP(bp), PACK(available, 1));
    removeEFLNode(bp);
  }
  return;
}

/* Coalesce a free block if applicable
 * Returns pointer to new coalesced block
 */
static void *coalesce(void *bp)
{
  /**************************************************************************************
   *    This code is based heavily off of the material in the textbook, attributed here
   *    Title: code/vm/malloc/mm.c
   *    Authors: Randal E. Bryant, David R. O’Hallaron
   *    Date: 11/22/21
   *    Availability: Computer Systems A Programmer’s Perspective 3rd Edition
   **************************************************************************************/

  size_t prevAlloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));
  size_t nextAlloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
  size_t size = GET_SIZE(HDRP(bp));

  // no free neighbors
  if (prevAlloc && nextAlloc)
  {
    addEFLNode(bp);
  }

  // next block is free, so delete its free node, combine the blocks, and recreate a free node here
  else if (prevAlloc && !nextAlloc)
  {
    size += GET_SIZE(HDRP(NEXT_BLKP(bp)));
    removeEFLNode(NEXT_BLKP(bp));
    PUT(HDRP(bp), PACK(size, 0));
    PUT(FTRP(bp), PACK(size, 0));
    addEFLNode(bp);
  }

  // previous block is free, simply combine as the previous block will have a free node already
  else if (!prevAlloc && nextAlloc)
  {
    size += GET_SIZE(HDRP(PREV_BLKP(bp)));
    PUT(FTRP(bp), PACK(size, 0));
    PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
    bp = PREV_BLKP(bp);
  }

  // both neighbors are free, prevois will have a free node, so combine all three and delete the free node in the next bloxk
  else
  {
    size += GET_SIZE(HDRP(PREV_BLKP(bp))) + GET_SIZE(HDRP(NEXT_BLKP(bp)));
    removeEFLNode(NEXT_BLKP(bp));
    PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
    PUT(FTRP(PREV_BLKP(bp)), PACK(size, 0));
    bp = PREV_BLKP(bp);
  }

  return bp;
}

/* Add the newly freed block as a free node to the Explicit Free List
 */
static void addEFLNode(void *newFree_)
{
  // cast as a free node
  eflNode *newFree = (eflNode *)newFree_;

  // if the free list is already started, set the new free node to point to the next available fre
  // node and vice-versa, then reassign the next free node pointer to the new node, this keeps the most recently
  // freed nodes at the top of the list, which improves throughput and makes code simpler
  if (nextFree != NULL)
  {
    newFree->prev = NULL;
    newFree->next = nextFree;
    nextFree->prev = newFree;
    nextFree = newFree;
  }

  // special case for no free nodes, simply create this one
  else
  {
    nextFree = newFree;
    nextFree->next = NULL;
    nextFree->prev = NULL;
  }
}

/* Remove the newly allocated block as a free node from the Explicit Free List
 * Combine with its neighbors as neccessary
 */
static void removeEFLNode(void *oldFree_)
{
  // cast as a free node
  eflNode *oldFree = (eflNode *)oldFree_;

  // exists between two other free nodes in the list
  if (oldFree->prev != NULL && oldFree->next != NULL)
  {
    // reassign the neighbors to bypass this node by pointing to each other
    oldFree->prev->next = oldFree->next;
    oldFree->next->prev = oldFree->prev;
  }

  // oldest free block (end of the free list)
  else if (oldFree->prev != NULL)
  {
    // reassign neighbor to bypass this node
    oldFree->prev->next = NULL;
  }

  // newest free block (nextFree)
  else if (oldFree->next != NULL)
  {
    // reassign nextFree and bypass this node
    nextFree = oldFree->next;
    nextFree->prev = NULL;
  }

  // only free block in the list, delete pointer
  else
  {
    nextFree = NULL;
  }
}