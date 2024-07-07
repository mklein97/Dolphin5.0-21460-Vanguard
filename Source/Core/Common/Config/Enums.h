// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

namespace Config
{
// Layers in ascending order of priority.
enum class LayerType
{
  Base,
  CommandLine,
  GlobalGame,
  LocalGame,
  Movie,
  Netplay,
  CurrentRun,
  // RTC_Hijack - Add Vanguard
  Vanguard,
  Meta,
};

enum class System
{
  Main,
  SYSCONF,
  GCPad,
  WiiPad,
  GCKeyboard,
  GFX,
  Logger,
  DualShockUDPClient,
  FreeLook,
  Session,
  GameSettingsOnly,
  Achievements,
};

// RTC_Hijack - Add Vanguard and change array size to 8
constexpr std::array<LayerType, 8> SEARCH_ORDER{{

    LayerType::Vanguard,
    LayerType::CurrentRun,
    LayerType::Netplay,
    LayerType::Movie,
    LayerType::LocalGame,
    LayerType::GlobalGame,
    LayerType::CommandLine,
    LayerType::Base,
}};
}  // namespace Config
