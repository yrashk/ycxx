// One resident workspace request. External tools run between query roots, and their successful
// results reach the graph as inputs at quiescence (#1255).
module;
#include <stdlib.h>

export module ycxx.build.project;

import std;
import ycxx.ast;
import ycxx.build.action;
import ycxx.build.plan;
import ycxx.engine;
import ycxx.hash;
import ycxx.pipeline;
import ycxx.pp;

export namespace ycxx::build {

struct ProjectReport {
  int status = 0;
  std::string output;
  std::vector<std::string> diagnostics;
  std::size_t actions_executed = 0;
};

class Coordinator {
 public:
  explicit Coordinator(pipeline::Session& session);

  ProjectReport run(const std::filesystem::path& cwd, const std::string& target,
                    const std::function<void()>& refresh, const std::function<bool()>& cancelled,
                    const std::function<void(const std::function<void(engine::Db&)>&)>& apply,
                    const std::function<bool(engine::Revision, const std::function<void()>&)>& publish,
                    engine::Tracer* tracer = nullptr);

 private:
  pipeline::Session& session_;
  std::mutex mutex_;
  std::map<std::string, std::set<std::string>> generated_paths_;
};

}  // namespace ycxx::build

namespace ycxx::build {
namespace {

namespace fs = std::filesystem;
using Apply = std::function<void(const std::function<void(engine::Db&)>&)>;

struct ProjectKey {
  std::string cwd;
  std::string target;
  bool operator==(const ProjectKey&) const = default;
};
struct [[=engine::input{engine::Durability::low}]] WorkspacePlan {
  using Key = ProjectKey;
  using Value = ProjectPlan;
  static void describe(std::string& out, const Key& key) { out += key.target; }
};
struct [[=engine::input{engine::Durability::low}]] ActionReceipt {
  using Key = Fp128;
  using Value = std::optional<std::vector<NamedArtifact>>;
};
struct Prepared {
  std::string target;
  Fp128 plan;
  pipeline::RequestId request;
};
struct [[=engine::input{engine::Durability::low}]] PreparedBuild {
  using Key = std::string;
  using Value = std::optional<Prepared>;
};

Fp128 action_key(const GeneratorPlan& generator) {
  return fingerprint(std::tie(generator.action, generator.inputs, generator.tool_fingerprint));
}

struct Demand {
  std::vector<std::size_t> pending;
  bool prepare = false;
  std::vector<std::uint8_t> image;
  std::vector<std::string> errors;
};

struct WorkspaceBuild {
  using Key = ProjectKey;
  using Value = Demand;
  static void describe(std::string& out, const Key& key) { out += key.target; }

