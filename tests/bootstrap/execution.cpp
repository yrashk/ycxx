// std::execution (P2300) from libycxx, in the shapes the query engine uses (DECISIONS §4.3):
// work on parallel_scheduler, when_all, a counting_scope joined before results are read, stop
// tokens, and a coroutine awaiting a sender.
import std;
import bootstrap.smoke;

namespace ex = std::execution;

namespace {

ex::task<int> awaiting(ex::parallel_scheduler sched) {
  int a = co_await (ex::schedule(sched) | ex::then([] { return 20; }));
  int b = co_await ex::just(22);
  co_return a + b;
}

}  // namespace

int main() {
  auto sched = ex::get_parallel_scheduler();

  auto [x, y] = std::this_thread::sync_wait(
      ex::when_all(ex::schedule(sched) | ex::then([] { return 6; }),
                   ex::schedule(sched) | ex::then([] { return 7; })))
                    .value();
  smoke::check(x * y == 42, "when_all on parallel_scheduler");

  std::atomic<int> done = 0;
  ex::counting_scope scope;
  for (int i = 0; i < 16; ++i) {
    // spawn takes senders that cannot fail ([exec.spawn]); scheduling on the pool can.
    ex::spawn(ex::schedule(sched) | ex::then([&] noexcept { done.fetch_add(1); }) |
                  ex::upon_error([](std::exception_ptr) noexcept {}),
              scope.get_token());
  }
  std::this_thread::sync_wait(scope.join());
  smoke::check(done.load() == 16, "counting_scope join");

  std::stop_source stop;
  stop.request_stop();
  auto [seen] = std::this_thread::sync_wait(
                    ex::write_env(ex::read_env(std::get_stop_token) |
                                      ex::then([](std::stoppable_token auto t) { return t.stop_requested(); }),
                                  ex::prop(std::get_stop_token, stop.get_token())))
                    .value();
  smoke::check(seen, "stop token reaches the work");

  auto [r] = std::this_thread::sync_wait(awaiting(sched)).value();
  smoke::check(r == 42, "task awaiting senders");
  return smoke::result("execution");
}
