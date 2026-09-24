#pragma once

// Changes to the game's own behaviour for headsets: black fades, a camera that turns only when you
// turn it, and third-person aiming in stereo (see game_tweaks.cpp).
namespace vr::game_tweaks {

void install();

// Stereo, aiming an item from the subject view: the view is pulled back behind Link and he stays
// drawn.
bool aiming_third_person();

} // namespace vr::game_tweaks
