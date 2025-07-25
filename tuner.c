/*
 * tuner.c: SAT>IP plugin for the Video Disk Recorder
 *
 * See the README file for copyright information and how to reach the author.
 *
 */

#define __STDC_FORMAT_MACROS // Required for format specifiers
#include <inttypes.h>

#include "common.h"
#include "config.h"
#include "discover.h"
#include "log.h"
#include "poller.h"
#include "tuner.h"

cSatipTuner::cSatipTuner(cSatipDeviceIf &deviceP, unsigned int packetLenP)
: cThread(cString::sprintf("SATIP#%d tuner", deviceP.GetId())),
  sleepM(),
  deviceM(&deviceP),
  deviceIdM(deviceP.GetId()),
  rtspM(*this),
  rtpM(*this),
  rtcpM(*this),
  baseURL(""),
  streamParamM(""),
  lastBaseURL(""),
  lastParamM(""),
  tnrParamM(""),
  currentServerM(NULL, deviceP.GetId(), 0),
  nextServerM(NULL, deviceP.GetId(), 0),
  mutexTunerM(),
  reConnectM(),
  keepAliveM(),
  statusUpdateM(),
  pidUpdateCacheM(),
  setupTimeoutM(-1),
  sessionM(""),
  currentStateM(tsIdle),
  internalStateM(),
  externalStateM(),
  timeoutM(eMinKeepAliveIntervalMs - eKeepAlivePreBufferMs),
  hasLockM(false),
  signalStrengthDBmM(0.0),
  signalStrengthM(-1),
  signalQualityM(-1),
  frontendIdM(-1),
  streamIdM(-1),
  pmtPidM(-1),
  addPidsM(),
  delPidsM(),
  pidsM()
{
  debug1("%s (, %d) [device %d]", __PRETTY_FUNCTION__, packetLenP, deviceIdM);

  // Open sockets
  int i = SatipConfig.GetPortRangeStart() ? SatipConfig.GetPortRangeStop() - SatipConfig.GetPortRangeStart() - 1 : 100;
  int port = SatipConfig.GetPortRangeStart();
  while (i-- > 0) {
        // RTP must use an even port number
        if (rtpM.Open(port) && (rtpM.Port() % 2 == 0) && rtcpM.Open(rtpM.Port() + 1))
           break;
        rtpM.Close();
        rtcpM.Close();
        if (SatipConfig.GetPortRangeStart())
           port += 2;
        }
  if ((rtpM.Port() <= 0) || (rtcpM.Port() <= 0)) {
     error("Cannot open required RTP/RTCP ports [device %d]", deviceIdM);
     }
  // Must be done after socket initialization!
  cSatipPoller::GetInstance()->Register(rtpM);
  cSatipPoller::GetInstance()->Register(rtcpM);

  // Start thread
  Start();
}

cSatipTuner::~cSatipTuner()
{
  debug1("%s [device %d]", __PRETTY_FUNCTION__, deviceIdM);

  // Stop thread
  sleepM.Signal();
  if (Running())
     Cancel(3);
  Close();
  currentStateM = tsIdle;
  internalStateM.Clear();
  externalStateM.Clear();

  // Close the listening sockets
  cSatipPoller::GetInstance()->Unregister(rtcpM);
  cSatipPoller::GetInstance()->Unregister(rtpM);
  rtcpM.Close();
  rtpM.Close();
}

