#pragma once

// Wire-format timecode conversions.
//
// The engine reports the show's time as double seconds plus a timecode
// string computed at poll time (Time::toTimecode). A client that wants to
// run that clock continuously between polls - the control GUI's Playback
// tab extrapolates the timecode at the UI frame rate so it advances
// smoothly between the 500 ms status polls - converts the reported
// timecode to a frame index, adds a frame delta for the elapsed wall time,
// and formats the result back. These helpers do exactly that in string
// space, so the client never has to reconstruct the show's exact rational
// frame rate: the FF field of a timecode only needs the nominal whole
// frame rate, which is what the protocol's "frameRate" string
// (FrameRate::toString) carries.

#include <cstdint>
#include <string>

namespace mediamax
{

// Parses an "HH:MM:SS:FF" timecode string into the absolute frame index at
// the given nominal frame rate (e.g. 30 for a 29.97 show; see
// FrameRate::nominalFramesPerSecond). The string must be exactly a
// non-negative timecode (the engine's "-00:00:00:00" for negative times is
// rejected). Returns -1 when the string is not a valid timecode for the
// nominal rate.
std::int64_t timecodeToFrames(const std::string& timecode, std::int64_t nominalFps);

// Inverse of timecodeToFrames: formats an absolute frame index at the
// nominal frame rate as "HH:MM:SS:FF". Matches Time::toTimecode for
// non-negative times; negative frame indexes format as "-00:00:00:00".
std::string framesToTimecode(std::int64_t frames, std::int64_t nominalFps);

// Parses the frame rate string the protocol reports (FrameRate::toString:
// "60", "29.97", ...) into the real (double) frame rate and the nominal
// whole frame rate. The double loses the exact rational value (29.97 is
// 30000/1001); that precision is only acceptable at the wire edge and only
// for advancing a timecode by a sub-poll delta (the error stays a tiny
// fraction of a frame), never for computing an absolute time. Returns
// false when the string is not a positive finite frame rate.
bool parseFrameRateString(const std::string& text, double& framesPerSecond,
                          std::int64_t& nominalFps);

} // namespace mediamax
