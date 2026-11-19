extern int elf_call(void);
extern const int *elf_pointer;
extern int elf_zero[2048];
extern const int *elf_weak_pointer;

int elf_external(void) {
  return 25;
}

int main(void) {
  if (elf_call() != 42) return 1;
  if (*elf_pointer != 29) return 2;
  if (elf_zero[0] != 0 || elf_zero[2047] != 0) return 3;
  if (elf_weak_pointer != 0) return 4;
  return 0;
}
