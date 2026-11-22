using Plain = int(double);
using Safe = int(double) noexcept;
void convert(Safe* ptr, Safe& ref) {
    Plain* relaxed = ptr;
    Plain& alias = ref;
}
