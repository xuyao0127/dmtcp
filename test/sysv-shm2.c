// shmget() needs sysv/ipc.h, which needs )XOPEN_SOURCE
#define _XOPEN_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <unistd.h>

#define SIZE 4096

void
parent(int fd)
{
  int shmid;
  srand(getpid());

  if ((shmid = shmget((key_t)rand(), SIZE, IPC_CREAT | 0666)) < 0) {
    perror("shmget");
    exit(1);
  }
  struct shmid_ds shmid_ds;
  if (shmctl(shmid, IPC_STAT, &shmid_ds) == -1) {
    perror("shmctl: shmctl failed");
    exit(1);
  }
  printf("Shmid: %d\n", shmid);

  void *addr = shmat(shmid, NULL, 0);
  if (addr == (void *)-1) {
    perror("main: shmat");
    abort();
  }
  memset(addr, 0, SIZE);

  void *addr2 = shmat(shmid, NULL, 0);
  if (addr2 == (void *)-1) {
    perror("Child: second shmat failed");
    abort();
  }

  // Now unmap the second address using munmap.
  if (munmap(addr2, SIZE) == -1) {
    perror("Child: munmap failed");
    abort();
  }

  if (write(fd, &shmid, sizeof(shmid)) != sizeof(shmid)) {
    perror("write");
    abort();
  }

  int *ptr = (int *)addr;
  int i;
  for (i = 1; i < 100000; i++) {
    printf("Server: %d\n", i);
    fflush(stdout);
    *ptr = i;
    while (*ptr != -i) {
      sleep(1);
    }
  }
  *ptr = 0;
  exit(0);
}

void
child(int fd)
{
  int shmid;

  if (read(fd, &shmid, sizeof(shmid)) != sizeof(shmid)) {
    perror("write");
    abort();
  }

  void *addr = shmat(shmid, NULL, 0);
  if (addr == (void *)-1) {
    perror("Child: shmat");
    abort();
  }

  void *addr2 = shmat(shmid, NULL, 0);
  if (addr2 == (void *)-1) {
    perror("Child: second shmat failed");
    abort();
  }

  // Now unmap the second address using munmap.
  if (munmap(addr2, SIZE) == -1) {
    perror("Child: munmap failed");
    abort();
  }

  int *ptr = (int *)addr;
  sleep(2);
  int val;
  while ((val = *ptr) != 0) {
    int i = *ptr;
    if (i > 0) {
      printf("Client: %d\n", i);
      fflush(stdout);
      *ptr = -i;
    } else {
      sleep(1);
    }
  }
  exit(0);
}

int
main(int argc, char **argv)
{
  int fds[2];

  if (pipe(fds) == -1) {
    perror("pipe");
    exit(1);
  }

  if (fork() == 0) {
    child(fds[0]);
  } else {
    parent(fds[1]);
  }
  return 0;
}
