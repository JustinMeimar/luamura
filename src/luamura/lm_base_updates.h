#ifndef LM_BASE_UPDATES_H
#define LM_BASE_UPDATES_H

#include "lm_ir.h"

namespace luamura {

// Updates the frame's base on entry and wherever a node may have moved
// it, so that code generation can rely on it.
void addBaseUpdates(LmIR& ir);

} // namespace luamura

#endif // LM_BASE_UPDATES_H
