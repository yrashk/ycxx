#pragma once

#include <ycxx/ownership.hpp>

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

using TransactionPolicy = ycxx::ownership::policy<
    ycxx::ownership::linear<Transaction>,
    ycxx::ownership::creates<decltype(&begin), &begin>,
    ycxx::ownership::transfers<decltype(&transfer), &transfer>,
    ycxx::ownership::consumes<decltype(&commit), &commit>,
    ycxx::ownership::consumes<decltype(&rollback), &rollback>>;
using CheckTransaction = ycxx::ownership::use_policy<TransactionPolicy>;

}  // namespace ycxx_affine
