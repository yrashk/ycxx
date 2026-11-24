#include "try_or_null.hpp"

extern "C" const char* marker_address() {
  return &__ycxx_fs_default_new;
}

extern "C" int observe_marker() {
  return &__ycxx_fs_default_new != nullptr ? __ycxx_fs_default_new : 0;
}
