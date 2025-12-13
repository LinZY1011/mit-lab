
# xv6 网络实验（Network Lab）实验报告

## 一、实验目的与环境

- 在 xv6 中编写 E1000 网卡设备驱动，实现以太网数据帧的发送与接收（NIC 驱动）。
- 在内核中实现简化的 IP/UDP 协议栈接收路径，实现 UDP 的 bind/recv 系统调用。
- 理解 DMA、设备中断、描述符环、睡眠/唤醒同步机制、内核与用户空间数据拷贝等操作系统关键原理。

代码基于 xv6 的 `net` 分支，主要修改的文件包括：

- `kernel/e1000.c`
- `kernel/net.c`
- `kernel/syscall.c`
- `kernel/syscall.h`

---

## 二、实验内容概述

1. **Part 1：NIC 驱动**
   - 完成 E1000 网卡驱动中的 `e1000_transmit` 和 `e1000_recv`，实现网卡的数据发送与接收。
   - 通过 `txone` 和 `rxone` 测试。

2. **Part 2：UDP 接收栈**
   - 在内核实现 `sys_bind`、`sys_recv` 系统调用，以及 IP 层的 `ip_rx` 函数。
   - 支持按端口号接收 UDP 报文，提供队列缓存与阻塞等待机制。
   - 通过 `ping*`、`dns` 等完整网络测试。

---

## 三、Part 1：E1000 网卡驱动

### 1. 任务要求

- 在网卡驱动中补全：
  - `int e1000_transmit(char *buf, int len)`：将一帧以太网数据放入 TX 描述符环，通知 E1000 发送。
  - `static void e1000_recv(void)`：从 RX 描述符环中取出硬件 DMA 写入的包，交给上层 `net_rx`，并补充新的接收缓冲区。
- 支持描述符环索引循环使用，能够处理超过环大小的多次收发。
- 使用锁保证并发安全，应对中断上下文与进程上下文并行访问。

### 2. 设计思路

- **发送路径（TX）**
  - 利用 E1000 的 TX 描述符环，索引由寄存器 `E1000_TDT` 指示。
  - 在每次发送前检查对应描述符的 `E1000_TXD_STAT_DD` 位是否已被硬件置位，避免覆盖未完成的发送请求。
  - 使用 `tx_bufs[]` 保存每个描述符对应的缓冲区指针，在确认硬件已经发送完后再 `kfree`。
  - 若环满（DD 未置位），当前发送失败，直接释放上层分配的 `buf`，返回 -1，避免内存泄漏。

- **接收路径（RX）**
  - 使用 RX 描述符环，头尾索引通过 `E1000_RDT` 控制。
  - 每次从 `(RDT + 1) % RX_RING_SIZE` 处检查 `E1000_RXD_STAT_DD` 位判断是否有新包。
  - 对于每个收到的包：
    - 将当前缓冲区交给 `net_rx`。
    - 使用 `kalloc` 分配新的缓冲区替换，写回描述符地址，清 status，并将 `E1000_RDT` 更新为当前索引。
  - 使用循环 `while (1)`，在一次中断中尽可能处理环上所有已到达的包。

- **锁与并发控制**
  - 使用 `struct spinlock e1000_lock` 保护 TX/RX 描述符环与寄存器访问，避免多核/中断竞态。
  - 重要原则：在调用 `net_rx` 之前释放 `e1000_lock`，避免 `net_rx` 中再次调用 `e1000_transmit` 导致死锁。

### 3. 具体代码与修改说明

#### 3.1 描述符环大小调整

在 `kernel/e1000.c` 中：

```c
#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static char *tx_bufs[TX_RING_SIZE];

#define RX_RING_SIZE 32
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static char *rx_bufs[RX_RING_SIZE];
```

- **修改原因：**
  - 发送环 `TX_RING_SIZE` 设为 16：足够应对普通突发流量，又避免占用过多缓冲，保证 `free` 测试中已发送包及时释放。
  - 接收环 `RX_RING_SIZE` 设为 32：稍大于 TX，用于抵御短暂接收突发，减小丢包概率。

#### 3.2 `e1000_transmit` 实现

```c
int
e1000_transmit(char *buf, int len)
{
  acquire(&e1000_lock);

  uint32 idx = regs[E1000_TDT];

  // 描述符未完成，环满，无法再用
  if((tx_ring[idx].status & E1000_TXD_STAT_DD) == 0){
    release(&e1000_lock);
    kfree(buf);     // 失败时释放调用者分配的 buf，避免泄漏
    return -1;
  }

  // 若该槽位有旧 buf，说明已经发送完成，可以释放
  if(tx_bufs[idx]){
    kfree(tx_bufs[idx]);
    tx_bufs[idx] = 0;
  }

  // 填写当前发送请求
  tx_bufs[idx] = buf;
  tx_ring[idx].addr   = (uint64)buf;
  tx_ring[idx].length = len;
  tx_ring[idx].cmd    = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;

  // 更新尾指针，通知硬件有新包
  regs[E1000_TDT] = (idx + 1) % TX_RING_SIZE;

  release(&e1000_lock);
  return 0;
}
```

