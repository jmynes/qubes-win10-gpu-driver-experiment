/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#pragma once

#include <Windows.h>
#include <wdf.h>
#include <IddCx.h>
#include <vector>

#include "CSettings.h"
#include "CEdid.h"

class CIndirectDeviceContext
{
private:
  WDFDEVICE     m_wdfDevice;
  IDDCX_ADAPTER m_adapter       = nullptr;
  IDDCX_MONITOR m_monitor       = nullptr;
  bool          m_replugMonitor = false;

  // M1: the IVSHMEM/LGMP frame + cursor sink is dropped (stubbed). Frames will be
  // granted to dom0 via the Xen grant API (M2+). Only the alignment hint survives
  // for the swap-chain page-size plumbing.
  size_t         m_alignSize    = 0;

  UINT m_iddCxVersion = 0;
  bool m_canProcessFP16 = false;

  void QueryIddCxCapabilities();
  bool CanUseIddCx110DDIs() const { return m_canProcessFP16; }

  void DeInitLGMP();
  void LGMPTimer();
  void ResendCursor() const;
  bool UpdateMonitorModes();

  CSettings::DisplayModes m_displayModes;
  CEdid                   m_edid;

  CSettings::DisplayMode m_setMode = {};
  bool m_doSetMode = false;
  volatile LONG m_replugMonitorQueued = 0;
  volatile LONG m_recoverModeUpdateSwapChain = 0;

public:
  CIndirectDeviceContext(_In_ WDFDEVICE wdfDevice) :
    m_wdfDevice(wdfDevice) {};

  virtual ~CIndirectDeviceContext() { DeInitLGMP(); }

  bool SetupLGMP(size_t alignSize);

  void PopulateDefaultModes();
  void InitAdapter();
  void FinishInit(UINT connectorIndex);
  void ReplugMonitor();

  void OnAssignSwapChain();
  void OnUnassignedSwapChain();
  void OnSwapChainLost();

  NTSTATUS ParseMonitorDescription(
    const IDARG_IN_PARSEMONITORDESCRIPTION* inArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* outArgs);
  NTSTATUS MonitorGetDefaultModes(
    const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* inArgs, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* outArgs);
  NTSTATUS MonitorQueryTargetModes(
    const IDARG_IN_QUERYTARGETMODES* inArgs, IDARG_OUT_QUERYTARGETMODES* outArgs);

#if defined(IDDCX_VERSION_MAJOR) && defined(IDDCX_VERSION_MINOR) && \
  (IDDCX_VERSION_MAJOR > 1 || (IDDCX_VERSION_MAJOR == 1 && IDDCX_VERSION_MINOR >= 10))
  NTSTATUS ParseMonitorDescription2(
    const IDARG_IN_PARSEMONITORDESCRIPTION2* inArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* outArgs);
  NTSTATUS MonitorQueryTargetModes2(
    const IDARG_IN_QUERYTARGETMODES2* inArgs, IDARG_OUT_QUERYTARGETMODES* outArgs);
#endif

  void SetResolution(int width, int height);

  size_t GetAlignSize   () const { return m_alignSize     ; }
  bool   CanProcessFP16 () const { return m_canProcessFP16; }

  // M1 frame/cursor sink — all stubbed no-ops (see .cpp). The only callers live
  // in the gutted CSwapChainProcessor; kept so the interface stays stable for M2.
  struct PreparedFrameBuffer
  {
    unsigned frameIndex;
    uint8_t* mem;
  };

  PreparedFrameBuffer PrepareFrameBuffer(unsigned pitch, const RECT * dirtyRects, unsigned nbDirtyRects);
  void WriteFrameBuffer(unsigned frameIndex, void* src, size_t offset, size_t len, bool setWritePos) const;
  void FinalizeFrameBuffer(unsigned frameIndex) const;

  void SendCursor(const IDARG_OUT_QUERY_HWCURSOR & info, const BYTE * data);
};

struct CIndirectDeviceContextWrapper
{
  CIndirectDeviceContext* context;

  void Cleanup()
  {
    delete context;
    context = nullptr;
  }
};

WDF_DECLARE_CONTEXT_TYPE(CIndirectDeviceContextWrapper);