#ifndef LM_EFFECTS_H
#define LM_EFFECTS_H

#include "lm_ir.h"

#include <vector>

namespace luamura {

// The frame slots each bytecode instruction reads and writes, and
// whether it can run Lua code. Lowering still runs each instruction's
// handler, so the model describes the handlers in lvmops.h.
class EffectModel {
public:
    explicit EffectModel(LmIR& ir);

    void model(Node& node);

private:
    void findCapturedSlots();
    bool reads(FrameSlot slot) const;
    bool writes(FrameSlot slot) const;

    void read(FrameSlot slot);
    void write(FrameSlot slot);
    void readRange(FrameSlot from, FrameSlot to);
    void writeRange(FrameSlot from, FrameSlot to);
    void runsLua();
    FrameSlot upTo(uint32_t count, FrameSlot end) const;

    LmIR& ir;
    FrameSlot top;
    // slots that closures capture, which Lua code can change through
    // open upvalues
    std::vector<FrameSlot> captured;
    Node* node = nullptr;
};

} // namespace luamura

#endif
