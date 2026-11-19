// The daemon's file table (daemon design §5.2, §5.3): what the compiler reads, by absolute path
// (lexically normalized, symlinks kept), each entry filled on first read and changed only when a
// batch applies between epochs. A batch is a set of dirty real paths from the watcher; a path no
// entry knows is dropped, which is how the build's own outputs fall out.
//
// The table backs the engine's `pp::FileContent` inputs through a provider (#95, #93): a cell is
// provided from its entry on first read, and an entry a batch changes is reported (`take_changed`)
// so the daemon sets the cells that were provided. It is also the `pp::Host` of `-E`.
export module ycxx.daemon:files;

import std;
import ycxx.hash;
import ycxx.pp;
import :posix;

export namespace ycxx::daemon {

class FileTable final : public pp::Host {
 public:
  // `watch(dir)` is called with a real directory before any file in it is read, so a change
  // after the read is always seen (§5.5).
  explicit FileTable(std::function<void(const std::string&)> watch) : watch_(std::move(watch)) {}

  // The directory relative paths are relative to, for reads on this thread: the request's that
  // runs on it. Requests run at once, each on its own thread (#139); the engine's reads use
  // absolute paths.
  void set_cwd(std::string dir) { cwd() = std::move(dir); }

  // Safe to call from any thread (a provider may run on any worker).
  std::optional<std::string> read(const std::string& path) override {
    std::lock_guard g(mu_);
    std::string key = absolute(path);
    auto it = entries_.find(key);
    if (it == entries_.end()) it = fill(std::move(key));
    if (!it->second.present) return std::nullopt;
    return it->second.text;
  }

  // Applies a batch (§5.3): every entry a dirty path reaches, by its real path or by a directory
  // above it, is re-stat'ed and re-hashed. The number of entries that changed.
  std::size_t apply(std::span<const std::string> dirty) {
    std::lock_guard g(mu_);
    std::set<std::string> touched;
    for (const std::string& d : dirty) reached(d, touched);
    std::size_t changed = 0;
    for (const std::string& key : touched) changed += refresh(key, true);
    return changed;
  }

  // Whether any dirty path reaches an entry: a batch that would change nothing is dropped.
  bool knows_any(std::span<const std::string> dirty) const {
    std::lock_guard g(mu_);
    std::set<std::string> touched;
    for (const std::string& d : dirty) {
      reached(d, touched);
      if (!touched.empty()) return true;
    }
    return false;
  }

  // The full rescan (§5.4): stat every entry, re-hash those whose stat identity changed. With
  // `under`, only the entries that watch one of those directories (the unwatched ones, §5.5).
  std::size_t rescan(std::optional<std::span<const std::string>> under = std::nullopt) {
    std::lock_guard g(mu_);
    std::size_t changed = 0;
    std::vector<std::string> keys;
    for (const auto& [key, e] : entries_) {
      if (under && !std::ranges::any_of(*under, [&](const std::string& d) { return std::ranges::contains(e.dirs, d); }))
        continue;
      keys.push_back(key);
    }
    for (const std::string& key : keys) changed += refresh(key, false);
    return changed;
  }

  std::size_t size() const {
    std::lock_guard g(mu_);
    return entries_.size();
  }

  // The files builds have read that exist (`--watch` lists them, #245), in key order.
  std::vector<std::string> present_keys() const {
    std::lock_guard g(mu_);
    std::vector<std::string> out;
    for (const auto& [key, e] : entries_) {
      if (e.present) out.push_back(key);
    }
    return out;
  }

  // The keys of the entries changed by the batches and rescans since the last call.
  std::vector<std::string> take_changed() {
    std::lock_guard g(mu_);
    return std::exchange(changed_, {});
  }

  // The absolute key of a path of the current request.
  std::string absolute(const std::string& path) const {
    std::filesystem::path p(path);
    if (p.is_relative() && !cwd().empty()) p = std::filesystem::path(cwd()) / p;
    return p.lexically_normal().generic_native_encoded_string();
  }

 private:
  static std::string& cwd() {
    static thread_local std::string c;
    return c;
  }

  struct Entry {
    bool present = false;
    std::optional<Identity> id;
    Fp128 fp;
    std::string text;
    std::string real;  // the real path, or where it would be (§5.5)
    std::vector<std::string> watched;  // the real names that reach it, in the reverse index
    std::vector<std::string> dirs;     // the real directories watched for it
  };

