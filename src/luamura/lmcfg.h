#ifndef LMCFG_H
#define LMCFG_H

#include "lmlua.h"

#include <vector>

// The control-flow graph of a function's bytecode.
namespace cfg {

// The pcs the instruction at 'pc' may continue at, the most common
// first.
std::vector<int> successors(const Proto* proto, int pc);

// Whether each pc starts a basic block.
std::vector<bool> leaders(const Proto* proto);

// Where Lua may enter the function: its start, and after each call,
// once the callee returns.
std::vector<int> entries(const Proto* proto);

} // namespace cfg

#endif // LMCFG_H
