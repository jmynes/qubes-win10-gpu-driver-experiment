/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "CEdid.h"

#include <algorithm>
#include <string.h>

static const UINT EDID_BLOCK_SIZE = 128;
static const UINT EDID_DTD_SIZE   = 18;

static const UINT EDID_STANDARD_TIMING_COUNT              = 8;
static const UINT EDID_BASE_DESCRIPTOR_COUNT              = 4;
static const UINT EDID_BASE_DETAILED_TIMING_COUNT         = 3;
static const UINT EDID_BASE_MONITOR_NAME_DESCRIPTOR_INDEX = 3;

static const BYTE EDID_HEADER[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };

static const WORD EDID_MANUFACTURER_ID_QBS = 0x5344; // PNP id "QBS" (Qubes)
static const WORD EDID_PRODUCT_CODE        = 0x1ddd;
static const BYTE EDID_SERIAL_NUMBER[4]    = { 0x01, 0x00, 0x00, 0x00 };

static const BYTE EDID_MANUFACTURE_WEEK      = 1;
static const BYTE EDID_MANUFACTURE_YEAR_2026 = 36; // 1990 + 36 = 2026
static const BYTE EDID_VERSION               = 1;
static const BYTE EDID_REVISION              = 4;

static const BYTE EDID_VIDEO_INPUT_DIGITAL_10BPC_HDMI_A = 0xb2;
static const BYTE EDID_HORIZONTAL_SIZE_CM               = 52;
static const BYTE EDID_VERTICAL_SIZE_CM                 = 29;
static const BYTE EDID_DISPLAY_GAMMA_2_2                = 0x78;
static const BYTE EDID_FEATURES_PREFERRED_TIMING_RGB    = 0x0a;

static const BYTE EDID_STANDARD_TIMING_UNUSED_X          = 0x01;
static const BYTE EDID_STANDARD_TIMING_UNUSED_AR_REFRESH = 0x01;

static const DWORD EDID_IMAGE_WIDTH_MM  = 520;
static const DWORD EDID_IMAGE_HEIGHT_MM = 290;

static const BYTE EDID_DESCRIPTOR_MONITOR_NAME                  = 0xfc;
static const BYTE EDID_DTD_FLAGS_DIGITAL_SEPARATE_SYNC_POSITIVE = 0x1e;

#pragma pack(push, 1)
struct EdidLe16
{
  BYTE lo;
  BYTE hi;
};

struct EdidStandardTiming
{
  BYTE horizontalActivePixels;
  BYTE aspectRatioAndRefreshRate;
};

struct EdidDetailedTimingDescriptor
{
  EdidLe16 pixelClock10KHz;

  BYTE hActiveLo;
  BYTE hBlankLo;
  BYTE hActiveBlankHi;

  BYTE vActiveLo;
  BYTE vBlankLo;
  BYTE vActiveBlankHi;

  BYTE hFrontPorchLo;
  BYTE hSyncPulseWidthLo;
  BYTE vFrontPorchSyncPulseWidthLo;
  BYTE syncPorchPulseWidthHi;

  BYTE imageWidthMmLo;
  BYTE imageHeightMmLo;
  BYTE imageSizeMmHi;

  BYTE hBorder;
  BYTE vBorder;
  BYTE flags;
};

struct EdidMonitorNameDescriptor
{
  EdidLe16 pixelClock;
  BYTE reserved0;
  BYTE descriptorTag;
  BYTE reserved1;
  char name[13];
};

union EdidDescriptor
{
  EdidDetailedTimingDescriptor detailedTiming;
  EdidMonitorNameDescriptor monitorName;
  BYTE raw[EDID_DTD_SIZE];
};

struct EdidBaseBlock
{
  BYTE header[8];

  EdidLe16 manufacturerId;
  EdidLe16 productCode;
  BYTE serialNumber[4];

  BYTE manufactureWeek;
  BYTE manufactureYear;
  BYTE version;
  BYTE revision;

  BYTE videoInputDefinition;
  BYTE horizontalSizeCm;
  BYTE verticalSizeCm;
  BYTE displayGamma;
  BYTE supportedFeatures;

  BYTE chromaticityCoordinates[10];
  BYTE establishedTimings[3];
  EdidStandardTiming standardTimings[EDID_STANDARD_TIMING_COUNT];

  EdidDescriptor descriptors[EDID_BASE_DESCRIPTOR_COUNT];

  BYTE extensionBlockCount;
  BYTE checksum;
};
#pragma pack(pop)

static_assert(sizeof(EdidLe16) == 2, "Unexpected EDID little-endian word size");
static_assert(sizeof(EdidStandardTiming) == 2, "Unexpected EDID standard timing size");
static_assert(sizeof(EdidDetailedTimingDescriptor) == EDID_DTD_SIZE,
  "Unexpected EDID detailed timing descriptor size");
