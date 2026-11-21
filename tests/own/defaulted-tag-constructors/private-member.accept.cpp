class HiddenTag {
  HiddenTag() = default;

  static void permitted() {
    HiddenTag object;
    (void)HiddenTag{};
  }
};
