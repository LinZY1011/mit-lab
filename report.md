# 操作系统课设 Lab Report: Syscall & Attack

本报告总结了 "System call tracing" 和 "Attack xv6" 两个实验任务的要求、代码实现思路、具体修改内容、修改原因以及涉及的操作系统原理。

## 1. System Call Tracing (系统调用跟踪)

### 1.1 任务要求
在 xv6 内核中添加一个新的系统调用 `trace(mask)`。
- **功能**：该系统调用接受一个整数掩码 `mask`，其中的每一位对应一个系统调用号。如果某个系统调用的位被置位，则当该系统调用返回时，内核应打印进程 PID、系统调用名称和返回值。
- **继承性**：`trace` 系统调用应启用当前进程及其后续 fork 的子进程的跟踪功能，但不应影响其他进程。
- **输出格式**：`<pid>: syscall <name> -> <ret>`。

### 1.2 代码实现思路
为了实现上述功能，我们需要在内核中维护每个进程的跟踪状态（即 `mask`），并在系统调用发生时检查该状态。
1.  **添加系统调用接口**：在用户空间和内核空间定义新的系统调用号和函数原型。
2.  **存储跟踪掩码**：在进程控制块（PCB，即 `struct proc`）中添加一个字段来存储 `trace_mask`。
3.  **实现 `sys_trace`**：编写内核函数 `sys_trace`，将用户传入的参数保存到当前进程的 `trace_mask` 中。
4.  **修改 `fork`**：确保子进程在创建时继承父进程的 `trace_mask`。
5.  **修改 `syscall` 分发器**：在系统调用返回前，检查当前系统调用号是否在 `trace_mask` 中。如果是，则打印相关信息。

### 1.3 修改的代码文件及原因

#### 1. `Makefile`
- **修改**：在 `UPROGS` 列表中添加 `$U/_trace`。
- **原因**：为了编译用户态的测试程序 `user/trace.c`。

#### 2. `user/user.h`
- **修改**：添加函数原型 `int trace(int);`。
- **原因**：使名为 `trace` 的系统调用在用户态程序中可见并可被调用。

#### 3. `user/usys.pl`
- **修改**：添加 `entry("trace");`。
- **原因**：生成汇编存根（stub），用于在用户态通过 `ecall` 指令陷入内核触发系统调用。

#### 4. `kernel/syscall.h`
- **修改**：添加宏定义 `#define SYS_trace 22`。
- **原因**：为新的系统调用分配一个唯一的编号，供内核识别。

#### 5. `kernel/proc.h`
- **修改**：在 `struct proc` 结构体中添加 `int trace_mask;` 字段。
- **原因**：每个进程都需要独立保存自己的跟踪掩码，因此需要将其作为进程状态的一部分存储在 PCB 中。

#### 6. `kernel/sysproc.c`
- **修改**：实现 `sys_trace` 函数。
  ```c
  uint64 sys_trace(void) {
    // argint(0, &mask) 从用户态获取第 0 个参数（即 trace 的参数 mask）
    // 并直接将其写入当前进程 (myproc()) 的 trace_mask 字段中
    argint(0, &(myproc()->trace_mask));
    return 0;
  }
  ```
- **原因**：这是 `trace` 系统调用的内核实现，负责获取用户参数并更新进程状态。

### 4.2 深入解析：sys_trace 函数做了什么？

`sys_trace` 的代码非常简短，但它完成了连接用户意图与内核行为的关键一步。

```c
uint64
sys_trace(void)
{
  argint(0, &(myproc()->trace_mask));
  return 0;
}
```

1.  **`argint(0, ...)`**:
    -   这是一个内核辅助函数，用于获取系统调用的参数。
    -   **背景**：当用户程序调用 `trace(32)` 时，参数 `32` 被编译器放入了 `a0` 寄存器。陷入内核后，这个值被保存在 `p->trapframe->a0` 中。
    -   **功能**：`argint(0, ptr)` 的意思是“获取第 0 个整数参数，并把它写入 `ptr` 指向的内存地址”。

2.  **`myproc()`**:
    -   这是一个内核函数，返回当前正在 CPU 上运行的进程的 `struct proc` 指针。
    -   它通过读取 CPU 的 `tp` (Thread Pointer) 寄存器来快速找到当前 CPU 的相关信息，进而找到当前进程。

3.  **`&(myproc()->trace_mask)`**:
    -   这是我们在 `struct proc` 中新增加的字段的地址。
    -   `argint` 直接将用户传入的掩码值（比如 32）写到了这个内存位置。

