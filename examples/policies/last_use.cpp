#include <ycxx/ownership.hpp>
#include <ycxx/ownership/runtime.hpp>

namespace own = ycxx::ownership;

struct
#ifdef __ycxx__
[[=own::resource_mode::linear]]
#endif
Receipt {
  own::linear_state state;
  int number;
};

#ifdef __ycxx__
[[=own::operation::creates]]
#endif
Receipt issue_receipt() noexcept { return Receipt{own::linear_state{}, 7}; }

#ifdef __ycxx__
[[=own::operation::observes]]
#endif
int read_receipt(const Receipt& receipt) noexcept {
  receipt.state.observe();
  return receipt.number;
}

#ifdef __ycxx__
[[=own::operation::consumes]]
#endif
void close_receipt(Receipt&& receipt) noexcept { receipt.state.consume(); }

int same_block() {
  auto receipt = issue_receipt();
  const auto& view = receipt;
#ifdef YCXX_DEMO_USE_AFTER_CLOSE
  close_receipt(static_cast<Receipt&&>(receipt));
#endif
  int number = read_receipt(view);
#ifndef YCXX_DEMO_USE_AFTER_CLOSE
  close_receipt(static_cast<Receipt&&>(receipt));
#endif
  return number;
}

int branch(bool read_first) {
  auto receipt = issue_receipt();
  const auto& view = receipt;
  if (!read_first) {
    close_receipt(static_cast<Receipt&&>(receipt));
    return 0;
  }
  int number = read_receipt(view);
  close_receipt(static_cast<Receipt&&>(receipt));
  return number;
}

void loop(bool stop_early) {
  auto receipt = issue_receipt();
  const auto& view = receipt;
  for (int i = 0; i < 2; ++i) {
    read_receipt(view);
#ifdef YCXX_DEMO_LOOP_USE_AFTER_CLOSE
    close_receipt(static_cast<Receipt&&>(receipt));
#endif
    if (stop_early) break;
  }
#ifndef YCXX_DEMO_LOOP_USE_AFTER_CLOSE
  close_receipt(static_cast<Receipt&&>(receipt));
#endif
}

int main() {
  loop(false);
  return same_block() != 7 || branch(true) != 7 || branch(false) != 0;
}
