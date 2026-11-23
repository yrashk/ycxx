// #842: <cstdio> through libycxx's header, which references the linkage marker only libycxx's
// static archive defines (`__ycxx_linkage_static_v1`); the driver links libycxx's runtime by
// default, so the program links and prints.
// STDOUT: hello, world
// STDOUT: 42
#include <cstdio>

int main() {
  std::printf("hello, %s\n", "world");
  std::printf("%d\n", 42);
  return 0;
}
