# xv6 mmap 实验报告（Lab: mmap）

---

## 一、实验目标与整体要求

本实验在 xv6 的 mmap 分支上实现内存映射文件（memory-mapped files），通过新增 `mmap` / `munmap` 系统调用，让用户进程可以：

- 将文件映射到自己的虚拟地址空间，像访问内存一样读写文件内容；
- 支持 `PROT_READ`/`PROT_WRITE` 权限控制，以及 `MAP_SHARED` / `MAP_PRIVATE` 两种写入语义；
- 采用按需分页（lazy allocation），仅在发生缺页异常时分配物理页和读取文件；
- 在 `munmap` 和进程 `exit` 时，对 `MAP_SHARED` 且被修改的页回写到文件；
- 在 `fork` 后子进程继承父进程的映射区域，并在缺页时自己分配独立物理页。

验证要求：

- [user/mmaptest.c](user/mmaptest.c) 中的所有测试通过：basic mmap、private、read-only、read/write、dirty、lazy、two files、fork、munmap prevents access、read-only write 等；
- [user/usertests.c](user/usertests.c) 以 `usertests -q` 运行全部通过。

主要改动文件包括：

- 进程与 VMA 结构：[kernel/proc.h](kernel/proc.h)、[kernel/proc.c](kernel/proc.c)
- 缺页异常处理：[kernel/trap.c](kernel/trap.c)
- 系统调用实现：[kernel/sysfile.c](kernel/sysfile.c)、[kernel/syscall.c](kernel/syscall.c)、[kernel/syscall.h](kernel/syscall.h)
- 权限 / 标志定义与用户接口：[kernel/fcntl.h](kernel/fcntl.h)、[user/user.h](user/user.h)、[user/usys.pl](user/usys.pl)
- 测试程序与构建：[user/mmaptest.c](user/mmaptest.c)、[Makefile](Makefile)

---

## 二、VMA 结构与进程状态扩展

### 1. 任务要求

- 为每个进程维护一张“虚拟内存区域表”（VMA 表），记录所有由 `mmap` 创建的映射区：起始虚拟地址、长度、权限、标志、关联文件及偏移等；
- 由于 xv6 内核缺乏小对象动态分配器，VMA 表采用固定大小数组实现（实验提示建议 16 个）。

### 2. 实现思路

- 在进程结构 `struct proc` 中增加一个 `struct vma vma[16]` 数组；
- 每个 `vma` 记录：是否使用、起始 VA、长度、`prot`、`flags`、文件指针 `struct file *f`、文件偏移 `offset` 等；
- 在进程创建（`allocproc`）时初始化 `vma[i].used = 0`，在 `fork` / `exit` 时复制或清理这些区域。

### 3. 具体代码实现与修改原因

文件：[kernel/proc.h](kernel/proc.h)

- 新增 VMA 结构体，并将其纳入 `struct proc`：

  ```c
  struct vma {
    int used;           // 是否被使用
    uint64 addr;        // 映射的虚拟起始地址
    int length;         // 长度（字节）
    int prot;           // 权限 (PROT_READ/WRITE/EXEC)
    int flags;          // 标志 (MAP_SHARED/PRIVATE)
    int fd;             // 对应的文件描述符
    struct file *f;     // 映射的文件
    int offset;         // 文件偏移
  };

  struct proc {
    ...
    struct vma vma[16]; // 每个进程最多 16 个映射区
  };
  ```

文件：[kernel/proc.c](kernel/proc.c)

- 在 `allocproc()` 中初始化 VMA：

  ```c
  for(int i = 0; i < 16; i++){
    p->vma[i].used = 0;
  }
  ```

- 在 `fork()` 中拷贝 VMA，并对每个使用中的 VMA 调用 `filedup`，保证文件在子进程中也保持有效：

  ```c
  for(i = 0; i < 16; i++){
    struct vma *v = &p->vma[i];
    if(v->used){
      np->vma[i] = *v;
      filedup(v->f);
    }
  }
  ```

原因：

- VMA 表提供了进程级别的“用户地址空间布局描述”，是缺页异常处理中定位映射区、找到对应文件和偏移的基础。
- fork 时共享映射的文件对象，而不是复制文件内容，符合 mmap 语义和实验对 `fork` 测试的要求。

