# Lab: Lock

本实验的核心目标是：在多核环境下，通过**重新设计内核中的数据结构与加锁策略**，降低锁竞争、提升并行度。主要改动点包括：

- 物理内存分配器（`kalloc`/`kfree`）：把“单锁 + 单链表”的全局设计拆成 per-CPU 结构，并实现“偷内存”。
- 块缓存（`bcache`）：把保护所有缓存块的全局大锁拆成按桶划分的多把细粒度锁。
- 读写锁：为读多写少的共享数据提供比普通自旋锁并发度更高的访问方式。

下面按实验要求，分模块详细说明**问题背景与测试程序、实现目标与约束、具体设计与关键代码、以及涉及到的操作系统与锁的概念**。

---

## 1. 内存分配器 (Memory Allocator)

### 问题描述与实验要求

原始的 xv6 物理内存分配器在 [kernel/kalloc.c](kernel/kalloc.c) 中，使用：

- 一个全局空闲链表 `freelist` 表示所有可分配的物理页；
- 一把全局自旋锁 `kmem.lock` 来保护这条链表。

在多核场景下，所有 CPU 在执行 `kalloc()` / `kfree()` 时都必须争用同一把锁，典型症状是：

- 在 `user/kalloctest` 中，三个进程并发不断扩大/缩小地址空间，频繁触发系统调用 `sbrk` → 内核多次调用 `kalloc/kfree`；
- 由于大量 CPU 同时竞争 `kmem.lock`，`acquire()` 内部自旋循环的计数（`#test-and-set`）很高，说明锁高度竞争；
- kalloctest 的 test1/test3 会根据 kmem 锁的 test-and-set 次数判断是否“FAIL”。

实验对内存分配器的具体要求：

- 将单一的 `freelist` 改造为 **每个 CPU 一个 freelist**，每个 freelist 都有独立的自旋锁，锁名必须以 `"kmem"` 开头（便于统计）；
- 在**绝大部分分配、释放路径**上，不再需要不同 CPU 之间争用同一把锁，从而显著降低锁竞争；
- 当某个 CPU 的 freelist 为空，而其他 CPU 仍有空闲页时，需要实现**“偷 (steal)”** 机制，从其它 CPU 的 freelist 中搬运一批页；
- 改造后要保持语义不变：
  - `kalloc()` 仍然返回一页对齐的、未使用的物理内存；
  - `kfree()` 后的页面可以被其他 CPU 再次分配；
  - `usertests sbrkmuch` 可以成功把物理内存几乎全部分配出去并正确回收；
- `kalloctest` 的锁统计中，kmem 相关的 `#test-and-set` 数量应接近 0，总和低于参考输出，且 `usertests -q` 全部通过。

从操作系统角度看，这一部分主要练习：

- **Per-CPU 数据结构**：降低跨 CPU 共享的频率，把大部分操作限制在本地 CPU；
- **锁竞争与可伸缩性 (scalability)**：通过拆分锁、缩短临界区来减少自旋；
- **中断与 CPU ID**：`cpuid()` 只有在关中断 (`push_off/pop_off`) 的情况下才是安全的，这体现了**内核中断上下文与并发执行的交互**；
- **工作窃取 (stealing)** 思想在内核资源分配中的应用。

### 解决方案设计概述

整体思路是实现 **Per-CPU Freelists + Steal All**：

- 每个 CPU 有自己独立的 `{lock, freelist}`，互不共享；
- 普通分配/释放操作只在**本 CPU freelist** 上加锁，几乎不会与其他 CPU 冲突；
- 只有当本地 freelist 耗尽时，才会短暂持有其它 CPU 的锁，把对方 freelist 中的一整条链表“偷到”本地，这个跨 CPU 访问非常少见；
- 为了把 victim CPU 被占用的时间降到最低，采用 **“偷整条链表”** 的策略，使得在 victim 上持锁时间为 O(1)，而不是遍历链表 O(n)。

这种设计利用了锁的两个关键性质：

- **互斥性**：确保每条 freelist 在任意时刻只被一个 CPU 修改；
- **临界区越小，竞争越少**：通过 per-CPU + O(1) 窃取，缩小临界区长度和出现频率。

### 代码修改与关键点

#### (1) 数据结构修改：per-CPU kmem

在 [kernel/kalloc.c](kernel/kalloc.c) 中，把原先的 `kmem` 改为数组：

