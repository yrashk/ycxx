// Run ycxx's large frame on a thread stack whose guard page has writable memory below it.
// A prologue that touches each page faults in the guard page before writing anything below
// it. One that skips the guard writes the sentinel region first (#644).
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int large_frame(int index, int value);
int deep(int depth);

enum { page = 4096, sentinel_pages = 1024, stack_pages = 8 };
static unsigned char* region;  // [sentinel][guard][stack]
static unsigned char* guard;

static int sentinel_clean(void) {
  for (size_t i = 0; i < sentinel_pages * page; ++i)
    if (region[i] != 0xa5) return 0;
  return 1;
}

static void on_fault(int sig, siginfo_t* info, void* context) {
  (void)sig, (void)context;
  unsigned char* at = info->si_addr;
  if (at < guard || at >= guard + page) {
    static const char msg[] = "fault outside the guard page\n";
    write(2, msg, sizeof msg - 1);
    _exit(3);
  }
  if (!sentinel_clean()) {
    static const char msg[] = "the frame wrote below the guard page before touching it\n";
    write(2, msg, sizeof msg - 1);
    _exit(4);
  }
  _exit(0);
}

static void* body(void* unused) {
  (void)unused;
  static unsigned char alternate[64 * 1024];
  stack_t ss = {.ss_sp = alternate, .ss_size = sizeof alternate};
  if (sigaltstack(&ss, 0)) _exit(6);
  int r = large_frame(0, 7);
  fprintf(stderr, "large_frame returned %d without touching the guard page; sentinel %s\n", r,
          sentinel_clean() ? "clean" : "written");
  _exit(5);
}

int main(void) {
  // Frames that fit: a 64 KiB frame on the main thread and recursion over partial pages.
  if (large_frame(3, 11) != 23 || deep(50) != 50) return 1;

  size_t size = (sentinel_pages + 1 + stack_pages) * page;
  region = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (region == MAP_FAILED) return 6;
  memset(region, 0xa5, sentinel_pages * page);
  guard = region + sentinel_pages * page;
  if (mprotect(guard, page, PROT_NONE)) return 6;

  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = on_fault;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigaction(SIGSEGV, &sa, 0);

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  if (pthread_attr_setstack(&attr, guard + page, stack_pages * page)) return 6;
  pthread_t thread;
  if (pthread_create(&thread, &attr, body, 0)) return 6;
  pthread_join(thread, 0);
  return 7;
}
