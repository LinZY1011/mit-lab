#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define RD 0
#define WR 1

void prime(int fd_rd)
{
  int n;
  if (read(fd_rd, &n, 4) != 4) {   // 读不到立即返回
    close(fd_rd);
    exit(0);
  }
  printf("prime %d\n", n);

  int p[2];
  pipe(p);
  int pid = fork();
  if (pid == 0) {          // 子进程
    close(p[WR]);
    close(fd_rd);
    prime(p[RD]);
    close(p[RD]);
    exit(0);
  }

  // 父进程：把不能被 n 整除的数写下去
  close(p[RD]);
  int num;
  while (read(fd_rd, &num, 4) == 4) {
    if (num % n != 0) write(p[WR], &num, 4);
  }
  close(fd_rd);
  close(p[WR]);
  wait(0);
}

int main(int argc, char *argv[])
{
  int p[2];
  pipe(p);

  int pid = fork();
  if (pid == 0) {          // 子进程
    close(p[WR]);
    prime(p[RD]);
    close(p[RD]);
    exit(0);
  }

  close(p[RD]);
  for (int i = 2; i <= 280; i++) write(p[WR], &i, 4);
  close(p[WR]);

  wait(0);
  exit(0);
}