```c
// kernel/kalloc.c
struct {
  struct spinlock lock;   // 保护本 CPU 的 freelist
  struct run *freelist;   // 本 CPU 的空闲物理页链表
} kmem[NCPU];

// 可选：存放锁名字符串
char kmem_lock_names[NCPU][16];
```

这里的 `NCPU` 来自 [kernel/param.h](kernel/param.h)，表示系统最大 CPU 数量。

#### (2) 初始化 (`kinit`)

`kinit()` 在启动早期由单个 CPU 调用，此时仍可以安全地把所有空闲页放到这个 CPU 的 freelist 中。初始化逻辑：

```c
// kernel/kalloc.c
void
kinit()
{
  for(int i = 0; i < NCPU; i++) {
    snprintf(kmem_lock_names[i], sizeof(kmem_lock_names[i]), "kmem%d", i);
    initlock(&kmem[i].lock, kmem_lock_names[i]);
    kmem[i].freelist = 0;
  }

  // 让 freerange 把所有空闲内存页面交给当前 CPU
  freerange(end, (void*)PHYSTOP);
}
```

`freerange` 内部调用 `kfree`，由于此时只在一个 CPU 上执行，所以所有页面最终都会进入当前 CPU 的 freelist。这样一开始所有内存集中在一个 CPU，随后再通过“偷”机制逐步扩散到其它 CPU。

#### (3) 释放内存 (`kfree`)：释放到当前 CPU freelist

`kfree()` 的关键在于：**如何安全地获取当前 CPU 号，并只操作本 CPU 的 freelist**：

```c
// kernel/kalloc.c
void
kfree(void *pa)
{
  struct run *r;

  // 省略对 pa 对齐范围等的检查...

  r = (struct run*)pa;

  // 关闭中断以安全地使用 cpuid()
  push_off();
  int id = cpuid();

  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);

  pop_off();
}
```

这里使用 `push_off/pop_off` 防止在 `cpuid()` 与使用其返回值之间发生中断切换到其他 CPU，保证“当前 CPU”概念的一致性，这是本实验中**CPU 与中断子系统交互**的一个重要点。

#### (4) 分配内存 (`kalloc`)：本地优先 + Steal All

`kalloc()` 的核心流程：

1. 关闭中断，获取当前 CPU id；
2. 尝试从本地 freelist 分配；
3. 如果本地为空，则在其他 CPU 上按顺序尝试“偷”整条链表；
4. 把偷来的链表头返回给调用者，其余结点挂回本地 freelist。

实现示意：

```c
// kernel/kalloc.c
void *
kalloc(void)
{
  struct run *r = 0;

  push_off();
  int id = cpuid();

  // 1. 本地分配
  acquire(&kmem[id].lock);
  r = kmem[id].freelist;
  if(r)
    kmem[id].freelist = r->next;
  release(&kmem[id].lock);

  // 2. 本地为空，尝试从其它 CPU 偷整条链表
  if(r == 0){
    for(int i = 0; i < NCPU; i++){
      int victim = (id + 1 + i) % NCPU;
      if(victim == id)
        continue;

      acquire(&kmem[victim].lock);
      struct run *head = kmem[victim].freelist;
      if(head){
        // 从 victim 偷走整条链表
        kmem[victim].freelist = 0;
        release(&kmem[victim].lock);

        // 拿走第一个页面作为返回值
        r = head;
        struct run *rest = head->next;

        // 剩余页面挂回本地 freelist
        if(rest){
          acquire(&kmem[id].lock);
          rest->next = kmem[id].freelist;
          kmem[id].freelist = rest;
          release(&kmem[id].lock);
        }
        break;
      }
      release(&kmem[victim].lock);
    }
  }

  pop_off();

  if(r)
    memset((char*)r, 5, PGSIZE); // 与原实现一致，填充调试模式

  return (void*)r;
}
```

这里体现了几个和锁相关的重要实践：

- 窃取时**不同时持有两把 kmem 锁**（先锁 victim，拿走链表再解锁），避免死锁；
- 只在 victim 上执行 O(1) 操作，快速释放 victim 的锁，减小对 victim CPU 的干扰；
- 尽量缩小临界区，把链表拆分/连接操作局限在短路径中。

运行 `kalloctest` 可以看到 kmem 系列锁的 `#test-and-set` 数量大幅下降，说明锁竞争显著减少；再用 `usertests sbrkmuch` 和 `usertests -q` 验证功能正确性。

---

## 2. 读写锁 (Read-Write Lock)

