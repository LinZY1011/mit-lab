#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

#ifdef LAB_NET
  // PCI-E ECAM (configuration space), for pci.c
  kvmmap(kpgtbl, 0x30000000L, 0x30000000L, 0x10000000, PTE_R | PTE_W);

  // pci.c maps the e1000's registers here.
  kvmmap(kpgtbl, 0x40000000L, 0x40000000L, 0x20000, PTE_R | PTE_W);
#endif  

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// Initialize the one kernel_pagetable
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

static pte_t *walk_internal(pagetable_t pagetable, uint64 va, int alloc, int *level_out);
#ifdef LAB_PGTBL
static pte_t *walk_to_level(pagetable_t pagetable, uint64 va, int target_level, int alloc);
static int mappages_super(pagetable_t pagetable, uint64 va, uint64 pa, int perm);
static int split_superpage(pagetable_t pagetable, uint64 va, pte_t *pte);
static void vmprint_walk(pagetable_t pagetable, int level, uint64 base);
#endif

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  return walk_internal(pagetable, va, alloc, 0);
}

static pte_t *
walk_internal(pagetable_t pagetable, uint64 va, int alloc, int *level_out)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
#ifdef LAB_PGTBL
      if(PTE_LEAF(*pte)) {
        if(level_out)
          *level_out = level;
        return pte;
      }
#endif
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  if(level_out)
    *level_out = 0;
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;
#ifdef LAB_PGTBL
  int level = 0;
#endif

  if(va >= MAXVA)
    return 0;

#ifdef LAB_PGTBL
  pte = walk_internal(pagetable, va, 0, &level);
#else
  pte = walk_internal(pagetable, va, 0, 0);
#endif
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
#ifdef LAB_PGTBL
  if(level == 1)
    pa += (va & (SUPERPGSIZE - 1));
#endif
  return pa;
}


// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a = va;
  uint64 sz;
  uint64 end = va + npages*PGSIZE;
  pte_t *pte;
#ifdef LAB_PGTBL
  int level;
