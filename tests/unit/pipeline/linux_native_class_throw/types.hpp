#ifndef YCXX_TEST_LINUX_NATIVE_CLASS_THROW_TYPES_HPP
#define YCXX_TEST_LINUX_NATIVE_CLASS_THROW_TYPES_HPP

struct Fault {
  int payload;
};

struct Decoy {
  int payload;
};

int produce(int value);
int produce_int(int value);

#endif
