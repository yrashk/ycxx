// FIXTURE: TIMEOUT, loops forever.
// TIMEOUT: 2
int main() { for (volatile int i = 0;; ) ++i; }
