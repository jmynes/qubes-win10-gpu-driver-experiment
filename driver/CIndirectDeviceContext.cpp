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

#include "CIndirectDeviceContext.h"
#include "CIndirectMonitorContext.h"

#include "CSettings.h"
#include "CPlatformInfo.h"
#include "CPipeServer.h"
#include "CDebug.h"
#include "VersionInfo.h"

static const UINT IDDCX_VERSION_1_10 = 0x1A00;

#if defined(IDDCX_VERSION_MAJOR) && defined(IDDCX_VERSION_MINOR) && \
  (IDDCX_VERSION_MAJOR > 1 || (IDDCX_VERSION_MAJOR == 1 && IDDCX_VERSION_MINOR >= 10))
static inline IDDCX_WIRE_BITS_PER_COMPONENT GetWireBitsPerComponent(bool hdr)
{
  IDDCX_WIRE_BITS_PER_COMPONENT bits = {};
  bits.Rgb = IDDCX_BITS_PER_COMPONENT_8;
  if (hdr)
    bits.Rgb = (IDDCX_BITS_PER_COMPONENT)(bits.Rgb | IDDCX_BITS_PER_COMPONENT_10);
  bits.YCbCr444 = IDDCX_BITS_PER_COMPONENT_NONE;
  bits.YCbCr422 = IDDCX_BITS_PER_COMPONENT_NONE;
  bits.YCbCr420 = IDDCX_BITS_PER_COMPONENT_NONE;
  return bits;
}
#endif

void CIndirectDeviceContext::QueryIddCxCapabilities()
{
  IDARG_OUT_GETVERSION ver = {};
  NTSTATUS status = IddCxGetVersion(&ver);
  if (!NT_SUCCESS(status))
  {
    m_iddCxVersion = 0;
    m_canProcessFP16 = false;
    DEBUG_ERROR_HR(status, "IddCxGetVersion Failed");
    return;
  }

  m_iddCxVersion = ver.IddCxVersion;

#if defined(IDDCX_VERSION_MAJOR) && defined(IDDCX_VERSION_MINOR) && \
  (IDDCX_VERSION_MAJOR > 1 || (IDDCX_VERSION_MAJOR == 1 && IDDCX_VERSION_MINOR >= 10))
  const bool hasIddCx110DDIs =
    !!IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2) &&
    !!IDD_IS_FUNCTION_AVAILABLE(IddCxMonitorQueryHardwareCursor3) &&
    !!IDD_IS_FUNCTION_AVAILABLE(IddCxMonitorUpdateModes2) &&
    IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterQueryTargetInfo) &&
    IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterCommitModes2) &&
    IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxParseMonitorDescription2) &&
    IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxMonitorQueryTargetModes2) &&
    IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxMonitorSetDefaultHdrMetaData) &&
    IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxMonitorSetGammaRamp);
#else
  const bool hasIddCx110DDIs = false;
#endif

  m_canProcessFP16 = m_iddCxVersion >= IDDCX_VERSION_1_10 && hasIddCx110DDIs;

  DEBUG_INFO("IddCx version: 0x%04x", m_iddCxVersion);
  DEBUG_INFO("IddCx 1.10 HDR/WCG DDIs: %s", m_canProcessFP16 ? "available" : "unavailable");
}

void CIndirectDeviceContext::PopulateDefaultModes()
{
  g_settings.LoadModes();

  m_displayModes.clear();
  m_displayModes.reserve(g_settings.GetDisplayModes().size());
  for (auto& dm : g_settings.GetDisplayModes())
    m_displayModes.push_back(dm);

  m_edid.Build(m_displayModes);
}

