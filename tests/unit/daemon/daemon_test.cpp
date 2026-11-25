// The daemon's parts (daemon design §8.2, unit tests): framing round trips and malformed frames,
// root discovery (worktrees, submodules, `.ycxx`, home), the admission quantum under simulated
// time, the file table's batches, the platform watcher with cookie sync on a real directory, and
// the requests an LSP session checks documents with.
import std;
import ycxx.unit;
import ycxx.compile;
import ycxx.daemon;

using namespace ycxx;
using namespace ycxx::daemon;
using unit::check;
namespace fs = std::filesystem;

namespace {

// A fresh directory under the system's temporary directory, by its real path.
fs::path temp_dir(std::string_view name) {
  fs::path base = fs::temp_directory_path() / std::format("ycxx-daemon-test-{}-{}", my_pid(), name);
  fs::remove_all(base);
  fs::create_directories(base);
  return fs::path(*real_path(base.native_encoded_string()));
}

void write(const fs::path& p, std::string_view text) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
}

std::string s(const fs::path& p) { return p.native_encoded_string(); }

// --- framing ---------------------------------------------------------------------------------

void framing() {
  auto pipe = make_pipe();
  check(pipe.has_value(), "pipe");
  int r = pipe->first.get(), w = pipe->second.get();

  compile::Request req{{"-c", "a.cpp", "-o", "a.o"}, "/tmp/x", {{"SDKROOT", "/sdk"}}, 1234};
  check(send(w, Kind::compile, req), "send a request");
  auto f = receive(r);
  check(f && f->kind == Kind::compile, "receive a request");
  auto back = decode<compile::Request>(*f);
  check(back && back->argv == req.argv && back->cwd == req.cwd && back->env == req.env && back->build_time == 1234,
        "a request round-trips");

  compile::Done done{1, "out", "err", {{"a.o", {1, 2, 3}, false}}, {7, 1, 2, 3, 4}};
  send(w, Kind::done, done);
  auto d = decode<compile::Done>(*receive(r));
  check(d && d->exit_status == 1 && d->out == "out" && d->err == "err" && d->outputs.size() == 1 &&
            d->outputs[0].bytes == std::vector<std::uint8_t>{1, 2, 3} && d->report.revision == 7 && d->report.restarts == 1,
        "an answer round-trips");

  // A body of the wrong kind, a truncated body and trailing bytes are rejected.
  send(w, Kind::welcome, Welcome{42, "/root"});
  auto wf = receive(r);
  check(wf && !decode<compile::Done>(*wf), "a welcome is not a done");
  Frame truncated{Kind::welcome, wf->body};
  truncated.body.pop_back();
  check(!decode<Welcome>(truncated), "a truncated body is rejected");
  Frame trailing{Kind::welcome, wf->body};
  trailing.body.push_back(std::byte{0});
  check(!decode<Welcome>(trailing), "trailing bytes are rejected");

  // An unknown kind and an impossible length close the connection.
  write_all(w, encode_frame(static_cast<Kind>(999), {}));
  check(!receive(r), "an unknown kind is rejected");
  std::array<std::byte, 6> huge{std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{1}, std::byte{0}};
  write_all(w, huge);
  check(!receive(r), "a frame over the limit is rejected");
  // End of file in the middle of a frame.
  auto head = encode_frame(Kind::status, std::vector<std::byte>(8));
  write_all(w, std::span(head).first(10));
  pipe->second.reset();
  check(!receive(r), "a short frame is rejected");
}

// --- root discovery (§2) ----------------------------------------------------------------------

