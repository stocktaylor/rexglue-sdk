#pragma once
/**
 * @file        rex/input/keystroke_synth.h
 * @brief       XInputGetKeystroke events from successive controller states.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>

#include <rex/input/input.h>

namespace rex::input {

/// Turns a device's controller state into keystroke events, one per call:
/// button, trigger and stick-direction downs and ups, and auto-repeat of the
/// last button pressed. For drivers whose device has no keystroke queue of its
/// own (SDL pads, the keyboard). XUI titles such as the dashboard navigate on
/// these events alone.
class KeystrokeSynthesizer {
 public:
  /// The next event for `gamepad`; X_ERROR_EMPTY when there is none. Pass a
  /// neutral gamepad while input is inactive, so held buttons send their ups.
  X_RESULT Next(const X_INPUT_GAMEPAD& gamepad, X_INPUT_KEYSTROKE* out_keystroke);

 private:
  enum class RepeatState {
    Idle,       // no buttons pressed or repeating has ended
    Waiting,    // a button is held and the delay is awaited
    Repeating,  // actively repeating at a rate
  };
  uint64_t buttons_ = 0;
  RepeatState repeat_state_ = RepeatState::Idle;
  // The button number that was pressed last.
  uint8_t repeat_butt_idx_ = 0;
  // The last time (ms) a down (and/or repeat) event for that button was sent.
  uint32_t repeat_time_ = 0;
};

}  // namespace rex::input