void CIndirectDeviceContext::InitAdapter()
{
  // M1: the upstream IVSHMEM gate (m_ivshmem.Init()/Open()) is removed — the Qubes
  // guest has no IVSHMEM PCI device, and gating here would stop the adapter+monitor
  // lifecycle before IddCxMonitorArrival, so no virtual monitor would ever enumerate.

  QueryIddCxCapabilities();
  PopulateDefaultModes();

  IDDCX_ADAPTER_CAPS caps = {};
  caps.Size = sizeof(caps);

  /**
   * For some reason if we do not set this flag sometimes windows will
   * refuse to enumerate our virtual monitor. Intel also noted in their
   * sources that if this is not set dynamic resolution changes from this
   * driver will not work. This behaviour is not documented by Microsoft.
   */
  caps.Flags = IDDCX_ADAPTER_FLAGS_USE_SMALLEST_MODE;
#if defined(IDDCX_VERSION_MAJOR) && defined(IDDCX_VERSION_MINOR) && \
  (IDDCX_VERSION_MAJOR > 1 || (IDDCX_VERSION_MAJOR == 1 && IDDCX_VERSION_MINOR >= 10))
  if (CanUseIddCx110DDIs())
    caps.Flags |= IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16;
#endif

  caps.MaxMonitorsSupported = 1;

  caps.EndPointDiagnostics.Size             = sizeof(caps.EndPointDiagnostics);
  caps.EndPointDiagnostics.GammaSupport     = IDDCX_FEATURE_IMPLEMENTATION_NONE;
  caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_OTHER;

  caps.EndPointDiagnostics.pEndPointFriendlyName     = L"Qubes IDD Driver";
  caps.EndPointDiagnostics.pEndPointManufacturerName = L"Qubes";
  caps.EndPointDiagnostics.pEndPointModelName        = L"Qubes";

  IDDCX_ENDPOINT_VERSION ver = {};
  ver.Size     = sizeof(ver);
  ver.MajorVer = 1;
  caps.EndPointDiagnostics.pFirmwareVersion = &ver;
  caps.EndPointDiagnostics.pHardwareVersion = &ver;

  WDF_OBJECT_ATTRIBUTES attr;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, CIndirectDeviceContextWrapper);

  IDARG_IN_ADAPTER_INIT init = {};
  init.WdfDevice        = m_wdfDevice;
  init.pCaps            = &caps;
  init.ObjectAttributes = &attr;

  IDARG_OUT_ADAPTER_INIT initOut;
  NTSTATUS status = IddCxAdapterInitAsync(&init, &initOut);
  if (!NT_SUCCESS(status) && CanUseIddCx110DDIs())
  {
    DEBUG_WARN(
      "IddCxAdapterInitAsync rejected FP16 adapter capabilities (0x%08x), retrying without HDR/WCG",
      status);
    m_canProcessFP16 = false;
    caps.Flags = (IDDCX_ADAPTER_FLAGS)(caps.Flags & ~IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16);
    ZeroMemory(&initOut, sizeof(initOut));
    status = IddCxAdapterInitAsync(&init, &initOut);
  }

  if (!NT_SUCCESS(status))
  {
    DEBUG_ERROR_HR(status, "IddCxAdapterInitAsync Failed");
    return;
  }

  m_adapter = initOut.AdapterObject;

  // try to co-exist with the virtual video device by telling IddCx which adapter we prefer to render on
  IDXGIFactory * factory = NULL;
  IDXGIAdapter * dxgiAdapter;
  CreateDXGIFactory(__uuidof(IDXGIFactory), (void **)&factory);
  for (UINT i = 0; factory->EnumAdapters(i, &dxgiAdapter) != DXGI_ERROR_NOT_FOUND; ++i)
  {
    DXGI_ADAPTER_DESC adapterDesc;
    dxgiAdapter->GetDesc(&adapterDesc);
    dxgiAdapter->Release();

    if ((adapterDesc.VendorId == 0x1414 && adapterDesc.DeviceId == 0x008c) || // Microsoft Basic Render Driver
        (adapterDesc.VendorId == 0x1b36 && adapterDesc.DeviceId == 0x000d) || // QXL      
        (adapterDesc.VendorId == 0x1234 && adapterDesc.DeviceId == 0x1111))   // QEMU Standard VGA
      continue;

    IDARG_IN_ADAPTERSETRENDERADAPTER args = {};
    args.PreferredRenderAdapter = adapterDesc.AdapterLuid;
    IddCxAdapterSetRenderAdapter(m_adapter, &args);
    break;
  }
  factory->Release();

  auto * wrapper = WdfObjectGet_CIndirectDeviceContextWrapper(m_adapter);
  wrapper->context = this;  
}