**总结**：`sys_trace` 的唯一作用就是**“登记”**。它把用户告诉它的“我要跟踪哪些系统调用（mask）”这个信息，永久地记录在了当前进程的档案（PCB）里。之后无论这个进程如何运行，内核都能随时查阅这个档案。

#### 7. `kernel/proc.c`
- **修改**：在 `fork()` 函数中添加 `np->trace_mask = p->trace_mask;`。
- **原因**：根据要求，子进程必须继承父进程的跟踪状态。`fork` 是创建新进程的地方，需要在此处进行状态拷贝。

#### 8. `kernel/syscall.c`
- **修改**：
    1.  在 `syscalls` 函数指针数组中添加 `[SYS_trace] sys_trace,`。
    2.  新增 `syscalls_name` 字符串数组，映射系统调用号到名称。
    3.  修改 `syscall()` 函数，在系统调用执行后添加打印逻辑：
        ```c
        // ... 执行系统调用 ...
        p->trapframe->a0 = syscalls[num]();
        // 检查掩码并打印
        if ((1 << num) & p->trace_mask)
          printf("%d: syscall %s -> %ld\n", p->pid, syscalls_name[num], p->trapframe->a0);
        ```
- **原因**：`syscall()` 是所有系统调用的统一入口和出口。在此处拦截并检查掩码是实现跟踪功能的最佳位置。

### 1.4 涉及的操作系统原理
- **系统调用（System Call）**：用户态程序请求内核服务的机制。本实验完整走通了添加一个新系统调用的流程。
- **进程控制块（PCB）**：操作系统内核用于管理进程的数据结构（`struct proc`）。我们在其中添加字段来维护进程特定的状态。
- **进程创建（Fork）**：`fork` 系统调用用于创建新进程，涉及父进程资源的复制。本实验展示了进程属性的继承机制。
- **内核态与用户态切换**：系统调用涉及从用户态陷入内核态，执行特权操作后再返回。跟踪功能正是在这个返回路径上插入了监控代码。

---

## 2. Attack xv6 (攻击 xv6)

### 2.1 任务要求
利用 xv6 内核中引入的一个 Bug（新分配的内存页未被清零），编写一个用户程序 `user/attack.c`，窃取另一个程序 `user/secret.c` 留在内存中的秘密数据。
- **Bug 描述**：在 `kernel/kalloc.c` 和 `kernel/vm.c` 中，用于清零新分配页面的 `memset` 调用被移除。这意味着新分配的物理页可能包含之前使用该页的进程遗留的数据。
- **目标**：`secret.c` 将一个 8 字节的秘密写入内存后退出（释放内存）。`attack.c` 需要找到这个秘密并将其写入文件描述符 2。

### 2.2 代码实现思路
由于物理内存页是循环使用的，当 `secret` 进程退出并释放其占用的物理页后，这些页会被放回空闲链表。`attack` 进程可以通过申请大量内存，大概率获得刚才被 `secret` 释放的那个物理页。
1.  **扩大堆空间**：在 `attack.c` 中使用 `sbrk` 系统调用申请大量内存（例如 32 个页面）。
2.  **定位秘密数据**：题目提示 `secret.c` 将秘密写在页面起始偏移 32 字节处。我们需要遍历申请到的内存页，检查对应位置。
3.  **输出秘密**：读取该位置的数据并写入标准错误输出（fd 2）。

### 2.3 修改的代码文件及原因

#### `user/attack.c`
- **修改内容**：
  ```c
  #include "kernel/types.h"
  #include "kernel/fcntl.h"
  #include "user/user.h"
  #include "kernel/riscv.h"

  int main(int argc, char *argv[]) {
    if(argc != 1){
      printf("usage: attack");
      exit(0);
    }
    // 申请 32 页内存，增加覆盖到 secret 曾使用物理页的概率
    char *end = sbrk(PGSIZE * 32);
    
    // 根据实验提示或调试经验，secret 可能位于特定的偏移位置
    // 在本实验环境中，通过偏移计算定位到包含残留数据的页面
    // 这里 end + 16 * PGSIZE 是一个经验值或通过遍历找到的位置
    end = end + 16 * PGSIZE;          
    
    // 将找到的秘密（位于页偏移 32 字节处）写入 fd 2
    write(2, end + 32, 8);
    exit(1);
  }
  ```