### 问题描述

在 xv6 中存在一些**读多写少**的共享数据，例如：

- 全局时钟 `ticks`；
- 某些统计信息、只在少数路径中更新、在多数路径中读取的数据。

如果仍然用普通自旋锁 (spinlock) 来保护这类数据：

- 所有读操作和写操作都要互斥执行；
- 即使有很多读者可以安全地并发访问，也会被互斥锁“强行串行化”；
- 在多核环境下，这导致 CPU 无谓等待，降低系统吞吐量。

读写锁 (Read-Write Lock, RWLock) 的目的就是：

- **多个读者之间可以并发**；
- **读写、写写之间仍然互斥**；
- 通过策略控制避免饥饿，例如“写者优先”。

实验中，我们实现一个基于自旋锁的读写锁，并采用**写者优先策略**：一旦有写者等待，后续读者必须排队等待，以避免写者一直被大量读者“饿死”。

### 解决方案与锁性质

设计目标：

- 使用一把内部自旋锁保护读写锁内部状态（计数器等）；
- 通过计数器区分“读者个数 / 是否有写者持锁 / 是否有写者等待”；
- 读锁获取时：在没有写者持锁且没有写者等待时立即成功，否则自旋；
- 写锁获取时：在所有读者退出且没有其他写者占有时才成功；
- 写者优先：一旦有写者等待，新的读者即使看到当前没有写者持锁，也必须等待。

这体现了锁的三个重要性质：

- **互斥性**：同一时刻最多只有 1 个写者；
- **共享性**：在没有写者时，允许多个读者同时持锁，提升并发度；
- **公平性/优先级策略**：通过 `writer_waiting` 计数器实现写者优先，减少写者饥饿风险。

### 代码修改

#### `kernel/spinlock.h`

定义读写锁结构体：

```c
// kernel/spinlock.h
struct rwspinlock {
  struct spinlock lock;   // 内部自旋锁，保护下面的计数器
  int readers;            // 当前持有读锁的读者数量
  int writer_waiting;     // 正在等待获取写锁的写者数量
  int writer;             // 是否有写者持有锁 (0/1)
};
```

#### `kernel/spinlock.c`

实现读写锁的操作函数：

1. **初始化 (`initrwlock`)**

   ```c
   void
   initrwlock(struct rwspinlock *rwlk)
   {
     initlock(&rwlk->lock, "rwlock");
     rwlk->readers = 0;
     rwlk->writer_waiting = 0;
     rwlk->writer = 0;
   }
   ```

2. **获取读锁 (`read_acquire`)**

   核心是“写者优先”：只要有写者在等待或当前有写者持锁，读者就必须等待：

   ```c
   void
   read_acquire(struct rwspinlock *rwlk)
   {
     acquire(&rwlk->lock);
     // 写者优先：有写者等待或写者持锁时，读者不能进入
     while(rwlk->writer_waiting > 0 || rwlk->writer){
       release(&rwlk->lock);
       acquire(&rwlk->lock);
     }
     rwlk->readers++;
     release(&rwlk->lock);
   }
   ```

3. **释放读锁 (`read_release`)**

   只需在内部自旋锁保护下把读者计数减一：

   ```c
   void
   read_release(struct rwspinlock *rwlk)
   {
     acquire(&rwlk->lock);
     rwlk->readers--;
     release(&rwlk->lock);
   }
   ```

4. **获取写锁 (`write_acquire`)**

   写者获取锁时：

   - 先把 `writer_waiting` 加一，声明“有写者在排队”；
   - 等待所有读者退出且没有写者占有；
   - 拿到锁后把 `writer_waiting` 减一，并设置 `writer=1`：

   ```c
   void
   write_acquire(struct rwspinlock *rwlk)
   {
     acquire(&rwlk->lock);
     rwlk->writer_waiting++;

     // 等待所有读者和其他写者
     while(rwlk->readers > 0 || rwlk->writer){
       release(&rwlk->lock);
       acquire(&rwlk->lock);
     }

     rwlk->writer_waiting--;
     rwlk->writer = 1;
     release(&rwlk->lock);
   }
   ```

5. **释放写锁 (`write_release`)**

   写者释放时清除 `writer` 标记，唤醒后续读/写者竞争：

   ```c
   void
   write_release(struct rwspinlock *rwlk)
   {
     acquire(&rwlk->lock);
     rwlk->writer = 0;
     release(&rwlk->lock);
   }
   ```

