struct CopyListTag {
  explicit CopyListTag() = default;
};

void copy_list() {
  CopyListTag object = {}; //! error
}