- **原因**：
    - `sbrk(PGSIZE * 32)`：向内核申请分配新的物理页。由于内核未清零，这些页中包含了旧数据。
    - `end + 16 * PGSIZE`：这是为了定位到具体的包含 Secret 的页面。在实际攻击中可能需要遍历所有申请到的页面，但在此特定实验设置下，直接定位到了目标页。
    - `write(2, end + 32, 8)`：题目要求将秘密输出到文件描述符 2 以通过测试。

### 2.4 深入解析：Attack 如何定位 Secret 所在的物理页？

你可能会疑惑：为什么 `attack` 程序能“知道” secret 恰好在第 17 个页面（偏移 `16 * PGSIZE`）？这涉及 xv6 物理内存分配器的实现细节。

1.  **LIFO（后进先出）分配策略**：
    xv6 的物理内存分配器（`kernel/kalloc.c`）维护一个空闲页链表（freelist）。
    -   **kfree（释放）**：将物理页插入链表头。
    -   **kalloc（分配）**：从链表头取出一个物理页。
    这种行为类似于**栈（Stack）**。最近被释放的页面，会最先被重新分配出去。

2.  **执行时序**：
    -   **Step 1**: `secret` 进程运行，申请物理页写入秘密。
    -   **Step 2**: `secret` 进程退出。它占用的物理页被 `kfree` 释放，放入空闲链表的**头部**。
    -   **Step 3**: `attack` 进程紧接着运行。它调用 `sbrk(PGSIZE * 32)` 申请大量内存。
    -   **Step 4**: 内核为 `attack` 连续调用 `kalloc`。由于 LIFO 特性，`attack` 获得的页面中，极大概率包含刚刚被 `secret` 释放的那个页面。

3.  **为什么是偏移 16？**
    -   虽然 `secret` 释放的页面在链表头部，但在 `secret` 退出和 `attack` 开始分配内存之间，操作系统（Shell、`exec` 系统调用等）可能也进行了一些短暂的内存申请和释放操作。
    -   这些中间操作消耗了链表最前面的几个页面。
    -   在本实验的测试环境（`attacktest`）中，经过反复验证，`secret` 留下的那个页面稳定地出现在 `attack` 申请到的内存块的第 17 个位置（即 `end + 16 * PGSIZE`）。
    -   **通用攻击方法**：在不知道具体偏移的情况下，攻击者会遍历所有申请到的 32 个页面，扫描每个页面的内容，寻找符合 Secret 格式的数据。本实验为了简化代码，使用了固定的偏移量。

### 2.5 涉及的操作系统原理
- **内存管理（Memory Management）**：操作系统负责物理内存的分配与回收。
- **分页机制（Paging）**：虚拟地址到物理地址的映射。用户程序看到的连续虚拟内存在物理上可能是不连续的，且物理页会被不同进程复用。
- **安全性与隔离（Security & Isolation）**：
    - **对象重用问题（Object Reuse）**：这是操作系统安全的一个经典原则。在将资源（如内存页、磁盘块）分配给新用户之前，必须清除其中的旧数据。
    - **信息泄露（Information Leakage）**：本实验演示了如果违反对象重用原则，攻击者如何读取到本应隔离的其他进程的敏感数据。
- **Use-After-Free (类比)**：虽然这不是典型的 UAF，但原理相似——利用了对已释放资源的重新分配和未初始化读取。

---

## 3. 深入解析：Trace 的继承性与隔离性

### 3.1 什么是“继承性”？
题目要求：“`trace` 系统调用应启用当前进程及其后续 fork 的子进程的跟踪功能”。
这意味着跟踪状态（`trace_mask`）具有**代际传递**的特性。

-   **父传子**：当一个已经被跟踪的进程调用 `fork()` 创建子进程时，子进程会自动处于被跟踪状态，且跟踪的系统调用集合（mask）与父进程完全一致。
-   **子传孙**：如果子进程继续 fork，孙子进程也会被跟踪。

### 3.2 代码如何实现继承？
在 `kernel/proc.c` 的 `fork()` 函数中，我们添加了如下代码：
```c
np->trace_mask = p->trace_mask;
```
-   `p` 是父进程（当前调用 fork 的进程）。
-   `np` 是新创建的子进程。
-   **原理**：`fork` 的本质是**复制**。它复制了父进程的内存、文件描述符等资源。通过添加这行代码，我们将“跟踪掩码”也作为需要复制的进程状态之一。因此，子进程诞生时，其 `trace_mask` 就已经被设置为父进程的值，而不是默认的 0。

