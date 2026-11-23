// The interactive trace browser on a terminal (#245): raw mode on the alternate screen over
// termios, with SGR mouse reports (#635), drawing `:view`'s frames and feeding it what the terminal
// sends. The terminal is restored when the browser ends, and on a signal or a crash.
module;
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
export module ycxx.trace:browse;

import std;
import :model;
import :view;
import :clipboard;

export namespace ycxx::trace {

// A live source of events (`ycxx ctl trace`, `--watch`, a build being traced, #939): when `fd` is
// readable, `pump` reads from it into the tree; `tick`, on every frame while the source runs,
// takes what came without a descriptor to wake for and moves running cells' times to now. Either
// is false once the source has ended.
struct Feed {
  int fd = -1;
  std::function<bool()> pump;
  std::string source;  // what it watches, for the status line (`daemon of /path/to/project`)
  std::function<bool()> tick = {};
  std::string ended = "the daemon has gone";  // the status line's words once it has ended
  bool newest_first = true;                   // the latest request on top
};

// Whether there is a terminal to browse on (the controlling one).
bool has_terminal();

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

  enum class Wake : std::uint8_t { input, feed, resize, tick };
  // Waits for the terminal, the feed or a resize. select, not poll: macOS's poll does not support
  // devices, and flags the terminal invalid.
  // `tick`: after that long with nothing to read (none: wait however long).
  Wake wait(int feed, std::optional<std::chrono::milliseconds> tick = std::nullopt) const {
    if (pending_) return Wake::input;
    const int winch = restore_state.winch[0];
    while (true) {
      fd_set in;
      FD_ZERO(&in);
      FD_SET(fd_, &in);
      if (feed >= 0) FD_SET(feed, &in);
      if (winch >= 0) FD_SET(winch, &in);
      timeval tv{0, tick ? static_cast<suseconds_t>(tick->count() * 1000) : 0};
      const int r = ::select(std::max({fd_, feed, winch}) + 1, &in, nullptr, nullptr, tick ? &tv : nullptr);
      if (r < 0) continue;
      if (r == 0) return Wake::tick;
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

// The clipboard beyond the terminal (#636). OSC 52 reaches it from anywhere a terminal supports it
// (over SSH too), but some terminals (Terminal.app) ignore it, so a local session also writes the
// system's clipboard: `YCXX_CLIPBOARD` when set (a command; `osc52`: none); else the native one
// where there is one (macOS: NSPasteboard, `:clipboard`); else the first of pbcopy, wl-copy
// (Wayland), xclip and xsel (X) on the PATH (Linux until #730). Over SSH none runs: it would copy
// on the remote host.
bool over_ssh() { return std::getenv("SSH_CONNECTION") || std::getenv("SSH_TTY"); }
std::optional<std::string> clipboard_command() {
  if (const char* c = std::getenv("YCXX_CLIPBOARD"); c && *c) {
    if (std::string_view(c) == "osc52") return std::nullopt;
    return std::string(c);
  }
  if (over_ssh()) return std::nullopt;
  const auto found = [](std::string_view name) {
    const char* path = std::getenv("PATH");
    for (auto dir : std::string_view(path ? path : "/usr/bin:/bin") | std::views::split(':')) {
      const std::string file = std::format("{}/{}", std::string_view(dir.begin(), dir.end()), name);
      if (::access(file.c_str(), X_OK) == 0) return true;
    }
    return false;
  };
  const bool wayland = std::getenv("WAYLAND_DISPLAY"), x11 = std::getenv("DISPLAY");
  if (found("pbcopy")) return "pbcopy";
  if (wayland && found("wl-copy")) return "wl-copy";
  if (x11 && found("xclip")) return "xclip -selection clipboard";
  if (x11 && found("xsel")) return "xsel --clipboard --input";
  return std::nullopt;
}
// Runs `command` under /bin/sh with `text` on its standard input; true if it exits 0 within 2 s.
bool run_clipboard(const std::string& command, std::string_view text);
// Writes the local clipboard, as above; says how (`pasteboard`, `pbcopy`), or nothing.
std::optional<std::string> copy_locally(std::string_view text) {
  const char* given = std::getenv("YCXX_CLIPBOARD");
  if (!(given && *given) && !over_ssh() && !native_clipboard.empty() && pasteboard_write(text)) return std::string(native_clipboard);
  const std::optional<std::string> command = clipboard_command();
  if (command && run_clipboard(*command, text)) return command->substr(0, command->find(' '));
  return std::nullopt;
}
bool run_clipboard(const std::string& command, std::string_view text) {
  int in[2];
  if (::pipe(in) != 0) return false;
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(in[0]);
    ::close(in[1]);
    return false;
  }
  if (pid == 0) {
    // Only async-signal-safe calls between fork and exec: the parent may have threads. A process
    // group of its own, so a timeout reaches whatever the command started.
    ::setpgid(0, 0);
    ::dup2(in[0], 0);
    ::close(in[0]);
    ::close(in[1]);
    const int null = ::open("/dev/null", O_WRONLY);
    if (null >= 0) ::dup2(null, 1), ::dup2(null, 2);
    ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
    ::_exit(127);
  }
  ::setpgid(pid, pid);  // as the child does: whichever runs first
  ::close(in[0]);
  // Everything within 2 s, the writing too: a command that does not read never holds the browser.
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  const auto left_ms = [&] {
    return static_cast<int>(std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count()));
  };
  // A command that stops reading must not kill the browser with SIGPIPE.
  struct sigaction ignore{}, old{};
  ignore.sa_handler = SIG_IGN;
  ::sigaction(SIGPIPE, &ignore, &old);
  ::fcntl(in[1], F_SETFL, ::fcntl(in[1], F_GETFL) | O_NONBLOCK);
  bool written = true;
  for (std::string_view rest = text; !rest.empty();) {
    const auto n = ::write(in[1], rest.data(), rest.size());
    if (n > 0) {
      rest.remove_prefix(static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    pollfd p{in[1], POLLOUT, 0};
    if (n < 0 && errno == EAGAIN && left_ms() > 0 && ::poll(&p, 1, left_ms()) > 0) continue;
    written = false;
    break;
  }
  ::close(in[1]);
  ::sigaction(SIGPIPE, &old, nullptr);
  int status = 0;
  while (true) {
    if (::waitpid(pid, &status, WNOHANG) == pid) return written && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (left_ms() == 0) break;
    ::usleep(10'000);
  }
  ::kill(-pid, SIGKILL);
  ::waitpid(pid, &status, 0);
  return false;
}

}  // namespace

bool has_terminal() {
  const int fd = ::open("/dev/tty", O_RDWR | O_CLOEXEC);
  if (fd < 0) return false;
  termios t{};
  const bool ok = ::tcgetattr(fd, &t) == 0;
  ::close(fd);
  return ok;
}

bool browse(Tree& tree, Feed* feed) {
  Terminal term;
  if (!term) return false;
  Browser b(tree);
  if (feed) b.set_live(feed->source, feed->newest_first);
  using clock = std::chrono::steady_clock;
  const auto start = clock::now();
  auto next_tick = start;  // while a drag is held past an edge: ticks on time, whatever else wakes us
  // A live source (#939) is looked at, and the screen drawn, at most every `gap`: 30 times a second,
  // or less often when drawing takes long (a frame of a large tree), so drawing never takes more
  // than a third of a core from the build.
  constexpr std::chrono::milliseconds frame{33};
  std::chrono::nanoseconds gap = frame;
  auto next_frame = start;
  bool dirty = true;
  std::pair<std::size_t, std::size_t> size{0, 0};
  const auto live_step = [&](const std::function<bool()>& f) {
    const std::uint64_t before = tree.version();
    const bool more = b.pump(f);
    if (!more) {
      b.end_live(feed->ended);
      feed = nullptr;
    }
    // Running cells' times move on every frame.
    if (tree.version() != before || tree.running() > 0 || !more) dirty = true;
  };
  while (true) {
    if (const auto now_size = term.size(); now_size != size) size = now_size, dirty = true;
    const auto now = clock::now();
    if (feed && feed->tick && now >= next_frame) {
      live_step(feed->tick);
      next_frame = now + gap;
    }
    if (dirty) {
      const auto t0 = clock::now();
      b.resize(size.first, size.second);
      term.draw(b.frame());
      dirty = false;
      gap = std::max<std::chrono::nanoseconds>(frame, 2 * (clock::now() - t0));
    }
    std::optional<std::chrono::milliseconds> wait;
    if (b.ticking()) {
      const auto t = clock::now();
      if (t >= next_tick) {
        b.tick();
        next_tick = t + Browser::tick_every;
        dirty = true;
        continue;
      }
      wait = std::chrono::duration_cast<std::chrono::milliseconds>(next_tick - t) + std::chrono::milliseconds(1);
    } else {
      next_tick = clock::now() + Browser::tick_every;
    }
    if (feed && feed->tick) {
      const auto until = std::chrono::duration_cast<std::chrono::milliseconds>(next_frame - clock::now()) + std::chrono::milliseconds(1);
      wait = std::max(std::chrono::milliseconds(1), wait ? std::min(*wait, until) : until);
    }
    switch (term.wait(feed && feed->pump ? feed->fd : -1, wait)) {
      case Terminal::Wake::resize: dirty = true; continue;
      case Terminal::Wake::tick: continue;
      case Terminal::Wake::feed:
        live_step(feed->pump);
        continue;
      case Terminal::Wake::input: break;
    }
    dirty = true;
    const auto at = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start);
    const Action a = b.handle(parse_input(term.read_input()), at);
    if (a.quit) return true;
    if (a.copy) {
      term.write(osc52(*a.copy, std::getenv("TMUX") != nullptr));
      const std::optional<std::string> how = copy_locally(*a.copy);
      b.note(std::format("{} (OSC 52{})", b.notice(), how ? ", " + *how : std::string()));
    }
  }
}

}  // namespace ycxx::trace
