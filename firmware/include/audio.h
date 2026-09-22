#ifndef AUDIO_H
#define AUDIO_H

// Starts voice/button capture; completed phrases upload or replay per AUDIO_AI_REPLY_TEST.
// Returns false if disabled or initialization fails; other subsystems may run.
bool init_audio();

#endif // AUDIO_H
