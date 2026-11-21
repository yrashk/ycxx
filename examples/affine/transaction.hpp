#pragma once

namespace ycxx_affine {

class Transaction {
  int token_;
  explicit Transaction(int token) : token_(token) {}
  Transaction(const Transaction&) = default;
  friend Transaction begin() noexcept;
  friend Transaction transfer(Transaction&&) noexcept;

 public:
  Transaction& operator=(const Transaction&) = delete;
};

inline Transaction begin() noexcept { return Transaction(0); }
inline Transaction transfer(Transaction&& source) noexcept { return Transaction(source.token_); }
inline void commit(Transaction&&) noexcept {}
inline void rollback(Transaction&&) noexcept {}

}  // namespace ycxx_affine
