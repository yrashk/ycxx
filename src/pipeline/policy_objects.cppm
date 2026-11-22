// Checked observation contracts in ordinary ELF and Mach-O objects.
module ycxx.pipeline:policy_objects;

import std;
import ycxx.ast;
import ycxx.obj;
import :back;
import :policies;

namespace ycxx::pipeline {

namespace {

constexpr std::string_view prefix = "__ycxx_policy.";
constexpr std::string_view coverage = "1.checked.";
constexpr std::string_view requirement = "1.need.";
constexpr std::string_view guarantee = "1.have.";
constexpr std::size_t digest_size = 32;

bool text_symbol(const obj::object& object, const obj::symbol& symbol) {
  if (!symbol.section || *symbol.section >= object.sections.size()) return false;
  const auto& section = object.sections[*symbol.section];
  return section.segment == "__TEXT" && section.name == "__text" && symbol.offset < section.size();
}

}  // namespace

std::optional<std::string> embed_observation_contracts(obj::object& object, const TuValue& compiled,
                                                      std::span<const ObservationContract> contracts) {
  if (std::ranges::any_of(object.symbols, [](const auto& s) { return s.name_view().starts_with(prefix); }))
    return "a symbol uses the reserved ownership contract prefix";
  if (object.sections.empty()) object.add_section(obj::standard_section::text);

  std::map<ast::EntityId, const aarch64::Symbol*> names;
  std::map<std::string_view, std::pair<std::uint32_t, std::uint64_t>> functions;
  if (!contracts.empty()) {
    for (const auto& symbol : compiled.symbols) names.emplace(symbol.entity, &symbol);
    for (const auto& symbol : object.symbols)
      if (text_symbol(object, symbol))
        functions.emplace(symbol.name_view(), std::pair{*symbol.section, symbol.offset});
  }

  // Gather the records first. Appending symbols can invalidate views into existing names.
  std::vector<obj::symbol> metadata;
  for (const auto& contract : contracts) {
    const auto origin = names.find(contract.function);
    const auto observer = names.find(contract.observer);
    // The backend may eliminate the only use of an observer before it names symbols.
    if (observer == names.end() && !contract.provides) continue;
    if (origin == names.end() || observer == names.end()) return "cannot name a verified observation contract";
    const auto anchor = functions.find(origin->second->name);
    if (anchor == functions.end()) return "cannot locate the code for a verified observation contract";
    if (observer->second->bind == obj::binding::local) {
      if (functions.contains(observer->second->name)) continue;
      return "a local verified observer must be defined in its object";
    }
    metadata.push_back({std::format("{}{}{:016x}{:016x}.{}", prefix, contract.provides ? guarantee : requirement,
                                    contract.signature.hi, contract.signature.lo, observer->second->name),
                        obj::binding::local, anchor->second.first, anchor->second.second});
  }
  metadata.push_back({std::format("{}{}{}", prefix, coverage, metadata.size()), obj::binding::local, 0, 0});
  object.symbols.append_range(std::move(metadata));
  return std::nullopt;
}

std::vector<std::string> check_object_observations(std::span<const obj::object> objects) {
  using Location = std::pair<std::uint32_t, std::uint64_t>;
  struct Record {
    std::size_t object;
    std::string_view name;
    std::string_view digest;
    Location at;
  };
  std::vector<Record> requirements;
  std::map<std::pair<std::size_t, std::string_view>, Record> guarantees;
  std::map<std::string_view, std::vector<std::pair<std::size_t, const obj::symbol*>>> definitions;
  std::vector<std::string> errors;
  for (std::size_t i = 0; i < objects.size(); ++i) {
    const auto& object = objects[i];
    std::optional<std::size_t> expected_records;
    std::size_t records = 0;
    std::set<Location> functions;
    for (const auto& symbol : object.symbols) {
      if (symbol.name_view().starts_with(prefix) || !text_symbol(object, symbol)) continue;
      functions.emplace(*symbol.section, symbol.offset);
      if (symbol.bind != obj::binding::local) definitions[symbol.name_view()].emplace_back(i, &symbol);
    }
    for (const auto& symbol : object.symbols) {
      auto name = symbol.name_view();
      if (!name.starts_with(prefix)) continue;
      name.remove_prefix(prefix.size());
      auto malformed = [&] { errors.push_back(std::format("{}: invalid ownership contract metadata", object.name)); };
      if (symbol.bind != obj::binding::local || !symbol.defined() || symbol.weak ||
          *symbol.section >= object.sections.size()) {
        malformed();
        continue;
      }
      if (name.starts_with(coverage)) {
        name.remove_prefix(coverage.size());
        std::size_t count = 0;
        const auto parsed = std::from_chars(name.data(), name.data() + name.size(), count);
        if (expected_records || symbol.offset != 0 || parsed.ec != std::errc{} ||
            parsed.ptr != name.data() + name.size()) malformed();
        else expected_records = count;
        continue;
      }
      const bool provides = name.starts_with(guarantee);
      const auto kind = provides ? guarantee : requirement;
      if (!name.starts_with(kind) || !functions.contains({*symbol.section, symbol.offset})) {
        malformed();
        continue;
      }
      name.remove_prefix(kind.size());
      const auto digest = name.substr(0, digest_size);
      if (name.size() <= digest_size + 1 || name[digest_size] != '.' ||
          digest.find_first_not_of("0123456789abcdef") != std::string_view::npos) {
        malformed();
        continue;
      }
      const Record record{i, name.substr(digest_size + 1), digest, {*symbol.section, symbol.offset}};
      ++records;
      if (provides) {
        if (!guarantees.try_emplace(std::pair{i, record.name}, record).second) malformed();
      } else requirements.push_back(record);
    }
    if (!expected_records) errors.push_back(std::format("{}: missing checked ownership coverage", object.name));
    else if (*expected_records != records)
      errors.push_back(std::format("{}: incomplete checked ownership metadata", object.name));
  }
  for (const auto& call : requirements) {
    const auto it = definitions.find(call.name);
    if (it == definitions.end() || it->second.size() != 1 || it->second.front().second->weak) {
      errors.push_back(std::format("{}: verified observation '{}' requires one strong definition",
                                   objects[call.object].name, call.name));
      continue;
    }
    const auto [owner, definition] = it->second.front();
    const auto proof = guarantees.find({owner, call.name});
    if (proof == guarantees.end() || proof->second.at != Location{*definition->section, definition->offset} ||
        proof->second.digest != call.digest)
      errors.push_back(std::format("{}: verified observation '{}' has no matching guarantee in {}",
                                   objects[call.object].name, call.name, objects[owner].name));
  }
  return errors;
}

}  // namespace ycxx::pipeline
