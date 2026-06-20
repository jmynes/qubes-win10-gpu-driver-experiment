/**
 * qubes-win10-gpu-driver-experiment — M1 compat stub
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Replaces LookingGlass LGIdd's CIVSHMEM (the IVSHMEM shared-memory frame sink).
 * The Qubes port does not use IVSHMEM — frames will be granted to dom0 via the
 * Xen grant API (M2+). This header-only no-op keeps the remaining call sites
 * compiling/linking; the real CIVSHMEM.cpp is dropped from the project, and the
 * IVSHMEM gate in CIndirectDeviceContext::InitAdapter() is deleted regardless.
 */
#pragma once
#include <cstddef>

class CIVSHMEM
{
public:
  CIVSHMEM()  {}
  ~CIVSHMEM() {}

  bool   Init()    { return true; }   // was: SetupDi-enumerate the IVSHMEM device
  bool   Open()    { return true; }   // was: IOCTL-map the shared BAR
  void   Close()   {}

  size_t GetSize() { return 0; }
  void * GetMem () { return nullptr; }
};
