typedef int (*Step)(int);

int external_step(int value) { return value + 3; }
int step_alias(int value);
Step original_address(void);
Step alias_address(void);

int main(void) {
    Step original = original_address();
    Step alias = alias_address();
    if (original != &external_step || original(41) != 44) return 1;
    if (alias != &step_alias || alias(41) != 50) return 2;
    return 0;
}
