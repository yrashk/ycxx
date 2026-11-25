#include <stdlib.h>
#include <sys/stat.h>

import std;
import ycxx.build.action;
import ycxx.build.publication;
import ycxx.unit;

namespace {

using namespace ycxx::build;
using namespace ycxx::unit;
namespace fs = std::filesystem;

std::string read_file(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), {}};
}

void restores_executable_when_asset_exchange_fails(const fs::path& workspace) {
  const auto output = workspace / ".ycxx" / "out" / "app";
  const auto assets = workspace / ".ycxx" / "assets" / "app";
  const Artifact old_a{{'a', '1'}}, old_b{{'b', '1'}};
  const std::array previous{RuntimeAssetOutput{"a", old_a}, RuntimeAssetOutput{"b", old_b}};
  constexpr std::array<std::uint8_t, 3> old_image{'o', 'l', 'd'};
  auto initial = PreparedPublication::stage(workspace, "app", previous, old_image);
  check(initial.has_value(), initial ? "stage the initial outputs" : initial.error());
  if (!initial) return;
  auto published = std::move(*initial).commit();
  check(published.value_or(false), published ? "install the initial generation" : published.error());
  if (!published.value_or(false)) return;

  const auto unrelated_asset = assets / "notes.txt";
  const auto unrelated_output = output.parent_path() / "other";
  std::ofstream(unrelated_asset) << "notes";
  std::ofstream(unrelated_output) << "other output";
  struct stat before{};
  const bool has_previous_inode = ::stat(output.c_str(), &before) == 0;
  check(has_previous_inode, "stat the previous executable");
  if (!has_previous_inode) return;

  const Artifact new_a{{'a', '2'}}, new_c{{'c', '2'}};
  const std::array next{RuntimeAssetOutput{"a", new_a}, RuntimeAssetOutput{"c", new_c}};
  constexpr std::array<std::uint8_t, 3> new_image{'n', 'e', 'w'};
  auto pending = PreparedPublication::stage(workspace, "app", next, new_image);
  check(pending.has_value(), pending ? "stage the replacement outputs" : pending.error());
  if (!pending) return;

  // Remove the candidate after staging so the kernel exchange fails after the executable swap.
  std::vector<fs::path> candidates;
  for (const auto& entry : fs::directory_iterator(assets.parent_path())) {
    if (entry.path().filename().native_encoded_string().starts_with(".publish-"))
      candidates.push_back(entry.path());
  }
  check(candidates.size() == 1, "find one staged asset generation");
  if (candidates.size() != 1) return;
  check(read_file(candidates.front() / "a") == "a2" && read_file(candidates.front() / "c") == "c2",
        "candidate contains the complete replacement asset set");
  fs::remove_all(candidates.front());

  const auto failed = std::move(*pending).commit();
  const auto missing = std::make_error_code(std::errc::no_such_file_or_directory);
  const auto expected =
      std::format("cannot publish runtime assets at '{}': {}", assets.display_string(), missing.message());
  check(!failed && failed.error() == expected, failed ? "asset exchange must fail" : failed.error());
  check(read_file(output) == "old", "restore the previous executable bytes");
  struct stat after{};
  check(::stat(output.c_str(), &after) == 0 && before.st_dev == after.st_dev && before.st_ino == after.st_ino,
        "restore the previous executable inode");
  check(read_file(assets / "a") == "a1" && read_file(assets / "b") == "b1" && !fs::exists(assets / "c"),
        "preserve the previous asset set");
  check(read_file(unrelated_asset) == "notes" && read_file(unrelated_output) == "other output",
        "preserve unrelated files on failure");

  auto retry = PreparedPublication::stage(workspace, "app", next, new_image);
  check(retry.has_value(), retry ? "stage the retry" : retry.error());
  if (!retry) return;
  auto committed = std::move(*retry).commit();
  check(committed.value_or(false), committed ? "install the replacement generation" : committed.error());
  check(read_file(output) == "new", "publish the new executable on retry");
  check(read_file(assets / "a") == "a2" && !fs::exists(assets / "b") && read_file(assets / "c") == "c2",
        "publish the complete replacement asset set on retry");
  check(read_file(unrelated_asset) == "notes" && read_file(unrelated_output) == "other output",
        "preserve unrelated files on retry");
}

void rejects_a_nested_asset_directory_change(const fs::path& workspace) {
  const auto output = workspace / ".ycxx" / "out" / "app";
  const auto assets = workspace / ".ycxx" / "assets" / "app";
  const Artifact old_asset{{'1'}}, new_asset{{'2'}};
  const std::array previous{RuntimeAssetOutput{"managed", old_asset}};
  const std::array next{RuntimeAssetOutput{"managed", new_asset}};
  constexpr std::array<std::uint8_t, 1> old_image{'1'}, new_image{'2'};
  auto initial = PreparedPublication::stage(workspace, "app", previous, old_image);
  check(initial.has_value(), initial ? "stage the initial outputs" : initial.error());
  if (!initial) return;
  auto published = std::move(*initial).commit();
  check(published.value_or(false), published ? "install the initial generation" : published.error());
  if (!published.value_or(false)) return;

  const auto local = assets / "local";
  fs::create_directory(local);
  std::ofstream(local / "note") << "original";
  const auto local_time = fs::last_write_time(local);
  auto pending = PreparedPublication::stage(workspace, "app", next, new_image);
  check(pending.has_value(), pending ? "stage the replacement outputs" : pending.error());
  if (!pending) return;

  std::ofstream(local / "new") << "added during staging";
  fs::last_write_time(local, local_time + std::chrono::seconds(2));
  const auto failed = std::move(*pending).commit();
  check(!failed && failed.error().contains("changed during staging"),
        failed ? "nested directory change must prevent publication" : failed.error());
  check(read_file(output) == "1" && read_file(assets / "managed") == "1",
        "preserve the previous executable and asset after the nested change");
  check(read_file(local / "note") == "original" && read_file(local / "new") == "added during staging",
        "preserve both files in the unrelated directory");
}

}  // namespace

int main() {
  auto directory = (fs::temp_directory_path() / "ycxx-publication-test-XXXXXX").native_encoded_string();
  if (!::mkdtemp(directory.data())) {
    check(false, "could not create test workspace");
    return result("build publication");
  }
  const fs::path workspace(directory);
  fs::create_directory(workspace / "exchange");
  restores_executable_when_asset_exchange_fails(workspace / "exchange");
  fs::create_directory(workspace / "nested");
  rejects_a_nested_asset_directory_change(workspace / "nested");
  fs::remove_all(workspace);
  return result("build publication");
}
