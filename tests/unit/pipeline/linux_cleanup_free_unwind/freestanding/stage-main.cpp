extern "C" int tile_value(int index);

int inspect_tiles() {
    if (tile_value(-1) != 11)
        return 1;
    if (tile_value(0) != 5)
        return 2;
    return tile_value(13);
}

int main() {
    return inspect_tiles();
}