void CIndirectDeviceContext::FinishInit(UINT connectorIndex)
{
  WDF_OBJECT_ATTRIBUTES attr;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, CIndirectMonitorContextWrapper);

  IDDCX_MONITOR_INFO info = {};
  info.Size           = sizeof(info);
  info.MonitorType    = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
  info.ConnectorIndex = connectorIndex;

  info.MonitorDescription.Size     = sizeof(info.MonitorDescription);
  info.MonitorDescription.Type     = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
  info.MonitorDescription.DataSize = m_edid.Size();
  info.MonitorDescription.pData    = const_cast<BYTE*>(m_edid.Data());

  CoCreateGuid(&info.MonitorContainerId);

  IDARG_IN_MONITORCREATE create = {};
  create.ObjectAttributes = &attr;
  create.pMonitorInfo     = &info;

  IDARG_OUT_MONITORCREATE createOut;
  NTSTATUS status = IddCxMonitorCreate(m_adapter, &create, &createOut);
  if (!NT_SUCCESS(status))
  {
    DEBUG_ERROR_HR(status, "IddCxMonitorCreate Failed");
    return;
  }

  m_monitor = createOut.MonitorObject;
  auto * wrapper = WdfObjectGet_CIndirectMonitorContextWrapper(m_monitor);
  wrapper->context = new CIndirectMonitorContext(m_monitor, this);

  IDARG_OUT_MONITORARRIVAL out;
  status = IddCxMonitorArrival(m_monitor, &out);
  if (FAILED(status))
  {
    DEBUG_ERROR_HR(status, "IddCxMonitorArrival Failed");
    return;
  }
}

void CIndirectDeviceContext::ReplugMonitor()
{
  if (m_monitor == WDF_NO_HANDLE)
  {
    FinishInit(0);
    return;
  }

  if (m_replugMonitor)
    return;

  DEBUG_TRACE("ReplugMonitor");
  m_replugMonitor = true;
  NTSTATUS status = IddCxMonitorDeparture(m_monitor);
  if (!NT_SUCCESS(status))
  {
    m_replugMonitor = false;
    DEBUG_ERROR("IddCxMonitorDeparture Failed (0x%08x)", status);
    return;
  }
}

void CIndirectDeviceContext::OnAssignSwapChain()
{
  InterlockedExchange(&m_recoverModeUpdateSwapChain, 0);

  if (m_doSetMode)
  {
    m_doSetMode = false;
    g_pipe.SetDisplayMode(m_setMode.width, m_setMode.height, m_setMode.refresh);
  }
}

void CIndirectDeviceContext::OnUnassignedSwapChain()
{
  InterlockedExchange(&m_replugMonitorQueued, 0);
  InterlockedExchange(&m_recoverModeUpdateSwapChain, 0);

  if (m_replugMonitor)
  {
    m_replugMonitor = false;
    FinishInit(0);
  }
}

void CIndirectDeviceContext::OnSwapChainLost()
{
  // A mode update normally keeps the swap chain alive. If Windows instead
  // reports the existing path disappeared before we see a frame at the new
  // size, recover by scheduling the old replug path from the LGMP timer so we
  // do not tear down the swap chain from one of its worker threads.
  if (!InterlockedCompareExchange(&m_recoverModeUpdateSwapChain, 0, 0))
    return;

  if (InterlockedExchange(&m_replugMonitorQueued, 1))
    return;

  DEBUG_WARN("Swap chain was lost after a mode update, falling back to monitor replug");
}

static inline void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO & mode, DWORD width, DWORD height, DWORD vsync, bool monitorMode)
{
  mode.totalSize.cx = mode.activeSize.cx = width;
  mode.totalSize.cy = mode.activeSize.cy = height;

  mode.AdditionalSignalInfo.vSyncFreqDivider = monitorMode ? 0 : 1;
  mode.AdditionalSignalInfo.videoStandard    = 255;
  
  mode.vSyncFreq.Numerator   = vsync;
  mode.vSyncFreq.Denominator = 1;
  mode.hSyncFreq.Numerator   = vsync * height;
  mode.hSyncFreq.Denominator = 1;

  mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
  mode.pixelRate        = ((UINT64)vsync) * ((UINT64)width) * ((UINT64)height);
}