  // The entries a dirty real path reaches: exactly, or as a directory above them.
  void reached(const std::string& d, std::set<std::string>& out) const {
    if (auto it = by_real_.find(d); it != by_real_.end()) out.insert(it->second.begin(), it->second.end());
    std::string dir = d == "/" ? d : d + "/";
    for (auto it = by_real_.lower_bound(dir); it != by_real_.end() && it->first.starts_with(dir); ++it)
      out.insert(it->second.begin(), it->second.end());
  }

  void watch(Entry& e, std::string dir) {
    watch_(dir);
    if (!std::ranges::contains(e.dirs, dir)) e.dirs.push_back(std::move(dir));
  }

  // Where `key` really is: its real path if it exists, else the real path of its nearest existing
  // ancestor with the rest appended. Watches that ancestor (§5.5: the watch moves down when the
  // directory appears).
  std::string locate(const std::string& key, Entry& e) {
    namespace fs = std::filesystem;
    fs::path p(key);
    fs::path rest;
    for (;;) {
      fs::path parent = p.parent_path();
      if (auto real = real_path(parent.native_encoded_string())) {
        watch(e, *real);
        fs::path r = fs::path(*real) / p.filename();
        if (!rest.empty()) r /= rest;
        // A symbolic link to a file is watched where its target is, too.
        if (auto target = real_path(key); target && rest.empty()) {
          watch(e, fs::path(*target).parent_path().native_encoded_string());
          return *target;
        }
        return r.lexically_normal().native_encoded_string();
      }
      if (parent == p) return key;
      rest = p.filename() / rest;
      p = parent;
    }
  }

  // The names a change to `key` can arrive under besides its real path: each symbolic link on the
  // way to it, by its own name in its real directory, which is watched too. Retargeting a link
  // (`ln -sfn v2 include/current`, or replacing a link to a file) is then seen (§5.5).
  std::vector<std::string> links_to(const std::string& key, Entry& e) {
    namespace fs = std::filesystem;
    std::vector<std::string> names;
    fs::path prefix;
    for (const fs::path& c : fs::path(key)) {
      prefix /= c;
      if (prefix == prefix.root_path()) continue;
      std::error_code ec;
      auto st = fs::symlink_status(prefix, ec);
      if (ec || !fs::exists(st)) break;
      if (!fs::is_symlink(st)) continue;
      if (auto dir = real_path(prefix.parent_path().native_encoded_string())) {
        watch(e, *dir);
        names.push_back((fs::path(*dir) / prefix.filename()).native_encoded_string());
      }
    }
    return names;
  }

  void index(const std::string& key, Entry& e) {
    std::vector<std::string> names{e.real};
    for (std::string& n : links_to(key, e))
      if (!std::ranges::contains(names, n)) names.push_back(std::move(n));
    if (names == e.watched) return;
    unindex(key, e);
    for (const std::string& n : names) by_real_[n].insert(key);
    e.watched = std::move(names);
  }
  void unindex(const std::string& key, Entry& e) {
    for (const std::string& n : e.watched) {
      auto it = by_real_.find(n);
      if (it == by_real_.end()) continue;
      it->second.erase(key);
      if (it->second.empty()) by_real_.erase(it);
    }
    e.watched.clear();
  }

  // Reads the file as it is now into `e`.
  void load(const std::string& key, Entry& e) {
    e.dirs.clear();
    e.real = locate(key, e);
    e.id = stat_path(key);
    e.present = false;
    e.text.clear();
    if (e.id && e.id->regular) {
      std::ifstream in(key, std::ios::binary);
      if (in) {
        e.text.assign(std::istreambuf_iterator<char>(in), {});
        e.present = true;
      }
    }
    e.fp = e.present ? fingerprint128(e.text) : Fp128{};
  }

  std::map<std::string, Entry>::iterator fill(std::string key) {
    auto it = entries_.emplace(std::move(key), Entry{}).first;
    load(it->first, it->second);
    index(it->first, it->second);
    return it;
  }

  // Re-reads an entry; 1 if its content or existence changed. `always` re-hashes even an equal
  // stat identity (a same-size rewrite within one timestamp tick, §5.3).
  std::size_t refresh(const std::string& key, bool always) {
    Entry& e = entries_.at(key);
    if (!always) {
      auto id = stat_path(key);
      if (id == e.id) return 0;
    }
    bool was = e.present;
    Fp128 fp = e.fp;
    load(key, e);
    index(key, e);
    if (was == e.present && fp == e.fp) return 0;
    changed_.push_back(key);
    return 1;
  }

  mutable std::mutex mu_;
  std::function<void(const std::string&)> watch_;
  std::vector<std::string> changed_;
  std::map<std::string, Entry> entries_;
  std::map<std::string, std::set<std::string>> by_real_;  // real name -> keys (§5.5)
};

}  // namespace ycxx::daemon
