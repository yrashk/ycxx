extern "C" int direct_breakpoint() {
  asm("int3");
  return 13;
}

extern "C" int guarded_breakpoint(int selected) {
  if (selected) {
    if constexpr (true)
      asm volatile("int3");
    else
      asm("brk #0");
  }
  return 17;
}
