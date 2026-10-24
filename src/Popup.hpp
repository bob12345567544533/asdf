#pragma once

#include <string>

class PlayLayer;

// Short line bottom left in game that fades out (how early a held-back click was).
namespace pb::popup {

void attach(PlayLayer* layer);
void show(std::string const& text, bool good = true);

} // namespace pb::popup