void cSatipTuner::Action(void)
{
  debug1("%s Entering [device %d]", __PRETTY_FUNCTION__, deviceIdM);

  bool lastIdleStatus = false;
  cTimeMs idleCheck(eIdleCheckTimeoutMs);
  cTimeMs tuning(eTuningTimeoutMs);
  reConnectM.Set(eConnectTimeoutMs);
  // Do the thread loop
  while (Running()) {
        UpdateCurrentState();
        switch (currentStateM) {
          case tsIdle:
               debug4("%s: tsIdle [device %d]", __PRETTY_FUNCTION__, deviceIdM);
               break;
          case tsRelease:
               debug4("%s: tsRelease [device %d]", __PRETTY_FUNCTION__, deviceIdM);
               Disconnect();
               RequestState(tsIdle, smInternal);
               break;
          case tsSet:
               debug4("%s: tsSet [device %d]", __PRETTY_FUNCTION__, deviceIdM);
               if (currentServerM.IsQuirk(cSatipServer::eSatipQuirkTearAndPlay))
                  Disconnect();
               if (Connect()) {
                  tuning.Set(eTuningTimeoutMs);
                  RequestState(tsTuned, smInternal);
                  UpdatePids(true);
                  }
               else
                  Disconnect();
               break;
          case tsTuned:
               debug4("%s: tsTuned [device %d]", __PRETTY_FUNCTION__, deviceIdM);
               deviceM->SetChannelTuned();
               reConnectM.Set(eConnectTimeoutMs);
               idleCheck.Set(eIdleCheckTimeoutMs);
               lastIdleStatus = false;
               // Read reception statistics via DESCRIBE and RTCP
               if (hasLockM || ReadReceptionStatus()) {
                  // Quirk for devices without valid reception data
                  if (currentServerM.IsQuirk(cSatipServer::eSatipQuirkForceLock)) {
                     hasLockM = true;
                     signalStrengthDBmM = eDefaultSignalStrengthDBm;
                     signalStrengthM = eDefaultSignalStrength;
                     signalQualityM = eDefaultSignalQuality;
                     }
                  if (hasLockM)
                     RequestState(tsLocked, smInternal);
                  }
               else if (tuning.TimedOut()) {
                  error("Tuning timeout - retuning [device %d]", deviceIdM);
                  RequestState(tsSet, smInternal);
                  }
               break;
          case tsLocked:
               debug4("%s: tsLocked [device %d]", __PRETTY_FUNCTION__, deviceIdM);
               if (!UpdatePids()) {
                  error("Pid update failed - retuning [device %d]", deviceIdM);
                  RequestState(tsSet, smInternal);
                  break;
                  }
               if (!KeepAlive()) {
                  error("Keep-alive failed - retuning [device %d]", deviceIdM);
                  RequestState(tsSet, smInternal);
                  break;
                  }
               if (reConnectM.TimedOut()) {
                  error("Connection timeout - retuning [device %d]", deviceIdM);
                  RequestState(tsSet, smInternal);
                  break;
                  }
               if (idleCheck.TimedOut()) {
                  bool currentIdleStatus = deviceM->IsIdle();
                  if (lastIdleStatus && currentIdleStatus) {
                     info("Idle timeout - releasing [device %d]", deviceIdM);
                     RequestState(tsRelease, smInternal);
                     }
                  lastIdleStatus = currentIdleStatus;
                  idleCheck.Set(eIdleCheckTimeoutMs);
                  break;
                  }
               Receive();
               break;
          default:
               error("Unknown tuner status %d [device %d]", currentStateM, deviceIdM);
               break;
          }
        if (!StateRequested())
           sleepM.Wait(eSleepTimeoutMs); // to avoid busy loop and reduce cpu load
        }
  debug1("%s Exiting [device %d]", __PRETTY_FUNCTION__, deviceIdM);
}

bool cSatipTuner::Open(void)
{
  cMutexLock MutexLock(&mutexTunerM);
  debug1("%s [device %d]", __PRETTY_FUNCTION__, deviceIdM);

  // return always true
  return true;
}

bool cSatipTuner::Close(void)
{
  cMutexLock MutexLock(&mutexTunerM);
  debug1("%s [device %d]", __PRETTY_FUNCTION__, deviceIdM);

  if (setupTimeoutM.TimedOut())
     RequestState(tsRelease, smExternal);

  // return always true
  return true;
}

