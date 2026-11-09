#ifndef LM_CFG_H
#define LM_CFG_H

#include "lm_lua.h"

#include <vector>

// The control-flow graph of a function's bytecode, with the blocks Lua
// can reach, in pc order.
class CFG {
public:
    struct Block {
        int start;
        int end;
        // indices in blocks, the most common first
        std::vector<int> successors;

        int last() const { return end - 1; }
    };

    explicit CFG(const Proto* proto);

    std::vector<Block> blocks;
    // The blocks where Lua may enter the function: its start, and after
    // each call, once the callee returns.
    std::vector<int> entries;
};

#endif // LM_CFG_H