#### `kernel/defs.h`

在 [kernel/defs.h](kernel/defs.h) 中为上述函数添加声明，便于在需要的模块中使用。

通过这个读写锁的实现，可以直观感受到：

- 在读多写少的场景，多个 CPU 可以**并行读取**共享数据，而不会因为互斥锁而完全串行化；
- 同时也能通过“写者优先”保证写操作不会被饿死，体现了**锁的公平性与策略设计**在操作系统中的重要性。

## 3. 缓冲区缓存 (Buffer Cache)

### 问题描述

xv6 的块缓存 (buffer cache) 位于 [kernel/bio.c](kernel/bio.c)，主要负责：

- 把磁盘上的块缓存在内存中，避免频繁访问慢速磁盘；
- 通过引用计数 `b->refcnt` 追踪每个缓冲块是否正在被使用；
- 使用 `bcache.lock` 保护以下共享状态：
  - 所有缓存块组成的双向链表（LRU 近似策略）；
  - 每个缓冲块的 `dev`、`blockno`、`refcnt`；
  - 链表头部/尾部等元数据。

问题在于：

- 所有对块缓存的操作（查找、插入、释放）都要持有**同一把 `bcache.lock`**；
- 当多个进程并发访问不同文件、不同块时，本来可以并发的操作被全局锁串行化；
- `user/bcachetest` 中的 test0/后续测试特意构造高并发文件系统访问，导致 `bcache.lock` 的 `#test-and-set` 数变得很大，成为 top 5 contended locks 之一，测试因此 FAIL。

实验的主要要求：

- 设计一种新的块缓存结构，使得：
  - 在 **不同块号** 的并发访问场景下，几乎不会在同一把锁上发生竞争；
  - 仍然满足“同一 (dev, blockno) 在缓存中最多只有一份”的一致性约束；
  - 缓冲块数量仍然为 NBUF（30），不能简单通过“加缓存数量”掩盖问题；
  - 在缓存命中时，查找应尽量只加一个局部锁，而不是全局锁；
  - 在缓存未命中时，可以串行地选择“任一 `refcnt==0` 的块”进行替换（不要求保持 LRU）。
- 所有与 bcache 相关的自旋锁名字必须以 `"bcache"` 开头，便于测试统计；
- 运行 `bcachetest` 时，所有 bcache 相关锁的 `#test-and-set` 和尽量接近 0，总和小于 500；
- 保证 `usertests -q` 仍然全部通过。

从操作系统角度，这一部分训练了：

- **共享缓存数据结构的并发访问**（磁盘块缓存）；
- **细粒度锁 (fine-grained locking)**、**分片 (sharding)** 与哈希表技术减少锁冲突；
- **保持全局不变式 (at most one cached copy)** 与本地并发之间的权衡；
- 在不增加资源数量前提下，通过调整结构/锁策略提升系统伸缩性。

### 解决方案设计概述

整体思路：

- 使用 **哈希表 (多个桶 bucket)** 来管理块缓存；
- 每个桶有自己的自旋锁和双向链表，分别管理哈希到该桶的所有 `struct buf`；
- `bget()` 查找某个 `(dev, blockno)` 时，仅需持有对应桶的锁；
- `brelse()` 释放某个块时，只需持有该块所在桶的锁；
- 当桶内找不到空闲块时，可以：
  - 或者串行地在所有桶中扫描 `refcnt==0` 的块并迁移到目标桶；
  - 或者在一个简化的全局路径下选一个可用块再放入目标桶（该部分即使有少量冲突也被测试允许）。

这样做，把原先“一个全局锁保护所有块”的粗粒度设计，改成“每个桶一把锁”的细粒度设计，使得大部分并发访问都只在小范围内竞争。

### 代码修改

#### (1) 数据结构修改：哈希桶

在 [kernel/bio.c](kernel/bio.c) 中定义桶结构和哈希函数，例如：

```c
#define NBUCKET 13
#define HASH(blockno) ((blockno) % NBUCKET)

struct {
  struct spinlock lock;  // 保护该桶的链表与 buf 元数据
  struct buf head;       // 该桶的双向链表哨兵结点
} hashtable[NBUCKET];
```

选择 13 这样的质数作为桶数可以减少哈希冲突的概率。每个桶中的 `head` 是一个哨兵结点，其 `next/prev` 把本桶中所有缓存块（`struct buf`）连成循环双向链表。

