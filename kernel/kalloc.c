// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

#ifdef LAB_PGTBL
#define SUPERPOOL_PAGES 8   // 预留的超页数量，够实验使用即可

struct superrun {
  struct superrun *next;
};

static void superfreerange(void *pa_start, void *pa_end);
void superfree(void *pa);

struct {
  struct spinlock lock;
  struct superrun *freelist;
} superkmem;
#endif

void
kinit()
{
  initlock(&kmem.lock, "kmem");
#ifdef LAB_PGTBL
  initlock(&superkmem.lock, "superkmem");
  uint64 super_end = SUPERPGROUNDDOWN(PHYSTOP);
  uint64 super_start = PHYSTOP;
  uint64 min_start = SUPERPGROUNDUP((uint64)end);
  uint64 reserve = (uint64)SUPERPOOL_PAGES * SUPERPGSIZE;
  if(super_end > min_start && reserve > 0) {
    if(super_end > reserve + min_start)
      super_start = super_end - reserve;
    else
      super_start = min_start;
    superfreerange((void*)super_start, (void*)super_end);
  }
  freerange(end, (void*)super_start);
  if(super_start < super_end && super_end < PHYSTOP)
    freerange((void*)super_end, (void*)PHYSTOP);
#else
  freerange(end, (void*)PHYSTOP);
#endif
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

#ifdef LAB_PGTBL
static void
superfreerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)SUPERPGROUNDUP((uint64)pa_start);
  for(; p + SUPERPGSIZE <= (char*)pa_end; p += SUPERPGSIZE)
    superfree(p);
}

void
superfree(void *pa)
{
  struct superrun *r;

  if(((uint64)pa % SUPERPGSIZE) != 0 || (char*)pa < end || (uint64)pa + SUPERPGSIZE > PHYSTOP)
    panic("superfree");

  memset(pa, 1, SUPERPGSIZE);
  r = (struct superrun*)pa;

  acquire(&superkmem.lock);
  r->next = superkmem.freelist;
  superkmem.freelist = r;
  release(&superkmem.lock);
}

void *
superalloc(void)
{
  struct superrun *r;

  acquire(&superkmem.lock);
  r = superkmem.freelist;
  if(r)
    superkmem.freelist = r->next;
  release(&superkmem.lock);

  if(r)
    memset((char*)r, 5, SUPERPGSIZE);
  return (void*)r;
}
#endif
