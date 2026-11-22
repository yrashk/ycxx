// [class.mem.general], [basic.lookup.unqual]
struct Record {
  int value = later(); //! error
  Record() noexcept;
};

int later() noexcept { return 1; }
Record::Record() noexcept {}
