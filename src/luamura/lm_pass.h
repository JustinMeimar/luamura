#ifndef LM_PASS_H
#define LM_PASS_H

#include "lm_ir.h"

#include <vector>

namespace luamura {

// A pass over one function's LmIR.
using LmIRPass = void (*)(LmIR&);

class PassManager {
public:
    void add(LmIRPass pass) { passes.push_back(pass); }
    void run(LmModule& module) const {
        for (auto& ir : module.functions)
            for (LmIRPass pass : passes)
                pass(*ir);
    }

private:
    std::vector<LmIRPass> passes;
};

} // namespace luamura

#endif
