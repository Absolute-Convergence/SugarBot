#pragma once

namespace sugar::numeric {

struct kahan_accumulator {
    void reset() {
        sum = 0.0;
        compensation = 0.0;
    }

    void add(double value) {
        const double adjusted = value - compensation;
        const double updated = sum + adjusted;
        compensation = (updated - sum) - adjusted;
        sum = updated;
    }

    void subtract(double value) {
        add(-value);
    }

    [[nodiscard]] double value() const {
        return sum;
    }

    double sum{ 0.0 };
    double compensation{ 0.0 };
};

} // namespace sugar::numeric
