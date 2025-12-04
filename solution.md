# Network Driver Lab Solution

## 代码修改

### 1. `kernel/e1000.c`

#### 宏定义修改

为了通过 `ping3` 测试（处理突发流量）并避免在 `free` 测试中因持有过多发送缓冲区而失败，调整了环的大小：

```c
#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static char *tx_bufs[TX_RING_SIZE];

#define RX_RING_SIZE 32
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static char *rx_bufs[RX_RING_SIZE];
```

#### `e1000_transmit`

```c
int
e1000_transmit(char *buf, int len)
{
  // 获取 e1000 锁，防止多进程并发访问 TX 环
  acquire(&e1000_lock);

  // 获取下一个可用的 TX 描述符索引
  uint32 idx = regs[E1000_TDT];

  // 检查描述符是否可用 (E1000_TXD_STAT_DD 标志位)
  // 如果未设置，说明硬件尚未完成上一次传输，环已满
  if((tx_ring[idx].status & E1000_TXD_STAT_DD) == 0){
    release(&e1000_lock);
    kfree(buf); // 发送失败时释放缓冲区，避免内存泄漏
    return -1;
  }

  // 如果该位置有旧的缓冲区，释放它
  if(tx_bufs[idx]){
    kfree(tx_bufs[idx]);
    tx_bufs[idx] = 0;
  }

  // 将新缓冲区的指针保存，以便后续释放
  tx_bufs[idx] = buf;
  // 设置描述符指向缓冲区物理地址
  tx_ring[idx].addr = (uint64)buf;
  // 设置长度
  tx_ring[idx].length = len;
  // 设置命令：EOP (End of Packet) 和 RS (Report Status)
  // RS 告诉硬件在完成后设置 DD 位
  tx_ring[idx].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;

  // 更新 TDT (Transmit Descriptor Tail) 指针，通知硬件有新包
  regs[E1000_TDT] = (idx + 1) % TX_RING_SIZE;

  release(&e1000_lock);
  
  return 0;
}
```

#### `e1000_recv`

```c
static void
e1000_recv(void)
{
  // 循环处理所有已接收的数据包
  while(1){
    acquire(&e1000_lock);
    // 计算下一个期望接收的描述符索引
    // RDT 指向硬件可用的最后一个描述符，所以下一个是 (RDT + 1) % Size
    uint32 idx = (regs[E1000_RDT] + 1) % RX_RING_SIZE;

    // 检查 DD 标志位，看是否有新包到达
    if((rx_ring[idx].status & E1000_RXD_STAT_DD) == 0){
      release(&e1000_lock);
      return;
    }

    // 获取接收到的缓冲区
    char *buf = rx_bufs[idx];
    int len = rx_ring[idx].length;

    // 分配一个新的缓冲区替换旧的，供硬件接收下一个包
    char *new_buf = kalloc();
    if(new_buf == 0){
      // 如果分配失败，丢弃当前包，重用旧缓冲区给硬件
      rx_ring[idx].status = 0;
      regs[E1000_RDT] = idx;
      release(&e1000_lock);
      continue;
    }

    // 更新描述符指向新缓冲区
    rx_bufs[idx] = new_buf;
    rx_ring[idx].addr = (uint64)new_buf;
    rx_ring[idx].status = 0;
    
    // 更新 RDT，通知硬件该描述符可用
    regs[E1000_RDT] = idx;
    release(&e1000_lock);
    
    // 将接收到的包传递给网络栈处理
    // 注意：在调用 net_rx 之前释放锁，避免死锁（net_rx 可能调用 e1000_transmit）
    net_rx(buf, len);
  }
}
```

### 2. `kernel/net.c`

#### 数据结构定义

```c
#define NSOCK 16

// UDP 套接字结构
struct sock {
  int port; // 绑定的端口号，0 表示未使用
  char *bufs[16]; // 接收队列（循环缓冲区），存储数据包指针
  int lens[16];   // 数据包长度
  int r, w; // 读写索引
};

struct sock sockets[NSOCK];
```

#### `sys_bind`