static_assert(sizeof(EdidMonitorNameDescriptor) == EDID_DTD_SIZE,
  "Unexpected EDID monitor name descriptor size");
static_assert(sizeof(EdidDescriptor) == EDID_DTD_SIZE,
  "Unexpected EDID descriptor size");
static_assert(sizeof(EdidBaseBlock) == EDID_BLOCK_SIZE,
  "Unexpected EDID base block size");

static void SetLe16(EdidLe16& dst, DWORD value)
{
  dst.lo = (BYTE)(value & 0xff);
  dst.hi = (BYTE)((value >> 8) & 0xff);
}

static BYTE Lo8(DWORD value)
{
  return (BYTE)(value & 0xff);
}

static BYTE PackMsbNibbles(DWORD upperValue, DWORD lowerValue)
{
  return (BYTE)((((upperValue >> 8) & 0x0f) << 4) |
    ((lowerValue >> 8) & 0x0f));
}

static BYTE PackLowNibbles(DWORD upperValue, DWORD lowerValue)
{
  return (BYTE)(((upperValue & 0x0f) << 4) |
    (lowerValue & 0x0f));
}

static BYTE PackSyncPorchPulseWidthHi(
  DWORD hFrontPorch,
  DWORD hSyncPulseWidth,
  DWORD vFrontPorch,
  DWORD vSyncPulseWidth)
{
  return (BYTE)(
    (((hFrontPorch     >> 8) & 0x03) << 6) |
    (((hSyncPulseWidth >> 8) & 0x03) << 4) |
    (((vFrontPorch     >> 4) & 0x03) << 2) |
    ((vSyncPulseWidth  >> 4) & 0x03));
}

static EdidStandardTiming MakeUnusedStandardTiming()
{
  EdidStandardTiming timing = {};
  timing.horizontalActivePixels    = EDID_STANDARD_TIMING_UNUSED_X;
  timing.aspectRatioAndRefreshRate = EDID_STANDARD_TIMING_UNUSED_AR_REFRESH;
  return timing;
}

static void InitEdidBaseBlock(EdidBaseBlock& base)
{
  memcpy(base.header, EDID_HEADER, sizeof(base.header));

  // Manufacturer ID: QBS (Qubes), product/serial values are arbitrary.
  SetLe16(base.manufacturerId, EDID_MANUFACTURER_ID_QBS);
  SetLe16(base.productCode, EDID_PRODUCT_CODE);
  memcpy(base.serialNumber, EDID_SERIAL_NUMBER, sizeof(base.serialNumber));

  base.manufactureWeek = EDID_MANUFACTURE_WEEK;
  base.manufactureYear = EDID_MANUFACTURE_YEAR_2026;
  base.version         = EDID_VERSION;
  base.revision        = EDID_REVISION;

  base.videoInputDefinition = EDID_VIDEO_INPUT_DIGITAL_10BPC_HDMI_A;
  base.horizontalSizeCm     = EDID_HORIZONTAL_SIZE_CM;
  base.verticalSizeCm       = EDID_VERTICAL_SIZE_CM;
  base.displayGamma         = EDID_DISPLAY_GAMMA_2_2;
  base.supportedFeatures    = EDID_FEATURES_PREFERRED_TIMING_RGB;

  for (UINT i = 0; i < EDID_STANDARD_TIMING_COUNT; ++i)
    base.standardTimings[i] = MakeUnusedStandardTiming();

  base.extensionBlockCount = 0;
}

