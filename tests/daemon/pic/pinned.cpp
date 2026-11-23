#include <ycxx/pal.h>

extern "C" const char __ycxx_linkage_static_v1;

int main() {
  return ycxx_pal_thread_self() != 1 || __ycxx_linkage_static_v1 != 0;
}