  static engine::query_task<Value> run(engine::Ctx& cx, const Key& key) {
    const auto& plan = cx.fetch<WorkspacePlan>(key);
    Demand demand;
    for (std::size_t i = 0; i < plan.generators.size(); ++i)
      if (!cx.fetch<ActionReceipt>(action_key(plan.generators[i]))) demand.pending.push_back(i);
    if (!demand.pending.empty()) co_return demand;
    const auto& prepared = cx.fetch<PreparedBuild>(key.cwd);
    if (!prepared || prepared->target != key.target || prepared->plan != fingerprint(plan)) {
      demand.prepare = true;
      co_return demand;
    }
    const auto& native = co_await cx.fetch<pipeline::Build>(prepared->request);
    if (native.linked) {
      demand.image = native.linked->image;
      demand.errors = native.linked->errors;
    }
    if (demand.image.empty()) {
      std::string errors = pipeline::Build::errors(native);
      if (!errors.empty() && demand.errors.empty()) demand.errors.push_back(std::move(errors));
    }
    co_return demand;
  }
};

std::string native(const fs::path& path) { return path.generic_native_encoded_string(); }

pipeline::RequestId register_native(pipeline::Session& session, const ProjectKey& key, const ProjectPlan& plan) {
  pipeline::BuildKey build;
  for (const auto& source : plan.sources) {
    pp::Config config;
    config.view = pp::View::build;
#ifdef __APPLE__
    config.target = &ast::aarch64_darwin;
#else
    config.target = &ast::x86_64_linux;
    config.freestanding = true;
#endif
    config.angle_dirs = source.includes;
    config.system_from = config.angle_dirs.size();
    const std::string object = source.path + ".o";
    build.units.push_back(session.unit({source.path, key.cwd, std::move(config), object}));
  }
  build.program = session.program({.tus = build.units, .objects = {}, .identifier = key.target});
  return session.request(std::move(build));
}

struct Scratch {
  fs::path path;
  ~Scratch() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

std::optional<std::string> install(const fs::path& output, std::span<const std::uint8_t> image) {
  std::error_code ec;
  fs::create_directories(output.parent_path(), ec);
  if (ec) return std::format("cannot create build output directory: {}", ec.message());
  auto path = native(output.parent_path() / ".publish-XXXXXX");
  if (!::mkdtemp(path.data())) return "cannot create temporary build output directory";
  const Scratch scratch{fs::path(path)};
  const fs::path temporary = scratch.path / "image";
  std::ofstream file(temporary, std::ios::binary | std::ios::noreplace);
  file.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
  file.close();
  if (!file) return "cannot write executable";
  using enum fs::perms;
  fs::permissions(temporary, owner_read | owner_write | owner_exec | group_read | group_exec | others_read | others_exec,
                  fs::perm_options::replace, ec);
  if (ec) return std::format("cannot set executable permissions: {}", ec.message());
  fs::rename(temporary, output, ec);
  if (ec) return std::format("cannot publish executable: {}", ec.message());
  return std::nullopt;
}

}  // namespace

Coordinator::Coordinator(pipeline::Session& session) : session_(session) {
  using enum engine::Durability;
  session_.db().set_provider<ActionReceipt>([](const Fp128&) -> engine::provided<ActionReceipt::Value> {
    return {std::nullopt, low};
  });
  session_.db().set_provider<PreparedBuild>([](const std::string&) -> engine::provided<PreparedBuild::Value> {
    return {std::nullopt, low};
  });
}

ProjectReport Coordinator::run(const fs::path& cwd, const std::string& target,
                               const std::function<void()>& refresh, const std::function<bool()>& cancelled,
                               const Apply& apply,
                               const std::function<bool(engine::Revision, const std::function<void()>&)>& publish,
                               engine::Tracer* tracer) {
  std::lock_guard lock(mutex_);
  ProjectReport report;
  const ProjectKey key{native(cwd), target};
  const auto fail = [&](std::string diagnostic) {
    report.status = 1;
    report.diagnostics.push_back(std::move(diagnostic));
    return report;
  };

  while (!cancelled()) {
    refresh();
    auto plan = plan_project(cwd, target);
    if (!plan) return fail(plan.error());
    const Fp128 plan_id = fingerprint(*plan);
    if (const auto* prior = session_.db().peek<WorkspacePlan>(key); !prior || fingerprint(*prior) != plan_id)
      apply([&](engine::Db& db) { db.set<WorkspacePlan>(key, *plan); });

    for (;;) {
      const auto before = session_.db().revision();
      auto demand = session_.run<WorkspaceBuild>(key, cancelled, nullptr, tracer);
      if (!demand) return fail("build cancelled");

      if (!demand->pending.empty()) {
        const auto scratch = cwd / ".ycxx" / "jobs";
        std::error_code ec;
        fs::create_directories(scratch, ec);
        if (ec) return fail(std::format("cannot create action scratch directory: {}", ec.message()));
        for (const auto index : demand->pending) {
          if (cancelled()) return fail("build cancelled");
          const auto& generator = plan->generators[index];
          const auto id = action_key(generator);
          // Two targets in a plan can describe the same execution.
          if (const auto* receipt = session_.db().peek<ActionReceipt>(id); receipt && *receipt) continue;
          std::inplace_stop_source stop;
          std::jthread monitor([&](std::stop_token finished) {
            while (!finished.stop_requested()) {
              if (cancelled()) {
                stop.request_stop();
                return;
              }
              std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
          });
          ++report.actions_executed;
          const auto started = std::chrono::steady_clock::now();
          const auto trace_id = tracer ? session_.db().reserve_trace_id() : 0;
          auto result = execute(generator.action, generator.inputs, scratch, stop.get_token());
          monitor.request_stop();
          monitor.join();
          if (tracer) {
            tracer->event({.id = trace_id, .kind = "ExternalAction", .key = generator.name,
                           .outcome = result.exit_status == 0 ? engine::Outcome::executed : engine::Outcome::failed,
                           .start = started, .end = std::chrono::steady_clock::now(), .value = {},
                           .errors = result.exit_status == 0 ? std::string{} : result.err, .span = {}});
          }
          if (result.exit_status != 0) {
            report.status = result.exit_status;
            report.diagnostics.push_back(std::format("generator '{}': {}", generator.name,
                result.err.empty() ? std::format("exit status {}", result.exit_status) : result.err));
            return report;
          }
          if (!result.err.empty()) report.diagnostics.push_back(std::move(result.err));
          apply([&](engine::Db& db) { db.set<ActionReceipt>(id, std::move(result.outputs)); });
        }
        break;  // Reobserve disk inputs before admitting a native build.
      }

      if (demand->prepare) {
        const auto request = register_native(session_, key, *plan);
        std::map<std::string, std::string> generated;
        for (const auto& generator : plan->generators) {
          const auto& receipt = *session_.db().peek<ActionReceipt>(action_key(generator));
          for (std::size_t i = 0; i < generator.action.outputs.size(); ++i) {
            const auto& output = generator.action.outputs[i];
            const auto& bytes = (*receipt)[i].contents.bytes;
            generated.emplace(generated_path(cwd, generator.name, output.filename),
                              std::string(bytes.begin(), bytes.end()));
          }
        }
        auto& previous = generated_paths_[key.cwd];
        apply([&](engine::Db& db) {
          for (const auto& path : previous)
            if (!generated.contains(path)) session_.set_generated(path, std::nullopt);
          for (const auto& [path, bytes] : generated) session_.set_generated(path, bytes);
          db.set<PreparedBuild>(key.cwd, Prepared{key.target, plan_id, request});
        });
        previous.clear();
        for (const auto& [path, bytes] : generated) previous.insert(path);
        continue;
      }

      if (cancelled()) return fail("build cancelled");
      refresh();
      const auto observed = plan_project(cwd, target);
      if (!observed || fingerprint(*observed) != plan_id || before != session_.db().revision()) break;
      if (demand->image.empty()) {
        report.status = 1;
        report.diagnostics.append_range(std::move(demand->errors));
        if (report.diagnostics.empty()) report.diagnostics.push_back("build produced no executable");
        return report;
      }
      const auto output = cwd / ".ycxx" / "out" / target;
      std::optional<std::string> error;
      if (!publish(before, [&] { error = install(output, demand->image); })) break;
      if (error) return fail(std::move(*error));
      report.output = native(output);
      return report;
    }
  }
  return fail("build cancelled");
}

}  // namespace ycxx::build
