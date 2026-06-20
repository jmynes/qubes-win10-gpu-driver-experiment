/* qubes-win10-gpu-driver-experiment -- M1: WPP tracing disabled (no-op shim).
 * LGIdd's real logging is CDebug (DEBUG_*); the WPP scaffolding inherited from
 * the IddSampleDriver template is stubbed so no .tmh generation is required.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#define WPP_INIT_TRACING(...) ((void)0)
#define WPP_CLEANUP(...)      ((void)0)
#define TraceEvents(...)      ((void)0)
