#pragma once

#include "compress.hpp"

namespace dana::compress::cpp_symbols {

    // The `cpp.symbols` compressor's body, exposed so `gtest.units` can fall back to
    // it when a region turns out not to contain any test bodies after all.
    [[nodiscard]] outcome compress(region const& r, signals const& s);

} // namespace dana::compress::cpp_symbols
