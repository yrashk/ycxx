extern "C" int tile_base(int index) {
    if (index < 0)
        return 9;
    return index * 3 + 1;
}

extern "C" int tile_value(int index) {
    if (index == 0)
        return 5;
    return tile_base(index) + 2;
}
