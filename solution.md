# Lab: Lock

## 1. 内存分配器 (Memory Allocator)

### 问题描述
原始的 xv6 内存分配器使用一个全局的空闲链表 (`freelist`) 和一把全局锁 (`kmem.lock`)。当多个 CPU 同时进行内存分配或释放时，会产生严重的锁竞争，导致性能下降。`kalloctest` 测试显示了这种竞争。

### 解决方案
为了减少锁竞争，我们采用 **Per-CPU Freelists** 的策略。每个 CPU 维护自己独立的空闲链表和锁。
- 当 CPU 需要分配内存时，优先从本地链表获取，无需竞争全局锁。
- 当 CPU 释放内存时，将其归还到本地链表。
- 当本地链表为空时，CPU 需要从其他 CPU 的链表中"偷取" (steal) 内存。

### 代码修改

#### `kernel/kalloc.c`

1.  **数据结构修改**：将全局的 `kmem` 修改为数组，为每个 CPU 分配独立的锁和链表。
    ```c
    // kernel/kalloc.c
    struct {
      struct spinlock lock;
      struct run *freelist;
    } kmem[NCPU];
    ```

2.  **初始化 (`kinit`)**：初始化所有 CPU 的锁。
    ```c
    // kernel/kalloc.c
    void
    kinit()
    {
      for(int i = 0; i < NCPU; i++) {
        snprintf(kmem_lock_names[i], sizeof(kmem_lock_names[i]), "kmem%d", i);
        initlock(&kmem[i].lock, kmem_lock_names[i]);
      }
      freerange(end, (void*)PHYSTOP);
    }
    ```

3.  **释放内存 (`kfree`)**：将内存释放到当前 CPU 的链表中。
    ```c
    // kernel/kalloc.c
    void
    kfree(void *pa)
    {
      // ... (检查代码) ...
      
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
    ```

4.  **分配内存 (`kalloc`)**：优先从本地分配，失败则从其他 CPU 偷取。为了最大程度减少锁竞争，每次偷取**整个链表**。这样可以使持有 victim 锁的时间降到最低（O(1)），避免了遍历链表带来的开销。
    ```c
    // kernel/kalloc.c
    void *
    kalloc(void)
    {
      struct run *r;
      push_off();
      int cpu = cpuid();
      
      // 1. 尝试从本地分配
      acquire(&kmem[cpu].lock);
      r = kmem[cpu].freelist;
      if(r)
        kmem[cpu].freelist = r->next;
      release(&kmem[cpu].lock);

      // 2. 本地为空，尝试偷取
      if(!r) {
        for(int i = 0; i < NCPU; i++) {
          int victim = (cpu + 1 + i) % NCPU;
          if(victim == cpu) continue;
          
          acquire(&kmem[victim].lock);
          struct run *head = kmem[victim].freelist;
          if(head) {
            // 偷取整个链表，最小化持有锁时间
            kmem[victim].freelist = 0;
            release(&kmem[victim].lock);
            
            r = head;
            
            // 将剩余页面加入本地链表
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
      // ...
    }
    ```

---

## 2. 读写锁 (Read-Write Lock)

### 问题描述
某些数据结构（如全局 `ticks` 变量）读多写少。使用普通的互斥锁（Spinlock）会导致多个读者无法并发读取，降低性能。

### 解决方案
实现读写锁（Read-Write Lock），允许多个读者同时持有锁，但写者必须独占锁。为了防止写者饥饿（Starvation），实现**写者优先**策略：一旦有写者等待，后续读者必须等待。

### 代码修改

#### `kernel/spinlock.h`

定义读写锁结构体：
```c
// kernel/spinlock.h
struct rwspinlock {
  struct spinlock lock;     // 内部自旋锁，保护结构体字段
  int readers;              // 当前读者数量
  int writer_waiting;       // 等待的写者数量 (用于写者优先)
  int writer;               // 是否有写者持有锁
};
```

#### `kernel/spinlock.c`

实现读写锁的操作函数：

1.  **初始化 (`initrwlock`)**
    ```c
    void initrwlock(struct rwspinlock *rwlk) {
      initlock(&rwlk->lock, "rwlock");
      rwlk->readers = 0;
      rwlk->writer_waiting = 0;
      rwlk->writer = 0;
    }
    ```

2.  **获取读锁 (`read_acquire`)**
    ```c
    void read_acquire(struct rwspinlock *rwlk) {
      acquire(&rwlk->lock);
      // 写者优先：如果有写者在等待或持有锁，读者必须等待
      while(rwlk->writer_waiting > 0 || rwlk->writer) {
        release(&rwlk->lock);
        acquire(&rwlk->lock);
      }
      rwlk->readers++;
      release(&rwlk->lock);
    }
    ```

3.  **释放读锁 (`read_release`)**
    ```c
    void read_release(struct rwspinlock *rwlk) {
      acquire(&rwlk->lock);
      rwlk->readers--;
      release(&rwlk->lock);
    }
    ```

