// Reaching the daemon from a client (daemon design §7.1, §7.2): connect to the scope's socket; if
// nothing listens, take the start lock, try again, and start `ycxx daemon` detached, waiting for
// its readiness byte; then the `hello`/`welcome` handshake. Every failure is a reason the client
// may print under `-v` before it runs the request in-process.
export module ycxx.daemon:connect;

import std;
import ycxx.compile;
import :posix;
import :os;
import :scope;
import :protocol;

export namespace ycxx::daemon {

// A connection that passed the handshake.
struct Connection {
  Fd fd;
  Welcome welcome;
};

// Connects to the daemon of `root`, starting one if `start` and none listens.
std::expected<Connection, std::string> reach(const std::string& root, bool start);

// Why a request got no answer. `lost`: the daemon closed the connection without answering (it
// crashed or exited), so a retry may succeed (§7.2); otherwise it refused the request.
struct Failure {
  std::string why;
  bool lost = false;
};

// `on_trace` gets the request's trace batches as they arrive, when it asked for them (#245).
std::expected<compile::Done, Failure> request_compile(Connection& c, const compile::Request& r,
                                                      const std::function<void(TraceBatch&&)>& on_trace = {});
// Attaches to the daemon's traces (#245); then `next_trace` reads them, one batch at a time,
// nothing once the daemon has gone.
std::expected<void, std::string> watch_traces(Connection& c, bool values);
std::optional<TraceBatch> next_trace(Connection& c);
// Waits until a file some build read changes (#245); how many entries changed, or why there is no
// answer (the daemon stopped).
std::expected<std::uint64_t, std::string> await_change(Connection& c);
std::expected<Status, std::string> request_status(Connection& c);
// Turns the connection into an LSP tunnel (§6.4): copies `in` to the daemon and the daemon's
// answers to `out` until the session ends; its exit status, or why there is none.
std::expected<int, std::string> tunnel_lsp(Connection& c, int in, int out);
std::expected<void, std::string> request_shutdown(Connection& c);

}  // namespace ycxx::daemon

