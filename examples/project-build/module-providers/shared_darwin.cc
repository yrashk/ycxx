#if defined(__APPLE__)
export module demo.shared;

export int shared_score() {
  return 9;
}
#endif
