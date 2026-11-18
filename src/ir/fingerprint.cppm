// The IR's view of ycxx's one hash (`ycxx.hash`, query-engine design §A1.3): a body's fingerprint
// is an `Fp128`, built with a streaming `Hasher` over 64-bit words.
export module ycxx.ir:fingerprint;

import std;
import ycxx.hash;

namespace ycxx::ir {

export using fingerprint128 = Fp128;

export class hasher {
 public:
  constexpr hasher& add(std::uint64_t x) {
    h_.uint(x);
    return *this;
  }
  constexpr hasher& add(fingerprint128 f) {
    h_.fp(f);
    return *this;
  }
  constexpr fingerprint128 finish() const { return h_.finish(); }

 private:
  Hasher h_;
};

}  // namespace ycxx::ir
