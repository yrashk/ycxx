// [temp.arg.nontype], [conv.ptr]
int sensor = 6;

template<const void* Address> struct Erased {
  static int read() noexcept { return *static_cast<const int*>(Address); }
};

int (*erased_reader())() noexcept {
  return &Erased<static_cast<const void*>(&sensor)>::read;
}

int main() {
  auto read = erased_reader();
  if (read() != 6)
    return 1;
  sensor = 19;
  return read() == 19 ? 0 : 2;
}