```c
uint64
sys_bind(void)
{
  int port;
  argint(0, &port); // 获取端口参数

  acquire(&netlock);
  // 检查端口是否已被占用
  for(int i = 0; i < NSOCK; i++){
    if(sockets[i].port == port){
      release(&netlock);
      return -1;
    }
  }

  // 寻找空闲的 socket 槽位进行绑定
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

#### `sys_recv`

```c
uint64
sys_recv(void)
{
  int dport;
  uint64 src_addr;
  uint64 sport_addr;
  uint64 buf_addr;
  int maxlen;

  // 获取系统调用参数
  argint(0, &dport);
  argaddr(1, &src_addr);
  argaddr(2, &sport_addr);
  argaddr(3, &buf_addr);
  argint(4, &maxlen);

  struct sock *s = 0;
  acquire(&netlock);
  // 查找绑定的 socket
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

  // 如果队列为空，睡眠等待
  while(s->r == s->w){
    if(killed(myproc())){
      release(&netlock);
      return -1;
    }
    sleep(s, &netlock);
  }

  // 从队列中取出数据包
  char *buf = s->bufs[s->r];
  s->r = (s->r + 1) % 16;

  release(&netlock);

  // 解析协议头
  struct eth *eth = (struct eth *)buf;
  struct ip *ip = (struct ip *)(eth + 1);
  struct udp *udp = (struct udp *)(ip + 1);
  char *payload = (char *)(udp + 1);
  int payload_len = ntohs(udp->ulen) - sizeof(struct udp);

  uint32 src_ip = ntohl(ip->ip_src);
  uint16 src_port = ntohs(udp->sport);

  // 将源 IP 和端口复制到用户空间
  if(copyout(myproc()->pagetable, src_addr, (char*)&src_ip, sizeof(src_ip)) < 0 ||
     copyout(myproc()->pagetable, sport_addr, (char*)&src_port, sizeof(src_port)) < 0){
       kfree(buf);
       return -1;
  }

  // 将负载数据复制到用户空间
  int n = payload_len;
  if(n > maxlen) n = maxlen;
  if(copyout(myproc()->pagetable, buf_addr, payload, n) < 0){
    kfree(buf);
    return -1;
  }

  kfree(buf); // 释放内核缓冲区
  return n;
}
```

#### `ip_rx`

```c
void
ip_rx(char *buf, int len)
{
  // ... (保留原有 printf)

  struct eth *eth = (struct eth *)buf;
  struct ip *ip = (struct ip *)(eth + 1);

  // 仅处理 UDP 包
  if(ip->ip_p != IPPROTO_UDP){
    kfree(buf);
    return;
  }

  struct udp *udp = (struct udp *)(ip + 1);
  uint16 dport = ntohs(udp->dport);

  acquire(&netlock);
  struct sock *s = 0;
  // 查找目标端口对应的 socket
  for(int i = 0; i < NSOCK; i++){
    if(sockets[i].port == dport){
      s = &sockets[i];
      break;
    }
  }

  if(s){
    // 检查队列是否已满
    if((s->w + 1) % 16 == s->r){
      // 队列满，丢弃包
      release(&netlock);
      kfree(buf);
    } else {
      // 入队
      s->bufs[s->w] = buf;
      s->lens[s->w] = len;
      s->w = (s->w + 1) % 16;
      // 唤醒等待的进程
      wakeup(s);
      release(&netlock);
    }
  } else {
    // 未绑定端口，丢弃包
    release(&netlock);
    kfree(buf);
  }
}
```

## 总结

### 解决的问题与实现的功能

本实验主要解决了 xv6 操作系统中网络通信功能的缺失问题。具体实现了以下功能：

1.  **网卡驱动程序 (NIC Driver)**:
    *   实现了 `e1000_transmit`：允许内核将数据包发送到 E1000 网卡。它管理发送描述符环（TX Ring），将数据包缓冲区映射到 DMA 描述符，并通知硬件发送。
    *   实现了 `e1000_recv`：允许内核从 E1000 网卡接收数据包。它扫描接收描述符环（RX Ring），处理硬件 DMA 写入的数据包，并将其传递给上层网络栈，同时为硬件补充新的空闲缓冲区。

2.  **UDP 协议栈支持**:
    *   实现了 `sys_bind`：允许用户进程绑定特定的 UDP 端口，内核为此分配接收队列。
    *   实现了 `ip_rx`：在 IP 层接收数据包时，识别 UDP 包，并根据目的端口将其分发到相应的 socket 接收队列中。
    *   实现了 `sys_recv`：允许用户进程从绑定的端口接收数据，支持阻塞等待（sleep/wakeup 机制），并将内核缓冲区的数据复制到用户空间。

### 涉及的操作系统原理

1.  **设备驱动与 DMA (Direct Memory Access)**:
    *   驱动程序通过读写内存映射寄存器（MMIO）来控制硬件（E1000）。
    *   使用描述符环（Descriptor Rings）管理 DMA 传输。CPU 准备描述符，硬件直接从内存读取数据（发送）或写入数据（接收），减少了 CPU 的数据拷贝负担。
    *   **生产者-消费者模型**：TX 环和 RX 环本质上是硬件和软件之间的循环缓冲区。在发送时，软件是生产者，硬件是消费者；在接收时，硬件是生产者，软件是消费者。

2.  **中断处理**:
    *   网卡收到数据包后触发中断，CPU 暂停当前执行流，跳转到中断处理程序（`e1000_intr`），进而调用 `e1000_recv` 处理接收到的数据包。这体现了异步事件处理机制。

3.  **并发控制与锁**:
    *   使用自旋锁（`e1000_lock`, `netlock`）保护共享资源（描述符环、socket 队列），防止多核 CPU 或中断上下文与进程上下文之间的竞争条件（Race Condition）。
    *   特别注意了锁的粒度和死锁避免（例如在调用 `net_rx` 前释放 `e1000_lock`）。

4.  **进程同步与睡眠/唤醒机制**:
    *   `sys_recv` 使用 `sleep` 在队列为空时阻塞进程，`ip_rx` 使用 `wakeup` 在收到数据包时唤醒进程。这是经典的条件同步机制。

5.  **内存管理**:
    *   使用 `kalloc` 和 `kfree` 管理网络数据包的缓冲区（mbuf/pages）。
    *   涉及内核空间到用户空间的数据拷贝（`copyout`），这是系统调用传递大块数据的标准方式。
    *   **内存泄漏防护**：在 `e1000_transmit` 中，如果发送环已满，必须释放传入的缓冲区，否则会导致内存泄漏（如 `free` 测试所示）。

6.  **网络协议栈分层**:
    *   代码体现了网络协议的分层结构：以太网层 -> IP 层 -> UDP 层 -> Socket 接口。每一层负责处理相应的头部信息并剥离/封装。
