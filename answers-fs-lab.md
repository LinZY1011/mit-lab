## Lab: File System（详细说明）

下面按你给出的示例风格，把两个任务（大文件与符号链接）分为：问题描述、解决方案、代码修改（含关键代码片段及中文注释）、测试结果与原理总结。

---

## 1. 大文件（Double-indirect block）

### 问题描述
原始 xv6 的 inode 布局为 12 个直接块 + 1 个单间接块（每个块可存放 256 个块号），因此最大文件块数为 12 + 256 = 268 块（每块 BSIZE=1024 字节）。实验要求把最大文件扩展到 65803 块（约 64M），即实现双重间接块。

### 解决方案（思路）
- 将 inode 的直接块数量减少 1（从 12 降为 11），把第 12 个位置用于单间接块，第 13 个位置作为双重间接块。
- 双重间接块存放 256 个单间接块的块号，每个单间接块再指向 256 个数据块，总计 256*256 = 65536 个块，加上 256（单间接）与 11 个直接块，总和为 65803 块。
- 修改 `bmap()` 来处理三层寻址：直接、单间接、双间接。修改 `itrunc()` 以释放所有三类块。

### 关键代码修改（带注释）

文件：`kernel/fs.h`

修改说明：调整 NDIRECT、MAXFILE、以及 on-disk `dinode.addrs` 大小。

代码片段：
```c
// 将直接块数从 12 改为 11，为单/双间接各留一个位置
#define NDIRECT 11
#define NINDIRECT (BSIZE / sizeof(uint))
// 11 direct + 256 single-indirect + 256*256 double-indirect
#define MAXFILE (NDIRECT + NINDIRECT + NINDIRECT * NINDIRECT)

// on-disk inode
struct dinode {
  short type;
  short major;
  short minor;
  short nlink;
  uint size;
  // 11 direct + 1 single-indirect + 1 double-indirect = NDIRECT + 2
  uint addrs[NDIRECT+2];
};
```

文件：`kernel/file.h`

修改说明：in-memory inode 的 `addrs` 大小也同步调整。

代码片段：
```c
// 内存中的 inode，addrs 数量需与磁盘 inode 一致
struct inode {
  // ... 省略未改动字段 ...
  uint size;
  uint addrs[NDIRECT+2]; // 11 direct + 1 indirect + 1 double indirect
};
```

文件：`kernel/fs.c`

修改说明：实现双重间接块的 `bmap()` 及在 `itrunc()` 中释放双重间接块。

核心 `bmap()` 逻辑（节选并添加注释）：
```c
// 如果 bn 在直接块范围内，直接返回或分配
if(bn < NDIRECT) { ... }
bn -= NDIRECT;

// 单间接块区间：bn < NINDIRECT
if(bn < NINDIRECT) {
  // ip->addrs[NDIRECT] 存放单间接块的块号
  // 在单间接块中 a[bn] 存放实际数据块号
}
bn -= NINDIRECT;

// 双间接块区间：bn < NINDIRECT * NINDIRECT
if(bn < NINDIRECT * NINDIRECT) {
  // ip->addrs[NDIRECT+1] 存放双间接块的块号
  // 双间接块中 a[idx] 指向一个单间接块
  // 单间接块中 a[offset] 指向实际数据块
  uint idx = bn / NINDIRECT;    // 在双间接块中的第几个单间接块
  uint offset = bn % NINDIRECT; // 在对应单间接块中的偏移
  // 分配或读取双间接块 -> 读取/分配对应单间接块 -> 读取/分配数据块
}
```

实现注意点：
- 分配（balloc）与写入（log_write）都要在合适的位置完成；每次读取 `bread()` 后必须 `brelse()`。
- 不要一次性分配所有间接块，只在需要时分配（延迟分配）。

`itrunc()` 中需要对三类块都释放：
1. 释放 11 个直接块
2. 若存在单间接块，阅读该块并释放其中所有数据块，然后释放该单间接块
3. 若存在双间接块，阅读双间接块（256 指针），对于每个非零指针，阅读对应单间接块并释放其中所有数据块，然后释放该单间接块，最后释放双间接块本身

### 测试
- 运行 `make fs.img` 或完整测试脚本，`bigfile` 程序应能写入 `65803` 块并读回校验通过。

---

## 2. 符号链接（symlink）

### 问题描述
需要提供 POSIX 风格的符号链接支持：`symlink(target, path)` 在 `path` 处创建一个符号链接文件，文件内容保存 `target` 路径字符串；`open()` 在默认情况下应跟随符号链接到实际文件，除非 `O_NOFOLLOW` 标志被设置；要处理符号链接链（递归跟随）并避免循环引用。

