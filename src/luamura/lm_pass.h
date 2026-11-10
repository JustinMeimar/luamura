#ifndef LM_PASS_H
#define LM_PASS_H

#include "lm_ir.h"

#include <vector>

namespace luamura {

class PassManager {
public:
    using Pass = void (*)(LmIR&);

    void add(Pass pass) { passes.push_back(pass); }
    void run(LmIR& ir) const {
        for (Pass pass : passes)
            pass(ir);
    }

private:
    std::vector<Pass> passes;
};

} // namespace luamura

#endif