bool cSatipTuner::Connect(void)
{
  cMutexLock MutexLock(&mutexTunerM);
  debug1("%s [device %d]", __PRETTY_FUNCTION__, deviceIdM);

  rtspM.Create();
  if (!isempty(*baseURL)) {
     tnrParamM = "";
     // Just retune
     if (streamIdM >= 0) {
        if (!strcmp(*streamParamM, *lastParamM) && hasLockM) {
           debug1("%s Identical parameters [device %d]", __PRETTY_FUNCTION__, deviceIdM);
           return true;
           }
        cString uri = cString::sprintf("%sstream=%d?%s", *baseURL, streamIdM, *streamParamM);
        debug1("%s Retuning [device %d]", __PRETTY_FUNCTION__, deviceIdM);
        if (rtspM.Play(*uri)) {
           keepAliveM.Set(timeoutM);
           lastParamM = streamParamM;
           return true;
           }
        }
     else if (rtspM.SetInterface(nextServerM.IsValid() ? *nextServerM.GetSrcAddress() : NULL) && rtspM.Options(*baseURL)) {
        cString uri = cString::sprintf("%s?%s", *baseURL, *streamParamM);
        bool useTcp = SatipConfig.IsTransportModeRtpOverTcp() && nextServerM.IsValid() && nextServerM.IsQuirk(cSatipServer::eSatipQuirkRtpOverTcp);
        // Flush any old content
        //rtpM.Flush();
        //rtcpM.Flush();
        if (useTcp)
           debug1("%s Requesting TCP [device %d]", __PRETTY_FUNCTION__, deviceIdM);
        if (rtspM.Setup(*uri, rtpM.Port(), rtcpM.Port(), useTcp)) {
           lastParamM = streamParamM;
           keepAliveM.Set(timeoutM);
           if (nextServerM.IsValid()) {
              currentServerM = nextServerM;
              nextServerM.Reset();
              }
           lastBaseURL = baseURL;
           currentServerM.Attach();
           return true;
           }
        }
     rtspM.Destroy();
     streamIdM = -1;
     error("Connect failed [device %d]", deviceIdM);
     }

  return false;
}

bool cSatipTuner::Disconnect(void)
{
  cMutexLock MutexLock(&mutexTunerM);
  debug1("%s [device %d]", __PRETTY_FUNCTION__, deviceIdM);

  if (!isempty(*lastBaseURL) && (streamIdM >= 0)) {
     cString uri = cString::sprintf("%sstream=%d", *lastBaseURL, streamIdM);
     rtspM.Teardown(*uri);
     // some devices requires a teardown for TCP connection also
     rtspM.Destroy();
     streamIdM = -1;
     }

  // Reset signal parameters
  hasLockM = false;
  signalStrengthDBmM = 0.0;
  signalStrengthM = -1;
  signalQualityM = -1;
  frontendIdM = -1;

  currentServerM.Detach();
  statusUpdateM.Set(0);
  timeoutM = eMinKeepAliveIntervalMs - eKeepAlivePreBufferMs;
  pmtPidM = -1;
  addPidsM.Clear();
  delPidsM.Clear();

  // return always true
  return true;
}

void cSatipTuner::ProcessVideoData(u_char *bufferP, int lengthP)
{
  debug16("%s (, %d) [device %d]", __PRETTY_FUNCTION__, lengthP, deviceIdM);
  if (lengthP > 0) {
     uint64_t elapsed;
     cTimeMs processing(0);

     AddTunerStatistic(lengthP);
     elapsed = processing.Elapsed();
     if (elapsed > 1)
        debug6("%s AddTunerStatistic() took %" PRIu64 " ms [device %d]", __PRETTY_FUNCTION__, elapsed, deviceIdM);

     processing.Set(0);
     deviceM->WriteData(bufferP, lengthP);
     elapsed = processing.Elapsed();
     if (elapsed > 1)
        debug6("%s WriteData() took %" PRIu64 " ms [device %d]", __FUNCTION__, elapsed, deviceIdM);
     }
  reConnectM.Set(eConnectTimeoutMs);
}

void cSatipTuner::ProcessRtpData(u_char *bufferP, int lengthP)
{
  rtpM.Process(bufferP, lengthP);
}

