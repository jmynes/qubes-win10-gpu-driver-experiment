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

#include "CD3D11Device.h"
#include "CIndirectDeviceContext.h"

#include <Windows.h>
#include <wrl.h>
#include <IddCx.h>
#include <memory>

using namespace Microsoft::WRL;

class CSwapChainProcessor
{
private:
  IDDCX_MONITOR                   m_monitor;
  CIndirectDeviceContext        * m_devContext;
  IDDCX_SWAPCHAIN                 m_hSwapChain;
  std::shared_ptr<CD3D11Device>   m_dx11Device;
  HANDLE                          m_newFrameEvent;

  Wrappers::HandleT<Wrappers::HandleTraits::HANDLENullTraits> m_thread[1];
  Wrappers::Event m_terminateEvent;

  static DWORD CALLBACK _SwapChainThread(LPVOID arg);
  void SwapChainThread();
  void SwapChainThreadCore();

  bool SwapChainNewFrame(ComPtr<IDXGIResource> acquiredBuffer, unsigned dirtyRectCount);

public:
  CSwapChainProcessor(IDDCX_MONITOR monitor, CIndirectDeviceContext * devContext, IDDCX_SWAPCHAIN hSwapChain,
    std::shared_ptr<CD3D11Device> dx11Device, HANDLE newFrameEvent);
  ~CSwapChainProcessor();

  CIndirectDeviceContext * GetDevice() { return m_devContext; }
};
