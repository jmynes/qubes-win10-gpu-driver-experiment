/**
 * qubes-win10-gpu-driver-experiment — M1 compat stub
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Replaces LookingGlass LGIdd's CPipeServer (the named-pipe control channel to
 * the Looking Glass host / LGIddHelper service). The Qubes port does not use it.
 * No-op so every `g_pipe.*` call site compiles/links; the real CPipeServer.cpp
 * is dropped from the project. Signatures match the upstream CPipeServer.h.
 *
 * NOTE: the `g_pipe` instance must be defined once in a .cpp (we add
 * `CPipeServer g_pipe;` to Driver.cpp, replacing the dropped CPipeServer.cpp).
 */
#pragma once
#include <cstdint>

class CPipeServer
{
public:
  bool Init()   { return true; }
  void DeInit() {}

  void SetCursorPos(uint32_t /*x*/, uint32_t /*y*/) {}
  void SetDisplayMode(uint32_t /*width*/, uint32_t /*height*/, uint32_t /*refresh*/) {}
  void SetGPUStatus(bool /*software*/) {}
};

extern CPipeServer g_pipe;
