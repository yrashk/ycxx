struct CopyTag {
  CopyTag() = default;
  CopyTag(const CopyTag&) = default;
};

void initialize_and_copy() {
  CopyTag original;
  CopyTag direct_copy(original);
  CopyTag copy_initialized = original;
}