---

## 三、mmap/munmap 系统调用

### 1. 任务要求

- 实现 `void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset)`：
  - 本实验中可假设 `addr == 0`、`offset == 0`；
  - `prot` 只使用 `PROT_READ` / `PROT_WRITE` / `PROT_EXEC` 组合；
  - `flags` 只需处理 `MAP_SHARED` 和 `MAP_PRIVATE`；
  - 要求 **延迟分配**：`mmap` 本身不分配物理页、不读文件，只登记 VMA；
  - 若成功返回映射起始虚拟地址，失败返回 `-1`（用户态看到 `MAP_FAILED`）。
- 实现 `int munmap(void *addr, size_t len)`：
  - 解除给定范围内的映射；
  - 对 `MAP_SHARED` 且被修改的页，在解除映射前写回文件；
  - 支持只 unmap 前半段、后半段或整个区域（不在中间打洞）。

### 2. 权限和标志定义

文件：[kernel/fcntl.h](kernel/fcntl.h)

- 在 `LAB_MMAP` 条件下定义 mmap 相关常量：

  ```c
  #define PROT_NONE       0x0
  #define PROT_READ       0x1
  #define PROT_WRITE      0x2
  #define PROT_EXEC       0x4

  #define MAP_SHARED      0x01
  #define MAP_PRIVATE     0x02
  ```

### 3. 用户态接口与系统调用号

文件：[user/user.h](user/user.h)

- 新增 mmap / munmap 用户态声明（使用 `size_t`/`off_t` 类型，仅在 LAB_MMAP 打开时定义）：

  ```c
  void* mmap(void*, int, int, int, int, int);
  int munmap(void*, int);
  ```

文件：[kernel/syscall.h](kernel/syscall.h)

- 分配新的系统调用号：

  ```c
  #define SYS_mmap   22
  #define SYS_munmap 23
  ```

文件：[kernel/syscall.c](kernel/syscall.c)

- 声明并注册内核实现函数：

  ```c
  extern uint64 sys_mmap(void);
  extern uint64 sys_munmap(void);

  static uint64 (*syscalls[])(void) = {
    ...
    [SYS_mmap]   sys_mmap,
    [SYS_munmap] sys_munmap,
  };
  ```

文件：[user/usys.pl](user/usys.pl)

- 为 mmap/munmap 生成 ecall stub：

  ```perl
  entry("mmap");
  entry("munmap");
  ```

### 4. sys_mmap：登记 VMA，地址空间分配

文件：[kernel/sysfile.c](kernel/sysfile.c)

- 核心流程：

  1. 使用 `argaddr/argint/argfd` 解析用户参数，并获取 `struct file *f`；
  2. 对 `PROT_WRITE + MAP_SHARED` 的情况，检查底层文件是否可写：
     ```c
     if(!f->writable && (prot & PROT_WRITE) && (flags == MAP_SHARED))
       return -1;
     ```
  3. 在当前进程的 `vma[16]` 数组中寻找一个 `used == 0` 的空槽；
  4. 选择映射虚拟地址：
     - 以高地址 `0xC0000000` 为起点，在所有已存在 VMA 之间寻找不重叠的“洞”；
     - 对每个候选 `va`，检查 `[va, va+length)` 是否和任何已用 VMA 区间重叠，若重叠则跳到当前 VMA 末尾的下一页继续；
  5. 填写 VMA 结构：`addr/length/prot/flags/fd/f/offset`，调用 `filedup(f)` 增加文件引用计数；
  6. 返回分配到的 `v->addr`。

- 设计要点：
  - `mmap` 本身**不分配物理内存、不读文件**，只在 VMA 表中登记映射；
  - 通过扫描已有 VMA 来简单实现“用户空间地址分配”，避免冲突；
  - 通过 `filedup` 确保即使用户关闭原始 `fd`，映射期间文件对象仍然存在。

### 5. sys_munmap：回写脏页并解除映射

