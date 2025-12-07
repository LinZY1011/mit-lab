这是一个关于 xv6 `mmap` 实验的完整总结报告。这份文档详细记录了所有的代码修改、涉及的操作系统原理、解决的问题以及在开发过程中遇到的挑战和解决方案。

---

# xv6 mmap (Memory Mapped Files) 实验总结报告

## 1. 问题背景与目标

**mmap (Memory Map)** 是 Unix/Linux 系统中一个非常重要的系统调用。它的主要功能是将一个文件（或其他对象）映射到进程的虚拟地址空间中。

**代码解决的问题：**
在传统的 I/O 操作中（如 `read`/`write`），程序必须显式地分配缓冲区，将数据从内核缓冲区拷贝到用户缓冲区，修改后再写回。这不仅繁琐，而且涉及多次数据拷贝，效率较低。

**实现的功能：**
通过 `mmap`，我们将文件内容直接“暴露”为内存数组。
1.  **像访问内存一样访问文件**：程序可以使用指针直接读写文件内容，而无需调用 `read`/`write`。
2.  **按需加载 (Lazy Allocation)**：大文件不会一次性读入内存，只有被访问的页面才会被加载（缺页中断）。
3.  **数据持久化**：对 `MAP_SHARED` 映射区的修改，会在 `munmap` 或进程退出时自动写回磁盘。

---

## 2. 涉及的操作系统原理

本次实验深入涉及了以下核心原理：

1.  **虚拟内存管理 (Virtual Memory Management)**
    *   **VMA (Virtual Memory Area)**：内核需要记录每个进程拥有的内存区域。除了代码段、数据段、栈、堆之外，现在增加了文件映射区。我们需要结构体来记录这段区域的起始地址、长度、权限和对应的文件。
    *   **页表 (Page Tables)**：通过操作 RISC-V 的页表项（PTE），控制虚拟地址到物理地址的映射，以及读/写/执行权限。

2.  **按需分页 (Demand Paging)**
    *   `mmap` 调用时并不分配物理内存，只建立 VMA 记录。
    *   当 CPU 访问该地址时，触发 **Page Fault (缺页异常)**。
    *   内核捕获异常，分配物理页，从磁盘读取数据，修改页表，然后恢复进程执行。

3.  **异常处理 (Trap Handling)**
    *   利用 RISC-V 的 `scause` 寄存器区分系统调用、中断和异常。
    *   `scause = 13` (Load Page Fault) 和 `15` (Store Page Fault) 是实现 Lazy Allocation 的关键。

4.  **文件系统接口 (File System Interface)**
    *   利用文件描述符引用计数 (`filedup`/`fileclose`) 保证文件在映射期间不被释放。
    *   利用 `readi` 和 `writei` 在内核态直接操作 inode 读写磁盘。

5.  **脏页回写 (Dirty Page Writeback)**
    *   利用页表项中的 **Dirty Bit (PTE_D)** 来优化性能：只有被修改过的页面才需要写回磁盘。

---

## 3. 代码修改详解

### 3.1 数据结构定义 (`kernel/proc.h`)
为了跟踪映射区域，我们在 `struct proc` 中增加了一个 VMA 数组。由于 xv6 内核没有动态内存分配器（针对小对象），我们使用了固定大小的数组。

```c
struct vma {
  int used;           // 是否被使用
  uint64 addr;        // 映射的虚拟起始地址
  int length;         // 长度
  int prot;           // 权限 (PROT_READ/WRITE)
  int flags;          // 标志 (MAP_SHARED/PRIVATE)
  struct file *f;     // 映射的文件
  int offset;         // 文件偏移
};

struct proc {
  // ... 其他字段
  struct vma vma[16]; // 每个进程最多 16 个映射区
};
```

### 3.2 系统调用层 (`kernel/sysfile.c`, `kernel/syscall.c`)

*   **`sys_mmap`**:
    1.  解析参数。
    2.  寻找空闲的 VMA 槽位。
    3.  **分配虚拟地址**：简单的策略是从高地址（如 `0xC0000000`）开始寻找未被重叠使用的区域。
    4.  填充 VMA 结构体，增加文件引用计数 (`filedup`)。
    5.  **注意**：此时**不**分配物理内存，也不读文件（Lazy）。

*   **`sys_munmap`**:
    1.  根据地址找到对应的 VMA。
    2.  遍历指定范围的每一页。
    3.  **写回脏页**：检查页表项是否存在且有效 (`PTE_V`)。如果映射是 `MAP_SHARED` 且页表项被标记为脏 (`PTE_D`)，调用 `writei` 将数据写回文件。
    4.  **解除映射**：调用 `uvmunmap` 释放物理页和页表映射。
    5.  更新或释放 VMA 结构。

### 3.3 异常处理与按需加载 (`kernel/trap.c`)

这是实验的核心。我们在 `usertrap` 中捕获 `scause` 为 13 或 15 的异常。