void roots() {
  fs::path t = temp_dir("roots");
  std::string home = s(t / "home");
  fs::create_directories(t / "home");
  // A real start under TMPDIR can have project markers above it.
  fs::create_directories(t / ".git");
  check(find_root({}, home) == home, "no path to walk: the home scope");
  check(find_root(home, home) == s(t), "an enclosing repository takes precedence over home");

  // A repository, a worktree of it (a `.git` file into .git/worktrees) and a submodule (a `.git`
  // file into .git/modules).
  fs::create_directories(t / "repo" / ".git" / "worktrees" / "wt");
  write(t / "repo" / "src" / "a.cpp", "");
  check(find_root(s(t / "repo" / "src" / "a.cpp"), home) == s(t / "repo"), "a repository's file");
  write(t / "wt" / ".git", std::format("gitdir: {}\n", s(t / "repo" / ".git" / "worktrees" / "wt")));
  write(t / "wt" / "b.cpp", "");
  check(find_root(s(t / "wt" / "b.cpp"), home) == s(t / "wt"), "a worktree is its own root");
  write(t / "repo" / "sub" / ".git", "gitdir: ../.git/modules/sub\n");
  write(t / "repo" / "sub" / "c.cpp", "");
  check(find_root(s(t / "repo" / "sub" / "c.cpp"), home) == s(t / "repo"), "a submodule belongs to its superproject");
  write(t / "wt" / "sub" / ".git", std::format("gitdir: {}\n", s(t / "repo" / ".git" / "worktrees" / "wt" / "modules" / "sub")));
  write(t / "wt" / "sub" / "d.cpp", "");
  check(find_root(s(t / "wt" / "sub" / "d.cpp"), home) == s(t / "wt"), "a worktree's submodule belongs to the worktree");

  fs::path project = t / "repo" / "vendor" / "lib";
  write(project / "e.cpp", "");
  fs::create_directory(project / "build.ycxx");
  check(find_root(s(project / "e.cpp"), home) == s(t / "repo"), "a directory named build.ycxx is not a marker");
  fs::remove(project / "build.ycxx");
  write(project / "build.ycxx", "");
  check(find_root(s(project / "e.cpp"), home) == s(project), "a build manifest defines its own scope");

  // `.ycxx` at the same directory still takes precedence, and `root =` widens.
  write(project / ".ycxx", "");
  check(find_root(s(project / "e.cpp"), home) == s(project), "a .ycxx marker");
  write(project / ".ycxx", "# widen\nroot = ../..\n");
  check(find_root(s(project / "e.cpp"), home) == s(t / "repo"), "a .ycxx root = widens the scope");

  // A symbolic link is walked from its target.
  fs::create_directory_symlink(t / "repo" / "src", t / "home" / "link");
  check(find_root(s(t / "home" / "link" / "a.cpp"), home) == s(t / "repo"), "a link is walked from its real path");

  // Socket names: 16 hex digits each, different roots apart.
  Paths a = paths_for(s(t / "repo"), "/run"), b = paths_for(s(t / "wt"), "/run");
  check(a.socket() != b.socket() && a.socket().ends_with(".sock") && a.base.size() == std::string("/run/").size() + 33,
        "socket names by scope and build hash");
  fs::remove_all(t);
}

// --- admission (§4.2) -------------------------------------------------------------------------

void admission() {
  using namespace std::chrono_literals;
  Admission a(50ms);
  auto t0 = Admission::Clock::time_point{};
  check(a.may_apply(t0), "the first batch applies at once");
  a.applied(t0);
  check(!a.may_apply(t0 + 49ms) && a.may_apply(t0 + 50ms), "no sooner than the floor");
  a.restarted(false);
  check(a.quantum() == 100ms, "a restart without progress doubles");
  a.restarted(false);
  check(a.quantum() == 200ms && !a.may_apply(t0 + 199ms) && a.may_apply(t0 + 200ms), "and doubles again");
  a.restarted(true);
  check(a.quantum() == 200ms, "a restart with progress keeps it");
  a.progressed();
  check(a.quantum() == 50ms, "progress by every live root returns it to the floor");
  for (int i = 0; i < 100; ++i) a.restarted(false);
  check(a.quantum() == Admission::ceiling, "the quantum is bounded");
}

// --- the file table (§5.2, §5.3) --------------------------------------------------------------

