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

#include "CPlatformInfo.h"

#include <Windows.h>

// M1 strip: only GetPageSize() survives (used by CIndirectMonitorContext). The
// UUID / CPU-name / product-name gathering fed only the LGMP VMInfo block, which
// is dropped from the Qubes port — so it (and its SMBIOS/registry helpers and the
// CDebug dependency) are removed here.

size_t CPlatformInfo::m_pageSize = 0;

void CPlatformInfo::Init()
{
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  m_pageSize = (size_t)si.dwPageSize;
}