4.  **获取写锁 (`write_acquire`)**
    ```c
    void write_acquire(struct rwspinlock *rwlk) {
      acquire(&rwlk->lock);
      rwlk->writer_waiting++; // 标记有写者等待
      
      // 等待所有读者和其他写者释放
      while(rwlk->readers > 0 || rwlk->writer) {
        release(&rwlk->lock);
        acquire(&rwlk->lock);
      }
      
      rwlk->writer_waiting--;
      rwlk->writer = 1;
      release(&rwlk->lock);
    }
    ```

5.  **释放写锁 (`write_release`)**
    ```c
    void write_release(struct rwspinlock *rwlk) {
      acquire(&rwlk->lock);
      rwlk->writer = 0;
      release(&rwlk->lock);
    }
    ```

#### `kernel/defs.h`
添加了上述函数的声明。

## 3. 缓冲区缓存 (Buffer Cache)

### 问题描述
原始的 xv6 缓冲区缓存使用一个全局锁 `bcache.lock` 来保护所有的缓冲区操作。当多个进程并发访问文件系统时，这个全局锁成为瓶颈，导致严重的锁竞争。`bcachetest` 测试显示了这种竞争。

### 解决方案
使用**哈希表**来分片管理缓冲区缓存。将缓冲区根据 `blockno` 哈希到不同的桶 (Bucket) 中，每个桶有自己的锁。
- `bget` 操作只需要获取对应桶的锁。
- 只有在需要从其他桶"偷取"缓冲区（Eviction）时，才需要获取其他桶的锁。

### 代码修改

#### `kernel/bio.c`

1.  **数据结构修改**：定义哈希表，包含多个桶，每个桶有独立的锁和链表。
    ```c
    #define NBUCKET 13
    #define HASH(blockno) (blockno % NBUCKET)

    struct {
      struct spinlock lock;
      struct buf head;
    } hashtable[NBUCKET];
    ```

2.  **初始化 (`binit`)**：初始化每个桶的锁和链表，并将所有缓冲区初始分配到桶 0。注意锁名字缓冲区的大小，防止截断警告。
    ```c
    void binit(void) {
      struct buf *b;
      char lockname[16]; // 足够大以容纳 "bcache12"

      for(int i = 0; i < NBUCKET; i++) {
        snprintf(lockname, sizeof(lockname), "bcache%d", i);
        initlock(&hashtable[i].lock, lockname);
        // 初始化链表头...
      }
      
      // 将所有 buffer 放入 bucket 0
      for(b = bcache.buf; b < bcache.buf+NBUF; b++){
        b->next = hashtable[0].head.next;
        b->prev = &hashtable[0].head;
        initsleeplock(&b->lock, "buffer");
        hashtable[0].head.next->prev = b;
        hashtable[0].head.next = b;
      }
    }
    ```

3.  **获取缓冲区 (`bget`)**：
    - 计算哈希值，获取对应桶的锁。
    - 在桶内查找缓存的块。
    - 如果未找到，在桶内查找空闲缓冲区（LRU）。
    - **Stealing**: 如果桶内无空闲，释放当前锁，遍历其他桶进行"偷取"。
        - 依次获取其他桶的锁。
        - 查找该桶中引用计数为 0 的缓冲区。
        - 如果找到，将其从该桶移除，释放该桶的锁。
        - 重新获取目标桶的锁，将缓冲区插入目标桶。
        - 注意：在重新获取锁后，需要再次检查块是否已被其他进程加载（Race Condition）。

4.  **释放缓冲区 (`brelse`)**：
    - 计算哈希值，获取对应桶的锁。
    - 将缓冲区移动到桶内链表的头部（MRU）。
    - 释放锁。

## 总结

本次实验主要涉及操作系统中的**并发控制**和**锁机制**。

1.  **Per-CPU 内存分配器**：通过将共享资源（全局空闲链表）拆分为每个 CPU 私有的资源，消除了大部分情况下的锁竞争。只有在本地资源耗尽时才需要访问其他 CPU 的资源（Stealing），这是一种典型的**空间换时间**和**分散竞争**的优化策略。**Steal All** 策略进一步将跨 CPU 锁竞争的临界区缩小到 O(1)，极大地提高了并发性能。

2.  **读写锁**：针对读多写少的场景，提供了比互斥锁更细粒度的并发控制。通过区分读者和写者，提高了系统的并发度。同时，通过引入 `writer_waiting` 计数器实现了**写者优先**，解决了在读者源源不断的情况下写者可能永远无法获取锁（饥饿）的问题。

3.  **Buffer Cache 优化**：通过哈希分桶（Hash Bucketing）技术，将单一的全局锁拆分为多个细粒度的锁。这使得对不同数据块的访问可以并行进行，极大地减少了锁竞争。这是**细粒度锁 (Fine-grained Locking)** 的典型应用。
