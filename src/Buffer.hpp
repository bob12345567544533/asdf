#pragma once

class PlayLayer;

// Holds back early inputs that a ghost run says would kill you, and fires them
// a few ticks later when that's safe.
namespace pb::buffer {

// True while the buffer is handing GD an input itself.
bool feeding();

// A jump input reached GD. Returns true if the buffer took it (GD must not get
// it now), false to let it through as normal.
bool take(PlayLayer* layer, bool down, bool isPlayer1);

// Called right before / after player 1 takes a physics step.
void beforeStep(PlayLayer* layer, float dt);
void afterStep(PlayLayer* layer, float dt);

void levelStarted(PlayLayer* layer);
void levelEnded();

// Death or restart: drop anything held back.
void reset();

// The buffer was switched off: fire whatever is held back right now.
void flush();

} // namespace pb::buffer
