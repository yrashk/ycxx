// Checked policy contracts in ordinary ELF and Mach-O objects.
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
constexpr std::size_t digest_size = 32;

struct RecordKind {
  std::string_view prefix;
  PolicyContractKind contract;
  bool provides;
};

constexpr RecordKind record_kinds[]{
    {"1.need.", PolicyContractKind::observation, false},
    {"1.have.", PolicyContractKind::observation, true},
    {"2.effect.need.", PolicyContractKind::effect, false},
    {"2.effect.have.", PolicyContractKind::effect, true},
    {"2.effect.trust.", PolicyContractKind::trusted_effect, false},
};

std::string_view description(PolicyContractKind kind) {
  if (kind == PolicyContractKind::observation) return "verified observation";
  if (kind == PolicyContractKind::effect) return "verified effect bound";
  return "trusted effect bound";
}

bool text_symbol(const obj::object& object, const obj::symbol& symbol) {
  if (!symbol.section || *symbol.section >= object.sections.size()) return false;
  const auto& section = object.sections[*symbol.section];
  return section.segment == "__TEXT" && section.name == "__text" && symbol.offset < section.size();
}

}  // namespace

std::optional<std::string> embed_policy_contracts(obj::object& object, const TuValue& compiled,
                                                  std::span<const PolicyContract> contracts) {
  if (std::ranges::any_of(object.symbols, [](const auto& s) { return s.name_view().starts_with(prefix); }))
    return "a symbol uses the reserved policy contract prefix";
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
    const auto target = names.find(contract.target);
    const auto kind = description(contract.kind);
    // The backend may eliminate the only use of a callee before it names symbols.
    if (target == names.end() && !contract.provides) continue;
    if (origin == names.end() || target == names.end()) return std::format("cannot name a {} contract", kind);
    const auto anchor = functions.find(origin->second->name);
    if (anchor == functions.end()) return std::format("cannot locate the code for a {} contract", kind);
    if (target->second->bind == obj::binding::local) {
      if (functions.contains(target->second->name)) continue;
      return std::format("a local function with a {} must be defined in its object", kind);
    }
    const auto record = std::ranges::find_if(record_kinds, [&](const RecordKind& record) {
      return record.contract == contract.kind && record.provides == contract.provides;
    });
    if (record == std::ranges::end(record_kinds)) return "unknown policy contract kind";
    metadata.push_back({std::format("{}{}{:016x}{:016x}.{}", prefix, record->prefix,
                                    contract.signature.hi, contract.signature.lo, target->second->name),
                        obj::binding::local, anchor->second.first, anchor->second.second});
  }
  metadata.push_back({std::format("{}{}{}", prefix, coverage, metadata.size()), obj::binding::local, 0, 0});
  object.symbols.append_range(std::move(metadata));
  return std::nullopt;
}

std::vector<std::string> check_object_policies(std::span<const obj::object> objects, bool allow_effects) {
  using Location = std::pair<std::uint32_t, std::uint64_t>;
  struct Record {
    std::size_t object;
    std::string_view name;
    std::string_view digest;
    Location at;
    PolicyContractKind kind;
  };
  std::vector<Record> requirements;
  std::map<std::tuple<PolicyContractKind, std::size_t, std::string_view>, Record> guarantees;
  std::map<std::string_view, Record> trusted_effects;
  std::map<std::string_view, Record> verified_effects;
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
      auto malformed = [&] { errors.push_back(std::format("{}: invalid policy contract metadata", object.name)); };
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
      const auto kind = std::ranges::find_if(record_kinds, [&](const RecordKind& kind) {
        return name.starts_with(kind.prefix);
      });
      if (kind == std::ranges::end(record_kinds) || !functions.contains({*symbol.section, symbol.offset})) {
        malformed();
        continue;
      }
      name.remove_prefix(kind->prefix.size());
      const auto digest = name.substr(0, digest_size);
      if (name.size() <= digest_size + 1 || name[digest_size] != '.' ||
          digest.find_first_not_of("0123456789abcdef") != std::string_view::npos) {
        malformed();
        continue;
      }
      const Record record{i, name.substr(digest_size + 1), digest, {*symbol.section, symbol.offset}, kind->contract};
      ++records;
      if (!allow_effects && record.kind != PolicyContractKind::observation) {
        errors.push_back(std::format("{}: checked effect bounds currently require explicit objects or sources at link", object.name));
        continue;
      }
      if (record.kind == PolicyContractKind::trusted_effect) {
        const auto [prior, fresh] = trusted_effects.try_emplace(record.name, record);
        if (!fresh && prior->second.digest != record.digest)
          errors.push_back(std::format("{}: conflicting trusted effect bound for '{}' (previous declaration in {})",
                                       object.name, record.name, objects[prior->second.object].name));
        continue;
      }
      if (record.kind == PolicyContractKind::effect) verified_effects.try_emplace(record.name, record);
      if (kind->provides) {
        if (!guarantees.try_emplace(std::tuple{record.kind, i, record.name}, record).second) malformed();
      } else requirements.push_back(record);
    }
    if (!expected_records) errors.push_back(std::format("{}: missing checked policy coverage", object.name));
    else if (*expected_records != records)
      errors.push_back(std::format("{}: incomplete checked policy metadata", object.name));
  }
  for (const auto& [name, trusted] : trusted_effects) {
    if (const auto verified = verified_effects.find(name); verified != verified_effects.end())
      errors.push_back(std::format("{}: trusted effect bound for '{}' conflicts with a verified bound in {}",
                                   objects[trusted.object].name, name, objects[verified->second.object].name));
  }
  for (const auto& call : requirements) {
    const auto kind = description(call.kind);
    const auto it = definitions.find(call.name);
    if (it == definitions.end() || it->second.size() != 1 || it->second.front().second->weak) {
      errors.push_back(std::format("{}: {} '{}' requires one strong definition", objects[call.object].name, kind, call.name));
      continue;
    }
    const auto [owner, definition] = it->second.front();
    const auto proof = guarantees.find({call.kind, owner, call.name});
    if (proof == guarantees.end() || proof->second.at != Location{*definition->section, definition->offset} ||
        proof->second.digest != call.digest)
      errors.push_back(std::format("{}: {} '{}' has no matching guarantee in {}",
                                   objects[call.object].name, kind, call.name, objects[owner].name));
  }
  return errors;
}

}  // namespace ycxx::pipeline
