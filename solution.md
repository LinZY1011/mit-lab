# xv6 lab solutions — 总结

## 1) Using GDB (easy) ✅

### 要求
- 使用 gdb 与 qemu-gdb 联合调试 xv6。练习在 gdb 中设断点（例如在 syscall() 处）、单步 (n)、查看布局 (layout src / layout asm) 与打印后端状态（backtrace、print /x 等）。
- 使用 gdb 检查陷入 panic 的内核状态：定位 faulting epc 并打印相关寄存器与内存信息以帮助定位问题。

### 实验步骤与实现（手动调试步骤）
- 在两条终端中运行：
  - 终端 A: `make qemu-gdb`（启动 qemu 并让其等待 gdb）
  - 终端 B: `gdb xv6` 并使用 gdb 命令连接到 qemu（`target remote :1234`）
- 在 gdb 中设置断点：`b syscall`，继续运行 `c`。当 syscall 被触发时，使用 `layout src`、`backtrace`、`n` 单步，并使用 `p /x *p` 打印 `proc` 结构。
- 通过查看 `p->trapframe->a7` 可知当前系统调用号；例如示例里为 0x7（即第 7 个 syscall）。
- 查看 `sstatus` 可以判断先前的 CPU 模式：若 SPP=0 则之前是 user mode。

### 调试 kernel panic
- 若在 `syscall` 中替换 `num = p->trapframe->a7;` 为 `num = * (int *)0;` 会触发页面错误 (page-fault)；内核 panic 输出 `scause` 与 `sepc`。在 gdb 中可以设置 `b *sepc` 去捕获并查明出错的汇编指令。
- 变量 `num` 对应的寄存器/内存位置可以在编译后的 `kernel/kernel.asm` 与 gdb 的 `layout asm` 下看到，通常 `num` 来自 `p->trapframe->a7`。

### 课堂练习答案示例（来自仓库 `answers-syscall.txt`）
- syscall 被 `usertrap()` 调用（stack backtrace 显示 `syscall` 在 kernel/syscall.c）。
- `p->trapframe->a7` 的值示例：0x7（表示当前 syscall 的号码）。
- `sstatus` 示例值：0x200000022，SPP=0 → 之前为 user 模式。

---

## 2) System call tracing (moderate) ✅

### 要求
- 在 xv6 中实现新的 `trace(mask)` 系统调用：允许用户为 _进程及其后代_ 打开系统调用跟踪。
- `mask` 是一个位掩码：如果某个系统调用编号的位被置 1，则在该系统调用返回时，内核应打印一行：`<pid>: syscall <name> -> <ret>`（只需要打印名称与返回值）。

### 已实现文件与关键点（仓库中已有实现）
- `user/trace.c`：用户级工具，将 `trace(atoi(argv[1]))` 传到内核，然后 `exec` 目标命令。
- `user/user.h`：包含了 `int trace(int);` 的声明。
- `user/usys.pl` 生成 `user/usys.S`（系统调用存根）——仓库已生成对 `trace` 的汇编接口（`user/trace.asm`）。
- `kernel/syscall.h`：新增宏 `#define SYS_trace 22`（syscall 编号）。
- `kernel/sysproc.c`：实现 `sys_trace()`，通过 `argint(0, &(myproc()->trace_mask));` 把掩码保存在 `proc->trace_mask`。
- `kernel/proc.c`：修改 `fork()`，确保 `np->trace_mask = p->trace_mask;` 把父进程的 trace 掩码继承到子进程。
- `kernel/syscall.c`：将 `sys_trace` 加入 `syscalls[]` 与 `syscalls_name[]`，并在 `syscall()` 中在调用返回后检查 `if ((1 << num) & p->trace_mask)`，若匹配则打印跟踪信息 `printf("%d: syscall %s -> %ld\n", p->pid, syscalls_name[num], p->trapframe->a0);`。

### 行为示例（仓库测试 `grade-lab-syscall`）
- `trace 32 grep hello README` 仅跟踪 `read`（32 == 1<<SYS_read），输出类似：
  - `3: syscall read -> 1023`
- `trace 2147483647 grep hello README` 跟踪所有 31 个低位 syscall（输出包含 `trace`, `exec`, `open`, `read`, 等等）。
- `trace 2 usertests forkforkfork` 会跟踪 `fork`（2 == 1<<SYS_fork）并显示被跟踪子孙进程的 fork 调用。

---

## 3) Attack xv6 (moderate) ✅

### 要求
- 该练习模拟内核层面的安全 bug：在本 lab 指定的编译条件下，清理（memset）新分配页帧的代码被注释掉，导致新分配页面可能包含先前使用者的数据。
- 目标：编写一个 `user/attack.c`，在 `secret.c` 运行后（它在内存中写下一个 8 字节 secret 并退出），`attack` 能够找到并把该 secret 写到 fd 2，使 `attacktest` 通过（打印 `OK: secret is <str>`）。只能修改 `user/attack.c`。

### 已实现攻击思路（仓库实现说明）
- `user/secret.c`：向 `sbrk()` 分配的区域写入 secret，放在所在页的 offset 32 字节处（原始题目使用 32；在这个仓库中 secret.c 将 secret 写入 end + 32）。
- `user/attack.c`：实现如下技巧：
  - 先调用 `sbrk(PGSIZE*32)` 扩展进程地址空间（分配许多页）并把返回值存为 `end`。
  - 将 `end` 向前偏移到特定页（示例中 `end + 16 * PGSIZE`）来取到之前 freed 页面所包含的内容。
  - 从 `end + 32` 处读 8 字节并 `write(2, ..., 8)` 把 secret 输出到文件描述符 2（stderr），从而被 `attacktest` 捕获并验证。

### 为什么能成功？
- 由于内核在本次 lab 下编译时没有把新分配页清零，`secret` 程序退出后释放的内存页仍然含有 secret内容。随后 `attack` 分配并使用同一页（或包含旧内容的页），读取已存在的字节并输出，因此泄露 secret。

### 额外观察
- 如果 `secret.c` 把 secret 写在页内较靠前或靠后的偏移（例如把 32 改成 0），攻击可能失败：攻击依赖于对内核内存分配与页面复用策略的利用。

