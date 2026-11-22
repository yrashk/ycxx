struct Small { int value; };
struct Pair { long first; int second; };
struct Wide { long first; long second; long third; };

int result_field(int input);
Small small_result(int input);
Pair named_result(int input);
Wide wide_result(int input);
