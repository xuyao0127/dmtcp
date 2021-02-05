#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>

int main(int argc, char **argv) {
  int fd;
  printf("calling open\n");
  fd = open("foo.txt", O_WRONLY|O_CREAT, 0664);
  char s[] = "Hello, World\n";
  write(fd, s, sizeof(s));
  close(fd);
  return 0;
}
