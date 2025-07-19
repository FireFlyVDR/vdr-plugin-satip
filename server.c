/*
 * server.c: SAT>IP plugin for the Video Disk Recorder
 *
 * See the README file for copyright information and how to reach the author.
 *
 */

#include <vdr/sources.h>

#include "config.h"
#include "common.h"
#include "log.h"
#include "server.h"

// --- cSatipFrontends --------------------------------------------------------

cSatipFrontends::cSatipFrontends(void)
{
   numDevices = 0;
}

bool cSatipFrontends::Init(const char *Type, int NumDevices)
{
   numDevices = NumDevices;
   type = Type;
   for(int i = 0; i < numDevices; i++) {
      devicesAssigned[i] = -1;
      devicesAttached[i] = false;
   }
   return true;
}

bool cSatipFrontends::Assign(int deviceIdP)
{
   int nextFree = -1;
   for(int i = 0; i < numDevices; i++) {
      if (devicesAssigned[i] == deviceIdP) {
         LogAssignments("Assign", true, deviceIdP);
         return true;
      }
      else if (nextFree == -1 && devicesAssigned[i] == -1)
         nextFree = i;
   }
   if (nextFree > -1) {
      devicesAssigned[nextFree] = deviceIdP;
      LogAssignments("Assign", true, deviceIdP);
      return true;
   }
   LogAssignments("Assign", false, deviceIdP);
   return false;
}

bool cSatipFrontends::Attach(int deviceIdP)
{ //
   for(int i = 0; i < numDevices; i++) {
      if (devicesAssigned[i] == deviceIdP) {
         devicesAttached[i] = true;
         LogAssignments("Attach", true, deviceIdP);
         return true;
      }
   }
   LogAssignments("Attach", false, deviceIdP);
   return false;
}

bool cSatipFrontends::Detach(int deviceIdP)
{ //
   for(int i = 0; i < numDevices; i++) {
      if (devicesAssigned[i] == deviceIdP) {
         devicesAttached[i] = false;
         devicesAssigned[i] = -1;
         LogAssignments("Detach", true, deviceIdP);
         return true;
      }
   }
   LogAssignments("Detach", false, deviceIdP);
   return false;
}

void cSatipFrontends::LogAssignments(const char* Func, bool Result, int deviceIdP)
{
   if (numDevices) {
      cString deviceList = cString::sprintf("cSatipFrontends::%s(deviceId=%d) deviceId list %s-%d %s:", Func, deviceIdP, *type, numDevices, Result?"T":"F");
      for(int i = 0; i < numDevices; i++) {
         deviceList.Append(cString::sprintf(" %d%c", devicesAssigned[i], devicesAttached[i]?'A':'D'));
      }
      isyslog(*deviceList);
   }
}

// --- cSatipServer -----------------------------------------------------------

