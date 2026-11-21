#pragma once

#include <ycxx/ownership.hpp>
#include <ycxx/ownership/runtime.hpp>

namespace example {

class Work {
  ycxx::ownership::linear_state state_;
  Work() noexcept {}
  explicit Work(Work&& source) noexcept
      : state_(static_cast<ycxx::ownership::linear_state&&>(source.state_)) {}

  friend Work start() noexcept;
  friend Work handoff(Work&&) noexcept;
  friend void finish(Work&&) noexcept;
};

inline Work start() noexcept { return Work(); }
inline Work handoff(Work&& source) noexcept { return Work(static_cast<Work&&>(source)); }
inline void finish(Work&& work) noexcept { work.state_.consume(); }

using WorkResource = ycxx::ownership::policy<ycxx::ownership::linear<Work>>;
using WorkOperations = ycxx::ownership::policy<
    ycxx::ownership::creates<decltype(&start), &start>,
    ycxx::ownership::transfers<decltype(&handoff), &handoff>,
    ycxx::ownership::consumes<decltype(&finish), &finish>>;
using WorkPolicy = ycxx::ownership::policy<WorkResource, WorkOperations>;
using CheckWork = ycxx::ownership::use_policy<WorkPolicy>;

}  // namespace example
