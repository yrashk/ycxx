// #1030 [dcl.enum], [meta.unary.prop].
enum class Stage : unsigned;
static_assert(__is_standard_layout(const Stage));

enum class Stage : unsigned {
  from_trait = __is_standard_layout(Stage)
};
static_assert(static_cast<unsigned>(Stage::from_trait) == 1);

enum LegacyStage : int {
  legacy_from_trait = __is_standard_layout(LegacyStage)
};
static_assert(legacy_from_trait == 1);