#endif

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  while(a < end){
#ifdef LAB_PGTBL
    if((pte = walk_internal(pagetable, a, 0, &level)) == 0)
      panic("uvmunmap: walk");
#else
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
#endif
    if((*pte & PTE_V) == 0) {
      printf("va=%ld pte=%ld\n", a, *pte);
      panic("uvmunmap: not mapped");
    }
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
#ifdef LAB_PGTBL
    if(level == 1){
      // 命中 2MB 超页，根据区间范围决定整块释放还是拆成 4KB 页
      uint64 chunk_start = SUPERPGROUNDDOWN(a);
      uint64 chunk_off = a - chunk_start;
      uint64 bytes_left = end - a;
      if(chunk_off == 0 && bytes_left >= SUPERPGSIZE){
        sz = SUPERPGSIZE;
      } else {
        if(split_superpage(pagetable, a, pte) < 0)
          panic("uvmunmap: split superpage");
        continue;
      }
    } else {
      sz = PGSIZE;
    }
#else
    sz = PGSIZE;
#endif
    if(do_free){
      uint64 pa = PTE2PA(*pte);
#ifdef LAB_PGTBL
      if(level == 1)
        superfree((void*)pa);
      else
        kfree((void*)pa);
#else
      kfree((void*)pa);
#endif
    }
    *pte = 0;
    a += sz;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvmfirst(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("uvmfirst: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}


// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;
  int sz;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += sz){
    sz = PGSIZE;
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
#ifndef LAB_SYSCALL
    memset(mem, 0, sz);
#endif
    if(mappages(pagetable, a, sz, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

#ifdef LAB_PGTBL
// 针对 sbrk 的专用增长函数，优先尝试分配 2MB 超页
uint64
uvmalloc_sbrk(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  while(a < newsz){
    if((a % SUPERPGSIZE) == 0 && (newsz - a) >= SUPERPGSIZE){
      void *chunk = superalloc();
      if(chunk){
        memset(chunk, 0, SUPERPGSIZE);
        if(mappages_super(pagetable, a, (uint64)chunk, PTE_R|PTE_W|PTE_U) != 0){
          superfree(chunk);
          uvmdealloc(pagetable, a, oldsz);
          return 0;
        }
        a += SUPERPGSIZE;
        continue;
      }
      // superalloc 失败时退化为 4KB 分配，保证 sbrk 语义
    }

    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
#ifndef LAB_SYSCALL
    memset(mem, 0, PGSIZE);
#endif
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_W|PTE_U) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    a += PGSIZE;
  }

  return newsz;
}
#endif

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa;
  uint64 va = 0;
  uint64 mapped = 0;
  uint flags;
  char *mem;
#ifdef LAB_PGTBL
  int level;
#endif

  while(va < sz){
#ifdef LAB_PGTBL
    if((pte = walk_internal(old, va, 0, &level)) == 0)
      panic("uvmcopy: pte should exist");
#else
    if((pte = walk(old, va, 0)) == 0)
      panic("uvmcopy: pte should exist");
#endif
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
#ifdef LAB_PGTBL
    if(level == 1){
      void *chunk = superalloc();
      if(chunk == 0)
        goto err;
      memmove(chunk, (char*)pa, SUPERPGSIZE);
      if(mappages_super(new, va, (uint64)chunk, flags & ~PTE_V) != 0){
        superfree(chunk);
        goto err;
      }
      va += SUPERPGSIZE;
      mapped = va;
      continue;
    }
#endif
    mem = kalloc();
    if(mem == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, va, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
    va += PGSIZE;
    mapped = va;
  }
  return 0;

 err:
  if(mapped)
    uvmunmap(new, 0, PGROUNDUP(mapped)/PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if (va0 >= MAXVA)
      return -1;
    if((pte = walk(pagetable, va0, 0)) == 0) {
      // printf("copyout: pte should exist 0x%x %d\n", dstva, len);
      return -1;
    }


    // forbid copyout over read-only user text pages.
    if((*pte & PTE_W) == 0)
      return -1;
    
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;
  
  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}


#ifdef LAB_PGTBL
void
vmprint(pagetable_t pagetable)
{
  printf("page table %p\n", pagetable);
  vmprint_walk(pagetable, 2, 0);
}

static void
vmprint_walk(pagetable_t pagetable, int level, uint64 base)
{
  if(level < 0)
    return;

  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) == 0)
      continue;
    uint64 va = base | ((uint64)i << PXSHIFT(level));
    int indent = 3 - level;
    for(int d = 0; d < indent; d++)
      printf(" ..");
    printf("%p: pte %p pa %p\n", (void*)va, (void*)pte, (void*)PTE2PA(pte));
    if((pte & (PTE_R|PTE_W|PTE_X)) == 0 && level > 0) {
      vmprint_walk((pagetable_t)PTE2PA(pte), level - 1, va);
    }
  }
}

pte_t*
pgpte(pagetable_t pagetable, uint64 va) {
  return walk(pagetable, va, 0);
}

static pte_t *
walk_to_level(pagetable_t pagetable, uint64 va, int target_level, int alloc)
{
  if(target_level < 0 || target_level > 2)
    panic("walk_to_level");

  for(int level = 2; level > target_level; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      if(PTE_LEAF(*pte))
        panic("walk_to_level: leaf");
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(target_level, va)];
}

static int
mappages_super(pagetable_t pagetable, uint64 va, uint64 pa, int perm)
{
  if((va % SUPERPGSIZE) != 0 || (pa % SUPERPGSIZE) != 0)
    panic("mappages_super: not aligned");

  pte_t *pte = walk_to_level(pagetable, va, 1, 1);
  if(pte == 0)
    return -1;
  if(*pte & PTE_V)
    panic("mappages_super: remap");
  *pte = PA2PTE(pa) | perm | PTE_V;
  return 0;
}

static int
split_superpage(pagetable_t pagetable, uint64 va, pte_t *pte)
{
  (void)pagetable;
  (void)va;
  uint64 pa = PTE2PA(*pte);
  uint64 flags = PTE_FLAGS(*pte);
  pagetable_t l0 = (pagetable_t)kalloc();
  if(l0 == 0)
    return -1;
  memset(l0, 0, PGSIZE);

  for(int i = 0; i < 512; i++){
    char *mem = kalloc();
    if(mem == 0){
      for(int j = 0; j < i; j++){
        if(l0[j] & PTE_V)
          kfree((void*)PTE2PA(l0[j]));
      }
      kfree((void*)l0);
      return -1;
    }
    // 拆分时直接把 2MB 数据拷贝到新分配的 4KB 物理页
    memmove(mem, (void*)(pa + i*PGSIZE), PGSIZE);
    l0[i] = PA2PTE((uint64)mem) | (flags & 0x3FF);
  }

  superfree((void*)pa);
  *pte = PA2PTE((uint64)l0) | PTE_V;
  return 0;
}
#endif