NTSTATUS CIndirectDeviceContext::ParseMonitorDescription(
  const IDARG_IN_PARSEMONITORDESCRIPTION* inArgs,
  IDARG_OUT_PARSEMONITORDESCRIPTION* outArgs)
{
  outArgs->MonitorModeBufferOutputCount = (UINT)m_displayModes.size();
  outArgs->PreferredMonitorModeIdx = 0;
  if (inArgs->MonitorModeBufferInputCount < (UINT)m_displayModes.size())
    return (inArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;

  auto * mode = inArgs->pMonitorModes;
  for (auto it = m_displayModes.cbegin(); it != m_displayModes.cend(); ++it, ++mode)
  {
    mode->Size = sizeof(IDDCX_MONITOR_MODE);
    mode->Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
    FillSignalInfo(mode->MonitorVideoSignalInfo, it->width, it->height, it->refresh, true);

    if (it->preferred)
      outArgs->PreferredMonitorModeIdx =
        (UINT)std::distance(m_displayModes.cbegin(), it);
  }

  return STATUS_SUCCESS;
}

NTSTATUS CIndirectDeviceContext::MonitorGetDefaultModes(
  const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* inArgs,
  IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* outArgs)
{
  outArgs->DefaultMonitorModeBufferOutputCount = (UINT)m_displayModes.size();
  outArgs->PreferredMonitorModeIdx = 0;
  if (inArgs->DefaultMonitorModeBufferInputCount < (UINT)m_displayModes.size())
    return (inArgs->DefaultMonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;

  auto* mode = inArgs->pDefaultMonitorModes;
  for (auto it = m_displayModes.cbegin(); it != m_displayModes.cend(); ++it, ++mode)
  {
    mode->Size = sizeof(IDDCX_MONITOR_MODE);
    mode->Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
    FillSignalInfo(mode->MonitorVideoSignalInfo, it->width, it->height, it->refresh, true);

    if (it->preferred)
      outArgs->PreferredMonitorModeIdx =
      (UINT)std::distance(m_displayModes.cbegin(), it);
  }

  return STATUS_SUCCESS;
}

NTSTATUS CIndirectDeviceContext::MonitorQueryTargetModes(
  const IDARG_IN_QUERYTARGETMODES* inArgs,
  IDARG_OUT_QUERYTARGETMODES* outArgs)
{
  outArgs->TargetModeBufferOutputCount = (UINT)m_displayModes.size();
  if (inArgs->TargetModeBufferInputCount < (UINT)m_displayModes.size())
    return (inArgs->TargetModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;

  auto* mode = inArgs->pTargetModes;
  for (auto it = m_displayModes.cbegin(); it != m_displayModes.cend(); ++it, ++mode)
  {
    mode->Size = sizeof(IDDCX_TARGET_MODE);
    FillSignalInfo(mode->TargetVideoSignalInfo.targetVideoSignalInfo, it->width, it->height, it->refresh, false);
  }

  return STATUS_SUCCESS;
}


#if defined(IDDCX_VERSION_MAJOR) && defined(IDDCX_VERSION_MINOR) && \
  (IDDCX_VERSION_MAJOR > 1 || (IDDCX_VERSION_MAJOR == 1 && IDDCX_VERSION_MINOR >= 10))
NTSTATUS CIndirectDeviceContext::ParseMonitorDescription2(
  const IDARG_IN_PARSEMONITORDESCRIPTION2* inArgs,
  IDARG_OUT_PARSEMONITORDESCRIPTION* outArgs)
{
  outArgs->MonitorModeBufferOutputCount = (UINT)m_displayModes.size();
  outArgs->PreferredMonitorModeIdx = 0;
  if (inArgs->MonitorModeBufferInputCount < (UINT)m_displayModes.size())
    return (inArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;

  auto * mode = inArgs->pMonitorModes;
  for (auto it = m_displayModes.cbegin(); it != m_displayModes.cend(); ++it, ++mode)
  {
    ZeroMemory(mode, sizeof(*mode));
    mode->Size = sizeof(IDDCX_MONITOR_MODE2);
    mode->Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
    FillSignalInfo(mode->MonitorVideoSignalInfo, it->width, it->height, it->refresh, true);
    mode->BitsPerComponent = GetWireBitsPerComponent(CanUseIddCx110DDIs());

    if (it->preferred)
      outArgs->PreferredMonitorModeIdx =
        (UINT)std::distance(m_displayModes.cbegin(), it);
  }

  return STATUS_SUCCESS;
}

NTSTATUS CIndirectDeviceContext::MonitorQueryTargetModes2(
  const IDARG_IN_QUERYTARGETMODES2* inArgs,
  IDARG_OUT_QUERYTARGETMODES* outArgs)
{
  outArgs->TargetModeBufferOutputCount = (UINT)m_displayModes.size();
  if (inArgs->TargetModeBufferInputCount < (UINT)m_displayModes.size())
    return STATUS_SUCCESS;

  if (!inArgs->pTargetModes)
    return STATUS_INVALID_PARAMETER;

  auto* mode = inArgs->pTargetModes;
  for (auto it = m_displayModes.cbegin(); it != m_displayModes.cend(); ++it, ++mode)
  {
    ZeroMemory(mode, sizeof(*mode));
    mode->Size = sizeof(IDDCX_TARGET_MODE2);
    FillSignalInfo(mode->TargetVideoSignalInfo.targetVideoSignalInfo, it->width, it->height, it->refresh, false);
    mode->BitsPerComponent = GetWireBitsPerComponent(CanUseIddCx110DDIs());
  }

  return STATUS_SUCCESS;
}
#endif

bool CIndirectDeviceContext::UpdateMonitorModes()
{
  if (!m_monitor)
    return false;

#if defined(IDDCX_VERSION_MAJOR) && defined(IDDCX_VERSION_MINOR) && \
  (IDDCX_VERSION_MAJOR > 1 || (IDDCX_VERSION_MAJOR == 1 && IDDCX_VERSION_MINOR >= 10))
  if (CanUseIddCx110DDIs())
  {
    IDDCX_TARGET_MODE2* modes = (IDDCX_TARGET_MODE2*)_malloca(
      m_displayModes.size() * sizeof(IDDCX_TARGET_MODE2));
    if (!modes)
    {
      DEBUG_WARN("Failed to allocate memory for the mode list");
      return false;
    }

    ZeroMemory(modes, m_displayModes.size() * sizeof(IDDCX_TARGET_MODE2));

    auto* mode = modes;
    for (auto it = m_displayModes.cbegin(); it != m_displayModes.cend(); ++it, ++mode)
    {
      mode->Size              = sizeof(IDDCX_TARGET_MODE2);
      mode->RequiredBandwidth = (UINT64)it->width * it->height * it->refresh * 32;
      mode->BitsPerComponent  = GetWireBitsPerComponent(CanUseIddCx110DDIs());
      FillSignalInfo(mode->TargetVideoSignalInfo.targetVideoSignalInfo, it->width, it->height, it->refresh, false);
    }

    IDARG_IN_UPDATEMODES2 updateModes = {};
    updateModes.Reason          = IDDCX_UPDATE_REASON_OTHER;
    updateModes.TargetModeCount = (UINT)m_displayModes.size();
    updateModes.pTargetModes    = modes;

    NTSTATUS status = IddCxMonitorUpdateModes2(m_monitor, &updateModes);
    _freea(modes);
    if (!NT_SUCCESS(status))
    {
      DEBUG_WARN("IddCxMonitorUpdateModes2 Failed (0x%08x)", status);
      return false;
    }

    return true;
  }
#endif

  IDDCX_TARGET_MODE* modes = (IDDCX_TARGET_MODE*)_malloca(
    m_displayModes.size() * sizeof(IDDCX_TARGET_MODE));
  if (!modes)
  {
    DEBUG_WARN("Failed to allocate memory for the mode list");
    return false;
  }

  ZeroMemory(modes, m_displayModes.size() * sizeof(IDDCX_TARGET_MODE));

  auto* mode = modes;
  for (auto it = m_displayModes.cbegin(); it != m_displayModes.cend(); ++it, ++mode)
  {
    mode->Size              = sizeof(IDDCX_TARGET_MODE);
    mode->RequiredBandwidth = (UINT64)it->width * it->height * it->refresh * 32;
    FillSignalInfo(mode->TargetVideoSignalInfo.targetVideoSignalInfo, it->width, it->height, it->refresh, false);
  }

  IDARG_IN_UPDATEMODES updateModes = {};
  updateModes.Reason          = IDDCX_UPDATE_REASON_OTHER;
  updateModes.TargetModeCount = (UINT)m_displayModes.size();
  updateModes.pTargetModes    = modes;

  NTSTATUS status = IddCxMonitorUpdateModes(m_monitor, &updateModes);
  _freea(modes);
  if (!NT_SUCCESS(status))
  {
    DEBUG_WARN("IddCxMonitorUpdateModes Failed (0x%08x)", status);
    return false;
  }

  return true;
}

void CIndirectDeviceContext::SetResolution(int width, int height)
{
  m_setMode.width     = width;
  m_setMode.height    = height;
  m_setMode.refresh   = g_settings.GetDefaultRefresh();
  m_setMode.preferred = true;
  g_settings.SetExtraMode(m_setMode);

  PopulateDefaultModes();

  if (UpdateMonitorModes())
  {
    DEBUG_TRACE("Updated monitor modes without replugging");
    m_doSetMode = false;
    InterlockedExchange(&m_recoverModeUpdateSwapChain, 1);
    g_pipe.SetDisplayMode(m_setMode.width, m_setMode.height, m_setMode.refresh);
    return;
  }

  DEBUG_TRACE("Falling back to monitor replug for mode update");
  m_doSetMode = true;
  InterlockedExchange(&m_recoverModeUpdateSwapChain, 0);
  ReplugMonitor();
}

// ---------------------------------------------------------------------------
// M1 frame/cursor sink — stubbed no-ops.
//
// Upstream LGIdd published frames + cursor shapes into an IVSHMEM-backed LGMP
// ring for the Looking Glass host. The Qubes port drops IVSHMEM/LGMP entirely;
// frames will instead be granted to dom0 via the Xen grant API (M2+). None of
// the methods below are reached at M1: their only callers live in the gutted
// CSwapChainProcessor, whose frame path is removed. They are retained as no-ops
// so the CIndirectDeviceContext interface stays stable for M2.
// ---------------------------------------------------------------------------

bool CIndirectDeviceContext::SetupLGMP(size_t alignSize)
{
  m_alignSize = alignSize;
  return true;
}

void CIndirectDeviceContext::DeInitLGMP()
{
}

void CIndirectDeviceContext::LGMPTimer()
{
}

CIndirectDeviceContext::PreparedFrameBuffer CIndirectDeviceContext::PrepareFrameBuffer(
  unsigned pitch, const RECT * dirtyRects, unsigned nbDirtyRects)
{
  UNREFERENCED_PARAMETER(pitch);
  UNREFERENCED_PARAMETER(dirtyRects);
  UNREFERENCED_PARAMETER(nbDirtyRects);
  PreparedFrameBuffer result = {};
  return result;
}

void CIndirectDeviceContext::WriteFrameBuffer(unsigned frameIndex, void* src, size_t offset, size_t len, bool setWritePos) const
{
  UNREFERENCED_PARAMETER(frameIndex);
  UNREFERENCED_PARAMETER(src);
  UNREFERENCED_PARAMETER(offset);
  UNREFERENCED_PARAMETER(len);
  UNREFERENCED_PARAMETER(setWritePos);
}

void CIndirectDeviceContext::FinalizeFrameBuffer(unsigned frameIndex) const
{
  UNREFERENCED_PARAMETER(frameIndex);
}

void CIndirectDeviceContext::SendCursor(const IDARG_OUT_QUERY_HWCURSOR& info, const BYTE * data)
{
  UNREFERENCED_PARAMETER(info);
  UNREFERENCED_PARAMETER(data);
}

void CIndirectDeviceContext::ResendCursor() const
{
}
