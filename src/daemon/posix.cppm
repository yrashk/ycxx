// The POSIX calls the daemon and its clients make (daemon design §3.1, §7): descriptors, Unix
// sockets, locks, stat and the detached spawn. What differs between macOS and Linux is in :os.
module;
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
export module ycxx.daemon:posix;

import std;
import :os;

export namespace ycxx::daemon {

// The message of the last failed call.
std::string os_error(std::string_view what) { return std::format("{}: {}", what, ::strerror(errno)); }

// An owned descriptor.
class Fd {
 public:
  Fd() = default;
  explicit Fd(int fd) noexcept : fd_(fd) {}
  Fd(Fd&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
  Fd& operator=(Fd&& o) noexcept {
    if (this != &o) {
      reset();
      fd_ = std::exchange(o.fd_, -1);
    }
    return *this;
  }
  ~Fd() { reset(); }
  int get() const noexcept { return fd_; }
  explicit operator bool() const noexcept { return fd_ >= 0; }
  int release() noexcept { return std::exchange(fd_, -1); }
  void reset() noexcept {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }

 private:
  int fd_ = -1;
};

std::uint32_t my_uid() { return ::getuid(); }
std::int64_t my_pid() { return ::getpid(); }

// A file's stat identity (§5.2): an edit changes at least one of these, except a same-size
// rewrite within one timestamp tick, which only the content hash sees.
struct Identity {
  std::uint64_t dev = 0;
  std::uint64_t ino = 0;
  std::uint64_t size = 0;
  std::int64_t mtime_ns = 0;
  std::int64_t ctime_ns = 0;
  bool regular = false;
  bool directory = false;
  bool operator==(const Identity&) const = default;
};
std::optional<Identity> stat_path(const std::string& path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) return std::nullopt;
  return Identity{static_cast<std::uint64_t>(st.st_dev), static_cast<std::uint64_t>(st.st_ino),
                  static_cast<std::uint64_t>(st.st_size), mtime_ns(st), ctime_ns(st), S_ISREG(st.st_mode), S_ISDIR(st.st_mode)};
}
std::optional<Identity> lstat_path(const std::string& path) {
  struct stat st{};
  if (::lstat(path.c_str(), &st) != 0) return std::nullopt;
  return Identity{static_cast<std::uint64_t>(st.st_dev), static_cast<std::uint64_t>(st.st_ino),
                  static_cast<std::uint64_t>(st.st_size), mtime_ns(st), ctime_ns(st), S_ISREG(st.st_mode), S_ISDIR(st.st_mode)};
}

// The real path (symlinks resolved), if the file exists.
std::optional<std::string> real_path(const std::string& path) {
  char* p = ::realpath(path.c_str(), nullptr);
  if (!p) return std::nullopt;
  std::string s(p);
  ::free(p);
  return s;
}

// The per-user runtime directory, created 0700 if missing; an error if it is not a directory
// owned by this user and closed to others (§3.1).
std::expected<std::string, std::string> ensure_private_dir(const std::string& dir) {
  if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return std::unexpected(os_error("cannot create " + dir));
  struct stat st{};
  if (::lstat(dir.c_str(), &st) != 0) return std::unexpected(os_error("cannot stat " + dir));
  if (!S_ISDIR(st.st_mode)) return std::unexpected(dir + " is not a directory");
  if (st.st_uid != ::getuid()) return std::unexpected(dir + " belongs to another user");
  if ((st.st_mode & 077) != 0) return std::unexpected(dir + " is open to other users");
  return dir;
}

// An exclusive `flock` on `path` (created if missing), waiting at most `wait`.
std::expected<Fd, std::string> lock_file(const std::string& path, std::chrono::milliseconds wait) {
  Fd fd(::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
  if (!fd) return std::unexpected(os_error("cannot open " + path));
  auto until = std::chrono::steady_clock::now() + wait;
  for (;;) {
    if (::flock(fd.get(), LOCK_EX | LOCK_NB) == 0) return fd;
    if (errno != EWOULDBLOCK && errno != EINTR) return std::unexpected(os_error("cannot lock " + path));
    if (std::chrono::steady_clock::now() >= until) return std::unexpected("timed out waiting for " + path);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

// Whether a socket path fits `sun_path` (104 bytes on macOS, 108 on Linux, with the NUL).
bool fits_socket_path(const std::string& path) { return path.size() < sizeof(sockaddr_un{}.sun_path); }

std::expected<Fd, std::string> listen_unix(const std::string& path) {
  if (!fits_socket_path(path)) return std::unexpected("socket path too long: " + path);
  Fd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
  if (!fd) return std::unexpected(os_error("socket"));
  ::fcntl(fd.get(), F_SETFD, FD_CLOEXEC);
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  std::ranges::copy(path, a.sun_path);
  if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) return std::unexpected(os_error("cannot bind " + path));
  if (::listen(fd.get(), 128) != 0) return std::unexpected(os_error("listen"));
  return fd;
}

// Connects; `absent` is set when nothing listens there (ENOENT, ECONNREFUSED), which is when a
// client starts the daemon (§7.1).
std::expected<Fd, std::string> connect_unix(const std::string& path, bool& absent) {
  absent = false;
  if (!fits_socket_path(path)) return std::unexpected("socket path too long: " + path);
  Fd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
  if (!fd) return std::unexpected(os_error("socket"));
  ::fcntl(fd.get(), F_SETFD, FD_CLOEXEC);
  no_sigpipe(fd.get());
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  std::ranges::copy(path, a.sun_path);
  while (::connect(fd.get(), reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    if (errno == EINTR) continue;
    absent = errno == ENOENT || errno == ECONNREFUSED;
    return std::unexpected(os_error("cannot connect to " + path));
  }
  return fd;
}

// Accepts a connection, or nothing if there was none ready.
std::optional<Fd> accept_one(int listener) {
  int c = ::accept(listener, nullptr, nullptr);
  if (c < 0) return std::nullopt;
  ::fcntl(c, F_SETFD, FD_CLOEXEC);
  no_sigpipe(c);
  return Fd(c);
}

// Waits until `fd` is readable (or hung up), at most `timeout` (none: forever). False on timeout.
bool wait_readable(int fd, std::optional<std::chrono::milliseconds> timeout) {
  pollfd p{fd, POLLIN, 0};
  for (;;) {
    int r = ::poll(&p, 1, timeout ? static_cast<int>(timeout->count()) : -1);
    if (r < 0 && errno == EINTR) continue;
    return r > 0;
  }
}

// Waits on several descriptors; the indexes of those readable or hung up. Empty on timeout.
std::vector<std::size_t> wait_any(std::span<const int> fds, std::optional<std::chrono::milliseconds> timeout) {
  std::vector<pollfd> ps;
  for (int fd : fds) ps.push_back({fd, POLLIN, 0});
  for (;;) {
    int r = ::poll(ps.data(), static_cast<nfds_t>(ps.size()), timeout ? static_cast<int>(timeout->count()) : -1);
    if (r < 0 && errno == EINTR) continue;
    std::vector<std::size_t> ready;
    if (r > 0)
      for (std::size_t i = 0; i < ps.size(); ++i)
        if (ps[i].revents != 0) ready.push_back(i);
    return ready;
  }
}

// Reads exactly `out.size()` bytes; false on end of file or error.
bool read_exact(int fd, std::span<std::byte> out) {
  std::size_t at = 0;
  while (at < out.size()) {
    ssize_t n = ::read(fd, out.data() + at, out.size() - at);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    at += static_cast<std::size_t>(n);
  }
  return true;
}

// Writes everything. To a socket it never raises SIGPIPE (a peer that went away is a failed
// write, which a client then treats as a lost daemon); other descriptors are written as they are.
bool write_all(int fd, std::span<const std::byte> in) {
  std::size_t at = 0;
  bool socket = true;
  while (at < in.size()) {
    ssize_t n = socket ? ::send(fd, in.data() + at, in.size() - at, send_flags) : -1;
    if (socket && n < 0 && errno == ENOTSOCK) socket = false;
    if (!socket) n = ::write(fd, in.data() + at, in.size() - at);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    at += static_cast<std::size_t>(n);
  }
  return true;
}

// Empties what is readable now from a pipe used only to wake a poll.
void drain(int fd) {
  char buf[64];
  [[maybe_unused]] auto n = ::read(fd, buf, sizeof buf);
}

// A pipe whose ends are closed on exec.
std::expected<std::pair<Fd, Fd>, std::string> make_pipe() {
  int p[2];
  if (::pipe(p) != 0) return std::unexpected(os_error("pipe"));
  ::fcntl(p[0], F_SETFD, FD_CLOEXEC);
  ::fcntl(p[1], F_SETFD, FD_CLOEXEC);
  return std::pair{Fd(p[0]), Fd(p[1])};
}

// Starts `argv` detached (§7.1): a new session, stdin from /dev/null, stdout and stderr to `log`,
// and every other descriptor closed except `keep`, which the child gets as descriptor 3. The
// intermediate child exits at once, so the daemon is nobody's child.
std::optional<std::string> spawn_detached(const std::vector<std::string>& argv, const std::string& log, int keep) {
  std::vector<char*> args;
  for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);
  int null_fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  int log_fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (null_fd < 0 || log_fd < 0) {
    if (null_fd >= 0) ::close(null_fd);
    if (log_fd >= 0) ::close(log_fd);
    return os_error("cannot open the daemon's log " + log);
  }
  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(null_fd);
    ::close(log_fd);
    return os_error("fork");
  }
  if (pid == 0) {
    ::setsid();
    if (::fork() != 0) ::_exit(0);
    // Descriptors 0–3 are set from ones above them (or from themselves, then kept by dup2).
    int fds[4] = {null_fd, log_fd, log_fd, keep};
    int high[4];
    for (int i = 0; i < 4; ++i) high[i] = ::fcntl(fds[i], F_DUPFD, 10);
    for (int i = 0; i < 4; ++i) ::dup2(high[i], i);
    close_fds_from(4);
    ::execv(args[0], args.data());
    ::_exit(127);
  }
  ::close(null_fd);
  ::close(log_fd);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return std::nullopt;
}

// Ignores SIGPIPE in the daemon: a hung-up client is seen as a failed write.
void ignore_sigpipe() { ::signal(SIGPIPE, SIG_IGN); }

// SIGTERM and SIGINT write a byte to the returned descriptor (§7.4).
namespace detail {
inline int term_fd = -1;
void on_term(int) {
  char c = 't';
  [[maybe_unused]] auto n = ::write(term_fd, &c, 1);
}
}  // namespace detail
std::expected<Fd, std::string> termination_pipe() {
  auto p = make_pipe();
  if (!p) return std::unexpected(p.error());
  detail::term_fd = p->second.release();
  ::fcntl(detail::term_fd, F_SETFL, O_NONBLOCK);
  struct sigaction sa{};
  sa.sa_handler = detail::on_term;
  sigemptyset(&sa.sa_mask);
  ::sigaction(SIGTERM, &sa, nullptr);
  ::sigaction(SIGINT, &sa, nullptr);
  return std::move(p->first);
}

bool remove_file(const std::string& path) { return ::unlink(path.c_str()) == 0; }

// Creates a new regular file with `text`, never following a symbolic link and never replacing an
// existing file: a repository cannot redirect what the daemon writes into it (§5.4's cookies).
bool create_new_file(const std::string& path, std::string_view text) {
  Fd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
  return fd && write_all(fd.get(), std::as_bytes(std::span(text)));
}

}  // namespace ycxx::daemon
