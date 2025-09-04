#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define MAXLINE 512
#define MAXARG  32

int
main(int argc, char *argv[])
{
  if (argc < 2) {
    fprintf(2, "usage: xargs command [arg...]\n");
    exit(1);
  }

  char *argv_new[MAXARG];
  int  cmd_cnt = argc - 1;          // 命令本身占用的槽位
  for (int i = 0; i < cmd_cnt; ++i)
    argv_new[i] = argv[i + 1];      // 先把命令及固定参数放好

  char line[MAXLINE];
  int  pos = 0;
  char c;

  /* 逐字符读标准输入，直到 EOF */
  while (read(0, &c, 1) == 1) {
    if (c == '\n') {                // 一行结束
      line[pos] = '\0';

      /* 按空格拆分这一行 */
      int arg_cnt = cmd_cnt;        // 从命令参数后面开始填
      char *p = line;
      while (*p) {
        while (*p == ' ') *p++ = '\0';   // 跳过连续空格
        if (*p == '\0') break;
        argv_new[arg_cnt++] = p;         // 记录单词起始
        while (*p && *p != ' ') p++;     // 跳到空格或结尾
      }
      argv_new[arg_cnt] = 0;             // NULL 结尾

      /* 执行这条完整命令 */
      if (fork() == 0) {
        exec(argv_new[0], argv_new);
        fprintf(2, "xargs: exec failed\n");
        exit(1);
      }
      wait(0);                  // 等子进程结束
      pos = 0;                  // 重置缓冲区，准备下一行
    } else {
      if (pos < MAXLINE - 1) line[pos++] = (char)c;
    }
  }

  /* 文件末尾没有换行也处理最后一行 */
  if (pos > 0) {
    line[pos] = '\0';
    int arg_cnt = cmd_cnt;
    char *p = line;
    while (*p) {
      while (*p == ' ') *p++ = '\0';
      if (*p == '\0') break;
      argv_new[arg_cnt++] = p;
      while (*p && *p != ' ') p++;
    }
    argv_new[arg_cnt] = 0;
    if (fork() == 0) {
      exec(argv_new[0], argv_new);
      fprintf(2, "xargs: exec failed\n");
      exit(1);
    }
    wait(0);
  }

  exit(0);
}