static bool MakeDetailedTiming(
  EdidDetailedTimingDescriptor& timing,
  const CSettings::DisplayMode& mode)
{
  memset(&timing, 0, sizeof(timing));

  const DWORD hActive = mode.width;
  const DWORD vActive = mode.height;
  const DWORD refresh = mode.refresh;

  if (hActive == 0 || vActive == 0 || refresh == 0 ||
    hActive > 4095 || vActive > 4095)
    return false;

  DWORD hBlank = std::max<DWORD>(160, ((hActive / 20) + 7) & ~7UL);
  DWORD vBlank = std::max<DWORD>(30, vActive / 20);
  if (hBlank > 4095 || vBlank > 4095)
    return false;

  DWORD hSync = std::max<DWORD>(32, hActive / 100);
  hSync = (hSync + 7) & ~7UL;

  DWORD hFront = std::max<DWORD>(48, hBlank / 3);
  hFront = (hFront + 7) & ~7UL;

  if (hFront + hSync >= hBlank)
  {
    hFront = 48;
    hSync = 32;
  }

  const DWORD vFront = 3;
  const DWORD vSync = 5;
  if (vFront + vSync >= vBlank)
    return false;

  const UINT64 pixelClock = (UINT64)(hActive + hBlank) *
    (UINT64)(vActive + vBlank) * (UINT64)refresh;
  const UINT64 pixelClock10KHz = (pixelClock + 5000) / 10000;
  if (pixelClock10KHz == 0 || pixelClock10KHz > 0xffff)
    return false;

  SetLe16(timing.pixelClock10KHz, (DWORD)pixelClock10KHz);

  timing.hActiveLo      = Lo8(hActive);
  timing.hBlankLo       = Lo8(hBlank);
  timing.hActiveBlankHi = PackMsbNibbles(hActive, hBlank);

  timing.vActiveLo      = Lo8(vActive);
  timing.vBlankLo       = Lo8(vBlank);
  timing.vActiveBlankHi = PackMsbNibbles(vActive, vBlank);

  timing.hFrontPorchLo               = Lo8(hFront);
  timing.hSyncPulseWidthLo           = Lo8(hSync);
  timing.vFrontPorchSyncPulseWidthLo = PackLowNibbles(vFront, vSync);
  timing.syncPorchPulseWidthHi       = PackSyncPorchPulseWidthHi(hFront, hSync, vFront, vSync);

  timing.imageWidthMmLo  = Lo8(EDID_IMAGE_WIDTH_MM);
  timing.imageHeightMmLo = Lo8(EDID_IMAGE_HEIGHT_MM);
  timing.imageSizeMmHi   = PackMsbNibbles(EDID_IMAGE_WIDTH_MM, EDID_IMAGE_HEIGHT_MM);

  timing.flags = EDID_DTD_FLAGS_DIGITAL_SEPARATE_SYNC_POSITIVE;
  return true;
}

static void MakeMonitorName(
  EdidMonitorNameDescriptor& monitorName,
  const char* name)
{
  memset(&monitorName, 0, sizeof(monitorName));

  monitorName.descriptorTag = EDID_DESCRIPTOR_MONITOR_NAME;

  UINT len = 0;
  for (; len < sizeof(monitorName.name) && name[len]; ++len)
    monitorName.name[len] = name[len];

  if (len < sizeof(monitorName.name))
    monitorName.name[len++] = '\n';

  for (; len < sizeof(monitorName.name); ++len)
    monitorName.name[len] = ' ';
}

void CEdid::SetChecksum(BYTE* block)
{
  BYTE sum = 0;
  for (UINT i = 0; i < EDID_BLOCK_SIZE - 1; ++i)
    sum = (BYTE)(sum + block[i]);

  block[EDID_BLOCK_SIZE - 1] = (BYTE)(0 - sum);
}

void CEdid::WriteMonitorName(BYTE* desc, const char* name)
{
  EdidMonitorNameDescriptor monitorName = {};
  MakeMonitorName(monitorName, name);
  memcpy(desc, &monitorName, sizeof(monitorName));
}

bool CEdid::WriteDetailedTiming(BYTE* dtd, const CSettings::DisplayMode& mode)
{
  EdidDetailedTimingDescriptor timing = {};

  if (!MakeDetailedTiming(timing, mode))
    return false;

  memcpy(dtd, &timing, sizeof(timing));
  return true;
}

void CEdid::Build(const CSettings::DisplayModes& modes)
{
  // M1: base block + DTDs only. The CTA-861 extension block (HDR / colorimetry /
  // HDMI VSDB) is stripped, so the produced EDID is a single 128-byte block with
  // no extensions.
  m_data.assign(static_cast<std::vector<BYTE, std::allocator<BYTE>>::size_type>(EDID_BLOCK_SIZE), 0);

  EdidBaseBlock baseBlock = {};
  InitEdidBaseBlock(baseBlock);

  CSettings::DisplayModes sorted = modes;
  std::stable_sort(sorted.begin(), sorted.end(),
    [](const CSettings::DisplayMode& a, const CSettings::DisplayMode& b)
    {
      if (a.preferred != b.preferred)
        return a.preferred && !b.preferred;
      if (a.width != b.width)
        return a.width > b.width;
      if (a.height != b.height)
        return a.height > b.height;
      return a.refresh > b.refresh;
    });

  UINT modeIndex    = 0;
  UINT baseDtdIndex = 0;

  for (; modeIndex < sorted.size() &&
    baseDtdIndex < EDID_BASE_DETAILED_TIMING_COUNT;
    ++modeIndex)
  {
    if (MakeDetailedTiming(
      baseBlock.descriptors[baseDtdIndex].detailedTiming,
      sorted[modeIndex]))
    {
      ++baseDtdIndex;
    }
  }

  MakeMonitorName(
    baseBlock.descriptors[EDID_BASE_MONITOR_NAME_DESCRIPTOR_INDEX].monitorName,
    "Qubes");

  SetChecksum(reinterpret_cast<BYTE*>(&baseBlock));
  memcpy(m_data.data(), &baseBlock, sizeof(baseBlock));
}