#### (2) 初始化 (`binit`)

原始的 `binit()` 会：

- 初始化 `bcache.lock` 和全局双向链表 `bcache.head`；
- 把所有 `bcache.buf` 串在一条全局链表上；
- 为每个 `struct buf` 初始化 sleep lock `b->lock`。

改造后：

- 保留 `bcache` 里 NBUF 个 `struct buf` 的分配方式；
- 取消全局 `bcache.head` 链表，改为：
  - 初始化 `hashtable[i].head` 的 `next/prev` 指向自身；
  - 为每个桶初始化自旋锁，锁名以 `"bcache"` 开头；
  - 把所有 buf 先插入到某个固定桶（如 bucket 0）的链表中，相当于初始时全部集中在一个桶里；
- 每个 `buf` 仍然使用 `initsleeplock(&b->lock, "buffer");` 初始化睡眠锁，用于与上层同步磁盘 I/O。

示意代码：

```c
void
binit(void)
{
  struct buf *b;
  char lockname[16];

  // 初始化每个桶
  for(int i = 0; i < NBUCKET; i++){
    snprintf(lockname, sizeof(lockname), "bcache.bucket%d", i);
    initlock(&hashtable[i].lock, lockname);
    hashtable[i].head.prev = &hashtable[i].head;
    hashtable[i].head.next = &hashtable[i].head;
  }

  // 初始化所有 buf，并先挂到 bucket 0
  for(b = bcache.buf; b < bcache.buf + NBUF; b++){
    b->refcnt = 0;
    initsleeplock(&b->lock, "buffer");

    b->next = hashtable[0].head.next;
    b->prev = &hashtable[0].head;
    hashtable[0].head.next->prev = b;
    hashtable[0].head.next = b;
  }
}
```

注意：锁名以 `"bcache"` 开头，满足实验要求，且 `lockname` 缓冲区足够大，避免截断警告。

#### (3) 获取缓冲区 (`bget`)

`bget(dev, blockno)` 是块缓存的核心函数：

1. 计算 `bucket = HASH(blockno)`；
2. 获取该桶的锁；
3. 在桶内查找是否已有对应 `(dev, blockno)` 的缓冲块：
   - 找到：`refcnt++`，持有桶锁期间不释放 `b->lock`，随后释放桶锁并返回；
4. 若桶内查找失败，说明缓存未命中：
   - 先在该桶内查找任何 `refcnt==0` 的块，选一个作为替换目标；
   - 若本桶找不到空闲块，可以释放该桶锁，然后串行地在其它桶中查找 `refcnt==0` 的块：
     - 依次获取其它桶锁，寻找 `refcnt==0` 的块；
     - 若找到，则从原桶链表中摘下该块，更新其 `dev`、`blockno`、`valid` 等字段；
     - 将其移动到目标桶（`bucket`）的链表中，并把 `refcnt` 设为 1；
   - 在重新获取目标桶锁前，要考虑可能发生的 race：另一 CPU 也在为同一 `(dev, blockno)` 分配缓存块。最安全的方式是：
     - 找到可用块后，回到目标桶锁下**重新检查**是否已经存在该 `(dev, blockno)`；
     - 如果已经存在，就放弃刚找到的块（重新标记为空闲）并使用已有块。

伪代码示意：

```c
struct buf*
bget(uint dev, uint blockno)
{
  int bucket = HASH(blockno);

  for(;;){
    acquire(&hashtable[bucket].lock);

    // 1. 在桶内查找是否命中
    struct buf *b;
    for(b = hashtable[bucket].head.next; b != &hashtable[bucket].head; b = b->next){
      if(b->dev == dev && b->blockno == blockno){
        b->refcnt++;
        release(&hashtable[bucket].lock);
        acquiresleep(&b->lock);
        return b;
      }
    }

    // 2. 查找空闲 buf（refcnt == 0）
    struct buf *empty = 0;
    for(b = hashtable[bucket].head.prev; b != &hashtable[bucket].head; b = b->prev){
      if(b->refcnt == 0){
        empty = b;
        break;
      }
    }

    if(empty){
      // 在本桶找到空闲 buf，直接复用
      empty->dev = dev;
      empty->blockno = blockno;
      empty->valid = 0;
      empty->refcnt = 1;
      release(&hashtable[bucket].lock);
      acquiresleep(&empty->lock);
      return empty;
    }

    // 3. 本桶没有空闲 buf，可以在这里实现“跨桶偷 buf”或
    //    退化为串行地在全局范围内找一个 refcnt==0 的 buf。
    //    这一步即使稍有锁竞争，测试也能接受。

    release(&hashtable[bucket].lock);
    // 串行查找其它桶中的空闲 buf... 找到后再回到本桶重试。
  }
}
```

