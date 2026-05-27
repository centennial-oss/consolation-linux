#pragma once

namespace consolation::audio {

// Default app playback volume when the user has not saved a preference (see AppSettings).
// Intentionally a single named constant so product default changes stay obvious.
inline constexpr int defaultPlaybackVolumePercent = 75;

} // namespace consolation::audio
