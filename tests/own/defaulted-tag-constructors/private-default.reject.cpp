class HiddenTag {
  HiddenTag() = default;
};

void inaccessible_default() {
  HiddenTag object; //! error
}
