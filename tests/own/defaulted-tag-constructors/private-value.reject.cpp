class HiddenTag {
  HiddenTag() = default;
};

void inaccessible_value() {
  HiddenTag object{}; //! error
}