- 核心流程：

  1. 根据 `addr` 在线性扫描 VMA 表，找到满足 `addr ∈ [v->addr, v->addr + v->length)` 的区域；
  2. 计算 `end = addr + length`，对 `[addr, end)` 区间每个页对齐地址 `cur`：
     - 用 `walk(p->pagetable, cur, 0)` 查找 PTE，若存在且有效：
       - 若 `v->flags == MAP_SHARED` 且 PTE 有 `PTE_D` 脏标志：
         - 计算文件偏移：`file_off = v->offset + (cur - v->addr)`；
         - 依据 `file_off` 与文件长度裁剪写回大小 `sz`，避免写越界：
           - 若 `file_off >= size`：`sz = 0` 不写；
           - 若 `file_off + PGSIZE > size`：`sz = size - file_off`；
         - 在日志事务中 `writei(v->f->ip, 0, pa, file_off, sz)` 写回；
       - 调用 `uvmunmap(p->pagetable, cur, 1, 1)` 解除映射并释放物理页；
  3. 更新 VMA 结构：
     - 若 `addr == v->addr && length == v->length`：完全解除该 VMA，`fileclose(v->f); v->used = 0`；
     - 若解除的是前半部分：`v->addr += length; v->length -= length; v->offset += length;`；
     - 若解除的是尾部：`v->length -= length;`。

- 原理和原因：
  - 通过 `PTE_D` 只回写真正被修改过的页，减少不必要的磁盘 I/O；
  - 按页循环并使用 `uvmunmap`，让页表管理和物理页释放沿用 xv6 既有机制；
  - 对长度非整页的文件进行精确裁剪，避免在 `mmap dirty` 场景中把多余的 0 写进文件尾，从而破坏文件内容，这正对应实验中提到的“文件长度小于映射页数”的边界情况。

---

## 四、缺页异常处理与 Lazy Allocation

### 1. 任务要求

- `mmap` 调用时不分配物理内存、不读取文件；
- 当用户首次访问映射地址（读/写）触发缺页异常时，由内核：
  - 判断该地址是否落入某个 VMA；
  - 分配一个物理页，按需从文件中读取数据；
  - 按 VMA 的 `prot` 设置页表项权限并建立映射；
- 对于非法访问（不在任何 VMA 内，或因权限导致的异常），应像普通页错误那样杀死进程。

### 2. 实现思路

文件：[kernel/trap.c](kernel/trap.c)

- 在 `usertrap()` 中读取 `scause`，对 `13`（Load Page Fault）和 `15`（Store Page Fault）做专门处理：

  1. 获取触发异常的虚拟地址 `va = r_stval()`；
  2. 在线性扫描 `p->vma[16]`，查找包含该地址的 VMA：
     - 条件：`v->used && va >= v->addr && va < v->addr + v->length`；
  3. 若找到了 VMA：
     - 首先使用 `walk(p->pagetable, va, 0)` 检查是否已存在 PTE：
       - 若 PTE 存在且 `PTE_V` 置位，说明不是“未映射”，而很可能是“权限不足”（比如试图写只读页），则直接 `setkilled(p)`；
       - 否则认为是“未映射缺页”，执行 Lazy Allocation：
         - 将 `va` 下取整到页边界：`va = PGROUNDDOWN(va)`；
         - `kalloc()` 分配物理页并清零；
         - 计算文件偏移：`v->offset + (va - v->addr)`，调用 `ilock/readi/iunlock` 将文件内容读入物理页；
         - 根据 VMA 的 `prot` 位构造 PTE 权限：
           ```c
           int perm = PTE_U;
           if(v->prot & PROT_READ)  perm |= PTE_R;
           if(v->prot & PROT_WRITE) perm |= PTE_W;
           if(v->prot & PROT_EXEC)  perm |= PTE_X;
           ```
         - 调用 `mappages(p->pagetable, va, PGSIZE, (uint64)mem, perm)` 安装页表项；
  4. 若没找到任何 VMA，或中间任意一步失败：
     - 打印 `usertrap(): unexpected scause` 等信息，`setkilled(p)`。

### 3. 设计要点与 Bug 处理

- 提前检查 PTE 是否存在：
  - 解决了 `read_only_write` 场景中，重复对已存在的只读页调用 `mappages` 导致 `panic("mappages: remap")` 的问题；
- 通过 `scause` 区分 load/store fault，但在 VMA 范围内只要 PTE 未建立就统一做 Lazy Allocation，简化逻辑；
- 使用 `readi` 直接操作 inode，实现“文件到物理页”的填充；
- `ilock`/`iunlock` 保证文件系统层的并发一致性。

