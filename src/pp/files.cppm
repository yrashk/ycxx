// Files and include resolution ([cpp.include]). The preprocessor reads the file system through a
// `Host`, and every answer it uses is recorded in the reading region's trace (`include` and
// `content` questions, :region), so a region is reused only while what it read is unchanged.
//
// Seam: the files a header names are discovered while preprocessing, so they cannot be engine
// inputs set beforehand (engine inputs are set before a request). Until the engine has inputs
// read on demand (#93), the host is read directly and the traces carry the dependency.
export module ycxx.pp:files;

import std;
import ycxx.hash;
import ycxx.lex;
import ycxx.ast.target;
import :spell;

export namespace ycxx::pp {

// A file's identity in a session: its normalized path (or a pseudo-file's name such as
// `<built-in>`). The driver may also number the files it sets as `FileText` inputs.
enum class FileId : u32 {};

// Where the preprocessor reads files from.
class Host {
public:
  virtual ~Host() = default;
  // The file's bytes, or nothing if there is no regular file at `path`.
  virtual std::optional<std::string> read(const std::string& path) = 0;
};

// The real file system.
class DiskHost final : public Host {
public:
  std::optional<std::string> read(const std::string& path) override {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::path(path), ec)) return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string{std::istreambuf_iterator<char>(in), {}};
  }
};

// Files by path, with their contents read once per generation (the daemon starts a new generation
// when files change; a one-shot compile has one).
class Files {
public:
  explicit Files(Host& host) : host_(&host) {}
  // Reads through another host from now on; the previous one.
  Host* swap_host(Host& host) { return std::exchange(host_, &host); }

  FileId id(std::string_view path) {
    std::string key = normalize(path);
    if (auto it = ids_.find(key); it != ids_.end()) return it->second;
    FileId f{static_cast<u32>(paths_.size())};
    paths_.push_back(key);
    ids_.emplace(std::move(key), f);
    return f;
  }
  const std::string& path(FileId f) const { return paths_[std::to_underlying(f)]; }

  // A file's text and its fingerprint, or nothing if it does not exist. Pseudo-files (`<...>`)
  // have the text they were given.
  struct Content {
    std::string text;
    Fp128 fp;
  };
  const Content* content(FileId f) {
    auto it = contents_.find(f);
    if (it == contents_.end()) {
      std::optional<Content> c;
      if (auto text = host_->read(path(f))) c = Content{*text, fingerprint128(*text)};
      it = contents_.emplace(f, std::move(c)).first;
    }
    return it->second ? &*it->second : nullptr;
  }
  // Sets the text of a file (a pseudo-file, or a file the driver has in memory).
  void set_content(FileId f, std::string text) {
    Fp128 fp = fingerprint128(text);
    contents_.insert_or_assign(f, Content{std::move(text), fp});
  }
  bool exists(FileId f) { return content(f) != nullptr; }
  // Forgets what was read: the next question reads the host again.
  void new_generation() { contents_.clear(); }

  // The lexically normal form of a path; `<...>` names are kept as they are.
  static std::string normalize(std::string_view path) {
    if (path.starts_with('<')) return std::string(path);
    std::string s = std::filesystem::path(path).lexically_normal().generic_string<char>();
    return s.empty() ? std::string(".") : s;
  }

private:
  Host* host_;
  std::vector<std::string> paths_;
  std::map<std::string, FileId, std::less<>> ids_;
  std::map<FileId, std::optional<Content>> contents_;
};

// The disk, the editor's open buffers over it (daemon design §6.1), or the disk with the
// build's generated files under a reserved virtual directory for its output scope.
enum class View : u8 { disk, editor, build, explicit_target_build };

// The configuration of a translation unit's preprocessing (§B1.1's root): search paths,
// command-line macros and the view. Its fingerprint keys every region a session caches.
struct Config {
  // `#include "..."` searches the includer's directory, then `quote_dirs`, then `angle_dirs`;
  // `#include <...>` searches `angle_dirs` only (-I, then -isystem, then the system
  // directories). Directories from `system_from` on are system directories.
  std::vector<std::string> quote_dirs;
  std::vector<std::string> angle_dirs;
  std::size_t system_from = 0;
  // `-D name[=value]` (value "1" when absent) and `-U name`, in command-line order: a definition
  // has a value, an undefinition none.
  std::vector<std::pair<std::string, std::optional<std::string>>> macros;
  // The target selects predefined macros (:predefined) and system include directories (:system).
  const ast::Target* target = &ast::aarch64_darwin;
  // `-ffreestanding` selects the hostedness predefined macro independently of the target.
  bool freestanding = false;
  // `-fno-exceptions` disables exception handling in semantic analysis and native lowering.
  bool exceptions_enabled = true;
  // `-fno-rtti` disables runtime type operations and the typeinfo word's relocation in vtables.
  bool rtti_enabled = true;
  // The root of every prefix (§6.1): regions are shared only within the same view.
  View view = View::disk;
  // A language option the unit's front end reads: `-ftemplate-depth=N`, the limit on nested
  // template instantiations ([temp.inst]/16, [implimits]).
  std::uint32_t template_depth = default_template_depth;
  static constexpr std::uint32_t default_template_depth = 900;
  // An ABI option the unit's front end reads: `-fclass-call-abi=itanium|apple`, which classes pass
  // in registers (DECISIONS §1.6). None: the target's default.
  std::optional<ast::CallTriviality> call_triviality;
  ast::Compatibility compatibility = ast::Compatibility::spec;
  // The PIC level `-fpic` (1) and `-fPIC` (2) ask for, which `__PIC__` and `__pic__` name; `-fno-pic`
  // is 0. A target that is always PIC ignores it (`Target::always_pic`).
  u8 pic = 0;
  // Setup options consumed by with_system_dirs. Queries use the effective directory lists.
  bool nostdinc = false;
  bool nostdincxx = false;
  ast::CallTriviality calls() const {
    if (call_triviality) return *call_triviality;
    if (compatibility == ast::Compatibility::gcc) return ast::CallTriviality::gcc;
    return target->call_triviality;
  }