- **关键点说明：**
  - 利用 `E1000_TDT` 获得当前可用槽位索引。
  - 通过 `E1000_TXD_STAT_DD` 判断该槽位的上一次发送是否已完成。
  - 使用 `tx_bufs[idx]` 管理发送缓冲区生命周期：旧的发送完成后释放内存，新 buf 记录下来。
  - 发送失败时立即 `kfree(buf)`，避免调用端忘记处理失败导致内存泄漏。

#### 3.3 `e1000_recv` 实现

```c
static void
e1000_recv(void)
{
  while(1){
    acquire(&e1000_lock);

    uint32 idx = (regs[E1000_RDT] + 1) % RX_RING_SIZE;

    // 没有新包，退出循环
    if((rx_ring[idx].status & E1000_RXD_STAT_DD) == 0){
      release(&e1000_lock);
      return;
    }

    char *buf = rx_bufs[idx];
    int len   = rx_ring[idx].length;

    // 为硬件下一次使用分配新的缓冲区
    char *new_buf = kalloc();
    if(new_buf == 0){
      rx_ring[idx].status = 0;
      regs[E1000_RDT] = idx;  // 仍然让硬件继续使用旧 buf
      release(&e1000_lock);
      continue;
    }

    // 更新描述符为新的缓冲区
    rx_bufs[idx]    = new_buf;
    rx_ring[idx].addr   = (uint64)new_buf;
    rx_ring[idx].status = 0;
    regs[E1000_RDT] = idx;

    release(&e1000_lock);

    // 交给协议栈处理，注意此时已不持有 e1000_lock
    net_rx(buf, len);
  }
}
```

- **关键点说明：**
  - 使用 `(RDT + 1) % RX_RING_SIZE` 获取“下一个”被硬件填充完的描述符位置。
  - 通过 DD 位判断是否真的有新包。
  - 先分配新缓冲区并替换环中的指针，然后再把旧缓冲区交给 `net_rx`，保证环上始终有可用 DMA 缓冲。
  - 在 `kalloc` 失败时，选择丢弃当前包并重用旧缓冲区，防止环被“空指针”破坏。

### 4. 涉及的操作系统原理

- **设备驱动与 DMA**
  - 通过内存映射寄存器 `regs[...]` 配置 E1000，同时使用描述符环让硬件直接在物理内存中读写数据（DMA），减少 CPU 拷贝。
- **生产者–消费者模型**
  - TX 环：软件为生产者（填 descriptor），硬件为消费者（从内存取数据发送）。
  - RX 环：硬件为生产者（写入数据包），软件为消费者（net_rx 消费并 kfree）。
- **中断机制**
  - `e1000_intr` 在中断到来时清除中断状态寄存器并调用 `e1000_recv`，实现异步包接收。
- **并发控制**
  - 使用自旋锁 `e1000_lock` 保护共享数据结构与寄存器，防止中断上下文和普通内核代码之间的竞态。
  - 注意锁的持有范围，避免在持锁情况下调用可能再次访问网卡的代码导致死锁。

---

## 四、Part 2：UDP 接收协议栈

### 1. 任务要求

- 在内核实现：
  - `sys_bind(short port)`：绑定 UDP 端口，为后续接收该端口数据包做准备。
  - `sys_recv(short dport, int *src, short *sport, char *buf, int maxlen)`：从指定端口接收一个 UDP 报文，支持无包时阻塞等待。
  - `ip_rx(char *buf, int len)`：IP 层收到 IP 包后，判断是否为 UDP，并根据目标端口分发到对应套接字队列。
- 要求：
  - 每个端口最多缓存 16 个尚未被 `recv` 消费的包，多余的丢弃，避免耗尽内存。
  - 不同端口相互独立，一个端口队列满不会影响其他端口。
  - 所有端口和队列操作都必须是线程安全的。

### 2. 设计思路

- **套接字表设计**

在 `kernel/net.c` 中定义一个简化的 UDP socket 结构：

```c
#define NSOCK 16

struct sock {
  int port;           // 绑定的 UDP 端口，0 表示未使用
  char *bufs[16];     // 接收队列（循环缓冲区）
  int  lens[16];      // 每个包的长度
  int  r, w;          // 读/写索引，(w + 1) % 16 == r 表示队列满
};

struct sock sockets[NSOCK];
static struct spinlock netlock;
```

- 每个绑定的端口占用一个 `sock` 槽，最多 `NSOCK=16` 个绑定。
- 每个 `sock` 内部最多缓存 16 个包，采用环形队列管理。
- 所有访问通过 `netlock` 进行互斥。

