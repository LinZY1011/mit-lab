# Traps Lab Report

## 修改概览
- **backtrace 支持**：在 `kernel/riscv.h` 提供 `r_fp()` 以读取当前帧指针；`kernel/printf.c` 实现 `backtrace()`，通过帧链输出返回地址，并在 `panic()` 及 `sys_sleep()` 中调用，方便调试，如 `bttest` 需要的栈回溯。
- **proc 结构扩展**：向 `kernel/proc.h` 添加报警相关字段（间隔、剩余 ticks、处理器地址、激活标志、保存的 trapframe），并在 `kernel/proc.c` 的分配、释放、fork 过程中初始化/继承这些状态，保证每个进程都有独立的报警上下文。
- **系统调用接口**：在 `user/user.h`、`user/usys.pl`、`kernel/syscall.[ch]`、`kernel/defs.h` 中声明/导出 `sigalarm` 与 `sigreturn`，并在 `kernel/sysproc.c` 实现其逻辑：`sigalarm` 记录周期与处理函数，`sigreturn` 恢复 trapframe、重新装填计数器，保证用户态能安全恢复。
- **陷阱处理**：`kernel/trap.c` 的 `usertrap()` 在定时器中断时递减 per-proc tick 计数，计数到 0 时保存用户 trapframe、切换 `epc` 到用户回调并阻止重入，`sigreturn` 完成恢复；同时保持原先的 `yield()` 行为，确保调度公平。
- **构建系统**：在 `Makefile` 的 traps 分支中加入 `alarmtest`，确保测试程序被打包进文件系统。
- **实验文档**：`answers-traps.txt` 回答了 warm-up 问题，`time.txt` 记录实验耗时。

## 关键操作系统原理
- **栈帧回溯**：RISC-V ABI 维护 `s0/fp` 作为帧指针，返回地址恒位于 `fp-8`，上一帧指针位于 `fp-16`。利用这些约定可以在内核中沿链遍历，输出调用栈，用于定位 panic 或 sleep 调用路径。
- **陷阱与特权切换**：用户代码由 `sret` 返回，`usertrap()` 保存 `sepc` 等寄存器，`usertrapret()` 恢复现场并重新开启用户态中断。alarm 功能正是利用定时器陷阱在内核中拦截，再将 `trapframe->epc` 改为用户 handler，实现用户级“中断”。
- **定时器驱动的用户级中断**：每个进程记录自身还需多少 tick，只有在该进程真正消耗 CPU（定时器在 user 态触发）时才递减；当计数归零即复制 trapframe、跳转到 handler。handler 结束必须 `sigreturn()`，以便恢复寄存器并重新武装定时器，防止重入（通过 `alarm_active` 标志）。
- **系统调用约定**：`sigalarm`/`sigreturn` 通过 `a7` 传递编号，参数仍走 `a0/a1`。需要注意函数指针可能位于地址 0（`alarmtest` 特意如此），因此内核不能以 0 判定“无效 handler”，而应仅依赖 `alarm_interval` 是否为 0 来判断启停。
- **进程继承与清理**：新增内核状态必须在 `allocproc()` 初始化、`freeproc()` 清零、`fork()` 复制，防止旧值泄露给其他进程或导致未定义行为。

## 测试
- `./grade-lab-traps` —— 通过 backtrace、alarmtest（全部子测）以及 `usertests -q`，总分 95/95。
