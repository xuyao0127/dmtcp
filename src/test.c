#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>

int main(int argc, char **argv) {
  int fd;
  printf("1 calling open\n");
  fd = open("foo.txt", O_WRONLY|O_CREAT, 0664);
  char s[] = "Hello, World\n";
  write(fd, s, sizeof(s));
  close(fd);

  // sleep for checkpointing
  printf("before checkpoint\n");
  sleep(2);
  printf("after checkpoint\n");

  printf("2 calling open\n");
  fd = open("foo.txt", O_RDONLY);
  char buf[20];
  read(fd, buf, 20);
  printf("%s", buf);
  close(fd);
  return 0;
}
