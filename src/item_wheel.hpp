#pragma once

// Tabletop: the item wheel can leave the game running ("quick" switching, see item_wheel.cpp).
namespace vr::item_wheel {

void install();
// Game thread, once per frame (dev harness hooks).
void debug_tick();

} // namespace vr::item_wheel
