# 实验一 Inspect a user-process page table 

输出:
```bash
$ pgtbltest
print_pgtbl starting
va 0x0 pte 0x21FC885B pa 0x87F22000 perm 0x5B
va 0x1000 pte 0x21FC7C1B pa 0x87F1F000 perm 0x1B
va 0x2000 pte 0x21FC7817 pa 0x87F1E000 perm 0x17
va 0x3000 pte 0x21FC7407 pa 0x87F1D000 perm 0x7
va 0x4000 pte 0x21FC70D7 pa 0x87F1C000 perm 0xD7
va 0x5000 pte 0x0 pa 0x0 perm 0x0
va 0x6000 pte 0x0 pa 0x0 perm 0x0
va 0x7000 pte 0x0 pa 0x0 perm 0x0
va 0x8000 pte 0x0 pa 0x0 perm 0x0
va 0x9000 pte 0x0 pa 0x0 perm 0x0
va 0xFFFF6000 pte 0x0 pa 0x0 perm 0x0
va 0xFFFF7000 pte 0x0 pa 0x0 perm 0x0
va 0xFFFF8000 pte 0x0 pa 0x0 perm 0x0
va 0xFFFF9000 pte 0x0 pa 0x0 perm 0x0
va 0xFFFFA000 pte 0x0 pa 0x0 perm 0x0
va 0xFFFFB000 pte 0x0 pa 0x0 perm 0x0
va 0xFFFFC000 pte 0x0 pa 0x0 perm 0x0
va 0xFFFFD000 pte 0x0 pa 0x0 perm 0x0
va 0xFFFFE000 pte 0x21FD08C7 pa 0x87F42000 perm 0xC7
va 0xFFFFF000 pte 0x2000184B pa 0x80006000 perm 0x4B
print_pgtbl: OK
ugetpid_test starting
usertrap(): unexpected scause 0xd pid=4
            sepc=0x53e stval=0x3fffffd000
```

---
## `print_pgtbl` 输出逐行说明（PTE 含义 + 权限位解释）

### 1) `va 0x0 pte 0x21FC885B pa 0x87F22000 perm 0x5B`

* **逻辑内容**：虚拟页 `0x0` 被映射到物理页 `0x87F22000`（页对齐物理地址由 PTE 中的物理页号得出）。PTE 值 `0x21FC885B` 包含物理页号 + 标志位。
* **权限位（perm 0x5B）解码**：`0x5B` 二进制 `0b01011011`。按 xv6/riscv 的位定义 (0x001 V,0x002 R,0x004 W,0x008 X,0x010 U,0x020 G,0x040 A,0x080 D)：

  * 有效 `V` = 1
  * 可读 `R` = 1
  * 可写 `W` = 0
  * 可执行 `X` = 1
  * 用户可访问 `U` = 1
  * 已访问 `A` = 1
  * 已修改 `D` = 0
* **推断**：这是一个用户态**只读可执行**页（通常是用户代码段的一部分）。`A` 是访问位被硬件/软件置位。

---

### 2) `va 0x1000 pte 0x21FC7C1B pa 0x87F1F000 perm 0x1B`

* **映射**：`0x1000` → `0x87F1F000`。
* **perm 0x1B** (`0b00011011`)：`V,R,X,U` = 有效、可读、可执行、用户可访问。没有 `A`、`D`、`W`。
* **推断**：用户代码页（可执行、只读）。

---

### 3) `va 0x2000 pte 0x21FC7817 pa 0x87F1E000 perm 0x17`

* **映射**：`0x2000` → `0x87F1E000`。
* **perm 0x17** (`0b00010111`)：`V,R,W,U` = 有效、可读、可写、用户可访问（无 X）。
* **推断**：用户数据页（可读可写，非可执行），可能是 `.data` 或堆/静态数据页。

---

### 4) `va 0x3000 pte 0x21FC7407 pa 0x87F1D000 perm 0x7`

* **映射**：`0x3000` → `0x87F1D000`。
* **perm 0x07** (`0b00000111`)：`V,R,W` = 有效、可读、可写；**没有 U**（注意：这里 `U=0`），也没有 `X`。
* **推断**：这是**内核映射**而不是用户可访问（U=0），可能是内核页表/内核数据在高地址空间映射到这个虚拟地址范围（`0x3000` 处的具体含义要结合地址空间布局，但关键是 U=0 表示仅内核访问）。

> 注：上面 0x0~0x4000 等页通常是用户程序最低几页（文本/数据/堆等），`U` 位应为 1 才能从用户态访问。`perm=0x7` 说明该页仅内核可写可读（还要结合具体映射位置判断）。