- **bind 逻辑（sys_bind）**
  - 检查端口是否已经被其他 socket 占用。
  - 在 `sockets[]` 中找到 `port==0` 的空槽，初始化 `port`、`r`、`w`。
  - 若无空槽或已有同端口，则返回 -1。

- **IP 层接收（ip_rx）**
  - 只处理 `ip_p == IPPROTO_UDP` 的包，其它协议释放缓冲区。
  - 从 UDP 头中取出目标端口 `dport`（注意使用 `ntohs`）。
  - 查找是否存在绑定到该端口的 `sock`，若不存在则直接 `kfree(buf)` 丢弃。
  - 若存在队列未满，将 `buf` 指针入队，更新 `w`，并调用 `wakeup(s)` 唤醒在该 socket 上睡眠的进程。

- **recv 逻辑（sys_recv）**
  - 根据参数中的 `dport` 查找对应 `sock`，不存在则立即返回 -1。
  - 若队列为空（`r == w`），在 `netlock` 下调用 `sleep(s, &netlock)` 阻塞等待，直到 `ip_rx` 中 `wakeup(s)`。
  - 取出队头 `buf`，出队后释放 `netlock`。
  - 在不持锁的情况下解析以太网 / IP / UDP 头：
    - 解析源 IP、源端口，并通过 `copyout` 拷贝到用户空间。
    - 计算 UDP 负载长度，按 `maxlen` 限制后拷贝到用户缓冲区。
  - 最后对内核缓冲区 `buf` 调用 `kfree`。

### 3. 具体代码与修改说明

#### 3.1 `sys_bind` 实现

```c
uint64
sys_bind(void)
{
  int port;
  argint(0, &port);

  acquire(&netlock);
  // 端口是否已被占用
  for(int i = 0; i < NSOCK; i++){
    if(sockets[i].port == port){
      release(&netlock);
      return -1;
    }
  }

  // 找空槽绑定
  for(int i = 0; i < NSOCK; i++){
    if(sockets[i].port == 0){
      sockets[i].port = port;
      sockets[i].r = 0;
      sockets[i].w = 0;
      release(&netlock);
      return 0;
    }
  }
  release(&netlock);
  return -1;
}
```

- **修改原因：**
  - 保证同一端口最多被一个进程绑定，简化实现。
  - 使用 `netlock` 保证多进程同时 bind 时端口分配的一致性。

#### 3.2 `sys_recv` 实现

```c
uint64
sys_recv(void)
{
  int dport;
  uint64 src_addr, sport_addr, buf_addr;
  int maxlen;

  argint(0, &dport);
  argaddr(1, &src_addr);
  argaddr(2, &sport_addr);
  argaddr(3, &buf_addr);
  argint(4, &maxlen);

  struct sock *s = 0;
  acquire(&netlock);
  for(int i = 0; i < NSOCK; i++){
    if(sockets[i].port == dport){
      s = &sockets[i];
      break;
    }
  }
  if(s == 0){
    release(&netlock);
    return -1;
  }

  // 队列为空则睡眠等待
  while(s->r == s->w){
    if(killed(myproc())){
      release(&netlock);
      return -1;
    }
    sleep(s, &netlock);
  }

  // 出队一个包
  char *buf = s->bufs[s->r];
  s->r = (s->r + 1) % 16;
  release(&netlock);

  // 解析头部
  struct eth *eth = (struct eth *)buf;
  struct ip  *ip  = (struct ip *)(eth + 1);
  struct udp *udp = (struct udp *)(ip + 1);
  char *payload   = (char *)(udp + 1);
  int payload_len = ntohs(udp->ulen) - sizeof(struct udp);

  uint32 src_ip   = ntohl(ip->ip_src);
  uint16 src_port = ntohs(udp->sport);

  // 拷贝源 IP 与端口到用户空间
  if(copyout(myproc()->pagetable, src_addr, (char*)&src_ip, sizeof(src_ip)) < 0 ||
     copyout(myproc()->pagetable, sport_addr, (char*)&src_port, sizeof(src_port)) < 0){
    kfree(buf);
    return -1;
  }

  // 拷贝负载到用户缓冲区
  int n = payload_len;
  if(n > maxlen) n = maxlen;
  if(copyout(myproc()->pagetable, buf_addr, payload, n) < 0){
    kfree(buf);
    return -1;
  }

  kfree(buf);
  return n;
}
```

- **关键点说明：**
  - `sleep(s, &netlock)` / `wakeup(s)`：
    - 使用 `sock` 结构地址作为睡眠 channel，一个端口对应一个等待队列。
    - 在睡眠前持有 `netlock`，由 `sleep` 内部原子释放锁，避免丢失唤醒。
  - 返回前必须对 `buf` 调用 `kfree`，否则会在高频 `recv` 下耗尽内核内存。
  - 所有与用户空间交互（`copyout`）在不持有 `netlock` 的情况下完成，避免长时间持锁。

