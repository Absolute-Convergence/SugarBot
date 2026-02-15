#pragma once

#include <iosfwd>

namespace sugar {

class CandleSeries;

// Debug-only interactive test harness used by main in _DEBUG builds.
// In Release builds this compiles to a no-op.
void maybe_run_debug_test_harness(const CandleSeries& series, std::istream& in, std::ostream& out);

} // namespace sugar
