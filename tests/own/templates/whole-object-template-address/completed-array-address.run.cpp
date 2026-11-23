// [temp.arg.nontype], [dcl.array]
extern int samples[];
int samples[3] = {7, 11, 13};

template<int (*Address)[3]> struct Bounded {
  static int read() noexcept { return (*Address)[1]; }
};

int (*array_reader())() noexcept {
  return &Bounded<&samples>::read;
}

int main() {
  auto read = array_reader();
  if (read() != 11)
    return 1;
  samples[1] = 17;
  return read() == 17 ? 0 : 2;
}
