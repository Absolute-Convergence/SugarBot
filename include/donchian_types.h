#pragma once

namespace sugar {

// Canonical Donchian band selector contract used by strategy/runtime surfaces.
enum class DonchianOutput {
    Upper,
    Lower,
    Middle,
    Width
};

} // namespace sugar