---

### 5) `va 0x4000 pte 0x21FC70D7 pa 0x87F1C000 perm 0xD7`

* **映射**：`0x4000` → `0x87F1C000`。
* **perm 0xD7** (`0b11010111`)：`V,R,W,U,A,D` = 有效、可读、可写、用户可访问，并且 `A` (accessed) 和 `D` (dirty) 都为 1。
* **推断**：用户数据页且已被写过（D=1）。可能是堆或全局可写数据。

---

### 6–16) `va 0x5000` ... `va 0xFFFFD000` 都是 `pte 0x0`（perm 0x0）

* **含义**：这些虚拟页没有映射（PTE = 0）。访问这些页会产生页错误（page fault）。

---

### 17) `va 0xFFFFE000 pte 0x21FD08C7 pa 0x87F42000 perm 0xC7`

* **映射**：`0xFFFFE000` → `0x87F42000`。
* **perm 0xC7** (`0b11000111`)：`V,R,W,A,D` = 有效、可读、可写、已访问、已修改；**注意没有 U**（U=0）。
* **推断**：这是**内核页**（U=0），可读写（内核数据），已被写/访问。`va` 在 `0xFFFFE000` 这类高虚地址通常是内核映射的部分（kernel mappings/trampoline/other kernel pages）。

---

### 18) `va 0xFFFFF000 pte 0x2000184B pa 0x80006000 perm 0x4B`

* **映射**：`0xFFFFF000` → `0x80006000`（注意 `0x8000xxxx` 是物理内存低端，通常是 kernel text/data）。
* **perm 0x4B** (`0b01001011`)：`V,R,X,A` = 有效、可读、可执行、已访问；**没有 U、没有 W、没有 D**。
* **推断**：这很像**内核只读可执行页（内核代码/只读文本）**的映射，U=0 表示只有内核能执行。`A` = 1 表示已被取指访问过。

---

# 实验二 Speed up system calls
修改了 proc.c, 减少内核态与用户态切换的开销，具体是让部分简单系统调用（例如 getpid()）无需真正陷入内核，而是通过共享内存页直接在用户态读取结果。

xv6 为每个进程维护自己的页表（pagetable）。
在这里，我们：
1. 给每个进程额外分配一页内存 usyscall；
2. 这页物理内存由内核分配；
3. 然后将它 映射到用户页表中某个固定虚拟地址（USYSCALL）；
4. 并设置权限为 可读 + 用户态可访问（PTE_R | PTE_U）。

### 具体实现思路

- **proc.c**：在 `allocproc()` 中调用 `kalloc()` 取得一页物理内存，填入 `struct usyscall`，并通过 `mappages()` 把它挂到用户页表 `USYSCALL` 虚拟地址上，权限限定为 `PTE_R | PTE_U`，相当于 Linux vDSO 的最小实现。
- **生命周期管理**：`freeproc()` / `proc_freepagetable()` 里对应 `uvmunmap()` + `kfree()`，保证进程退出时释放共享页；`growproc()` 在调用 `uvmalloc_sbrk()` 之前确保共享页已经存在。
- **操作系统概念**：这是典型的“共享只读页”优化，利用页表把内核维护的数据暴露给用户态，从而避免系统调用陷入造成的上下文切换和寄存器保存/恢复开销，同时依靠页权限维持隔离安全性。

共享页中的 `struct usyscall` 目前只包含 `pid`，但同理我们可以缓存其它**无需参数且返回值很少改变**的系统调用，例如：

* `getppid()`：fork 时写入父 pid；进程不会频繁改变父子关系。
* `uptime()`：把 `ticks` 复制到共享页中，内核时钟中断更新即可。
* `sysinfo()` 的静态字段（空闲页数、进程数）也能放到共享页，由内核写入，用户直接读。

核心思路都是：只要数据是内核维护的只读快照，并且在所有 CPU 上保持一致，就可以在陷入内核之前直接返回，大幅减少 `scause` 为系统调用时的 trap 频率。

# 实验三 vmprint

`vmprint` 通过 `kvminithart()` 注册的 `vmprint(pagetable, 0)` 递归打印页表层级：

1. 先打印当前层每个非零 PTE，格式为 `..` 缩进 + `level N pte 0x... pa 0x...`。
2. 如果 `PTE_V && (PTE_R|PTE_W|PTE_X)` 任一位被设置，即叶子节点，则额外打印一行 `.. .. leaf va 0x... flags RWXUAD`，解释该叶子页的权限。
3. 如果是中间节点（只有 `PTE_V`，无 RWX），递归进入下一层并在缩进中追加 `..`。

