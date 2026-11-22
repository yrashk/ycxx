constexpr int evaluation_context() noexcept {
  if consteval {
    return 23;
  } else {
    return 47;
  }
}

struct Record {
  unsigned char byte;
  int context;
  bool enabled;
  unsigned long long checksum;
};

constexpr Record stored{7, evaluation_context(), true, 0x123456789abcULL};

static_assert(stored.byte == 7);
static_assert(stored.context == 23);
static_assert(stored.enabled);
static_assert(stored.checksum == 0x123456789abcULL);

constexpr unsigned long long read_from_function() {
  return stored.byte + stored.context + stored.enabled + stored.checksum;
}

static_assert(read_from_function() == 0x123456789abcULL + 31);


constexpr int read_from_pointer(const Record* record) {
  return record->context;
}

constexpr bool distinct_field_addresses(const Record* record) {
  return static_cast<const void*>(&record->byte) !=
         static_cast<const void*>(&record->checksum);
}

static_assert(read_from_pointer(&stored) == 23);
static_assert(distinct_field_addresses(&stored));

int main() {
  Record automatic{3, evaluation_context(), false, 41};
  return stored.context != 23 || automatic.byte != 3 || automatic.context != 47 ||
         automatic.enabled || automatic.checksum != 41;
}