namespace ycxx::daemon {
namespace {

// `closed`: the daemon closed the connection during the handshake (it was exiting, idle or shut
// down, with this connection still in its backlog), so a new daemon may be started.
std::expected<Connection, std::string> handshake(Fd fd, bool& closed) {
  closed = false;
  if (!send(fd.get(), Kind::hello, Hello{build_hash(), schema_hash, my_uid(), my_pid()})) {
    closed = true;
    return std::unexpected("cannot send hello");
  }
  if (!wait_readable(fd.get(), std::chrono::seconds(10))) return std::unexpected("no welcome from the daemon");
  auto f = receive(fd.get());
  if (!f) {
    closed = true;
    return std::unexpected("handshake: " + f.error());
  }
  if (f->kind == Kind::refused) {
    auto r = decode<Refused>(*f);
    return std::unexpected("the daemon refused: " + (r ? r->reason : "?"));
  }
  if (f->kind != Kind::welcome) return std::unexpected("handshake: unexpected message");
  auto w = decode<Welcome>(*f);
  if (!w) return std::unexpected("handshake: malformed welcome");
  return Connection{std::move(fd), std::move(*w)};
}

// `gone`: nothing listens there, or what listened went away during the handshake.
std::expected<Connection, std::string> try_connect(const Paths& p, bool& gone) {
  bool absent = false, closed = false;
  auto fd = connect_unix(p.socket(), absent);
  gone = absent;
  if (!fd) return std::unexpected(fd.error());
  auto c = handshake(std::move(*fd), closed);
  gone = closed;
  return c;
}

// Starts `ycxx daemon --root <root>` and waits for its readiness byte; end of file means it did
// not start (its log says why).
std::optional<std::string> start_daemon(const Paths& p) {
  auto pipe = make_pipe();
  if (!pipe) return pipe.error();
  std::string exe = executable_path();
  if (exe.empty()) return "cannot find the ycxx executable";
  std::vector<std::string> argv{exe, "daemon", "--root", p.root, "--ready-fd", "3"};
  if (auto err = spawn_detached(argv, p.log(), pipe->second.get())) return err;
  pipe->second.reset();
  // The daemon may wait up to 10 s for an exiting predecessor's lock (§7.3).
  if (!wait_readable(pipe->first.get(), std::chrono::seconds(30))) return "the daemon did not start in time";
  std::array<std::byte, 1> b{};
  if (!read_exact(pipe->first.get(), b)) return std::format("the daemon failed to start (see {})", p.log());
  return std::nullopt;
}

}  // namespace

std::expected<Connection, std::string> reach(const std::string& root, bool start) {
  auto dir = ensure_private_dir(runtime_dir());
  if (!dir) return std::unexpected(dir.error());
  Paths p = paths_for(root, *dir);
  if (!fits_socket_path(p.socket())) return std::unexpected("socket path too long: " + p.socket());
  bool absent = false;
  auto c = try_connect(p, absent);
  if (c || !absent || !start) return c;
  // A daemon that is exiting removes its socket once its requests finish: give it a moment.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  // Another client may be starting it: under the start lock, look again before starting one.
  auto lock = lock_file(p.start_lock(), std::chrono::seconds(40));
  if (!lock) return std::unexpected(lock.error());
  c = try_connect(p, absent);
  if (c || !absent) return c;
  if (auto err = start_daemon(p)) return std::unexpected(*err);
  return try_connect(p, absent);
}

std::expected<compile::Done, Failure> request_compile(Connection& c, const compile::Request& r,
                                                      const std::function<void(TraceBatch&&)>& on_trace) {
  if (!send(c.fd.get(), Kind::compile, r)) return std::unexpected(Failure{"cannot send the request", true});
  auto f = receive(c.fd.get());
  while (f && f->kind == Kind::trace) {
    auto b = decode<TraceBatch>(*f);
    if (!b) return std::unexpected(Failure{"malformed trace", false});
    if (on_trace) on_trace(std::move(*b));
    f = receive(c.fd.get());
  }
  if (!f) return std::unexpected(Failure{"the daemon closed the connection: " + f.error(), true});
  if (f->kind == Kind::refused) {
    auto why = decode<Refused>(*f);
    return std::unexpected(Failure{"the daemon refused: " + (why ? why->reason : "?"), false});
  }
  if (f->kind != Kind::done) return std::unexpected(Failure{"unexpected answer", false});
  auto d = decode<compile::Done>(*f);
  if (!d) return std::unexpected(Failure{"malformed answer", false});
  return std::move(*d);
}

std::expected<void, std::string> watch_traces(Connection& c, bool values) {
  if (!send(c.fd.get(), Kind::watch, WatchRequest{values})) return std::unexpected("cannot send the request");
  auto f = receive(c.fd.get());
  if (!f || f->kind != Kind::watch) return std::unexpected("the daemon did not attach the watch");
  return {};
}

std::optional<TraceBatch> next_trace(Connection& c) {
  auto f = receive(c.fd.get());
  if (!f || f->kind != Kind::trace) return std::nullopt;
  return decode<TraceBatch>(*f);
}

std::expected<std::uint64_t, std::string> await_change(Connection& c) {
  if (!send(c.fd.get(), Kind::await_change, AwaitChange{})) return std::unexpected("cannot send the request");
  auto f = receive(c.fd.get());
  if (!f) return std::unexpected("the daemon closed the connection: " + f.error());
  if (f->kind != Kind::changed) return std::unexpected("unexpected answer");
  auto ch = decode<Changed>(*f);
  if (!ch) return std::unexpected("malformed answer");
  return ch->entries;
}

std::expected<Status, std::string> request_status(Connection& c) {
  if (!send(c.fd.get(), Kind::status, StatusRequest{})) return std::unexpected("cannot send the request");
  auto f = receive(c.fd.get());
  if (!f || f->kind != Kind::status_reply) return std::unexpected("no status from the daemon");
  auto s = decode<Status>(*f);
  if (!s) return std::unexpected("malformed status");
  return std::move(*s);
}

std::expected<int, std::string> tunnel_lsp(Connection& c, int in, int out) {
  const int sock = c.fd.get();
  if (!send(sock, Kind::lsp, LspRequest{})) return std::unexpected("cannot send the request");
  auto f = receive(sock);
  if (!f) return std::unexpected("the daemon closed the connection: " + f.error());
  if (f->kind == Kind::refused) {
    auto why = decode<Refused>(*f);
    return std::unexpected("the daemon refused: " + (why ? why->reason : "?"));
  }
  if (f->kind != Kind::lsp) return std::unexpected("unexpected answer");
  // One thread per direction: a blocking write one way never holds up a read the other way, so a
  // large answer and a large edit crossing do not wait on each other. The editor's end of input
  // ends what we send. The input thread is left blocked on `in` when the session ends: the
  // process exits next.
  std::thread([sock, in] {
    std::array<char, 1 << 16> buf;
    for (;;) {
      const std::size_t n = read_some(in, buf);
      if (n == 0 || !write_all(sock, std::as_bytes(std::span(buf.data(), n)))) break;
    }
    shutdown_write(sock);
  }).detach();
  for (;;) {
    auto frame = receive(sock);
    if (!frame) return std::unexpected("the daemon closed the session: " + frame.error());
    auto o = frame->kind == Kind::lsp ? decode<LspOut>(*frame) : std::nullopt;
    if (!o) return std::unexpected("unexpected answer");
    if (o->end) return o->exit_status;
    if (!write_all(out, std::as_bytes(std::span(o->bytes)))) return std::unexpected("cannot write to the editor");
  }
}

std::expected<void, std::string> request_shutdown(Connection& c) {
  if (!send(c.fd.get(), Kind::shutdown, ShutdownRequest{})) return std::unexpected("cannot send the request");
  auto f = receive(c.fd.get());
  if (!f || f->kind != Kind::bye) return std::unexpected("no answer from the daemon");
  return {};
}

}  // namespace ycxx::daemon
