#pragma once

#include <string>

#include "ir.h"

namespace lf {

// Structural Verilog that lfc itself can read back (used by the round-trip test).
std::string write_verilog(const Graph& g);

// Graphviz dot, handy for looking at small designs before/after optimization.
std::string write_dot(const Graph& g);

}  // namespace lf
