#pragma once

#include "Buffer.hpp"

namespace pb {

// The on-screen text: a short "how early was that" popup plus an optional counter.
class BufferOverlay : public cocos2d::CCNode {
public:
    static BufferOverlay* create();
    static BufferOverlay* get(PlayLayer* layer);

    void showBuffered(double earlyMs, int ticks, Mode mode, bool release);
    void showToggle(bool enabled);
    void refresh();

protected:
    bool init() override;
    void layout();

    cocos2d::CCLabelBMFont* m_feedback = nullptr;
    cocos2d::CCLabelBMFont* m_stats = nullptr;
};

} // namespace pb