cSatipServer::cSatipServer(const char *srcAddressP, const char *addressP, const int portP, const char *modelP, const char *filtersP, const char *descriptionP, const int quirkP)
: srcAddressM((srcAddressP && *srcAddressP) ? srcAddressP : ""),
  addressM((addressP && *addressP) ? addressP : "0.0.0.0"),
  modelM((modelP && *modelP) ? modelP : "DVBS-1"),
  filtersM((filtersP && *filtersP) ? filtersP : ""),
  descriptionM(!isempty(descriptionP) ? descriptionP : "MyBrokenHardware"),
  quirksM(""),
  portM(portP),
  quirkM(quirkP),
  hasCiM(false),
  activeM(true),
  createdM(time(NULL)),
  lastSeenM(0)
{
  memset(sourceFiltersM, 0, sizeof(sourceFiltersM));
  if (!isempty(*filtersM)) {
     char *s, *p = strdup(*filtersM);
     char *r = strtok_r(p, ",", &s);
     unsigned int i = 0;
     while (r) {
           int t = cSource::FromString(skipspace(r));
           if (t && i < ELEMENTS(sourceFiltersM))
              sourceFiltersM[i++] = t;
           r = strtok_r(NULL, ",", &s);
           }
     if (i) {
        filtersM = "";
        for (unsigned int j = 0; j < i; ++j)
            filtersM = cString::sprintf("%s%s%s", *filtersM, isempty(*filtersM) ? "" : ",", *cSource::ToString(sourceFiltersM[j]));
        debug3("%s filters=%s", __PRETTY_FUNCTION__, *filtersM);
        }
     FREE_POINTER(p);
     }
  if (!SatipConfig.GetDisableServerQuirks()) {
     // These devices contain a session id bug:
     // Inverto Airscreen Server IDL 400 ?
     // Elgato EyeTV Netstream 4Sat ?
     if (strstr(*descriptionM, "GSSBOX") ||                    // Grundig Sat Systems GSS.box DSI 400
         strstr(*descriptionM, "DIGIBIT") ||                   // Telestar Digibit R1
         strstr(*descriptionM, "Multibox-") ||                 // Inverto IDL-400s: Multibox-<MMAACC>:SAT>IP
         strstr(*descriptionM, "Triax SatIP Converter")        // Triax TSS 400
        )
        quirkM |= eSatipQuirkSessionId;
     // These devices contain support for RTP over TCP:
     if (strstr(*descriptionM, "minisatip") ||                 // minisatip server
         strstr(*descriptionM, "DVBViewer")                    // DVBViewer Media Server
        )
        quirkM |= eSatipQuirkRtpOverTcp;
     // These devices contain a play (add/delpids) parameter bug:
     if (strstr(*descriptionM, "FRITZ!WLAN Repeater DVB-C") || // FRITZ!WLAN Repeater DVB-C
         strstr(*descriptionM, "fritzdvbc")                    // FRITZ!WLAN Repeater DVB-C (old firmware)
        )
        quirkM |= eSatipQuirkPlayPids;
     // These devices contain a frontend locking bug:
     if (strstr(*descriptionM, "FRITZ!WLAN Repeater DVB-C") || // FRITZ!WLAN Repeater DVB-C
         strstr(*descriptionM, "fritzdvbc") ||                 // FRITZ!WLAN Repeater DVB-C (old firmware)
         strstr(*descriptionM, "Schwaiger Sat>IP Server")      // Schwaiger MS41IP
        )
        quirkM |= eSatipQuirkForceLock;
     // These devices support the X_PMT protocol extension:
     if (strstr(*descriptionM, "OctopusNet") ||                // Digital Devices OctopusNet
         strstr(*descriptionM, "minisatip")                    // minisatip server
        )
        quirkM |= eSatipQuirkCiXpmt;
     // These devices support the TNR protocol extension:
     if (strstr(*descriptionM, "DVBViewer")                    // DVBViewer Media Server
        )
        quirkM |= eSatipQuirkCiTnr;
     // These devices don't support auto-detection of pilot tones:
     if (strstr(*descriptionM, "GSSBOX") ||                    // Grundig Sat Systems GSS.box DSI 400
         strstr(*descriptionM, "DIGIBIT") ||                   // Telestar Digibit R1
         strstr(*descriptionM, "Multibox-") ||                 // Inverto IDL-400s: Multibox-<MMAACC>:SAT>IP
         strstr(*descriptionM, "Triax SatIP Converter") ||     // Triax TSS 400
         strstr(*descriptionM, "KATHREIN SatIP Server")        // Kathrein ExIP 414/E
        )
        quirkM |= eSatipQuirkForcePilot;
     // These devices require TEARDOWN before new PLAY command:
     if (strstr(*descriptionM, "FRITZ!WLAN Repeater DVB-C") || // FRITZ!WLAN Repeater DVB-C
         strstr(*descriptionM, "fritzdvbc")                    // FRITZ!WLAN Repeater DVB-C (old firmware)
        )
        quirkM |= eSatipQuirkTearAndPlay;
     }
  if ((quirkM & eSatipQuirkMask) & eSatipQuirkSessionId)
     quirksM = cString::sprintf("%s%sSessionId", *quirksM, isempty(*quirksM) ? "" : ",");
  if ((quirkM & eSatipQuirkMask) & eSatipQuirkPlayPids)
     quirksM = cString::sprintf("%s%sPlayPids", *quirksM, isempty(*quirksM) ? "" : ",");
  if ((quirkM & eSatipQuirkMask) & eSatipQuirkForceLock)
     quirksM = cString::sprintf("%s%sForceLock", *quirksM, isempty(*quirksM) ? "" : ",");
  if ((quirkM & eSatipQuirkMask) & eSatipQuirkRtpOverTcp)
     quirksM = cString::sprintf("%s%sRtpOverTcp", *quirksM, isempty(*quirksM) ? "" : ",");
  if ((quirkM & eSatipQuirkMask) & eSatipQuirkCiXpmt)
     quirksM = cString::sprintf("%s%sCiXpmt", *quirksM, isempty(*quirksM) ? "" : ",");
  if ((quirkM & eSatipQuirkMask) & eSatipQuirkCiTnr)
     quirksM = cString::sprintf("%s%sCiTnr", *quirksM, isempty(*quirksM) ? "" : ",");
  if ((quirkM & eSatipQuirkMask) & eSatipQuirkForcePilot)
     quirksM = cString::sprintf("%s%sForcePilot", *quirksM, isempty(*quirksM) ? "" : ",");
  debug3("%s description=%s quirks=%s", __PRETTY_FUNCTION__, *descriptionM, *quirksM);
  // These devices support external CI
  if (strstr(*descriptionM, "OctopusNet") ||            // Digital Devices OctopusNet
      strstr(*descriptionM, "minisatip") ||             // minisatip server
      strstr(*descriptionM, "DVBViewer")                // DVBViewer Media Server
     ) {
     hasCiM = true;
     }
  char *s, *p = strdup(*modelM);
  char *r = strtok_r(p, ",", &s);
  while (r) {
        char *c;
        if (c = strstr(r, "DVBS2-")) {
           int count = atoi(c + 6);
           frontends[eSatipFrontendDVBS2].Init("DVBS2", count);
           }
        else if (c = strstr(r, "DVBT-")) {
           int count = atoi(c + 5);
           frontends[eSatipFrontendDVBT].Init("DVBT", count);
           }
        else if (c = strstr(r, "DVBT2-")) {
           int count = atoi(c + 6);
           frontends[eSatipFrontendDVBT2].Init("DVBT2", count);
           }
        else if (c = strstr(r, "DVBC-")) {
           int count = atoi(c + 5);
           frontends[eSatipFrontendDVBC].Init("DVBC", count);
           }
        else if (c = strstr(r, "DVBC2-")) {
           int count = atoi(c + 6);
           frontends[eSatipFrontendDVBC2].Init("DVBC2", count);
           }
        else if (c = strstr(r, "ATSC-")) {
           int count = atoi(c + 5);
           frontends[eSatipFrontendATSC].Init("ATSC", count);
           }
        r = strtok_r(NULL, ",", &s);
        }
  FREE_POINTER(p);
}

