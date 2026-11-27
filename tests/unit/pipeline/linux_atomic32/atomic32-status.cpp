// GNU __atomic builtins and [atomics.order]
static_assert(__ATOMIC_RELAXED == 0);
static_assert(__ATOMIC_CONSUME == 1);
static_assert(__ATOMIC_ACQUIRE == 2);
static_assert(__ATOMIC_RELEASE == 3);
static_assert(__ATOMIC_ACQ_REL == 4);
static_assert(__ATOMIC_SEQ_CST == 5);

static_assert(__atomic_always_lock_free(4, nullptr));
static_assert(!__atomic_always_lock_free(3, nullptr));

using Word = unsigned int;
static_assert(sizeof(Word) == 4 && alignof(Word) == 4);

extern "C" int atomic32_status() {
  Word word = 0;
  Word* cell = &word;
  constexpr Word initial = 0x84a51239u;
  __atomic_store_n(cell, initial, __ATOMIC_RELEASE);
  if (__atomic_load_n(cell, __ATOMIC_ACQUIRE) != initial) return 1;
  if (__atomic_load_n(cell, __ATOMIC_CONSUME) != initial) return 2;

  if (__atomic_exchange_n(cell, 13u, __ATOMIC_ACQ_REL) != initial) return 3;
  if (__atomic_load_n(cell, __ATOMIC_RELAXED) != 13u) return 4;

  Word expected = 13u;
  if (!__atomic_compare_exchange_n(cell, &expected, 21u, false,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 5;
  if (expected != 13u || __atomic_load_n(cell, __ATOMIC_RELAXED) != 21u) return 6;

  expected = 13u;
  if (__atomic_compare_exchange_n(cell, &expected, 77u, false,
                                  __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE)) return 7;
  if (expected != 21u || __atomic_load_n(cell, __ATOMIC_RELAXED) != 21u) return 8;

  if (__atomic_fetch_add(cell, 7u, __ATOMIC_RELAXED) != 21u) return 9;
  return __atomic_load_n(cell, __ATOMIC_SEQ_CST) == 28u ? 0 : 10;
}