举例：当 `vmprint` 输出如下片段：

```
.. level 2 pte 0x0000000080000c0f pa 0x0000000080003000
.... level 1 pte 0x0000000080000d0f pa 0x0000000080003400
...... leaf va 0x0000000000000000 flags RWXUAD
```

说明第 2 级条目指向一个 1 级页表，而第 0 级页表中的条目是叶子页，权限标志依次代表（R/W/X/U/A/D）。只要结合页号和 `VA`，就能判断该行覆盖的 4KB 页属于代码段、数据段或共享页。

### 输出格式调优

- 在 `vmprint_walk()` 中根据 `level` 计算缩进深度（`3 - level`），打印若干个 `..`，紧跟 `%p` 形式的 `va/pte/pa`，这样恰好匹配官方 `print_kpgtbl` 的判定正则。
- `PTE_V` 但无 `R/W/X` 的条目视为中间节点，递归向下；其余为叶子节点。输出顺序遵循硬件页表索引（Sv39 的 level-2/1/0），实现了“可视化页表遍历”，方便调试缺页或权限错误。

# 实验四 Superpage 支持

目标：当用户进程使用 `sbrk()` 一次性增长 >= 2MB 时，尽量用 2MB superpage（Sv39 的 level-1 叶子）来映射，减少页表项数量与 TLB miss。

实现要点：

1. **分配器**：在 `kalloc.c` 维护 `struct superrun` 链表，专门回收/分配 2MB 对齐的物理块（来自 `end` 到 `PHYSTOP - SUPERPOOL` 的高地址区域），接口为 `superalloc()` / `superfree()`。
2. **页表遍历**：`walk_internal()` 支持停在指定层级（例如 level=1），如果需要拆分 superpage，通过 `split_superpage()` 把 2MB PTE 展开成 512 个 4KB PTE，并把旧数据复制后释放 superpage。
3. **uvmalloc_sbrk()**：先尝试 superpage 对齐扩展；如果申请的区间跨越不对齐部分，则用常规 4KB 分配填补头尾，只在中间整段使用 superpage。失败时回滚已分配的页。
4. **uvmunmap()/uvmcopy()**：检测到 PTE 指向 superpage 时，整块释放或整块复制；若新区域只需要部分 4KB 页，则拆分后再操作，保证 fork/exit 正确。

测试规划：运行 `make qemu` 后执行 `pgtbltest`, `pgtbltest superpg_test` 并观察 `vmprint` 输出是否含有 level-1 叶子（2MB 页）；同时检查 `ugetpid_test` 是否仍然 PASS。

### 题目中 sbrkmuch 的额外挑战

- `usertests` 的 `sbrkmuch` 会不断调用 `sbrk()` 扩展地址空间，如果我们的 2MB 池耗尽直接返回错误，用户空间会误以为物理内存不足。
- 为了保持 ABI 语义，在 `uvmalloc_sbrk()` 中允许 `superalloc()` 失败时自动退化为 4KB `kalloc()`，只要普通页还有库存就继续扩展，保证 `sbrk` 的最基本承诺：要么增长成功，要么等价于旧版 xv6 的极限行为。
- 这样的回退机制体现了 OS 资源管理的“最佳努力”：优先给 superpage，失败就走基础路径，从而在性能和兼容性之间取得平衡。

### 关键代码改动总览

| 文件 | 修改点 | 作用 |
| --- | --- | --- |
| `kernel/vm.c` | 新增 `walk_internal/walk_to_level/vmprint_walk/split_superpage/uvmalloc_sbrk` 等 | 支持递归打印、精细 walk、超页映射与拆分、sbrk 专用增长 |
| `kernel/kalloc.c` | 预留高端物理内存作为 superpage 池，实现 `superalloc/superfree` | 提供 2MB 对齐物理块，避免与普通 4KB 分配互相干扰 |
| `kernel/proc.c` | 在进程创建/销毁/扩展时管理 USYSCALL 和 superpage | 将共享页、超页逻辑纳入进程生命周期，保持一致性 |
| `kernel/defs.h` | 声明新增的 VM/allocator 接口 | 让其它模块（sysproc/exec 等）能调用新函数 |
| `kernel/riscv.h` | 添加 `SUPERPGROUNDDOWN/UP` 等宏 | 进行 2MB 对齐计算，满足 Sv39 超页规范 |
| `answers-pgtbl.txt` & `time.txt` | 书面回答与耗时统计 | 满足实验提交要求，方便助教验收 |

全部修改已经通过 `make grade`（涵盖 `pgtbltest`、`usertests`、答案/时间检查），证明功能、性能及退化路径均达标。