void file_table() {
  fs::path t = temp_dir("table");
  write(t / "inc" / "a.h", "one");
  std::vector<std::string> watched;
  FileTable table([&](const std::string& d) { watched.push_back(d); });
  table.set_cwd(s(t));
  check(table.read("inc/a.h") == "one", "a relative path is read against the request's directory");
  check(table.read(s(t / "inc" / "a.h")) == "one" && table.size() == 1, "one entry per absolute path");
  check(!table.read("inc/missing.h") && table.size() == 2, "a negative lookup is an entry");
  check(!table.read("gen/sub/b.h"), "a file in a missing directory is absent");
  check(std::ranges::contains(watched, s(t / "inc")) && std::ranges::contains(watched, s(t)),
        "directories are watched, a missing one at its nearest existing ancestor");

  // Paths no entry knows are dropped: a build's outputs.
  write(t / "a.o", "obj");
  check(!table.knows_any(std::vector{s(t / "a.o")}) && table.apply(std::vector{s(t / "a.o")}) == 0, "an unknown path is dropped");

  // Entries change only when a batch applies.
  write(t / "inc" / "a.h", "two");
  check(table.read("inc/a.h") == "one", "an edit is not seen before its batch");
  check(table.knows_any(std::vector{s(t / "inc" / "a.h")}), "a dirty entry is known");
  check(table.apply(std::vector{s(t / "inc" / "a.h")}) == 1 && table.read("inc/a.h") == "two", "a batch applies the edit");
  check(table.apply(std::vector{s(t / "inc" / "a.h")}) == 0, "an unchanged file changes nothing");

  // A same-size rewrite is re-hashed whatever its stat says.
  write(t / "inc" / "a.h", "six");
  check(table.apply(std::vector{s(t / "inc" / "a.h")}) == 1 && table.read("inc/a.h") == "six", "a same-size rewrite");

  // A created file, by its own event or by its directory's.
  write(t / "inc" / "missing.h", "here");
  check(table.apply(std::vector{s(t / "inc")}) == 1 && table.read("inc/missing.h") == "here", "a directory event reaches its files");
  // A directory that appears: the watch moves down, and its files are read.
  write(t / "gen" / "sub" / "b.h", "gen");
  check(table.apply(std::vector{s(t / "gen")}) == 1 && table.read("gen/sub/b.h") == "gen", "a new directory's files");
  check(std::ranges::contains(watched, s(t / "gen" / "sub")), "the watch moved down to the new directory");

  // A deleted file, and the rescan that needs no events.
  fs::remove(t / "inc" / "a.h");
  write(t / "gen" / "sub" / "b.h", "regenerated");
  check(table.rescan() == 2 && !table.read("inc/a.h") && table.read("gen/sub/b.h") == "regenerated", "a rescan");

  // A symbolic link: its target's edits and its own replacement are both seen.
  write(t / "real" / "c.h", "target");
  fs::create_symlink(t / "real" / "c.h", t / "inc" / "c.h");
  check(table.read("inc/c.h") == "target", "read through a link");
  write(t / "real" / "c.h", "edited");
  check(table.apply(std::vector{s(t / "real" / "c.h")}) == 1 && table.read("inc/c.h") == "edited", "the target's edit");
  fs::path link_dir = t / "inc";
  fs::remove(link_dir / "c.h");
  write(link_dir / "c.h", "replaced");
  check(table.apply(std::vector{s(link_dir / "c.h")}) == 1 && table.read("inc/c.h") == "replaced", "the link replaced by a file");

  // A symbolic link to a directory on the way (`-Iinclude/current`), retargeted: the change
  // arrives as the link's own name.
  write(t / "v1" / "d.h", "v1");
  write(t / "v2" / "d.h", "v2");
  fs::create_directory_symlink(t / "v1", t / "current");
  check(table.read("current/d.h") == "v1", "read through a directory link");
  check(std::ranges::contains(watched, s(t)), "the link's directory is watched");
  fs::remove(t / "current");
  fs::create_directory_symlink(t / "v2", t / "current");
  check(table.apply(std::vector{s(t / "current")}) == 1 && table.read("current/d.h") == "v2", "a retargeted directory link");

  // A directory that cannot be watched: its entries alone are stat-checked.
  write(t / "v2" / "d.h", "v2 edited");
  check(table.rescan(std::vector{s(t / "inc")}) == 0, "a rescan of another directory leaves it");
  check(table.rescan(std::vector{s(t / "v2")}) == 1 && table.read("current/d.h") == "v2 edited", "a rescan of its directory");
  fs::remove_all(t);
}

// --- the dirty-path queue ---------------------------------------------------------------------

