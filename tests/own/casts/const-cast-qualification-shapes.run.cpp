// #1424, [expr.const.cast]: ordinary pointers can change cv through matching array and pointer shapes.
int main() {
  int values[2]{2, 3};
  const int (&array_view)[2] = values;
  int* decayed = const_cast<int*>(array_view);
  const int (*array_pointer)[2] = &values;
  int (*array_alias)[2] = const_cast<int (*)[2]>(array_pointer);

  const int* const* nested_view = &decayed;
  int** nested_alias = const_cast<int**>(nested_view);

  const int* end_view = values + 2;
  if (const_cast<int*>(end_view) != values + 2 || nested_alias != &decayed)
    return 1;
  (*array_alias)[0] = 5;
  (*nested_alias)[1] = 7;
  return values[0] == 5 && values[1] == 7 ? 0 : 2;
}
