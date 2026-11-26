// The execution provider used by the query engine and its callers.
export module ycxx.execution;
import std;

export namespace ycxx::exec {

using ::std::execution::bulk;
using ::std::execution::completion_signatures;
using ::std::execution::counting_scope;
using ::std::execution::env_of_t;
using ::std::execution::forward_progress_guarantee;
using ::std::execution::get_completion_scheduler;
using ::std::execution::get_completion_scheduler_t;
using ::std::execution::get_env;
using ::std::execution::get_forward_progress_guarantee_t;
using ::std::execution::get_parallel_scheduler;
using ::std::execution::just;
using ::std::execution::operation_state_tag;
using ::std::execution::par;
using ::std::execution::parallel_scheduler;
using ::std::execution::prop;
using ::std::execution::read_env;
using ::std::execution::schedule;
using ::std::execution::scheduler;
using ::std::execution::scheduler_tag;
using ::std::execution::sender_tag;
using ::std::execution::set_error;
using ::std::execution::set_error_t;
using ::std::execution::set_stopped;
using ::std::execution::set_stopped_t;
using ::std::execution::set_value;
using ::std::execution::set_value_t;
using ::std::execution::spawn;
using ::std::execution::task;
using ::std::execution::then;
using ::std::execution::upon_error;
using ::std::execution::upon_stopped;
using ::std::execution::when_all;
using ::std::execution::write_env;

using ::std::get_stop_token;
using ::std::get_stop_token_t;
using ::std::inplace_stop_callback;
using ::std::inplace_stop_source;
using ::std::inplace_stop_token;
using ::std::never_stop_token;
using ::std::stop_callback_for_t;
using ::std::stop_token_of_t;
using ::std::stoppable_token;
using ::std::unstoppable_token;
using ::std::this_thread::sync_wait;

namespace parallel_scheduler_replacement = ::std::execution::parallel_scheduler_replacement;

using worker_thread = std::jthread;

template <class F>
void start_worker(worker_thread& worker, std::size_t stack, F&& fn) {
  worker = std::jthread(std::jthread::stack_size_hint(stack), std::forward<F>(fn));
}

} // namespace ycxx::exec