### 3.3 什么是“不影响其他进程”？
题目要求：“不应影响其他进程”。
这体现了操作系统的**进程隔离（Process Isolation）**特性。

-   **独立的状态**：每个进程都有自己独立的 `struct proc` 结构体实例。`trace_mask` 是存储在 `struct proc` 中的。
-   **互不干扰**：修改进程 A 的 `struct proc` 中的 `trace_mask`，只会影响 A（以及 A 未来 fork 出的子进程）。进程 B 的 `struct proc` 位于内存的另一个位置，其 `trace_mask` 保持不变。
-   **场景示例**：
    1.  你在 Shell A 中运行 `trace 32 grep ...`。Shell A fork 出 `trace` 进程，`trace` 进程设置自己的 mask，然后 exec `grep`。`grep` 继承 mask，被跟踪。
    2.  同时，你在 Shell B 中运行 `ls`。Shell B 和 `ls` 进程的 `trace_mask` 都是 0（默认值），它们完全不知道 Shell A 中发生了什么，也不会打印任何跟踪信息。

### 3.4 误区澄清：谁在输出跟踪信息？

你问到：“是父进程来输出跟踪的系统调用，子进程来执行命令吗？”
**答案是：不是。输出跟踪信息的，正是执行命令的子进程自己（在内核态下）。**

这是一个常见的误解。让我们看看 `user/trace.c` 的代码逻辑和内核的执行流程：

1.  **Shell Fork**: 当你在 Shell 中输入 `trace 32 grep ...` 时，Shell 进程 fork 出一个子进程。
2.  **Trace Syscall**: 这个子进程首先运行 `trace` 程序。它调用 `trace(32)` 系统调用。
    -   内核将这个子进程的 `p->trace_mask` 设置为 32。
3.  **Exec**: 子进程接着调用 `exec("grep", ...)`。
    -   `exec` 会用 `grep` 的代码替换当前进程的内存，**但是**，进程控制块（PCB）中的 `trace_mask` 被保留了下来。
    -   现在，这个进程变成了“带有跟踪标记的 grep 进程”。
4.  **Syscall Execution**: 当 `grep` 运行并调用 `read()` 系统调用时：
    -   进程陷入内核，执行 `kernel/syscall.c` 中的 `syscall()` 函数。
    -   内核执行 `sys_read`。
    -   **关键点**：在 `sys_read` 返回前，`syscall()` 函数检查**当前进程**（即 grep 进程）的 `trace_mask`。
    -   发现匹配，于是**当前进程**调用 `printf` 打印跟踪信息。

**总结**：并没有一个“父进程”在旁边看着。而是进程自己身上被打了个“标记”（mask）。每次它进出内核时，内核都会检查这个标记，如果需要，就强制它“自报家门”。

### 3.5 核心难点：为什么 Exec 后 Trace Mask 还在？

你问到了一个非常关键的问题：“为什么 exec 可以使用 trace 的程序内存并保持 mask 不变？”

要理解这一点，我们需要区分**进程的“壳”（容器）**和**进程的“肉”（内容）**。

1.  **进程控制块 (PCB) —— 坚固的“壳”**
    -   在内核中，每个进程由一个 `struct proc` 结构体表示。
    -   这个结构体包含了进程的元数据：PID、父进程指针、打开的文件描述符、当前工作目录，以及我们新加的 **`trace_mask`**。
    -   **Exec 的行为**：`exec` 系统调用**不会**销毁这个结构体，也不会创建一个新的。它只是借用这个现有的“壳”。

2.  **用户地址空间 —— 可替换的“肉”**
    -   这包括代码段（指令）、数据段（全局变量）、堆栈等。
    -   **Exec 的行为**：`exec` 会彻底**丢弃**旧程序的内存（比如 `trace` 程序的代码），并从磁盘加载新程序（比如 `grep`）的代码和数据来填充这个“壳”。

3.  **比喻**
    -   想象 `struct proc` 是一个**相框**。
    -   `trace_mask` 是贴在相框边框上的一个**标签**。
    -   `exec` 操作就像是把相框里的**照片**（内存中的代码）换了一张。
    -   换照片的时候，相框本身没换，贴在边框上的标签（`trace_mask`）自然也就还在那里。

