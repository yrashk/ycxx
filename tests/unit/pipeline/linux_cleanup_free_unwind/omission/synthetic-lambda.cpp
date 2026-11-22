extern "C" int uncertain_step(int index);

extern "C" int request_lambda_path(int index) {
    auto step = [](int value) { return uncertain_step(value) + 11; };
    return step(index) + 12;
}

extern "C" int ordinary_path(int index) {
    return uncertain_step(index) + 6;
}
