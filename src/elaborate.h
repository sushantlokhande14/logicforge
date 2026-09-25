#pragma once

#include "ast.h"
#include "ir.h"

namespace lf {

// Bit-blasts one module into a gate graph. The result is deliberately naive
// (no folding, no sharing) so that every optimization is done by a pass you can
// see and measure. Throws std::runtime_error with file:line on bad input.
Graph elaborate(const Module& m);

}  // namespace lf
