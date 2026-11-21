struct JumpTag {
  JumpTag() = default;
};

void jump_over_default_initialization() {
  goto after;
  JumpTag object;
after:
  (void)&object;
}
