#pragma once

class PlayLayer;

// Holds back early inputs that a ghost run says would kill you, and fires them
// a little later when that's safe.
namespace pb::buffer {

// True while the buffer is handing GD an input itself.
bool feeding();

// A jump input reached GD. Returns true if the buffer took it (GD must not get
// it now), false to let it through as normal.
bool take(PlayLayer* layer, bool down, bool isPlayer1);

// Called right before player 1 takes a physics step. Returns a fraction (0..1)
// when the held-back input should land that far into this step: the update
// hook then runs the step in two parts and calls fireSplit() in between.
float beforeStep(PlayLayer* layer, float dt);
void fireSplit(PlayLayer* layer);
void afterStep(PlayLayer* layer, float dt);

// Step time so far (moving objects are measured against it).
double clock();

// Player 1 died (death check).
void died(PlayLayer* layer);

void levelStarted(PlayLayer* layer);
void levelEnded();

// Death or restart: drop anything held back.
void reset();

// The buffer was switched off: fire whatever is held back right now.
void flush();

} // namespace pb::buffer