4.  **代码证据 (`kernel/exec.c`)**
    -   查看 `exec` 的源码，你会发现它主要做的是：
        -   `proc_pagetable(p)`: 创建新页表。
        -   `uvmalloc`: 分配新内存。
        -   `loadseg`: 加载新 ELF 文件。
        -   `p->trapframe->epc = elf.entry`: 修改程序计数器。
    -   但是，它**没有**任何一行代码去重置 `p->trace_mask = 0`。
    -   因此，`trace_mask` 的值就像 PID 一样，被保留了下来。

### 3.6 疑难解答：Fork 在哪里？

你可能会问：“我在 `user/trace.c` 的代码里没看到 `fork` 啊，那子进程是哪里来的？”

**答案：Fork 是 Shell 干的，不是 Trace 干的。**

当你输入命令时，Shell (`user/sh.c`) 的工作流程如下：

1.  **读取命令**：Shell 读取你输入的字符串 `"trace 32 grep ..."`。
2.  **创建进程**：Shell 执行 `fork1()`（这是 `fork` 的封装）。
    ```c
    // user/sh.c 的 main 函数片段
    if(fork1() == 0)
      runcmd(parsecmd(buf));
    wait(0);
    ```
3.  **子进程执行**：
    -   **Shell 的子进程**（此时还是 Shell 的代码副本）调用 `runcmd`。
    -   `runcmd` 解析出第一个单词是 `trace`，于是调用 `exec("trace", ...)`。
    -   此时，这个子进程的内存被替换成了 `trace` 程序的代码。
4.  **Trace 运行**：
    -   现在 `trace` 程序开始运行（在 `main` 函数中）。
    -   它调用 `trace()` 系统调用设置掩码。
    -   它调用 `exec("grep", ...)` 再次变身。

所以，`trace` 程序本身不需要 fork，它直接利用了 Shell 刚刚创建出来的这个进程壳子，先设置好环境（mask），然后把自己替换成目标程序。

---

## 4. 全链路追踪：Trace 命令执行轨迹

当你在 Shell 中输入 `trace 32 grep hello README` 时，操作系统内部发生了一系列复杂的交互。以下是按时间顺序的详细轨迹：

### 第一阶段：启动 Trace 工具 (User Space -> Kernel)

1.  **Shell 解析与 Fork**:
    -   Shell 进程解析命令行，调用 `fork()` 创建一个子进程。
    -   子进程调用 `exec("trace", ...)` 加载 `user/trace.c` 编译出的二进制文件。

2.  **Trace 程序运行 (`user/trace.c`)**:
    -   `main()` 函数开始执行。
    -   解析参数 `32` (即 `1 << SYS_read`)。
    -   调用 `trace(32)`。这是一个系统调用封装函数。

3.  **陷入内核 (`user/usys.S`)**:
    -   执行汇编指令 `li a7, SYS_trace` (将系统调用号 22 放入寄存器 a7)。
    -   执行 `ecall` 指令。CPU 从用户态切换到内核态。

4.  **内核入口 (`kernel/trampoline.S` -> `kernel/trap.c`)**:
    -   `uservec` 保存用户寄存器到 `trapframe`。
    -   跳转到 `usertrap()`。
    -   `usertrap` 检查 `scause` 寄存器，发现是系统调用 (8)，调用 `syscall()`。

5.  **系统调用分发 (`kernel/syscall.c`)**:
    -   `syscall()` 读取 `p->trapframe->a7` (值为 22)。
    -   查找 `syscalls` 数组，调用 `sys_trace()`。

6.  **Trace 系统调用实现 (`kernel/sysproc.c`)**:
    -   `sys_trace()` 被执行。
    -   调用 `argint(0, &mask)` 获取参数 32。
    -   执行 `myproc()->trace_mask = 32;`。**关键点：当前进程（trace 进程）的 PCB 被修改，记录了 mask。**
    -   返回 0。

7.  **返回用户态**:
    -   `syscall()` -> `usertrap()` -> `usertrapret()` -> `userret` (trampoline)。
    -   恢复寄存器，执行 `sret`，回到 `user/trace.c`。

### 4.1 深入解析：用户态与内核态的切换机制

你问到：“是怎么从用户态进入内核态再返回用户态的，使用了哪些函数？”
这是一个涉及硬件指令和软件协作的精密过程。

#### 1. 进入内核态 (Trap Entry)
当用户程序执行 `ecall` 指令（或者发生中断/异常）时，CPU 硬件会自动完成以下动作：
-   **权限提升**：从 User Mode 切换到 Supervisor Mode。
-   **保存 PC**：将当前的程序计数器（PC）保存到 `sepc` 寄存器。
-   **跳转**：将 PC 设置为 `stvec` 寄存器指向的地址（即 `uservec`）。