cSatipServer::~cSatipServer()
{
}

int cSatipServer::Compare(const cListObject &listObjectP) const
{
  const cSatipServer *s = (const cSatipServer *)&listObjectP;
  int result = strcasecmp(*addressM, *s->addressM);
  if (!result) {
     result = strcasecmp(*modelM, *s->modelM);
     if (!result)
        result = strcasecmp(*descriptionM, *s->descriptionM);
     }
  return result;
}

bool cSatipServer::IsValidSource(int sourceP)
{
  if (sourceFiltersM[0]) {
     for (unsigned int i = 0; i < ELEMENTS(sourceFiltersM); ++i) {
         if (sourceP == sourceFiltersM[i]) {
            return true;
            }
         }
     return false;
     }
  return true;
}

bool cSatipServer::Assign(int deviceIdP, int sourceP, int systemP)
{
  bool result = false;
  if (IsValidSource(sourceP)) {
     if (cSource::IsType(sourceP, 'S'))
        result = frontends[eSatipFrontendDVBS2].Assign(deviceIdP);
     else if (cSource::IsType(sourceP, 'T')) {
        if (systemP)
           result = frontends[eSatipFrontendDVBT2].Assign(deviceIdP);
        else
           result = frontends[eSatipFrontendDVBT].Assign(deviceIdP) || frontends[eSatipFrontendDVBT2].Assign(deviceIdP);
        }
     else if (cSource::IsType(sourceP, 'C')) {
        if (systemP)
           result = frontends[eSatipFrontendDVBC2].Assign(deviceIdP);
        else
           result = frontends[eSatipFrontendDVBC].Assign(deviceIdP) || frontends[eSatipFrontendDVBC2].Assign(deviceIdP);
        }
     else if (cSource::IsType(sourceP, 'A'))
        result = frontends[eSatipFrontendATSC].Assign(deviceIdP);
     }
  return result;
}

