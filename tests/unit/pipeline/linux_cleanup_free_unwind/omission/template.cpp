extern "C" int uncertain_step(int index);

template<class T>
int generated_path(T index) {
    return uncertain_step(index) + 7;
}

extern "C" int request_generated_path(int index) {
    return generated_path(index) + 8;
}

extern "C" int ordinary_path(int index) {
    return uncertain_step(index) + 6;
}
