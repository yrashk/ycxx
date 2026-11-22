// The regular driver, plus a fixture entry point with private, inherited control sockets.
// A gate is used once per source so interrupted Build attempts can restart normally.
import std;
import ycxx.daemon;
import ycxx.driver;

namespace {
using namespace ycxx::daemon;
using namespace std::chrono_literals;

enum class Mode { stop, release, observe };

std::optional<Mode> mode_of(std::string_view text) {
  if (text == "stop") return Mode::stop;
  if (text == "release") return Mode::release;
  if (text == "observe") return Mode::observe;
  return std::nullopt;
}

std::optional<int> descriptor(std::string_view text) {
  int fd = -1;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), fd);
  if (error != std::errc{} || end != text.data() + text.size() || fd < 3) return std::nullopt;
  return fd;
}

class Gate {
 public:
  Gate(Mode mode, int fd) : mode_(mode), fd_(fd) {}

  void reached(std::inplace_stop_token stop) const {
    if (used_.test_and_set()) return;
    if (mode_ == Mode::observe) {
      send('P');
      return;
    }
    auto pipe = make_pipe();
    if (!pipe) {
      send('E');
      return;
    }
    const auto& [reader, writer] = *pipe;
    // The engine's stop only wakes this worker. Build's normal stop point unwinds its frame.
    std::inplace_stop_callback wake(stop, [fd = writer.get()] noexcept {
      const std::byte byte{};
      write_all(fd, std::span(&byte, 1));
    });
    if (stop.stop_requested()) {
      send('S');
      return;
    }
    if (!send('P')) return;

    const int fds[] = {reader.get(), fd_.get()};
    const auto ready = wait_any(fds, 30s);
    if (stop.stop_requested()) {
      send('S');
    } else if (ready.empty()) {
      send('T');
    } else {
      char command{};
      if (read_some(fd_.get(), std::span(&command, 1)) == 0) return;  // fixture closed its end
      send(mode_ == Mode::release && command == 'R' ? 'R' : 'E');
    }
  }

 private:
  bool send(char status) const { return write_all(fd_.get(), std::as_bytes(std::span(&status, 1))); }

  Mode mode_;
  Fd fd_;
  mutable std::atomic_flag used_;
};

int serve_test(std::span<const std::string_view> args) {
  if (args.empty() || (args.size() - 1) % 3 != 0) return 2;
  ServerOptions options;
  options.root = root_for(std::nullopt, std::filesystem::current_path().native_encoded_string());
  options.ready_fd = descriptor(args.front());
  options.idle_exit = 300s;
  if (!options.ready_fd) return 2;

  std::map<std::string, Gate, std::less<>> gates;
  for (std::size_t i = 1; i < args.size(); i += 3) {
    auto mode = mode_of(args[i]);
    auto fd = descriptor(args[i + 2]);
    if (!mode || !fd || !gates.try_emplace(std::string(args[i + 1]), *mode, *fd).second) return 2;
  }
  options.build_checkpoint = [&gates](std::string_view path, std::inplace_stop_token stop) {
    if (auto it = std::as_const(gates).find(path); it != gates.cend()) it->second.reached(stop);
  };
  return serve(options);
}
}  // namespace

int main(int argc, char** argv) {
  const auto args = std::span(argv, static_cast<std::size_t>(argc)) |
                    std::views::transform([](const char* a) { return std::string_view(a); }) |
                    std::ranges::to<std::vector>();
  if (args.size() > 1 && args[1] == "serve-process-test") return serve_test(std::span(args).subspan(2));
  return ycxx::driver::run(args);
}