void cSatipTuner::ProcessApplicationData(u_char *bufferP, int lengthP)
{
  debug16("%s (%d) [device %d]", __PRETTY_FUNCTION__, lengthP, deviceIdM);
  // DVB-S2:
  // ver=<major>.<minor>;src=<srcID>;tuner=<feID>,<level>,<lock>,<quality>,<frequency>,<polarisation>,<system>,<type>,<pilots>,<roll_off>,<symbol_rate>,<fec_inner>;pids=<pid0>,...,<pidn>
  // DVB-T2:
  // ver=1.1;tuner=<feID>,<level>,<lock>,<quality>,<freq>,<bw>,<msys>,<tmode>,<mtype>,<gi>,<fec>,<plp>,<t2id>,<sm>;pids=<pid0>,...,<pidn>
  // DVB-C2:
  // ver=1.2;tuner=<feID>,<level>,<lock>,<quality>,<freq>,<bw>,<msys>,<mtype>,<sr>,<c2tft>,<ds>,<plp>,<specinv>;pids=<pid0>,...,<pidn>
  if (lengthP > 0) {
     char s[lengthP];
     memcpy(s, (char *)bufferP, lengthP);
     debug10("%s (%s) [device %d]", __PRETTY_FUNCTION__, s, deviceIdM);
     char *c = strstr(s, ";tuner=");
     if (c)  {
        int value;

        // feID:
        frontendIdM = atoi(c + 7);

        // level:
        // Numerical value between 0 and 255
        // An incoming L-band satellite signal of
        // -25dBm corresponds to 224
        // -65dBm corresponds to 32
        // No signal corresponds to 0
        c = strstr(c, ",");
        value = min(atoi(++c), 255);
        signalStrengthDBmM = (value > 0) ? 40.0 * (value - 32) / 192.0 - 65.0 : 0.0;
        // Scale value to 0-100
        signalStrengthM = (value >= 0) ? 0.5 + value * 100.0 / 255.0 : -1;

        // lock:
        // lock Set to one of the following values:
        // "0" the frontend is not locked
        // "1" the frontend is locked
        c = strstr(c, ",");
        hasLockM = !!atoi(++c);

        // quality:
        // Numerical value between 0 and 15
        // Lowest value corresponds to highest error rate
        // The value 15 shall correspond to
        // -a BER lower than 2x10-4 after Viterbi for DVB-S
        // -a PER lower than 10-7 for DVB-S2
        c = strstr(c, ",");
        value = min(atoi(++c), 15);
        // Scale value to 0-100
        signalQualityM = (hasLockM && (value >= 0)) ? 0.5 + (value * 100.0 / 15.0) : 0;
        }
     }
  reConnectM.Set(eConnectTimeoutMs);
}

void cSatipTuner::ProcessRtcpData(u_char *bufferP, int lengthP)
{
  rtcpM.Process(bufferP, lengthP);
}

void cSatipTuner::SetStreamId(int streamIdP)
{
  cMutexLock MutexLock(&mutexTunerM);
  debug1("%s (%d) [device %d]", __PRETTY_FUNCTION__, streamIdP, deviceIdM);
  streamIdM = streamIdP;
}

void cSatipTuner::SetSessionTimeout(const char *sessionP, int timeoutP)
{
  cMutexLock MutexLock(&mutexTunerM);
  debug1("%s (%s, %d) [device %d]", __PRETTY_FUNCTION__, sessionP, timeoutP, deviceIdM);
  sessionM = sessionP;
  if (nextServerM.IsQuirk(cSatipServer::eSatipQuirkSessionId) && !isempty(*sessionM) && startswith(*sessionM, "0"))
     rtspM.SetSession(SkipZeroes(*sessionM));
  timeoutM = (timeoutP > eMinKeepAliveIntervalMs) ? timeoutP : eMinKeepAliveIntervalMs;
  timeoutM -= eKeepAlivePreBufferMs;
}

void cSatipTuner::SetupTransport(int rtpPortP, int rtcpPortP, const char *streamAddrP, const char *sourceAddrP)
{
  cMutexLock MutexLock(&mutexTunerM);
  debug1("%s (%d, %d, %s, %s) [device %d]", __PRETTY_FUNCTION__, rtpPortP, rtcpPortP, streamAddrP, sourceAddrP, deviceIdM);
  bool multicast = !isempty(streamAddrP);
  // Adapt RTP to any transport media change
  if (multicast != rtpM.IsMulticast() || rtpPortP != rtpM.Port()) {
     cSatipPoller::GetInstance()->Unregister(rtpM);
     if (rtpPortP >= 0) {
        rtpM.Close();
        if (multicast)
           rtpM.OpenMulticast(rtpPortP, streamAddrP, sourceAddrP);
        else
           rtpM.Open(rtpPortP);
        cSatipPoller::GetInstance()->Register(rtpM);
        }
     }
  // Adapt RTCP to any transport media change
  if (multicast != rtcpM.IsMulticast() || rtcpPortP != rtcpM.Port()) {
     cSatipPoller::GetInstance()->Unregister(rtcpM);
     if (rtcpPortP >= 0) {
        rtcpM.Close();
        if (multicast)
           rtcpM.OpenMulticast(rtcpPortP, streamAddrP, sourceAddrP);
        else
           rtcpM.Open(rtcpPortP);
        cSatipPoller::GetInstance()->Register(rtcpM);
        }
     }
}

