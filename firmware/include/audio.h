#ifndef AUDIO_H
#define AUDIO_H

// Starts the microphone/speaker task. Hold KEY1 to record, release to replay.
// Returns false if disabled or initialization fails; other subsystems may run.
bool init_audio();

#endif // AUDIO_H
