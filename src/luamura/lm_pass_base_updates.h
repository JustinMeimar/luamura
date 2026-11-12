#ifndef LM_PASS_BASE_UPDATES_H
#define LM_PASS_BASE_UPDATES_H

#include "lm_pass.h"

namespace luamura {

// Updates the frame's base on entry and wherever a node may have moved
// it, so that code generation can rely on it.
class BaseUpdates : public LmIRPass {
public:
    void run(LmIR& ir) override;
};

} // namespace luamura

#endif // LM_PASS_BASE_UPDATES_H