bool cSatipServer::Matches(int sourceP)
{
  if (IsValidSource(sourceP)) {
     if (cSource::IsType(sourceP, 'S'))
        return GetModulesDVBS2();
     else if (cSource::IsType(sourceP, 'T'))
        return GetModulesDVBT() || GetModulesDVBT2();
     else if (cSource::IsType(sourceP, 'C'))
        return GetModulesDVBC() || GetModulesDVBC2();
     else if (cSource::IsType(sourceP, 'A'))
        return GetModulesATSC();
     }
  return false;
}

void cSatipServer::Attach(int deviceIdP)
{
  for (int i = 0; i < eSatipFrontendCount; ++i) {
      if (frontends[i].Attach(deviceIdP))
         return;
      }
}

void cSatipServer::Detach(int deviceIdP)
{
  for (int i = 0; i < eSatipFrontendCount; ++i) {
      if (frontends[i].Detach(deviceIdP))
         return;
      }
}

int cSatipServer::GetModulesDVBS2(void)
{
  return frontends[eSatipFrontendDVBS2].Count();
}

int cSatipServer::GetModulesDVBT(void)
{
  return frontends[eSatipFrontendDVBT].Count();
}

int cSatipServer::GetModulesDVBT2(void)
{
  return frontends[eSatipFrontendDVBT2].Count();
}

int cSatipServer::GetModulesDVBC(void)
{
  return frontends[eSatipFrontendDVBC].Count();
}

int cSatipServer::GetModulesDVBC2(void)
{
  return frontends[eSatipFrontendDVBC2].Count();
}

int cSatipServer::GetModulesATSC(void)
{
  return frontends[eSatipFrontendATSC].Count();
}

// --- cSatipServers ----------------------------------------------------------

cSatipServer *cSatipServers::Find(cSatipServer *serverP)
{
  for (cSatipServer *s = First(); s; s = Next(s)) {
      if (s->Compare(*serverP) == 0)
         return s;
      }
  return NULL;
}

cSatipServer *cSatipServers::Find(int sourceP)
{
  for (cSatipServer *s = First(); s; s = Next(s)) {
      if (s->Matches(sourceP))
         return s;
      }
  return NULL;
}

cSatipServer *cSatipServers::Assign(int deviceIdP, int sourceP, int transponderP, int systemP)
{
  for (cSatipServer *s = First(); s; s = Next(s)) {
      if (s->IsActive() && s->Assign(deviceIdP, sourceP, systemP))
         return s;
      }
  return NULL;
}

cSatipServer *cSatipServers::Update(cSatipServer *serverP)
{
  for (cSatipServer *s = First(); s; s = Next(s)) {
      if (s->Compare(*serverP) == 0) {
         s->Update();
         return s;
         }
      }
  return NULL;
}

void cSatipServers::Activate(cSatipServer *serverP, bool onOffP)
{
  for (cSatipServer *s = First(); s; s = Next(s)) {
      if (s == serverP) {
         s->Activate(onOffP);
         break;
         }
      }
}

