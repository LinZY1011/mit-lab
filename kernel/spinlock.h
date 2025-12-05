// Mutual exclusion lock.
struct spinlock {
  uint locked;       // Is the lock held?

  // For debugging:
  char *name;        // Name of lock.
  struct cpu *cpu;   // The cpu holding the lock.
#ifdef LAB_LOCK
  int nts;
  int n;
#endif
};

// 读写自旋锁结构
// 支持多个读者同时访问，或单个写者独占访问
struct rwspinlock {
  struct spinlock lock;     // 保护readers和writer_waiting字段的自旋锁
  int readers;              // 当前持有读锁的线程数
  int writer_waiting;       // 是否有写者在等待（实现写者优先）
  int writer;               // 是否有写者持有锁（0或1）
};
