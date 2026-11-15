/*
 * rtp.h: SAT>IP plugin for the Video Disk Recorder
 *
 * See the README file for copyright information and how to reach the author.
 *
 */

#ifndef __SATIP_RTP_H_
#define __SATIP_RTP_H_

#include "socket.h"
#include "tunerif.h"
#include "pollerif.h"

class cSatipRtp : public cSatipSocket, public cSatipPollerIf {
private:
  enum {
    eRtpPacketReadCount = 50,
    eMaxUdpPacketSizeB  = TS_SIZE * 7 + 12,
    eReportIntervalS    = 300, // in seconds
    eResyncWindowMs     = 3000 // RTP sequence may restart after (re)tuning
  };
  cSatipTunerIf &tunerM;
  unsigned int bufferLenM;
  unsigned char *bufferM;
  time_t lastErrorReportM;
  int packetErrorsM;
  int sequenceNumberM;
  cTimeMs resyncTimeoutM;
  int GetHeaderLength(unsigned char *bufferP, unsigned int lengthP);

public:
  explicit cSatipRtp(cSatipTunerIf &tunerP);
  virtual ~cSatipRtp();
  virtual void Close(void);
  void Resync(void);

  // for internal poller interface
public:
  virtual int GetFd(void);
  virtual void Process(void);
  virtual void Process(unsigned char *dataP, int lengthP);
  virtual cString ToString(void) const;
};

#endif /* __SATIP_RTP_H_ */