void cSatipTuner::SetBaseUrl(const char *addressP, const int portP)
{
  debug16("%s (%s, %d) [device %d]", __PRETTY_FUNCTION__, addressP, portP, deviceIdM);

  if (portP != SATIP_DEFAULT_RTSP_PORT)
     baseURL = cString::sprintf("rtsp://%s:%d/", addressP, portP);
  else
     baseURL = cString::sprintf("rtsp://%s/", addressP);
}

int cSatipTuner::GetId(void)
{
  debug16("%s [device %d]", __PRETTY_FUNCTION__, deviceIdM);
  return deviceIdM;
}

bool cSatipTuner::SetSource(cSatipServer *serverP, const int transponderP, const char *parameterP, const int indexP)
{
  debug1("%s (%d, %s, %d) [device %d]", __PRETTY_FUNCTION__, transponderP, parameterP, indexP, deviceIdM);
  cMutexLock MutexLock(&mutexTunerM);
  if (serverP) {
     nextServerM.Set(serverP, transponderP);
     if (!isempty(*nextServerM.GetAddress()) && !isempty(parameterP)) {
        // Update stream address and parameter
        cString streamAddr = rtspM.RtspUnescapeString(*nextServerM.GetAddress());
        streamParamM = rtspM.RtspUnescapeString(parameterP);
        int streamPort = nextServerM.GetPort();
        SetBaseUrl(*streamAddr, streamPort);
        // Modify parameter if required
        if (nextServerM.IsQuirk(cSatipServer::eSatipQuirkForcePilot) && strstr(parameterP, "msys=dvbs2") && !strstr(parameterP, "plts="))
           streamParamM = rtspM.RtspUnescapeString(*cString::sprintf("%s&plts=on", parameterP));
        // Reconnect
        if (!isempty(*lastBaseURL)) {
           if (strcmp(*baseURL, *lastBaseURL))
              RequestState(tsRelease, smInternal);
           }
        RequestState(tsSet, smExternal);
        setupTimeoutM.Set(eSetupTimeoutMs);
        }
     }
  else {
     baseURL = "";
     streamParamM = "";
     }

  return true;
}

bool cSatipTuner::SetPid(int pidP, int typeP, bool onP)
{
  debug16("%s (%d, %d, %d) [device %d]", __PRETTY_FUNCTION__, pidP, typeP, onP, deviceIdM);
  cMutexLock MutexLock(&mutexTunerM);
  if (onP) {
     pidsM.AddPid(pidP);
     addPidsM.AddPid(pidP);
     delPidsM.RemovePid(pidP);
     }
  else {
     pidsM.RemovePid(pidP);
     delPidsM.AddPid(pidP);
     addPidsM.RemovePid(pidP);
     }
  debug12("%s (%d, %d, %d) pids=%s [device %d]", __PRETTY_FUNCTION__, pidP, typeP, onP, *pidsM.ListPids(), deviceIdM);
  sleepM.Signal();

  return true;
}

