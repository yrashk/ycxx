// The standalone native archive front (#389).
module;
#include <errno.h>
#include <sys/stat.h>
export module ycxx.driver.archive;

import std;
import ycxx.obj.archive;

namespace ycxx::driver {
namespace {

std::expected<std::string, std::string> read_input(std::string_view name) {
  const auto cannot_read = [name] { return std::unexpected(std::format("cannot read '{}'", name)); };
  const auto too_large = [name] {
    return std::unexpected(std::format("{}: archive member size exceeds 10 decimal digits", name));
  };
  const std::filesystem::path path(name);
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) return cannot_read();
  if (size > obj::archive::max_member_size) return too_large();

  auto close = [](std::FILE* stream) { std::fclose(stream); };
  const std::unique_ptr<std::FILE, decltype(close)> file(std::fopen(path.c_str(), "rb"), close);
  if (!file) return cannot_read();
  std::array<char, 64 * 1024> buffer;
  std::string bytes;
  while (const auto count = std::fread(buffer.data(), 1, buffer.size(), file.get())) {
    if (count > obj::archive::max_member_size - bytes.size()) return too_large();
    bytes.append(buffer.data(), count);
  }
  if (std::ferror(file.get())) return cannot_read();
  return bytes;
}

std::optional<std::filesystem::path> private_staging_directory(const std::filesystem::path& parent) try {
  std::random_device random;
  for (int attempt = 0; attempt < 16; ++attempt) {
    auto directory = parent / std::format(".ycxx-archive-{:08x}{:08x}", random(), random());
    if (::mkdir(directory.c_str(), 0700) == 0) return directory;
    if (errno != EEXIST) break;
  }
  return std::nullopt;
} catch (const std::system_error&) {
  return std::nullopt;
}

bool replace_output(const std::filesystem::path& path, std::string_view bytes) {
  using std::filesystem::file_type;
  std::error_code ec;
  const auto type = std::filesystem::symlink_status(path, ec).type();
  if (type != file_type::regular && type != file_type::symlink && type != file_type::not_found) return false;
  if (!std::in_range<std::streamsize>(bytes.size())) return false;

  // Stage the new file beside its destination. A private directory lets the file use the
  // usual creation permissions while keeping simultaneous writers' temporary names distinct.
  const auto directory = private_staging_directory(path.parent_path());
  if (!directory) return false;
  const auto peer = *directory / "archive";
  std::ofstream file(peer, std::ios::binary);
  if (file) {
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
  }
  if (file) std::filesystem::rename(peer, path, ec);
  const bool replaced = file && !ec;
  if (!replaced) std::filesystem::remove(peer, ec);
  std::filesystem::remove(*directory, ec);
  return replaced;
}

void archive_usage(std::ostream& out) {
  std::println(out, "usage: ycxx archive -o libfoo.a [--] [object ...]");
}

}  // namespace

export int run_archive(std::span<const std::string_view> args) {
  if (args.size() == 1 && args.front() == "--help") {
    archive_usage(std::cout);
    return 0;
  }
  if (args.size() < 2 || args[0] != "-o" || args[1].empty()) {
    archive_usage(std::cerr);
    return 2;
  }
  const auto output = args[1];
  args = args.subspan(2);
  if (!args.empty() && args.front() == "--") {
    args = args.subspan(1);
  } else {
    const auto option = std::ranges::find_if(args, [](std::string_view arg) { return arg.starts_with('-'); });
    if (option != args.end()) {
      std::println(std::cerr, "ycxx archive: unknown option '{}'", *option);
      return 2;
    }
  }

  std::vector<std::string> objects;
  std::vector<obj::archive::member_input> members;
  objects.reserve(args.size());
  members.reserve(args.size());
  const auto fail = [](std::string_view message) {
    std::println(std::cerr, "ycxx archive: error: {}", message);
    return 1;
  };
  for (const auto name : args) {
    auto bytes = read_input(name);
    if (!bytes) return fail(bytes.error());
    members.push_back({name, objects.emplace_back(std::move(*bytes))});
  }
  auto archive = obj::archive::write(members);
  if (!archive) return fail(archive.error());
  if (!replace_output(std::filesystem::path(output), *archive))
    return fail(std::format("cannot write '{}'", output));
  return 0;
}

}  // namespace ycxx::driver
