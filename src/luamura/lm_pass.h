#ifndef LM_PASS_H
#define LM_PASS_H

#include "lm_ir.h"
#include <memory>
#include <vector>

namespace luamura {

// A pass over one function's LmIR.
class LmIRPass {
public:
    virtual ~LmIRPass() = default;
    virtual void run(LmIR& ir) = 0;
};

class PassManager {
public:
    void add(std::unique_ptr<LmIRPass> pass) {
        passes.push_back(std::move(pass));
    }
    void run(LmModule& module) const {
        for (auto& ir : module.functions)
            for (const auto& pass : passes)
                pass->run(*ir);
    }

private:
    std::vector<std::unique_ptr<LmIRPass>> passes;
};

} // namespace luamura

#endif
