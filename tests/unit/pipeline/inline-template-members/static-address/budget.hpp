#pragma once

template<unsigned Limit> struct Budget {
  static unsigned initial() noexcept { return Limit; }
};

using Address = unsigned (*)() noexcept;
Address peer_two() noexcept;
Address peer_four() noexcept;