void queue() {
  Events ev("/nowhere");
  ev.push("/a/x.h");
  ev.push("/a/x.h");
  auto b = ev.drain();
  check(b.paths == std::vector<std::string>{"/a/x.h"} && !b.overflow, "a path is queued once");
  for (int i = 0; i <= 100'000; ++i) ev.push(std::format("/a/{}", i));
  b = ev.drain();
  check(b.overflow && b.paths.empty(), "past the cap, the queue becomes a rescan");
  check(!ev.drain().overflow, "and drains back to empty");
}

// --- the watcher and cookie sync (§5.4, §5.5) ---------------------------------------------------

bool drained_has(Events& ev, const std::string& path) {
  auto b = ev.drain();
  return std::ranges::contains(b.paths, path);
}

void watcher() {
  using namespace std::chrono_literals;
  fs::path t = temp_dir("watch");
  fs::create_directories(t / "root");
  fs::create_directories(t / "outside");
  Events ev(s(t / "root"));
  Watcher w(ev);
  check(!w.start({s(t / "root")}, s(t / "root")), "the watcher starts");
  FileTable table([&](const std::string& d) { w.add(d); });

  // A change made before sync() is queued when sync() returns.
  write(t / "root" / "a.h", "1");
  check(table.read(s(t / "root" / "a.h")) == "1", "read");
  write(t / "root" / "a.h", "2");
  check(ev.sync(5s), "cookie sync");
  auto b = ev.drain();
  check(std::ranges::contains(b.paths, s(t / "root" / "a.h")), "the edit is in the batch");
  check(table.apply(b.paths) == 1 && table.read(s(t / "root" / "a.h")) == "2", "and applies");

  // Outputs reach the queue and are dropped by the table.
  write(t / "root" / "a.o", "obj");
  check(ev.sync(5s), "cookie sync");
  check(table.apply(ev.drain().paths) == 0, "a build output changes nothing");

  // A directory outside the watched ones, edited between the read and the next epoch.
  write(t / "outside" / "b.h", "1");
  check(table.read(s(t / "outside" / "b.h")) == "1", "read outside");
  write(t / "outside" / "b.h", "2");
  // Long enough for the event to be journaled before the stream is recreated, so a stream that
  // only listens from now on would miss it.
  std::this_thread::sleep_for(1s);
  w.refresh();
  check(ev.sync(5s), "cookie sync");
  // FSEvents replays from the id taken before the read; inotify watched it before the read.
  bool seen = false;
  for (int i = 0; i < 50 && !seen; ++i) {
    seen = drained_has(ev, s(t / "outside" / "b.h"));
    if (!seen) {
      std::this_thread::sleep_for(20ms);
      ev.sync(5s);
    }
  }
  check(seen, "an edit outside, between the read and the epoch, is not lost");

  // A watched directory replaced by a rename (a generator swapping its output): later edits in the
  // new directory are still seen.
  write(t / "root" / "gen" / "x.h", "1");
  check(table.read(s(t / "root" / "gen" / "x.h")) == "1", "read in gen");
  fs::rename(t / "root" / "gen", t / "root" / "gen.old");
  write(t / "root" / "gen" / "x.h", "2");
  check(ev.sync(5s), "cookie sync");
  check(table.apply(ev.drain().paths) == 1 && table.read(s(t / "root" / "gen" / "x.h")) == "2", "the swapped directory");
  write(t / "root" / "gen" / "x.h", "3");
  check(ev.sync(5s), "cookie sync");
  check(table.apply(ev.drain().paths) == 1 && table.read(s(t / "root" / "gen" / "x.h")) == "3", "an edit in it afterwards");

  // A directory above the root is too wide for a recursive stream: stat-checked instead.
  w.add("/");
  check(os_name != "macos" || std::ranges::contains(w.unwatched(), std::string("/")), "/ is not streamed");

  // Several requests share cookies; each returns.
  std::vector<std::thread> ts;
  std::atomic<int> ok = 0;
  for (int i = 0; i < 8; ++i) ts.emplace_back([&] { ok += ev.sync(5s) ? 1 : 0; });
  for (auto& th : ts) th.join();
  check(ok == 8, "concurrent syncs all return");
  fs::remove_all(t);
}

}  // namespace

// What an LSP session checks a document with (§6.4): the unit of a served request, alone, with
// its preprocessing options and standard, and its other actions and inputs dropped.
void check_requests() {
  const compile::Request r{{"-Iinc", "-I", "more", "-DX=1", "-std=c++26", "-ftemplate-depth=64", "--target",
                            "arm64-apple-darwin", "-fclass-call-abi=apple", "-o", "prog", "a.cpp", "../b/b.cpp", "lib.o"},
                           "/w/src", {}, 5};
  check(compile::sources_of(r) == std::vector<std::string>{"/w/src/a.cpp", "/w/b/b.cpp"}, "a request's sources, absolute");
  auto one = compile::check_one(r, "/w/b/b.cpp");
  check(one && one->argv == std::vector<std::string>{"-Iinc", "-I", "more", "-DX=1", "-std=c++26",
                                                    "-ftemplate-depth=64", "--target", "arm64-apple-darwin",
                                                    "-fclass-call-abi=apple", "-fsyntax-only", "../b/b.cpp"} &&
            one->cwd == "/w/src" && one->build_time == 5,
        "one unit checked with the request's options, spelled as the request spells it");
  check(!compile::check_one(r, "/w/src/c.cpp"), "a source the request does not compile");
  check(compile::sources_of({{"-c", "-Q"}, "/w", {}, 0}).empty(), "a malformed command line has no sources");
}

int main() {
  check_requests();
  framing();
  roots();
  admission();
  file_table();
  queue();
  watcher();
  return unit::result("unit.daemon");
}
