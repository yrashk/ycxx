#pragma once

// [dcl.constexpr], [class.static.data]: the in-class declaration defines an inline member.
template<unsigned N> struct Profile {
  static constexpr unsigned limit = N;
};
