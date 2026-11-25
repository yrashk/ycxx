#if defined(__linux__)
export module demo.shared;

export int shared_score() {
  return 7;
}
#endif
