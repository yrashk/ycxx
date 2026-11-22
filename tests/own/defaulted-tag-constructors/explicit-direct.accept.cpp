struct DirectTag {
  explicit DirectTag() = default;
};

void initialize_explicit() {
  DirectTag default_initialized;
  DirectTag direct_list_initialized{};
  (void)DirectTag{};
  (void)DirectTag();
}
