#pragma once

#include <QString>
#include <vector>

// Procedurally synthesized background ambiences for the Mic FX chain
// ("you are talking from inside a laundromat"). No audio assets are
// shipped: every sound is generated at selection time into a seamless
// mono loop (a few seconds at 48 kHz) that MicFx mixes into the
// outgoing capture stream while the microphone transmits.
//
// Generation runs on the GUI thread (a few ms per sound, deterministic
// RNG so the same id always sounds the same); the audio thread only
// reads the finished loop buffer.
namespace MicAmbience {

// Number of available ambiences. Ids are 0..count()-1 and stable
// across versions (append-only — they are persisted in QSettings).
int count();

// Translated display name for the given ambience id.
QString name(int id);

// Build the loop for `id` at `sampleRate`. Returns an empty vector for
// an invalid id. The loop is seamless: the caller just wraps the read
// index. Samples are floats in [-1, 1], pre-levelled so the default
// mix gain sits comfortably under a voice.
std::vector<float> generate(int id, int sampleRate = 48000);

// Decode a USER-PICKED audio file (any format FFmpeg can open) into an
// ambience loop: mono, resampled to `sampleRate`, capped at 30 s,
// seamless + RMS-matched like the built-ins. Empty on failure.
std::vector<float> loadCustomFile(const QString &path, int sampleRate = 48000);

}
