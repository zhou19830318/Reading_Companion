#pragma once

#include "MappedInputManager.h"

// Logical-button semantics for learning (百词斩) screens — single source of
// truth for the functional-key model in docs/Reading_Companion_百词斩功能.md
// §3/§9/§15 (decision D1: 正面上/下 = front row physical Up/Down labels, i.e.
// logical Left/Right; honour remap via MappedInputManager::mapButton).
//
// Resolution rules stay in MappedInputManager::mapButton:
// - Left/Right/Back/Confirm follow the front-button remap (SETTINGS.frontButton*).
// - PageBack/PageForward follow sideButtonLayout (side-key swap).
// - Up/Down are the fixed side buttons.
//
// Button::Power is intentionally absent: the global power-hold handler in
// main.cpp owns sleep (§8 — power is never routed into learning semantics).
namespace LearningKeymap {
inline constexpr auto frontBack = MappedInputManager::Button::Back;
inline constexpr auto frontUp = MappedInputManager::Button::Left;
inline constexpr auto frontDown = MappedInputManager::Button::Right;
inline constexpr auto frontConfirm = MappedInputManager::Button::Confirm;
inline constexpr auto sidePrev = MappedInputManager::Button::PageBack;
inline constexpr auto sideNext = MappedInputManager::Button::PageForward;
}  // namespace LearningKeymap