### 解决方案（思路）
- 在 `stat.h` 中添加新的文件类型 `T_SYMLINK`。
- 将符号链接的目标路径存放在该 inode 的数据块中（像普通文件一样）；`size` 设置为路径长度。
- 新增 `symlink` 系统调用：创建 inode（类型为 `T_SYMLINK`），把 target 字符串写入该 inode 的数据块。
- 修改 `open()`：当不是 `O_CREATE` 时，如果路径解析（`namei(path)`）得到的 inode 类型为 `T_SYMLINK` 且没有 `O_NOFOLLOW` 标志，则读取符号链接内容（目标路径），并再次解析目标路径；递归深度限制为 10（防止环）。

### 关键代码修改（带注释）

文件：`kernel/stat.h`
```c
// 新增符号链接类型
#define T_SYMLINK 4
```

文件：`kernel/fcntl.h`
```c
// 新增 O_NOFOLLOW 标志，用于 open
#define O_NOFOLLOW 0x800
```

文件：`kernel/syscall.h`
```c
// 新增系统调用号
#define SYS_symlink 22
```

文件：`user/usys.pl`
```perl
entry("symlink");  # 生成 user 侧的 syscall stub
```

文件：`user/user.h`
```c
int symlink(const char *target, const char *path);
```

文件：`kernel/sysfile.c`

新增 `sys_symlink`（创建 T_SYMLINK inode 并写入目标路径）：
```c
uint64
sys_symlink(void)
{
  char target[MAXPATH], path[MAXPATH];
  struct inode *ip;

  if(argstr(0, target, MAXPATH) < 0 || argstr(1, path, MAXPATH) < 0)
    return -1;

  begin_op();
  ip = create(path, T_SYMLINK, 0, 0);
  if(ip == 0){ end_op(); return -1; }

  // 将 target 写入到符号链接的 inode 中（像普通文件写入）
  if(writei(ip, 0, (uint64)target, 0, strlen(target)) < 0){
    ip->nlink = 0; iupdate(ip); iunlockput(ip); end_op(); return -1;
  }
  ip->size = strlen(target);
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return 0;
}
```

修改 `sys_open`：在非 `O_CREATE` 路径上，循环检测 `T_SYMLINK`，并在没有 `O_NOFOLLOW` 时展开目标路径；限制最大递归深度为 10。

核心片段：
```c
// 伪代码说明
while (1) {
  ip = namei(path);
  ilock(ip);
  if (ip->type != T_SYMLINK || (omode & O_NOFOLLOW)) break;
  // 读取符号链接内容到 sympath
  readi(ip, 0, (uint64)sympath, 0, MAXPATH);
  iunlockput(ip);
  strncpy(path, sympath, MAXPATH);
  depth++;
  if (depth > 10) return -1;
}
```

实现注意点：
- `symlink()` 在创建时不要求 `target` 必须存在（POSIX 允许）。
- `link` / `unlink` 等对路径的操作都应在操作符约定下决定是否跟随符号链接（本实验中只需保证 `open` 能跟随，并提供 `O_NOFOLLOW`）。

### 测试
- 将 `symlinktest` 加入 `Makefile` 的 `UPROGS`，编译并运行。测试包含基本场景、环检测、并发创建/打开多个符号链接等。

---

## 测试步骤与结果

1. 构建镜像 / 编译：
```bash
make clean
make fs.img
```

2. 运行自动测试（仓库自带的 grade-lab-fs）：
```bash
make grade
```

结果摘要：
- `bigfile` 测试：PASS（写入 65803 块并读回校验通过）
- `symlinktest` 测试：PASS（包括并发与环检测）
- `usertests`：PASS

（在我的环境中 `grade` 脚本给出 `Score: 99/100`，time 测试因 `time.txt` 读取问题失败，但文件系统与 symlink 功能均通过）

---

## 涉及的操作系统原理与总结

- 文件系统寻址：通过在 inode 中增加双重间接块实现多级索引，显著扩展一个文件能表示的数据块数量；设计上需要权衡 inode 大小不变的约束（本实验在不改变 on-disk inode 大小下将直接块数量减为 11）。
- 延迟分配与按需分配：对单/双间接块与数据块的分配应当是按需的，避免一次性浪费块资源。
- 符号链接与路径解析：符号链接的实现把目标路径作为文件内容存放，open 时解析符号链接需要处理递归与环检测，并提供 `O_NOFOLLOW` 以支持不跟随语义。

以上为更具体的说明与代码片段。如果你希望我把每个修改处的完整 diff（即我实际修改的文件片段）也写进说明里，以便作业提交/审阅，我可以把每个文件的前后关键函数全部粘出来并逐行注释（会比较长），你想要哪种格式？

