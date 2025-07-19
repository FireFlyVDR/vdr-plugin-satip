/*
 * server.h: SAT>IP plugin for the Video Disk Recorder
 *
 * See the README file for copyright information and how to reach the author.
 *
 */

#ifndef __SATIP_SERVER_H
#define __SATIP_SERVER_H

class cSatipServer;


// --- cSatipFrontends --------------------------------------------------------

class cSatipFrontends {
private:
  int devicesAssigned[SATIP_MAX_DEVICES];
  bool devicesAttached[SATIP_MAX_DEVICES];
  int numDevices;
  cString type;
public:
  cSatipFrontends(void);
  bool Init(const char *Type, int NumDevices);
  int Count(void) { return numDevices; };
  bool Assign(int deviceIdP);
  bool Attach(int deviceIdP);
  bool Detach(int deviceIdP);
  void LogAssignments(const char *Func, bool Result, int deviceIdP);
};

// --- cSatipServer -----------------------------------------------------------

class cSatipServer : public cListObject {
private:
  enum eSatipFrontend {
    eSatipFrontendDVBS2 = 0,
    eSatipFrontendDVBT,
    eSatipFrontendDVBT2,
    eSatipFrontendDVBC,
    eSatipFrontendDVBC2,
    eSatipFrontendATSC,
    eSatipFrontendCount
  };
  enum {
    eSatipMaxSourceFilters = 16
  };
  cString srcAddressM;
  cString addressM;
  cString modelM;
  cString filtersM;
  cString descriptionM;
  cString quirksM;
  cSatipFrontends frontends[eSatipFrontendCount];
  int sourceFiltersM[eSatipMaxSourceFilters];
  int portM;
  int quirkM;
  bool hasCiM;
  bool activeM;
  time_t createdM;
  cTimeMs lastSeenM;
  bool IsValidSource(int sourceP);

public:
  enum eSatipQuirk {
    eSatipQuirkNone        = 0x00,
    eSatipQuirkSessionId   = 0x01,
    eSatipQuirkPlayPids    = 0x02,
    eSatipQuirkForceLock   = 0x04,
    eSatipQuirkRtpOverTcp  = 0x08,
    eSatipQuirkCiXpmt      = 0x10,
    eSatipQuirkCiTnr       = 0x20,
    eSatipQuirkForcePilot  = 0x40,
    eSatipQuirkTearAndPlay = 0x80,
    eSatipQuirkMask        = 0xFF
  };
  cSatipServer(const char *srcAddressP, const char *addressP, const int portP, const char *modelP, const char *filtersP, const char *descriptionP, const int quirkP);
  virtual ~cSatipServer();
  virtual int Compare(const cListObject &listObjectP) const;
  bool Assign(int deviceIdP, int sourceP, int systemP);
  bool Matches(int sourceP);
  void Attach(int deviceIdP);
  void Detach(int deviceIdP);
  int GetModulesDVBS2(void);
  int GetModulesDVBT(void);
  int GetModulesDVBT2(void);
  int GetModulesDVBC(void);
  int GetModulesDVBC2(void);
  int GetModulesATSC(void);
  void Activate(bool onOffP)    { activeM = onOffP; }
  const char *SrcAddress(void)  { return *srcAddressM; }
  const char *Address(void)     { return *addressM; }
  const char *Model(void)       { return *modelM; }
  const char *Filters(void)     { return *filtersM; }
  const char *Description(void) { return *descriptionM; }
  const char *Quirks(void)      { return *quirksM; }
  int Port(void)                { return portM; }
  bool Quirk(int quirkP)        { return ((quirkP & eSatipQuirkMask) & quirkM); }
  bool HasQuirk(void)           { return (quirkM != eSatipQuirkNone); }
  bool HasCI(void)              { return hasCiM; }
  bool IsActive(void)           { return activeM; }
  void Update(void)             { lastSeenM.Set(); }
  uint64_t LastSeen(void)       { return lastSeenM.Elapsed(); }
  time_t Created(void)          { return createdM; }
};

// --- cSatipServers ----------------------------------------------------------

class cSatipServers : public cList<cSatipServer> {
public:
  cSatipServer *Find(cSatipServer *serverP);
  cSatipServer *Find(int sourceP);
  cSatipServer *Assign(int deviceIdP, int sourceP, int transponderP, int systemP);
  cSatipServer *Update(cSatipServer *serverP);
  void Activate(cSatipServer *serverP, bool onOffP);
  void Attach(cSatipServer *serverP, int deviceIdP);
  void Detach(cSatipServer *serverP, int deviceIdP);
  bool IsQuirk(cSatipServer *serverP, int quirkP);
  bool HasCI(cSatipServer *serverP);
  void Cleanup(uint64_t intervalMsP = 0);
  cString GetAddress(cSatipServer *serverP);
  cString GetSrcAddress(cSatipServer *serverP);
  cString GetString(cSatipServer *serverP);
  int GetPort(cSatipServer *serverP);
  cString List(void);
  int NumProvidedSystems(void);
};

#endif // __SATIP_SERVER_H