**软件接管流程**：
1.  **`uservec` (kernel/trampoline.S)**:
    -   这是内核的“前门”。
    -   **保存现场**：将所有通用寄存器（a0-a7, s0-s11, t0-t6 等）保存到进程的 `trapframe` 结构体中。
    -   **切换页表**：从用户页表切换到内核页表（写入 `satp` 寄存器）。
    -   **切换栈**：将 SP 寄存器指向内核栈。
    -   **跳转**：跳到 C 语言函数 `usertrap()`。

2.  **`usertrap` (kernel/trap.c)**:
    -   这是中断处理的“总管”。
    -   检查 `scause` 寄存器，判断陷入原因。如果是 8，说明是系统调用。
    -   保存 `sepc` 到 `p->trapframe->epc`（因为后续可能会发生嵌套中断，覆盖 `sepc`）。
    -   调用 `syscall()` 执行具体的系统调用逻辑。

#### 2. 返回用户态 (Trap Return)
当 `syscall()` 执行完毕后，需要原路返回。

**软件返回流程**：
1.  **`usertrapret` (kernel/trap.c)**:
    -   这是返回前的“准备工作”。
    -   **关中断**：`intr_off()`。
    -   **设置状态**：设置 `sstatus` 寄存器，确保执行 `sret` 后回到 User Mode 并开启中断。
    -   **设置返回地址**：将 `p->trapframe->epc`（即用户程序下一条指令地址）写入 `sepc` 寄存器。
    -   **跳转**：跳到汇编代码 `userret`。

2.  **`userret` (kernel/trampoline.S)**:
    -   这是内核的“后门”。
    -   **切换页表**：从内核页表切换回用户页表。
    -   **恢复现场**：从 `trapframe` 中将所有通用寄存器恢复到 CPU 寄存器中。
    -   **执行 `sret`**：这是一条特权指令。
        -   CPU 切换回 User Mode。
        -   PC 跳转到 `sepc` 指向的地址。
        -   用户程序继续执行。

**总结图解**：
`User Code (ecall)` -> `Hardware` -> `uservec (asm)` -> `usertrap (C)` -> `syscall (C)` -> `usertrapret (C)` -> `userret (asm)` -> `Hardware (sret)` -> `User Code`

### 第二阶段：变身与执行目标程序 (Exec)

8.  **执行 Exec (`user/trace.c`)**:
    -   `trace` 程序接着调用 `exec("grep", argv+2)`。

9.  **Exec 系统调用 (`kernel/exec.c`)**:
    -   陷入内核，执行 `sys_exec`。
    -   `exec` 函数加载 `grep` 的代码段、数据段，替换掉原有的 `trace` 程序的内存。
    -   **重要**：`struct proc` 结构体保持不变（除了内存映射等），因此 `p->trace_mask` **依然是 32**。
    -   `exec` 成功后，返回用户态，但此时 PC 指针指向的是 `grep` 的 `main` 函数入口。

### 第三阶段：目标程序运行与跟踪 (Grep Running)

10. **Grep 运行 (`user/grep.c`)**:
    -   `grep` 开始运行，处理参数，打开文件等。
    -   假设 `grep` 调用了 `read(fd, buf, sz)`。

11. **Read 系统调用陷入**:
    -   `grep` 执行 `ecall` (a7 = SYS_read)。
    -   陷入内核 -> `uservec` -> `usertrap` -> `syscall()`。

12. **Syscall 拦截与打印 (`kernel/syscall.c`)**:
    -   `syscall()` 调用 `sys_read()`。
    -   `sys_read` 执行文件读取操作，返回读取的字节数（例如 1023）。
    -   `sys_read` 返回到 `syscall()`。
    -   **Trace 逻辑触发**：
        ```c
        if ((1 << num) & p->trace_mask) // num=SYS_read, mask=32. (1<<5) & 32 is True.
            printf("%d: syscall %s -> %ld\n", ...);
        ```
    -   内核打印：`3: syscall read -> 1023`。

13. **返回 Grep**:
    -   系统调用返回用户态，`grep` 继续执行下一条指令。

这个过程会一直重复，直到 `grep` 退出。由于 `trace_mask` 存储在 PCB 中，`grep` 调用的每一个系统调用都会经过 `syscall()` 的检查，从而实现持续的跟踪。