#### 3.3 `ip_rx` 实现

```c
void
ip_rx(char *buf, int len)
{
  // 不要删除这条打印；make grade 依赖它
  static int seen_ip = 0;
  if(seen_ip == 0)
    printf("ip_rx: received an IP packet\n");
  seen_ip = 1;

  struct eth *eth = (struct eth *)buf;
  struct ip  *ip  = (struct ip *)(eth + 1);

  // 仅处理 UDP 包
  if(ip->ip_p != IPPROTO_UDP){
    kfree(buf);
    return;
  }

  struct udp *udp   = (struct udp *)(ip + 1);
  uint16 dport      = ntohs(udp->dport);

  acquire(&netlock);
  struct sock *s = 0;
  for(int i = 0; i < NSOCK; i++){
    if(sockets[i].port == dport){
      s = &sockets[i];
      break;
    }
  }

  if(s){
    // 判断队列是否已满
    if((s->w + 1) % 16 == s->r){
      // 丢弃本端口上的过多包，不影响其他端口
      release(&netlock);
      kfree(buf);
    } else {
      s->bufs[s->w] = buf;
      s->lens[s->w] = len;
      s->w = (s->w + 1) % 16;
      wakeup(s);     // 唤醒等待该端口的进程
      release(&netlock);
    }
  } else {
    // 未绑定该端口，直接丢弃
    release(&netlock);
    kfree(buf);
  }
}
```

- **修改原因：**
  - 实现从网卡驱动上传上来的 IP 包到 UDP socket 的分发。
  - 队列满时丢包是为了防止单一恶意发送者耗尽内核内存，但不影响其它端口。

#### 3.4 系统调用表接入

在 `kernel/syscall.h` 中加入系统调用号：

```c
#define SYS_bind      29
#define SYS_unbind    30
#define SYS_send      31
#define SYS_recv      32
```

在 `kernel/syscall.c` 中声明并注册：

```c
#ifdef LAB_NET
extern uint64 sys_bind(void);
extern uint64 sys_unbind(void);
extern uint64 sys_send(void);
extern uint64 sys_recv(void);
#endif

static uint64 (*syscalls[])(void) = {
  ...
#ifdef LAB_NET
[SYS_bind]   sys_bind,
[SYS_unbind] sys_unbind,
[SYS_send]   sys_send,
[SYS_recv]   sys_recv,
#endif
  ...
};
```

- **说明：**
  - 将 `bind` / `send` / `recv` 等用户态 API 和内核实现连接起来，保证用户空间程序 `user/nettest.c` 能通过系统调用接口访问网络栈。

---

## 五、涉及的操作系统原理总结

1. **设备驱动与 DMA**
   - E1000 驱动通过内存映射寄存器和描述符环与硬件交互。
   - 硬件直接在物理内存中读写数据（DMA），CPU 只需准备好描述符和缓冲区，减少数据拷贝。

2. **中断与异步事件处理**
   - 网卡收到包后，通过 PLIC 触发中断，CPU 跳转至中断处理函数 `e1000_intr`。
   - 中断处理函数中调用 `e1000_recv` 扫描 RX 环，实现网络收包的异步处理。

3. **并发控制与同步**
   - 使用自旋锁 `e1000_lock`、`netlock` 保证共享资源（描述符环、socket 队列）在多核/中断上下文下的一致性。
   - 在 `sys_recv` / `ip_rx` 中使用 `sleep` / `wakeup` 进行条件同步，实现「无包则阻塞，有包则唤醒」的典型同步模式。

4. **内存管理与资源回收**
   - 使用 `kalloc` / `kfree` 为网络包分配和释放缓冲区。
   - 在各种失败路径（发送环满、队列满、copyout 失败等）注意调用 `kfree`，避免内核内存泄漏。
   - 控制每个端口至多缓存 16 个包，防止 DoS 式内存耗尽。

5. **网络协议分层与字节序**
   - 实现体现了经典的协议分层：以太网层（eth）→ IP 层（ip）→ UDP 层（udp）→ socket 接口。
   - 在解析和构造 IP/UDP 报头时使用 `ntohl` / `ntohs` / `htonl` / `htons` 处理大小端转换，保证网络字节序与主机字节序正确转换。

6. **系统调用与用户态接口**
   - 通过在 `syscall.h` / `syscall.c` 中注册 `SYS_bind` / `SYS_send` / `SYS_recv`，将内核网络栈能力暴露给用户进程。
   - 使用 `copyin` / `copyout` 在内核与用户空间之间安全传递数据，体现了操作系统在地址空间隔离基础上的受控交互。