bool cSatipTuner::UpdatePids(bool forceP)
{
  debug16("%s (%d) tunerState=%s [device %d]", __PRETTY_FUNCTION__, forceP, TunerStateString(currentStateM), deviceIdM);
  cMutexLock MutexLock(&mutexTunerM);
  if ((forceP || (pidUpdateCacheM.TimedOut() && (addPidsM.Size() || delPidsM.Size()))) &&
      !isempty(*baseURL) && (streamIdM >= 0)) {
     cString uri = cString::sprintf("%sstream=%d", *baseURL, streamIdM);
     bool useci = (SatipConfig.GetCIExtension() && currentServerM.HasCI());
     bool usedummy = currentServerM.IsQuirk(cSatipServer::eSatipQuirkPlayPids);
     bool paramadded = false;
     if (forceP || usedummy) {
        uri = cString::sprintf("%s?pids=%s", *uri, *pidsM.ListPids());
        paramadded = true;
        if (usedummy && (pidsM.Size() == 1) && (pidsM[0] < 0x20))
           uri = cString::sprintf("%s,%d", *uri, eDummyPid);
        }
     else {
        if (addPidsM.Size()) {
           uri = cString::sprintf("%s%saddpids=%s", *uri, paramadded ? "&" : "?", *addPidsM.ListPids());
           paramadded = true;
           }
        if (delPidsM.Size()) {
           uri = cString::sprintf("%s%sdelpids=%s", *uri, paramadded ? "&" : "?", *delPidsM.ListPids());
           paramadded = true;
           }
        }
     if (useci) {
        if (currentServerM.IsQuirk(cSatipServer::eSatipQuirkCiXpmt)) {
           // CI extension parameters:
           // - x_pmt : specifies the PMT of the service you want the CI to decode
           // - x_ci  : specfies which CI slot (1..n) to use
           //           value 0 releases the CI slot
           //           CI slot released automatically if the stream is released,
           //           but not when used retuning to another channel
           int pid = deviceM->GetPmtPid();
           if ((pid > 0) && (pid != pmtPidM)) {
              int slot = deviceM->GetCISlot();
              uri = cString::sprintf("%s%sx_pmt=%d", *uri, paramadded ? "&" : "?", pid);
              if (slot > 0)
                 uri = cString::sprintf("%s&x_ci=%d", *uri, slot);
              paramadded = true;
              }
           pmtPidM = pid;
           }
        else if (currentServerM.IsQuirk(cSatipServer::eSatipQuirkCiTnr)) {
           // CI extension parameters:
           // - tnr : specifies a channel config entry
           cString param = deviceM->GetTnrParameterString();
           if (!isempty(*param) && strcmp(*tnrParamM, *param) != 0) {
              uri = cString::sprintf("%s%stnr=%s", *uri, paramadded ? "&" : "?", *param);
              paramadded = true;
              }
           tnrParamM = param;
           }
        }
     if (paramadded) {
        pidUpdateCacheM.Set(ePidUpdateIntervalMs);
        if (!rtspM.Play(*uri))
           return false;
        }
     addPidsM.Clear();
     delPidsM.Clear();
     }

  return true;
}

bool cSatipTuner::Receive(void)
{
  debug16("%s tunerState=%s [device %d]", __PRETTY_FUNCTION__, TunerStateString(currentStateM), deviceIdM);
  cMutexLock MutexLock(&mutexTunerM);
  if (!isempty(*baseURL)) {
     if (!rtspM.Receive(*baseURL))
        return false;
     }

  return true;
}

bool cSatipTuner::KeepAlive(bool forceP)
{
  debug16("%s (%d) tunerState=%s [device %d]", __PRETTY_FUNCTION__, forceP, TunerStateString(currentStateM), deviceIdM);
  cMutexLock MutexLock(&mutexTunerM);
  if (keepAliveM.TimedOut()) {
     keepAliveM.Set(timeoutM);
     forceP = true;
     }
  if (forceP && !isempty(*baseURL)) {
     if (!rtspM.Options(*baseURL))
        return false;
     }

  return true;
}

bool cSatipTuner::ReadReceptionStatus(bool forceP)
{
  debug16("%s (%d) tunerState=%s [device %d]", __PRETTY_FUNCTION__, forceP, TunerStateString(currentStateM), deviceIdM);
  cMutexLock MutexLock(&mutexTunerM);
  if (statusUpdateM.TimedOut()) {
     statusUpdateM.Set(eStatusUpdateTimeoutMs);
     forceP = true;
     }
  if (forceP && !isempty(*baseURL) && (streamIdM >= 0)) {
     cString uri = cString::sprintf("%sstream=%d", *baseURL, streamIdM);
     if (rtspM.Describe(*uri))
        return true;
     }

  return false;
}