void cSatipServers::Attach(cSatipServer *serverP, int deviceIdP)
{
  for (cSatipServer *s = First(); s; s = Next(s)) {
      if (s == serverP) {
         s->Attach(deviceIdP);
         break;
         }
      }
}

void cSatipServers::Detach(cSatipServer *serverP, int deviceIdP)
{
  for (cSatipServer *s = First(); s; s = Next(s)) {
      if (s == serverP) {
         s->Detach(deviceIdP);
         break;
         }
      }
}

bool cSatipServers::IsQuirk(cSatipServer *serverP, int quirkP)
{
  bool result = false;
  for (cSatipServer *s = First(); s; s = Next(s)) {
      if (s == serverP) {
         result = s->Quirk(quirkP);
         break;
         }
      }
  return result;
}

bool cSatipServers::HasCI(cSatipServer *serverP)
{
  bool result = false;
  for (cSatipServer *s = First(); s; s = Next(s)) {
      if (s == serverP) {
         result = s->HasCI();
         break;
         }
      }
  return result;
}

void cSatipServers::Cleanup(uint64_t intervalMsP)
{
  for (cSatipServer *s = First(); s; s = Next(s)) {
      if (!intervalMsP || (s->LastSeen() > intervalMsP)) {
         info("Removing server %s (%s %s)", s->Description(), s->Address(), s->Model());
         Del(s);
         }
      }
}

cString cSatipServers::GetSrcAddress(cSatipServer *serverP)
{
  cString address = "";
  for (cSatipServer *s = First(); s; s = Next(s)) {
      if (s == serverP) {
         address = s->SrcAddress();
         break;
         }
      }
  return address;
}

cString cSatipServers::GetAddress(cSatipServer *serverP)
{
  cString address = "";
  for (cSatipServer *s = First(); s; s = Next(s)) {
      if (s == serverP) {
         address = s->Address();
         break;
         }
      }
  return address;
}

int cSatipServers::GetPort(cSatipServer *serverP)
{
  int port = SATIP_DEFAULT_RTSP_PORT;
  for (cSatipServer *s = First(); s; s = Next(s)) {
      if (s == serverP) {
         port = s->Port();
         break;
         }
      }
  return port;
}

cString cSatipServers::GetString(cSatipServer *serverP)
{
  cString list = "";
  for (cSatipServer *s = First(); s; s = Next(s)) {
      if (s == serverP) {
         list = cString::sprintf("%s|%s|%s", s->Address(), s->Model(), s->Description());
         break;
         }
      }
  return list;
}

cString cSatipServers::List(void)
{
  cString list = "";
  for (cSatipServer *s = First(); s; s = Next(s))
      if (isempty(s->SrcAddress()))
         list = cString::sprintf("%s%c %s|%s|%s\n", *list, s->IsActive() ? '+' : '-', s->Address(), s->Model(), s->Description());
      else
         list = cString::sprintf("%s%c %s@%s|%s|%s\n", *list, s->IsActive() ? '+' : '-', s->SrcAddress(), s->Address(), s->Model(), s->Description());
  return list;
}

int cSatipServers::NumProvidedSystems(void)
{
  int count = 0;
  for (cSatipServer *s = First(); s; s = Next(s)) {
      // DVB-S2: qpsk, 8psk, 16apsk, 32apsk
      count += s->GetModulesDVBS2() * 4;
      // DVB-T: qpsk, qam16, qam64
      count += s->GetModulesDVBT() * 3;
      // DVB-T2: qpsk, qam16, qam64, qam256
      count += s->GetModulesDVBT2() * 4;
      // DVB-C: qam64, qam128, qam256
      count += s->GetModulesDVBC() * 3;
      // DVB-C2: qam16, qam32, qam64, qam128, qam256
      count += s->GetModulesDVBC2() * 5;
      // ATSC: 8vbs, 16vbs, qam256
      count += s->GetModulesATSC() * 3;
      }
  return count;
}