上面示意代码体现了几点关键思想：

- **哈希分桶**：大多数命中只需获取一个桶锁；
- **局部线性扫描**：在桶内按 MRU/LRU 顺序查找 `refcnt==0` 的块即可，不必保持精确 LRU；
- **必要时可以退化为全局串行选择空闲块**，但频率远低于命中路径，对整体性能影响有限。

#### (4) 释放缓冲区 (`brelse`)

在原始实现中，`brelse()` 需要：

- 获取 `bcache.lock`；
- 把释放的块移到全局链表头部（MRU）；
- 减少 `refcnt`，如果变为 0 才放回链表；
- 释放 `bcache.lock`。

在哈希桶设计下：

- 首先通过 `HASH(b->blockno)` 算出桶号 `bucket`；
- 获取该桶的锁；
- `b->refcnt--`，如果变为 0，可以选择把该块移动到该桶链表头部，作为“最近使用”；
- 释放桶锁。

示意代码：

```c
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int bucket = HASH(b->blockno);
  acquire(&hashtable[bucket].lock);

  b->refcnt--;
  if(b->refcnt == 0){
    // 可选地：把空闲 buf 移动到桶链表头部
    // 以便更快被重用，类似 MRU 策略
    b->next->prev = b->prev;
    b->prev->next = b->next;

    b->next = hashtable[bucket].head.next;
    b->prev = &hashtable[bucket].head;
    hashtable[bucket].head.next->prev = b;
    hashtable[bucket].head.next = b;
  }

  release(&hashtable[bucket].lock);
}
```

注意：`brelse` 不再需要获取全局 `bcache.lock`，从而大幅降低不同块之间释放操作的锁竞争。

## 总结

本次实验主要围绕操作系统中的**并发控制**和**锁机制**展开，通过三个互相关联但各有侧重的部分，把书本中“锁”的理论用可观测的性能指标和代码改造具体化：

1. **Per-CPU 内存分配器**：
   - 从“单链表 + 单锁”的全局设计，改造成“每 CPU 一条 freelist + 一把锁”；
   - 大幅降低了 `kmem` 锁上的自旋次数，体现了 **per-CPU 数据、工作窃取、细粒度锁** 对可伸缩性的提升；
   - 实验中需要显式使用 `cpuid()`、`push_off/pop_off`，加深了对“CPU 与中断上下文、内核并发”的理解。

2. **读写锁**：
   - 面向读多写少场景，将“互斥锁 → 读写锁”，提高读路径的并行度；
   - 通过 `readers` / `writer` / `writer_waiting` 等字段设计，展示了**锁的互斥性、共享性、公平性/优先级策略**在一个简单实现中的综合体现；
   - 写者优先策略帮助理解“如何通过协议避免饥饿”，而不仅仅是“能互斥就行”。

3. **Buffer Cache 优化**：
   - 把保护全部缓存块的“全局大锁”拆成“哈希桶 + 每桶一锁”，是 **细粒度锁与哈希分桶** 的经典应用；
   - 在满足“每个块最多一份拷贝”的不变式下，通过结构调整与锁粒度控制，显著降低了 `bcache` 上的锁竞争；
   - 同时保留了 NBUF 的限制，说明**性能优化并不是简单堆资源，而是改造数据结构与并发控制策略**。

通过观察 `kalloctest` 与 `bcachetest` 中锁的 `#test-and-set` 统计、配合 `usertests` 的功能回归，可以直观感受到：

- 锁并不仅仅是“加上去就万事大吉”的安全工具；
- 不合理的锁粒度，会直接限制系统在多核上的扩展能力；
- 通过 per-CPU、读写锁、哈希分桶等技术，可以在**保持正确性**的前提下，大幅提升**并发度与性能**。

整个 Lab: Lock 让我们从实现层面体会到：

- 自旋锁 / 读写锁等抽象，与底层 CPU 指令 (test-and-set)、中断机制之间的实际联系；
- “锁竞争统计”如何作为衡量并行度的客观指标；
- 操作系统在设计同步原语和共享数据结构时，需要在**正确性、性能、公平性**三者之间不断权衡与取舍。