  // Whether a file found in directory `found` (an index into quote then angle directories, as
  // `Resolved::dir`) is a system header: found in a system directory, or included by a system
  // header (`includer_system`), wherever found, as with Clang and GCC. Warnings from a system
  // header are not reported (#229).
  bool system_header(u32 found, bool includer_system) const;

  Fp128 fingerprint() const {
    Hasher h;
    h.bytes(target->triple).uint(u8(0)).uint(std::to_underlying(view));
    for (const std::string& d : quote_dirs) h.bytes(d).uint(u8(0));
    h.uint(u8(1));
    for (const std::string& d : angle_dirs) h.bytes(d).uint(u8(0));
    h.uint(system_from);
    for (const auto& [name, value] : macros) {
      h.bytes(name).uint(u8(value ? 1 : 0));
      if (value) h.bytes(*value).uint(u8(0));
    }
    if (template_depth != default_template_depth) h.uint(u8(2)).uint(template_depth);
    if (call_triviality) h.uint(u8(3)).uint(std::to_underlying(*call_triviality));
    if (freestanding) h.uint(u8(4));
    if (!exceptions_enabled) h.uint(u8(5));
    if (!rtti_enabled) h.uint(u8(6));
    if (pic != 0 && !target->always_pic) h.uint(u8(7)).uint(pic);  // always PIC: the options change nothing
    return h.uint(u8(8)).uint(std::to_underlying(compatibility)).finish();
  }
};

// How an include is spelled ([cpp.include]): which form, and `#include_next` (an extension
// libycxx's C wrappers need: the search continues after the directory the includer was found in).
struct IncludeForm {
  bool angled = false;
  bool next = false;
  bool operator==(const IncludeForm&) const = default;
};

// Where an include resolves: the file, and the index in the search order of the directory it was
// found in (`includer_dir`: the includer's own directory; else an index into quote then angle
// directories), which `#include_next` continues after.
inline constexpr u32 includer_dir = 0xffffffffu;
struct Resolved {
  FileId file{};
  u32 dir = includer_dir;
  bool operator==(const Resolved&) const = default;
};

inline bool Config::system_header(u32 found, bool includer_system) const {
  return includer_system || (found != includer_dir && found >= quote_dirs.size() + system_from);
}

// The search order of `form` for a file found in `found_dir` whose directory is `here`.
std::optional<Resolved> resolve(Files& files, const Config& cfg, std::string_view header, IncludeForm form,
                                std::string_view here, u32 found_dir) {
  auto in = [](std::string_view dir, std::string_view h) {
    if (h.starts_with('/')) return std::string(h);
    if (dir.empty() || dir == ".") return std::string(h);
    std::string p(dir);
    if (!p.ends_with('/')) p += '/';
    return p + std::string(h);
  };
  if (header.starts_with('/')) {
    FileId f = files.id(header);
    if (files.exists(f)) return Resolved{f, includer_dir};
    return std::nullopt;
  }
  u32 n_quote = static_cast<u32>(cfg.quote_dirs.size());
  u32 total = n_quote + static_cast<u32>(cfg.angle_dirs.size());
  u32 from = 0;
  if (form.next && found_dir != includer_dir) {
    from = found_dir + 1;
  } else if (!form.angled && !form.next) {
    FileId f = files.id(in(here, header));
    if (files.exists(f)) return Resolved{f, includer_dir};
  }
  if (form.angled && from < n_quote) from = n_quote;
  for (u32 d = from; d < total; ++d) {
    const std::string& dir = d < n_quote ? cfg.quote_dirs[d] : cfg.angle_dirs[d - n_quote];
    FileId f = files.id(in(dir, header));
    if (files.exists(f)) return Resolved{f, d};
  }
  return std::nullopt;
}

// The directory part of a presumed or real path ("" for a bare file name).
std::string directory_of(std::string_view path) {
  auto slash = path.rfind('/');
  return slash == path.npos ? std::string() : std::string(path.substr(0, slash));
}

}  // namespace ycxx::pp