---

## 五、进程退出与 fork 场景中的 mmap 处理

### 1. 任务要求

- 当进程 `exit` 时，应当像对所有 `MAP_SHARED` 映射调用了 `munmap` 一样：
  - 将脏页回写到文件；
  - 解除映射并释放物理页；
  - 关闭相关文件引用，清理 VMA 表；
- 当 `fork` 发生时，子进程应继承父进程当前所有 mmap 映射：
  - VMA 描述应复制；
  - 文件引用计数应增加；
  - 缺页时各自 Lazy Allocation，允许父子进程物理页不共享（实验允许）。

### 2. exit 中的 mmap 清理

文件：[kernel/proc.c](kernel/proc.c) 中 `exit(int status)`

- 在关闭 open files 和 cwd 之前，先遍历 `p->vma[16]`：
  - 对每个 `v->used`：
    - 遍历 `[v->addr, v->addr + v->length)` 区间内每一页：
      - 若页已映射且 `MAP_SHARED` 且 PTE 有 `PTE_D`：与 `sys_munmap` 同样逻辑写回脏数据（含边界裁剪）；
      - 使用 `uvmunmap` 解除映射并释放物理页；
    - 完成本 VMA 范围的回写与解除映射后：`fileclose(v->f); v->used = 0;`。

原因：

- 保证即使用户忘记调用 `munmap`，`MAP_SHARED` 区域的修改也不会丢失；
- `exit` 层面的清理匹配 POSIX 语义和 mmaptest 对“进程退出时文件一致性”的隐含要求。

### 3. fork 中的 VMA 复制

- 前文已述，在 `fork()` 中直接复制 VMA 结构并对每个 VMA 的 `f` 执行 `filedup`：
  - 确保文件对象的生命周期覆盖父子进程；
  - 缺页逻辑在父子进程中各自独立执行，符合实验提示“可以让子进程在缺页时分配新页，而不必共享物理页”。

---

## 六、测试与结果

- Makefile 中，当 `LAB=mmap` 时，会将 `_mmaptest` 加入 UPROGS：
  - [Makefile](Makefile) 片段：
    ```makefile
    ifeq ($(LAB),mmap)
    UPROGS += \
        $U/_mmaptest
    endif
    ```

在 xv6 中运行：

```sh
mmaptest
# 依次输出各子测试 OK，最后：
# mmaptest: all tests succeeded

usertests -q
# ...
# ALL TESTS PASSED
```

说明：

- 基本 mmap / private / read-only / read-write / dirty / lazy / two files / fork / munmap prevents access / read-only write 等所有场景均符合预期；
- 说明 lazy allocation、权限检查、VMA 管理与回写策略正确实现。

---

## 七、涉及的操作系统原理总结

- **虚拟内存与 VMA**：
  - 通过 VMA 结构抽象用户地址空间中的不同区域（代码、堆、栈、mmap 文件等），为缺页处理和权限检查提供元数据；

- **按需分页（Demand Paging）**：
  - `mmap` 只登记，不分配内存；首次访问时通过缺页异常触发实际分配和文件读取，实现大文件快速映射与内存节省；

- **异常与中断处理**：
  - 使用 RISC-V 的 `scause` / `stval` 区分系统调用、设备中断与缺页异常；
  - 在 `usertrap` 中融合“普通异常处理”和“mmap 特殊缺页处理”；

- **页表与访问权限**：
  - 通过 PTE 的 `R/W/X/U` 位实现用户态读/写/执行权限控制；
  - 通过 `PTE_D` 判断页面是否被修改，用于 `MAP_SHARED` 页的写回优化；

- **文件系统与缓存一致性**：
  - 利用 `readi` / `writei` 直接在内核态以字节精度访问 inode，并结合日志系统 `begin_op` / `end_op` 保障元数据一致性；

- **进程生命周期与资源管理**：
  - 在 `fork` / `exit` 中成对处理 VMA 和文件引用，使得 mmap 映射在进程复制与退出时行为一致且无资源泄露。

这套实现完整覆盖了实验要求中与 mmap 内存映射文件相关的核心功能，并在保持 xv6 结构简洁的前提下，展示了现代操作系统对虚拟内存、文件系统和进程管理多模块协作的典型模式。
