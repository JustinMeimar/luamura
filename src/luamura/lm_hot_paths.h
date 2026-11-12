#ifndef LM_HOT_PATHS_H
#define LM_HOT_PATHS_H

#include "lm_ir.h"

namespace luamura {

// Guards ADD and ADDI operands, then keeps a bounded arithmetic chain
// on its integer path, skipping each operation's metamethod fallback.
void addIntegerHotPaths(LmIR& ir);

} // namespace luamura

#endif // LM_HOT_PATHS_H
