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

// 为每个CPU维护独立的freelist和锁，减少锁竞争
// 每个CPU的锁名需要持久化存储
char kmem_lock_names[NCPU][8];

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

void
kinit()
{
  // 初始化每个CPU的锁，使用不同的名字以便调试
  // 锁名必须以"kmem"开头，用于统计跟踪
  for(int i = 0; i < NCPU; i++) {
    snprintf(kmem_lock_names[i], sizeof(kmem_lock_names[i]), "kmem%d", i);
    initlock(&kmem[i].lock, kmem_lock_names[i]);
  }
  freerange(end, (void*)PHYSTOP);
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

  // 关闭中断以安全地获取当前CPU ID
  push_off();
  int cpu = cpuid();
  
  // 将页面释放到当前CPU的freelist
  acquire(&kmem[cpu].lock);
  r->next = kmem[cpu].freelist;
  kmem[cpu].freelist = r;
  release(&kmem[cpu].lock);
  
  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  // 关闭中断以安全地获取当前CPU ID
  push_off();
  int cpu = cpuid();
  
  // 首先尝试从当前CPU的freelist分配
  acquire(&kmem[cpu].lock);
  r = kmem[cpu].freelist;
  if(r)
    kmem[cpu].freelist = r->next;
  release(&kmem[cpu].lock);

  // 如果当前CPU的freelist为空，尝试从其他CPU"偷取"
  if(!r) {
    for(int i = 0; i < NCPU; i++) {
      int victim = (cpu + 1 + i) % NCPU;
      if(victim == cpu)
        continue;
      
      acquire(&kmem[victim].lock);
      struct run *head = kmem[victim].freelist;
      if(head) {
        // 偷取整个链表，以最小化持有锁的时间
        kmem[victim].freelist = 0;
        release(&kmem[victim].lock);
        
        r = head;
        
        // 将剩余的页面加入当前CPU的freelist
        if(head->next) {
          acquire(&kmem[cpu].lock);
          kmem[cpu].freelist = head->next;
          release(&kmem[cpu].lock);
        }
        
        break;
      }
      release(&kmem[victim].lock);
    }
  }
  
  pop_off();

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
