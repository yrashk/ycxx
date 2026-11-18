// Coroutines: std::generator, and a hand-written lazy task with symmetric transfer, the shape of
// a query execution that suspends while it waits for another (DECISIONS §4.3).
import std;
import bootstrap.smoke;

namespace {

std::generator<int> iota(int n) {
  for (int i = 0; i < n; ++i) co_yield i;
}

template <class T>
class task {
public:
  struct promise_type {
    std::optional<T> value;
    std::coroutine_handle<> continuation = std::noop_coroutine();
    task get_return_object() { return task(std::coroutine_handle<promise_type>::from_promise(*this)); }
    std::suspend_always initial_suspend() noexcept { return {}; }
    auto final_suspend() noexcept {
      struct final_awaiter {
        bool await_ready() noexcept { return false; }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
          return h.promise().continuation;
        }
        void await_resume() noexcept {}
      };
      return final_awaiter{};
    }
    void return_value(T v) { value = std::move(v); }
    void unhandled_exception() { throw; }
  };

  task(task&& other) noexcept : h_(std::exchange(other.h_, {})) {}
  ~task() {
    if (h_) h_.destroy();
  }

  bool await_ready() const noexcept { return false; }
  std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept {
    h_.promise().continuation = awaiting;
    return h_;
  }
  T await_resume() { return std::move(*h_.promise().value); }

  T run() {
    h_.resume();
    return std::move(*h_.promise().value);
  }

private:
  explicit task(std::coroutine_handle<promise_type> h) : h_(h) {}
  std::coroutine_handle<promise_type> h_;
};

task<int> leaf(int x) { co_return x * x; }

task<int> query(int n) {
  int total = 0;
  for (int i = 1; i <= n; ++i) total += co_await leaf(i);
  co_return total;
}

}  // namespace

int main() {
  smoke::check(std::ranges::fold_left(iota(5), 0, std::plus{}) == 10, "std::generator");
  smoke::check(query(10).run() == 385, "nested tasks with symmetric transfer");
  return smoke::result("coroutines");
}
