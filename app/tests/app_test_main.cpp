#include <iostream>

namespace sugar::app::tests {
    bool run_all(std::ostream& out);
}

int main() {
    return sugar::app::tests::run_all(std::cout) ? 0 : 1;
}
