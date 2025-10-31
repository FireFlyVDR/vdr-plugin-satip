/*
 * config.c: SAT>IP plugin for the Video Disk Recorder
 *
 * See the README file for copyright information and how to reach the author.
 *
 */

#include "discover.h"
#include "log.h"
#include "config.h"

cSatipConfig SatipConfig;

cSatipConfig::cSatipConfig(void)
: operatingModeM(eOperatingModeLow),
  traceModeM(eTraceModeNormal),
  ciExtensionM(0),
  frontendReuseM(1),
  eitScanM(1),
  useBytesM(1),
  portRangeStartM(0),
  portRangeStopM(0),
  transportModeM(eTransportModeUnicast),
  detachedModeM(false),
  disableServerQuirksM(false),
  disconnectIdleStreams(true),
  useSingleModelServersM(false),
  rtpRcvBufSizeM(0)
{
  for (unsigned int i = 0; i < MAX_CICAM_COUNT; ++i)
     for (unsigned int j = 0; j <= MAX_CAID_COUNT; ++j)
        providedCAIds[i][j] = 0;
  for (unsigned int i = 0; i < ELEMENTS(ciAssignedDevice); ++i)
      ciAssignedDevice[i] = 0;
  for (unsigned int i = 0; i < ELEMENTS(disabledSourcesM); ++i)
      disabledSourcesM[i] = cSource::stNone;
  for (unsigned int i = 0; i < ELEMENTS(disabledFiltersM); ++i)
      disabledFiltersM[i] = -1;
}

int cSatipConfig::GetCAID(unsigned int camIndex, unsigned int CAIDIndex) const
{
  return (camIndex < MAX_CICAM_COUNT ? (CAIDIndex < MAX_CAID_COUNT ? providedCAIds[camIndex][CAIDIndex] : 0) : 0);
}

cString cSatipConfig::GetCAIDList(unsigned int CamIndex) const
{
   cString caIdList = "";
   if (CamIndex < MAX_CICAM_COUNT)
      for (int i = 0; providedCAIds[CamIndex][i]; i++)
         caIdList.Append(cString::sprintf("%s0x%04X", i > 0 ? ", " : "", providedCAIds[CamIndex][i]));

   return caIdList;
}

void cSatipConfig::SetCAID(unsigned int camIndex, unsigned int CAIDIndex, int CAID)
{
  if (camIndex < MAX_CICAM_COUNT && CAIDIndex < MAX_CAID_COUNT)
    providedCAIds[camIndex][CAIDIndex] = CAID;
}


int cSatipConfig::GetCIAssignedDevice(unsigned int indexP) const
{
  return (indexP < ELEMENTS(ciAssignedDevice)) ? ciAssignedDevice[indexP] : -1;
}

void cSatipConfig::SetCIAssignedDevice(unsigned int indexP, int DeviceIndex)
{
  if (indexP < ELEMENTS(ciAssignedDevice))
     ciAssignedDevice[indexP] = DeviceIndex;
}

unsigned int cSatipConfig::GetDisabledSourcesCount(void) const
{
  unsigned int n = 0;
  while ((n < ELEMENTS(disabledSourcesM) && (disabledSourcesM[n] != cSource::stNone)))
        n++;
  return n;
}

int cSatipConfig::GetDisabledSources(unsigned int indexP) const
{
  return (indexP < ELEMENTS(disabledSourcesM)) ? disabledSourcesM[indexP] : cSource::stNone;
}

void cSatipConfig::SetDisabledSources(unsigned int indexP, int sourceP)
{
  if (indexP < ELEMENTS(disabledSourcesM))
     disabledSourcesM[indexP] = sourceP;
}

unsigned int cSatipConfig::GetDisabledFiltersCount(void) const
{
  unsigned int n = 0;
  while ((n < ELEMENTS(disabledFiltersM) && (disabledFiltersM[n] != -1)))
        n++;
  return n;
}

int cSatipConfig::GetDisabledFilters(unsigned int indexP) const
{
  return (indexP < ELEMENTS(disabledFiltersM)) ? disabledFiltersM[indexP] : -1;
}

void cSatipConfig::SetDisabledFilters(unsigned int indexP, int numberP)
{
  if (indexP < ELEMENTS(disabledFiltersM))
     disabledFiltersM[indexP] = numberP;
}