1.  **地址检查**：检查故障虚拟地址 (`r_stval()`) 是否落在当前进程的某个 VMA 范围内。
2.  **物理页分配**：调用 `kalloc()` 分配一个物理页。
3.  **读取文件**：计算文件偏移量，调用 `readi()` 将文件内容读入该物理页。
4.  **权限设置**：根据 VMA 的 `prot` 设置 PTE 的 `R/W/X` 位，并设置 `PTE_U`（用户可访问）。
5.  **建立映射**：调用 `mappages` 将虚拟地址映射到物理页。

### 3.4 进程生命周期 (`kernel/proc.c`)

*   **`allocproc`**：初始化 VMA 数组，清零 `used` 标志。
*   **`fork`**：子进程通过 `struct assignment` 复制父进程的 VMA 数组，并增加文件的引用计数。
*   **`exit`**：这是资源回收的关键。进程退出时，必须像调用了 `munmap` 一样，遍历所有 VMA，写回脏页，解除映射，并关闭文件引用。

---

## 4. 遇到的挑战与解决方案

在开发过程中遇到了几个棘手的 Bug，以下是详细分析：

### 挑战 1：`PTE_D` 和结构体定义缺失
**问题**：编译时报错 `PTE_D undeclared` 和 `struct file` undefined。
**原因**：xv6 默认的 `riscv.h` 没有定义脏位（Dirty Bit，第 7 位），且 `proc.c` 等文件缺少文件系统相关的头文件。
**解决**：
1.  手动定义宏：`#define PTE_D (1L << 7)`。
2.  引入头文件：`fs.h`, `sleeplock.h`, `file.h`。

### 挑战 2：文件被错误延长 (`mmap dirty` 测试失败)
**问题**：在 `mmap dirty` 测试中，测试程序创建了一个 1.5 页大小的文件，但映射了 2 页。当程序修改了第 2 页（超出文件大小的部分）并触发写回时，原始的 `writei` 实现直接写入了 4096 字节。这导致文件中间出现空洞或文件大小被错误地增加，导致后续读取校验失败。
**原理**：`mmap` 映射的内存必须按页对齐，但文件大小不一定是页的整数倍。写回时不能无脑写整页。
**解决**：
在 `sys_munmap` 和 `exit` 的写回逻辑中增加边界检查：
```c
// 计算文件偏移
uint64 file_off = v->offset + (cur - v->addr);
int sz = PGSIZE;
// 如果当前偏移已经超过文件大小，不写
if(file_off >= v->f->ip->size) {
    sz = 0;
} 
// 如果写入会超出文件大小，截断写入长度
else if(file_off + sz > v->f->ip->size) {
    sz = v->f->ip->size - file_off;
}
if(sz > 0) writei(..., sz);
```

### 挑战 3：`panic: mappages: remap`
**问题**：在 `read_only_write` 测试中，系统发生 panic。
**场景**：
1.  进程映射了只读页面 (`PROT_READ`)。
2.  进程尝试写入，触发缺页异常（因为尚未加载）。
3.  内核加载页面，标记为**只读**。
4.  进程再次尝试写入，再次触发缺页异常（Store Page Fault，scause=15）。
5.  内核再次进入处理逻辑，试图重新分配并映射该页面，`mappages` 发现映射已存在，触发 panic。
**原理**：缺页异常不仅发生在页面不存在时，也发生在权限不足时。
**解决**：
在 `usertrap` 中，在分配内存前，先检查该地址是否已经存在有效的页表项映射：
```c
pte_t *pte = walk(p->pagetable, va, 0);
if(pte && (*pte & PTE_V)){
    // 页面已存在，说明是权限错误（试图写只读页面）
    setkilled(p); 
} else {
    // 页面不存在，执行 Lazy Allocation
    ...
}
```

### 挑战 4：`writei` 参数错误
**问题**：`writei` 调用时第二个参数传了 `1`，导致系统认为源地址是用户虚拟地址，但在 `munmap` 和 `exit` 中，我们使用的是通过 `walk` 得到的物理地址（在内核中即为内核地址）。
**解决**：将 `writei` 的第二个参数改为 `0`。

---

## 5. 总结

本次实验成功在 xv6 内核中实现了 `mmap` 和 `munmap` 系统调用。

1.  **功能完备**：支持 `MAP_SHARED`（持久化）和 `MAP_PRIVATE`（私有），支持 `PROT_READ/PROT_WRITE` 权限控制。
2.  **性能优化**：实现了 Lazy Allocation，仅在访问时加载页面；实现了 Dirty Bit 检测，仅写回修改过的页面。
3.  **鲁棒性**：处理了文件边界写回问题、权限校验问题以及进程退出时的自动清理。

代码通过了 `mmaptest` 的所有测试点（包括 dirty, two files, fork, read-only write 等）以及 `usertests`，证明了实现的正确性和稳定性。