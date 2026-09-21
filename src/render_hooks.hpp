#pragma once

#include <string>

namespace vr::render {

// Installs every game hook and the render-worker callbacks. Returns false (after logging) if a
// required hook could not be installed.
bool install();
void uninstall();

// Human-readable per-frame status for the settings panel.
std::string status();

} // namespace vr::render
