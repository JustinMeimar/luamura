#ifndef LM_CFG_H
#define LM_CFG_H

#include "lm_lua.h"

#include <cstdint>
#include <vector>

namespace luamura {

// an instruction's index in its function's code; signed, as jumps are
using PC = int32_t;

// The control-flow graph of a function's bytecode, with the blocks Lua
// can reach, in pc order.
class CFG {
public:
    using BlockIndex = uint32_t;

    struct Block {
        PC start;
        PC end;
        // the most common first
        std::vector<BlockIndex> successors;

        PC last() const { return end - 1; }
    };

    explicit CFG(const Proto& proto);

    std::vector<Block> blocks;
    // The blocks where Lua may enter the function: its start, and after
    // each call, once the callee returns.
    std::vector<BlockIndex> entries;

private:
    bool isCall(PC pc) const;
    std::vector<PC> successorPCs(PC pc) const;
    void findLeaders();
    PC blockEnd(PC start) const;
    std::vector<bool> reachable() const;
    void addBlocks(const std::vector<bool>& reached);

    const Proto& proto;
    // whether a block starts at each pc
    std::vector<bool> leader;
};

} // namespace luamura

#endif // LM_CFG_H