void cSatipTuner::UpdateCurrentState(void)
{
  debug16("%s [device %d]", __PRETTY_FUNCTION__, deviceIdM);
  cMutexLock MutexLock(&mutexTunerM);
  eTunerState state = currentStateM;

  if (internalStateM.Size()) {
     state = internalStateM.At(0);
     internalStateM.Remove(0);
     }
  else if (externalStateM.Size()) {
     state = externalStateM.At(0);
     externalStateM.Remove(0);
     }

  if (currentStateM != state) {
     debug1("%s: Switching from %s to %s [device %d]", __PRETTY_FUNCTION__, TunerStateString(currentStateM), TunerStateString(state), deviceIdM);
     currentStateM = state;
     }
}

bool cSatipTuner::StateRequested(void)
{
  cMutexLock MutexLock(&mutexTunerM);
  debug16("%s current=%s internal=%d external=%d [device %d]", __PRETTY_FUNCTION__, TunerStateString(currentStateM), internalStateM.Size(), externalStateM.Size(), deviceIdM);

  return (internalStateM.Size() || externalStateM.Size());
}

bool cSatipTuner::RequestState(eTunerState stateP, eStateMode modeP)
{
  cMutexLock MutexLock(&mutexTunerM);
  debug1("%s (%s, %s) current=%s internal=%d external=%d [device %d]", __PRETTY_FUNCTION__, TunerStateString(stateP), StateModeString(modeP), TunerStateString(currentStateM), internalStateM.Size(), externalStateM.Size(), deviceIdM);

  if (modeP == smExternal)
     externalStateM.Append(stateP);
  else if (modeP == smInternal) {
     eTunerState state = internalStateM.Size() ? internalStateM.At(internalStateM.Size() - 1) : currentStateM;

     // validate legal state changes
     switch (state) {
       case tsIdle:
            if (stateP == tsRelease)
               return false;
       case tsRelease:
       case tsSet:
       case tsLocked:
       case tsTuned:
       default:
            break;
       }

     internalStateM.Append(stateP);
     }
  else
     return false;

  return true;
}

const char *cSatipTuner::StateModeString(eStateMode modeP)
{
  switch (modeP) {
    case smInternal:
         return "smInternal";
    case smExternal:
         return "smExternal";
     default:
         break;
    }

   return "---";
}

const char *cSatipTuner::TunerStateString(eTunerState stateP)
{
  switch (stateP) {
    case tsIdle:
         return "tsIdle";
    case tsRelease:
         return "tsRelease";
    case tsSet:
         return "tsSet";
    case tsLocked:
         return "tsLocked";
    case tsTuned:
         return "tsTuned";
    default:
         break;
    }

  return "---";
}

int cSatipTuner::FrontendId(void)
{
  debug16("%s [device %d]", __PRETTY_FUNCTION__, deviceIdM);
  return frontendIdM;
}

int cSatipTuner::SignalStrength(void)
{
  debug16("%s [device %d]", __PRETTY_FUNCTION__, deviceIdM);
  return signalStrengthM;
}

double cSatipTuner::SignalStrengthDBm(void)
{
  debug16("%s [device %d]", __PRETTY_FUNCTION__, deviceIdM);
  return signalStrengthDBmM;
}

int cSatipTuner::SignalQuality(void)
{
  debug16("%s [device %d]", __PRETTY_FUNCTION__, deviceIdM);
  return signalQualityM;
}

bool cSatipTuner::HasLock(void)
{
  debug16("%s [device %d]", __PRETTY_FUNCTION__, deviceIdM);
  return (currentStateM >= tsTuned) && hasLockM;
}

cString cSatipTuner::GetSignalStatus(void)
{
  debug16("%s [device %d]", __PRETTY_FUNCTION__, deviceIdM);
  return cString::sprintf("lock=%d strength=%d quality=%d frontend=%d", HasLock(), SignalStrength(), SignalQuality(), FrontendId());
}

cString cSatipTuner::GetInformation(void)
{
  debug16("%s [device %d]", __PRETTY_FUNCTION__, deviceIdM);
  return (currentStateM >= tsTuned) ? cString::sprintf("%s?%s (%s) [stream=%d]", *baseURL, *streamParamM, *rtspM.GetActiveMode(), streamIdM) : "connection failed";
}
