// The client–daemon protocol (daemon design §3): length-prefixed frames over a Unix socket, each a
// little-endian u32 body length, a u16 kind and the body serialized by the engine's reflection
// serializer. Client and daemon are the same binary, so there is no cross-version compatibility;
// the handshake checks the build and schema hashes, and the reader never trusts its input.
export module ycxx.daemon:protocol;

import std;
import ycxx.engine;
import ycxx.compile;
import :posix;

export namespace ycxx::daemon {

enum class Kind : std::uint16_t { hello = 1, welcome, compile, done, status, status_reply, shutdown, bye, lsp, refused };

struct Hello {
  std::uint64_t build_hash = 0;
  Fp128 schema_hash;
  std::uint32_t uid = 0;
  std::int64_t pid = 0;
};
struct Welcome {
  std::int64_t daemon_pid = 0;
  std::string root;
};
// A request the daemon will not serve, and why; the client then runs it in-process (§7.1).
struct Refused {
  std::string reason;
};
struct LspRequest {};  // answered by an empty `lsp`; the connection is then the tunnel (§6.4)
struct StatusRequest {};
struct ShutdownRequest {};
struct Bye {};

// The daemon's state and totals (§8.1), for `ycxx ctl status`.
struct Status {
  std::int64_t pid = 0;
  std::string root;
  std::string socket;
  std::uint64_t revision = 0;
  std::uint64_t requests = 0;   // compile requests answered
  std::uint64_t cancelled = 0;  // stopped by a client's hang-up
  std::uint64_t restarts = 0;
  std::uint64_t live = 0;  // compile requests in flight or waiting
  std::uint64_t entries = 0;   // file-table entries
  std::uint64_t batches = 0;   // batches applied
  std::uint64_t inputs_changed = 0;
  std::uint64_t rescans = 0;   // full rescans (§5.4)
  std::uint64_t cookie_timeouts = 0;
  std::uint64_t regions_computed = 0;
  std::uint64_t regions_reused = 0;
  std::uint64_t executed = 0;       // engine cells executed and validated, in all requests
  std::uint64_t validated = 0;
  std::uint64_t last_executed = 0;  // in the last request answered
  std::uint64_t diagnoses = 0;      // editor-view checks for LSP sessions (§6.4)
  std::string watcher;           // "fsevents", "inotify", or why there is none
  std::uint64_t unwatched = 0;   // directories stat-checked per request instead (§5.5)
  std::uint64_t quantum_ms = 0;  // the admission quantum now (§4.2)
  std::uint64_t idle_exit_s = 0;
};

// The schema hash of the messages, derived by reflection (§3.1).
inline constexpr Fp128 schema_hash = [] {
  Hasher h;
  engine::fold_schema<Hello>(h);
  engine::fold_schema<Welcome>(h);
  engine::fold_schema<Refused>(h);
  engine::fold_schema<Status>(h);
  engine::fold_schema<compile::Request>(h);
  engine::fold_schema<compile::Done>(h);
  return h.finish();
}();

inline constexpr std::uint32_t max_frame = 1u << 30;

struct Frame {
  Kind kind{};
  std::vector<std::byte> body;
};

std::vector<std::byte> encode_frame(Kind kind, std::span<const std::byte> body) {
  engine::Sink s;
  s.uint(static_cast<std::uint32_t>(body.size()));
  s.uint(std::to_underlying(kind));
  s.raw(body);
  auto b = s.bytes();
  return {b.begin(), b.end()};
}

template <class T>
bool send(int fd, Kind kind, const T& v) {
  engine::Sink body;
  engine::serialize(body, v);
  return write_all(fd, encode_frame(kind, body.bytes()));
}

// One frame, or why not: end of file before a frame, a short frame, or an impossible length.
std::expected<Frame, std::string> receive(int fd) {
  std::array<std::byte, 6> head{};
  if (!read_exact(fd, head)) return std::unexpected("connection closed");
  engine::Source h(head);
  auto len = h.uint<std::uint32_t>();
  auto kind = h.uint<std::uint16_t>();
  if (len > max_frame) return std::unexpected("frame too large");
  if (kind < std::to_underlying(Kind::hello) || kind > std::to_underlying(Kind::refused)) return std::unexpected("unknown message kind");
  Frame f{static_cast<Kind>(kind), std::vector<std::byte>(len)};
  if (!read_exact(fd, f.body)) return std::unexpected("connection closed in a frame");
  return f;
}

// The body as a T: the whole body, read without error, or nothing.
template <class T>
std::optional<T> decode(const Frame& f) {
  engine::Source in(f.body);
  T v = engine::deserialize<T>(in);
  if (in.failed() || !in.done()) return std::nullopt;
  return v;
}

}  // namespace ycxx::daemon
