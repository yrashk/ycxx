struct SurveyTag {
  SurveyTag() = default;
};

void initialize() {
  SurveyTag default_initialized;
  SurveyTag direct_list_initialized{};
  (void)SurveyTag{};
  (void)SurveyTag();
}
