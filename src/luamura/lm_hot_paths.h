#ifndef LM_HOT_PATHS_H
#define LM_HOT_PATHS_H

#include "lm_ir.h"

namespace luamura {

// Gives each ADD and ADDI a hot path, where a guard has found its
// operands are integers, which skips the metamethod call after it.
void addIntegerHotPaths(LmIR& ir);

} // namespace luamura

#endif // LM_HOT_PATHS_H
