// The interactive trace browser on a terminal (#245): raw mode on the alternate screen over
// termios, with SGR mouse reports (#635), drawing `:view`'s frames and feeding it what the terminal
// sends. The terminal is restored when the browser ends, and on a signal or a crash.
module;
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
export module ycxx.trace:browse;

import std;
import :model;
import :view;

export namespace ycxx::trace {

// A live source of events (`ycxx ctl trace`): when `fd` is readable, `pump` reads from it into the
// tree; false once the source has ended.
struct Feed {
  int fd = -1;
  std::function<bool()> pump;
  std::string source;  // what it watches, for the status line (`daemon of /path/to/project`)
};

// Whether `fd` is a terminal that may get colors: a TTY, and `NO_COLOR` not set (no-color.org).
bool colors_for(int fd) {
  const char* no = std::getenv("NO_COLOR");
  return ::isatty(fd) == 1 && !(no && *no);
}
bool is_terminal(int fd) { return ::isatty(fd) == 1; }

// Runs the browser on the controlling terminal until `q`; false (and nothing drawn) when there is
// no terminal to run it on. With a feed, the tree grows while it is browsed.
bool browse(Tree& tree, Feed* feed = nullptr);

}  // namespace ycxx::trace

namespace ycxx::trace {
namespace {

// Mouse reports on: clicks (1000), motion while a button is down (1002), in SGR form (1006), which
// has no limit on columns and tells a release from a press. Autowrap off (7): a line the terminal
// draws wider than we count (a glyph it shows double-width) is cut, and never scrolls the screen.
// Off in the reverse order.
constexpr std::string_view enter_seq = "\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[?1000h\x1b[?1002h\x1b[?1006h";
constexpr std::string_view leave_seq = "\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[?7h\x1b[0m\x1b[?25h\x1b[?1049l";

// What a signal handler needs to give the terminal back: async-signal-safe calls only.
struct Restore {
  int fd = -1;
  termios saved{};
  int winch[2] = {-1, -1};  // SIGWINCH wakes the input wait through this pipe
};
Restore restore_state;
// Signals that end the process: the terminal is restored, then the earlier action runs.
constexpr std::array fatal_signals{SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGABRT, SIGSEGV, SIGBUS, SIGFPE, SIGILL};
std::array<struct sigaction, fatal_signals.size()> previous{};
struct sigaction previous_winch{};
alignas(16) std::array<char, 64 * 1024> alt_stack{};
stack_t previous_stack{};

void give_back() noexcept {
  if (restore_state.fd < 0) return;
  (void)::write(restore_state.fd, leave_seq.data(), leave_seq.size());
  ::tcsetattr(restore_state.fd, TCSAFLUSH, &restore_state.saved);
}
extern "C" void on_fatal(int sig) {
  give_back();
  restore_state.fd = -1;
  for (std::size_t i = 0; i < fatal_signals.size(); ++i) {
    if (fatal_signals[i] == sig) ::sigaction(sig, &previous[i], nullptr);
  }
  ::raise(sig);
}
extern "C" void on_winch(int) {
  const int saved = errno;
  const char c = 1;
  if (restore_state.winch[1] >= 0) (void)::write(restore_state.winch[1], &c, 1);
  errno = saved;
}

// The terminal in raw mode on the alternate screen, with mouse reports, restored when this goes
// away (or on a fatal signal).
class Terminal {
 public:
  Terminal() {
    fd_ = ::open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (fd_ < 0) return;
    if (::tcgetattr(fd_, &saved_) != 0) {
      ::close(fd_);
      fd_ = -1;
      return;
    }
    if (::pipe(restore_state.winch) == 0) {
      for (int p : restore_state.winch) {
        ::fcntl(p, F_SETFL, ::fcntl(p, F_GETFL) | O_NONBLOCK);
        ::fcntl(p, F_SETFD, FD_CLOEXEC);
      }
    }
    restore_state.fd = fd_;
    restore_state.saved = saved_;
    // A stack overflow's SIGSEGV runs on a stack of its own.
    stack_t alt{};
    alt.ss_sp = alt_stack.data();
    alt.ss_size = alt_stack.size();
    ::sigaltstack(&alt, &previous_stack);
    struct sigaction sa{};
    sa.sa_handler = on_fatal;
    sa.sa_flags = SA_ONSTACK;
    sigfillset(&sa.sa_mask);  // one restore, never nested
    for (std::size_t i = 0; i < fatal_signals.size(); ++i) {
      ::sigaction(fatal_signals[i], nullptr, &previous[i]);
      // An ignored signal (nohup, a background job) stays ignored.
      if (previous[i].sa_handler != SIG_IGN) ::sigaction(fatal_signals[i], &sa, nullptr);
    }
    struct sigaction wa{};
    wa.sa_handler = on_winch;
    sigemptyset(&wa.sa_mask);
    wa.sa_flags = SA_RESTART;
    ::sigaction(SIGWINCH, &wa, &previous_winch);

    termios raw = saved_;
    raw.c_iflag &= ~static_cast<tcflag_t>(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~static_cast<tcflag_t>(OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    ::tcsetattr(fd_, TCSAFLUSH, &raw);
    write(enter_seq);
  }
  ~Terminal() {
    if (fd_ < 0) return;
    give_back();
    restore_state.fd = -1;
    for (std::size_t i = 0; i < fatal_signals.size(); ++i) ::sigaction(fatal_signals[i], &previous[i], nullptr);
    ::sigaction(SIGWINCH, &previous_winch, nullptr);
    ::sigaltstack(&previous_stack, nullptr);
    for (int& p : restore_state.winch) {
      if (p >= 0) ::close(p);
      p = -1;
    }
    ::close(fd_);
  }
  Terminal(const Terminal&) = delete;

  explicit operator bool() const noexcept { return fd_ >= 0; }
  std::pair<std::size_t, std::size_t> size() const {
    winsize ws{};
    if (::ioctl(fd_, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) return {24, 80};
    return {ws.ws_row, ws.ws_col};
  }
  void write(std::string_view s) const {
    while (!s.empty()) {
      const auto n = ::write(fd_, s.data(), s.size());
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) return;
      s.remove_prefix(static_cast<std::size_t>(n));
    }
  }
  void draw(const Frame& f) const {
    // Each line at its own row (each is the screen's width): nothing depends on where the last
    // line left the cursor.
    std::string out;
    for (std::size_t i = 0; i < f.lines.size(); ++i) out += std::format("\x1b[{};1H{}", i + 1, f.lines[i]);
    if (f.cursor) out += std::format("\x1b[{};{}H\x1b[?25h", f.cursor->first + 1, f.cursor->second + 1);
    else out += "\x1b[?25l";
    write(out);
  }

  enum class Wake : std::uint8_t { input, feed, resize };
  // Waits for the terminal, the feed or a resize. select, not poll: macOS's poll does not support
  // devices, and flags the terminal invalid.
  Wake wait(int feed) const {
    if (pending_) return Wake::input;
    const int winch = restore_state.winch[0];
    while (true) {
      fd_set in;
      FD_ZERO(&in);
      FD_SET(fd_, &in);
      if (feed >= 0) FD_SET(feed, &in);
      if (winch >= 0) FD_SET(winch, &in);
      if (::select(std::max({fd_, feed, winch}) + 1, &in, nullptr, nullptr, nullptr) < 0) continue;
      if (winch >= 0 && FD_ISSET(winch, &in)) {
        char drain[64];
        while (::read(winch, drain, sizeof drain) > 0) {
        }
        return Wake::resize;
      }
      if (FD_ISSET(fd_, &in)) return Wake::input;
      if (feed >= 0 && FD_ISSET(feed, &in)) return Wake::feed;
    }
  }
  // One input: a byte, or an escape sequence as one string; `q` when the terminal is gone.
  std::string read_input() {
    char c = 0;
    if (!next(c, -1)) return "q";
    if (c != '\x1b') return std::string(1, c);
    std::string seq(1, c);
    // The rest of a sequence arrives at once; a lone Escape does not. ESC [ … final (0x40–0x7e),
    // ESC O x, ESC x (Alt+x); mouse reports `ESC [ < b;x;y M` and the legacy `ESC [ M` with three
    // bytes. Within a mouse report the wait is longer, so one split by a slow link stays whole.
    while (seq.size() < 32) {
      const bool report = seq.starts_with("\x1b[<") || seq.starts_with("\x1b[M");
      char n = 0;
      if (!next(n, report ? 1000 : 100)) break;
      if (n == '\x1b') {
        pending_ = n;  // the next sequence starts: this one (a lone Escape) ends here
        break;
      }
      seq += n;
      if (seq.size() == 2 && n != '[' && n != 'O') break;
      if (seq.size() == 3 && seq[1] == 'O') break;
      if (seq.starts_with("\x1b[M")) {
        if (seq.size() == 6) break;
        continue;
      }
      if (seq.size() > 2 && seq[1] == '[' && n >= 0x40 && n <= 0x7e && !(seq.size() == 3 && n == '<')) break;
    }
    return seq;
  }

 private:
  // The next byte: one pushed back, or one read within `ms` milliseconds (-1: however long).
  bool next(char& c, int ms) {
    if (pending_) {
      c = *pending_;
      pending_.reset();
      return true;
    }
    if (ms >= 0) {
      // A signal (a resize) in the middle of the wait waits out the rest, so it never cuts a
      // sequence in two.
      const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
      while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::microseconds>(until - std::chrono::steady_clock::now()).count();
        if (left <= 0) return false;
        fd_set in;
        FD_ZERO(&in);
        FD_SET(fd_, &in);
        timeval tv{static_cast<time_t>(left / 1'000'000), static_cast<suseconds_t>(left % 1'000'000)};
        const int r = ::select(fd_ + 1, &in, nullptr, nullptr, &tv);
        if (r > 0) break;
        if (r == 0 || errno != EINTR) return false;
      }
    }
    while (true) {
      const auto n = ::read(fd_, &c, 1);
      if (n == 1) return true;
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
  }

  int fd_ = -1;
  termios saved_{};
  std::optional<char> pending_;
};

}  // namespace

bool browse(Tree& tree, Feed* feed) {
  Terminal term;
  if (!term) return false;
  Browser b(tree);
  if (feed) b.set_live(feed->source);
  const auto start = std::chrono::steady_clock::now();
  while (true) {
    const auto [rows, cols] = term.size();
    b.resize(rows, cols);
    term.draw(b.frame());
    switch (term.wait(feed ? feed->fd : -1)) {
      case Terminal::Wake::resize: continue;
      case Terminal::Wake::feed:
        if (!b.pump(feed->pump)) feed = nullptr;
        continue;
      case Terminal::Wake::input: break;
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    if (b.handle(parse_input(term.read_input()), now).quit) return true;
  }
}

}  // namespace ycxx::trace
