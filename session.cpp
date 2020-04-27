#include "proxy.h"

#include "liveMedia.hh"
#include "RTSPCommon.hh"
#include "GroupsockHelper.hh" // for "our_random()"

#ifndef MILLION
#define MILLION 1000000
#endif

// A "OnDemandServerMediaSubsession" subclass, used to implement a unicast RTSP server that's proxying another RTSP stream:

class MyServerMediaSubsession: public OnDemandServerMediaSubsession {
public:
  MyServerMediaSubsession(MediaSubsession& mediaSubsession,
			     portNumBits initialPortNum, Boolean multiplexRTCPWithRTP);
  virtual ~MyServerMediaSubsession();

  char const* codecName() const { return fCodecName; }
  char const* url() const { return ((MyServerMediaSession*)fParentSession)->url(); }

private: // redefined virtual functions
  virtual FramedSource* createNewStreamSource(unsigned clientSessionId,
                                              unsigned& estBitrate);
  virtual void closeStreamSource(FramedSource *inputSource);
  virtual RTPSink* createNewRTPSink(Groupsock* rtpGroupsock,
                                    unsigned char rtpPayloadTypeIfDynamic,
                                    FramedSource* inputSource);
  virtual Groupsock* createGroupsock(struct in_addr const& addr, Port port);
  virtual RTCPInstance* createRTCP(Groupsock* RTCPgs, unsigned totSessionBW, /* in kbps */
				   unsigned char const* cname, RTPSink* sink);

private:
  static void subsessionByeHandler(void* clientData);
  void subsessionByeHandler();

  int verbosityLevel() const { return ((MyServerMediaSession*)fParentSession)->fVerbosityLevel; }

private:
  friend class MyRTSPClient;
  MediaSubsession& fClientMediaSubsession; // the 'client' media subsession object that corresponds to this 'server' media subsession
  char const* fCodecName;  // copied from "fClientMediaSubsession" once it's been set up
  MyServerMediaSubsession* fNext; // used when we're part of a queue
  Boolean fHaveSetupStream;
};


////////// MyServerMediaSession implementation //////////

UsageEnvironment& operator<<(UsageEnvironment& env, const MyServerMediaSession& psms) { // used for debugging
  return env << "MyServerMediaSession[" << psms.url() << "]";
}

MyRTSPClient*
defaultCreateNewMyRTSPClientFunc(MyServerMediaSession& ourServerMediaSession,
				    char const* rtspURL,
				    char const* username, char const* password,
				    portNumBits tunnelOverHTTPPortNum, int verbosityLevel,
				    int socketNumToServer) {
  return new MyRTSPClient(ourServerMediaSession, rtspURL, username, password,
			     tunnelOverHTTPPortNum, verbosityLevel, socketNumToServer);
}

MyServerMediaSession* MyServerMediaSession
::createNew(UsageEnvironment& env, GenericMediaServer* ourMediaServer,
	    char const* inputStreamURL, char const* streamName,
	    char const* username, char const* password,
	    portNumBits tunnelOverHTTPPortNum, int verbosityLevel, int socketNumToServer,
	    MediaTranscodingTable* transcodingTable) {
  return new MyServerMediaSession(env, ourMediaServer, inputStreamURL, streamName, username, password,
				     tunnelOverHTTPPortNum, verbosityLevel, socketNumToServer,
				     transcodingTable);
}


MyServerMediaSession
::MyServerMediaSession(UsageEnvironment& env, GenericMediaServer* ourMediaServer,
			  char const* inputStreamURL, char const* streamName,
			  char const* username, char const* password,
			  portNumBits tunnelOverHTTPPortNum, int verbosityLevel,
			  int socketNumToServer,
			  MediaTranscodingTable* transcodingTable,
			  createNewMyRTSPClientFunc* ourCreateNewMyRTSPClientFunc,
			  portNumBits initialPortNum, Boolean multiplexRTCPWithRTP)
  : ServerMediaSession(env, streamName, NULL, NULL, False, NULL),
    describeCompletedFlag(0), fOurMediaServer(ourMediaServer), fClientMediaSession(NULL),
    fVerbosityLevel(verbosityLevel),
    fPresentationTimeSessionNormalizer(new PresentationTimeSessionNormalizer(envir())),
    fCreateNewMyRTSPClientFunc(ourCreateNewMyRTSPClientFunc),
    fTranscodingTable(transcodingTable),
    fInitialPortNum(initialPortNum),
		fMultiplexRTCPWithRTP(multiplexRTCPWithRTP) {
  // Open a RTSP connection to the input stream, and send a "DESCRIBE" command.
  // We'll use the SDP description in the response to set ourselves up.
  fMyRTSPClient
    = (*fCreateNewMyRTSPClientFunc)(*this, inputStreamURL, username, password,
				       tunnelOverHTTPPortNum,
				       verbosityLevel > 0 ? verbosityLevel-1 : verbosityLevel,
				       socketNumToServer);
  MyRTSPClient::sendDESCRIBE(fMyRTSPClient);
}

MyServerMediaSession::~MyServerMediaSession() {
  if (fVerbosityLevel > 0) {
    envir() << *this << "::~MyServerMediaSession()\n";
  }

  // Begin by sending a "TEARDOWN" command (without checking for a response):
  if (fMyRTSPClient != NULL && fClientMediaSession != NULL) {
    fMyRTSPClient->sendTeardownCommand(*fClientMediaSession, NULL, fMyRTSPClient->auth());
  }

  // Then delete our state:
  Medium::close(fClientMediaSession);
  Medium::close(fMyRTSPClient);
  Medium::close(fPresentationTimeSessionNormalizer);
}

char const* MyServerMediaSession::url() const {
  return fMyRTSPClient == NULL ? NULL : fMyRTSPClient->url();
}

Groupsock* MyServerMediaSession::createGroupsock(struct in_addr const& addr, Port port) {
  // Default implementation; may be redefined by subclasses:
  return new Groupsock(envir(), addr, port, 255);
}

RTCPInstance* MyServerMediaSession
::createRTCP(Groupsock* RTCPgs, unsigned totSessionBW, /* in kbps */
	     unsigned char const* cname, RTPSink* sink) {
  // Default implementation; may be redefined by subclasses:
  return RTCPInstance::createNew(envir(), RTCPgs, totSessionBW, cname, sink, NULL/*we're a server*/);
}

Boolean MyServerMediaSession::allowProxyingForSubsession(MediaSubsession const& /*mss*/) {
  // Default implementation
  return True;
}

void MyServerMediaSession::continueAfterDESCRIBE(char const* sdpDescription) {
  describeCompletedFlag = 1;

  // Create a (client) "MediaSession" object from the stream's SDP description ("resultString"), then iterate through its
  // "MediaSubsession" objects, to set up corresponding "ServerMediaSubsession" objects that we'll use to serve the stream's tracks.
  do {
    fClientMediaSession = MediaSession::createNew(envir(), sdpDescription);
    if (fClientMediaSession == NULL) break;

    MediaSubsessionIterator iter(*fClientMediaSession);
    for (MediaSubsession* mss = iter.next(); mss != NULL; mss = iter.next()) {
      if (!allowProxyingForSubsession(*mss)) continue;

      ServerMediaSubsession* smss
	= new MyServerMediaSubsession(*mss, fInitialPortNum, fMultiplexRTCPWithRTP);
      addSubsession(smss);
      if (fVerbosityLevel > 0) {
	envir() << *this << " added new \"MyServerMediaSubsession\" for "
		<< mss->protocolName() << "/" << mss->mediumName() << "/" << mss->codecName() << " track\n";
      }
    }
  } while (0);
}

void MyServerMediaSession::resetDESCRIBEState() {
  // Delete all of our "MyServerMediaSubsession"s; they'll get set up again once we get a response to the new "DESCRIBE".
  if (fOurMediaServer != NULL) {
    // First, close any client connections that may have already been set up:
    fOurMediaServer->closeAllClientSessionsForServerMediaSession(this);
  }
  deleteAllSubsessions();

  // Finally, delete the client "MediaSession" object that we had set up after receiving the response to the previous "DESCRIBE":
  Medium::close(fClientMediaSession); fClientMediaSession = NULL;
}

///////// RTSP 'response handlers' //////////

static void continueAfterDESCRIBE(RTSPClient* rtspClient, int resultCode, char* resultString) {
  char const* res;

  if (resultCode == 0) {
    // The "DESCRIBE" command succeeded, so "resultString" should be the stream's SDP description.
    res = resultString;
  } else {
    // The "DESCRIBE" command failed.
    res = NULL;
  }
  ((MyRTSPClient*)rtspClient)->continueAfterDESCRIBE(res);
  delete[] resultString;
}

static void continueAfterSETUP(RTSPClient* rtspClient, int resultCode, char* resultString) {
  ((MyRTSPClient*)rtspClient)->continueAfterSETUP(resultCode);
  delete[] resultString;
}

static void continueAfterPLAY(RTSPClient* rtspClient, int resultCode, char* resultString) {
  ((MyRTSPClient*)rtspClient)->continueAfterPLAY(resultCode);
  delete[] resultString;
}

static void continueAfterOPTIONS(RTSPClient* rtspClient, int resultCode, char* resultString) {
  Boolean serverSupportsGetParameter = False;
  if (resultCode == 0) {
    // Note whether the server told us that it supports the "GET_PARAMETER" command:
    serverSupportsGetParameter = RTSPOptionIsSupported("GET_PARAMETER", resultString);
  } 
  ((MyRTSPClient*)rtspClient)->continueAfterLivenessCommand(resultCode, serverSupportsGetParameter);
  delete[] resultString;
}

#ifdef SEND_GET_PARAMETER_IF_SUPPORTED
static void continueAfterGET_PARAMETER(RTSPClient* rtspClient, int resultCode, char* resultString) {
  ((MyRTSPClient*)rtspClient)->continueAfterLivenessCommand(resultCode, True);
  delete[] resultString;
}
#endif


////////// "MyRTSPClient" implementation /////////

UsageEnvironment& operator<<(UsageEnvironment& env, const MyRTSPClient& proxyRTSPClient) { // used for debugging
  return env << "MyRTSPClient[" << proxyRTSPClient.url() << "]";
}

MyRTSPClient::MyRTSPClient(MyServerMediaSession& ourServerMediaSession, char const* rtspURL,
				 char const* username, char const* password,
				 portNumBits tunnelOverHTTPPortNum, int verbosityLevel, int socketNumToServer)
  : RTSPClient(ourServerMediaSession.envir(), rtspURL, verbosityLevel, "MyRTSPClient",
	       tunnelOverHTTPPortNum == (portNumBits)(~0) ? 0 : tunnelOverHTTPPortNum, socketNumToServer),
    fOurServerMediaSession(ourServerMediaSession), fOurURL(strDup(rtspURL)), fStreamRTPOverTCP(tunnelOverHTTPPortNum != 0),
    fSetupQueueHead(NULL), fSetupQueueTail(NULL), fNumSetupsDone(0), fNextDESCRIBEDelay(1),
    fServerSupportsGetParameter(False), fLastCommandWasPLAY(False), fDoneDESCRIBE(False),
    fLivenessCommandTask(NULL), fDESCRIBECommandTask(NULL), fSubsessionTimerTask(NULL), fResetTask(NULL) {
  if (username != NULL && password != NULL) {
    fOurAuthenticator = new Authenticator(username, password);
  } else {
    fOurAuthenticator = NULL;
  }
}

void MyRTSPClient::reset() {
  envir().taskScheduler().unscheduleDelayedTask(fLivenessCommandTask); fLivenessCommandTask = NULL;
  envir().taskScheduler().unscheduleDelayedTask(fDESCRIBECommandTask); fDESCRIBECommandTask = NULL;
  envir().taskScheduler().unscheduleDelayedTask(fSubsessionTimerTask); fSubsessionTimerTask = NULL;
  envir().taskScheduler().unscheduleDelayedTask(fResetTask); fResetTask = NULL;

  fSetupQueueHead = fSetupQueueTail = NULL;
  fNumSetupsDone = 0;
  fNextDESCRIBEDelay = 1;
  fLastCommandWasPLAY = False;
  fDoneDESCRIBE = False;

  RTSPClient::reset();
}

MyRTSPClient::~MyRTSPClient() {
  reset();

  delete fOurAuthenticator;
  delete[] fOurURL;
}

int MyRTSPClient::connectToServer(int socketNum, portNumBits remotePortNum) {
  int res;
  res = RTSPClient::connectToServer(socketNum, remotePortNum);
  
  if (res == 0 && fDoneDESCRIBE && fStreamRTPOverTCP) {
    if (fVerbosityLevel > 0) {
      envir() << "MyRTSPClient::connectToServer calling scheduleReset()\n";
    }
    scheduleReset();
  }
  
  return res;
}

void MyRTSPClient::continueAfterDESCRIBE(char const* sdpDescription) {
  if (sdpDescription != NULL) {
    fOurServerMediaSession.continueAfterDESCRIBE(sdpDescription);

    scheduleLivenessCommand();
  } else {
    scheduleDESCRIBECommand();
  }
  fDoneDESCRIBE = True;
}

void MyRTSPClient::continueAfterLivenessCommand(int resultCode, Boolean serverSupportsGetParameter) {
  if (resultCode != 0) {
      fServerSupportsGetParameter = False; // until we learn otherwise, in response to a future "OPTIONS" command

    if (resultCode < 0) {
      if (fVerbosityLevel > 0) {
	envir() << *this << ": lost connection to server ('errno': " << -resultCode << ").  Scheduling reset...\n";
      }
    }

    scheduleReset();
    return;
  }

  fServerSupportsGetParameter = serverSupportsGetParameter;

  // Schedule the next 'liveness' command (i.e., to tell the back-end server that we're still alive):
  scheduleLivenessCommand();
}

#define SUBSESSION_TIMEOUT_SECONDS 5 // how many seconds to wait for the last track's "SETUP" to be done (note below)

void MyRTSPClient::continueAfterSETUP(int resultCode) {
  if (resultCode != 0) {
    // The "SETUP" command failed, so arrange to reset the state. (We don't do this now, because it deletes the
    // "MyServerMediaSubsession", and we can't do that during "MyServerMediaSubsession::createNewStreamSource()".)
    scheduleReset();
    return;
  }

  if (fVerbosityLevel > 0) {
    envir() << *this << "::continueAfterSETUP(): head codec: " << fSetupQueueHead->codecName()
	    << "; numSubsessions " << fSetupQueueHead->fParentSession->numSubsessions() << "\n\tqueue:";
    for (MyServerMediaSubsession* p = fSetupQueueHead; p != NULL; p = p->fNext) {
      envir() << "\t" << p->codecName();
    }
    envir() << "\n";
  }
  envir().taskScheduler().unscheduleDelayedTask(fSubsessionTimerTask); // in case it had been set

  // Dequeue the first "MyServerMediaSubsession" from our 'SETUP queue'.  It will be the one for which this "SETUP" was done:
  MyServerMediaSubsession* smss = fSetupQueueHead; // Assert: != NULL
  fSetupQueueHead = fSetupQueueHead->fNext;
  if (fSetupQueueHead == NULL) fSetupQueueTail = NULL;

  if (fSetupQueueHead != NULL) {
    sendSetupCommand(fSetupQueueHead->fClientMediaSubsession, ::continueAfterSETUP,
		     False, fStreamRTPOverTCP, False, fOurAuthenticator);
    ++fNumSetupsDone;
    fSetupQueueHead->fHaveSetupStream = True;
  } else {
    if (fNumSetupsDone >= smss->fParentSession->numSubsessions()) {
      sendPlayCommand(smss->fClientMediaSubsession.parentSession(), ::continueAfterPLAY, -1.0f, -1.0f, 1.0f, fOurAuthenticator);
      fLastCommandWasPLAY = True;
    } else {
      fSubsessionTimerTask
	= envir().taskScheduler().scheduleDelayedTask(SUBSESSION_TIMEOUT_SECONDS*MILLION, (TaskFunc*)subsessionTimeout, this);
    }
  }
}

void MyRTSPClient::continueAfterPLAY(int resultCode) {
  if (resultCode != 0) {
    // The "PLAY" command failed, so arrange to reset the state. (We don't do this now, because it deletes the
    // "MyServerMediaSubsession", and we can't do that during "MyServerMediaSubsession::createNewStreamSource()".)
    scheduleReset();
    return;
  }
}

void MyRTSPClient::scheduleLivenessCommand() {
  // Delay a random time before sending another 'liveness' command.
  unsigned delayMax = sessionTimeoutParameter(); // if the server specified a maximum time between 'liveness' probes, then use that
  if (delayMax == 0) {
    delayMax = 60;
  }

  // Choose a random time from [delayMax/2,delayMax-1) seconds:
  unsigned const us_1stPart = delayMax*500000;
  unsigned uSecondsToDelay;
  if (us_1stPart <= 1000000) {
    uSecondsToDelay = us_1stPart;
  } else {
    unsigned const us_2ndPart = us_1stPart-1000000;
    uSecondsToDelay = us_1stPart + (us_2ndPart*our_random())%us_2ndPart;
  }
  fLivenessCommandTask = envir().taskScheduler().scheduleDelayedTask(uSecondsToDelay, sendLivenessCommand, this);
}

void MyRTSPClient::sendLivenessCommand(void* clientData) {
  MyRTSPClient* rtspClient = (MyRTSPClient*)clientData;

#ifdef SEND_GET_PARAMETER_IF_SUPPORTED
  MediaSession* sess = rtspClient->fOurServerMediaSession.fClientMediaSession;

  if (rtspClient->fServerSupportsGetParameter && rtspClient->fNumSetupsDone > 0 && sess != NULL) {
    rtspClient->sendGetParameterCommand(*sess, ::continueAfterGET_PARAMETER, "", rtspClient->auth());
  } else {
#endif
    rtspClient->sendOptionsCommand(::continueAfterOPTIONS, rtspClient->auth());
#ifdef SEND_GET_PARAMETER_IF_SUPPORTED
  }
#endif
}

void MyRTSPClient::scheduleReset() {
  if (fVerbosityLevel > 0) {
    envir() << "MyRTSPClient::scheduleReset\n";
  }
  envir().taskScheduler().rescheduleDelayedTask(fResetTask, 0, doReset, this);
}

void MyRTSPClient::doReset() {
  if (fVerbosityLevel > 0) {
    envir() << *this << "::doReset\n";
  }

  reset();
  fOurServerMediaSession.resetDESCRIBEState();

  setBaseURL(fOurURL); // because we'll be sending an initial "DESCRIBE" all over again
  sendDESCRIBE(this);
}

void MyRTSPClient::doReset(void* clientData) {
  MyRTSPClient* rtspClient = (MyRTSPClient*)clientData;
  rtspClient->doReset();
}

void MyRTSPClient::scheduleDESCRIBECommand() {
  // Delay 1s, 2s, 4s, 8s ... 256s until sending the next "DESCRIBE".  Then, keep delaying a random time from [256..511] seconds:
  unsigned secondsToDelay;
  if (fNextDESCRIBEDelay <= 256) {
    secondsToDelay = fNextDESCRIBEDelay;
    fNextDESCRIBEDelay *= 2;
  } else {
    secondsToDelay = 256 + (our_random()&0xFF); // [256..511] seconds
  }

  if (fVerbosityLevel > 0) {
    envir() << *this << ": RTSP \"DESCRIBE\" command failed; trying again in " << secondsToDelay << " seconds\n";
  }
  fDESCRIBECommandTask = envir().taskScheduler().scheduleDelayedTask(secondsToDelay*MILLION, sendDESCRIBE, this);
}

void MyRTSPClient::sendDESCRIBE(void* clientData) {
  MyRTSPClient* rtspClient = (MyRTSPClient*)clientData;
  if (rtspClient != NULL) rtspClient->sendDescribeCommand(::continueAfterDESCRIBE, rtspClient->auth());
}

void MyRTSPClient::subsessionTimeout(void* clientData) {
  ((MyRTSPClient*)clientData)->handleSubsessionTimeout();
}

void MyRTSPClient::handleSubsessionTimeout() {
  // We still have one or more subsessions ('tracks') left to "SETUP".  But we can't wait any longer for them.  Send a "PLAY" now:
  MediaSession* sess = fOurServerMediaSession.fClientMediaSession;
  if (sess != NULL) sendPlayCommand(*sess, ::continueAfterPLAY, -1.0f, -1.0f, 1.0f, fOurAuthenticator);
  fLastCommandWasPLAY = True;
}


//////// "MyServerMediaSubsession" implementation //////////

MyServerMediaSubsession
::MyServerMediaSubsession(MediaSubsession& mediaSubsession,
			     portNumBits initialPortNum, Boolean multiplexRTCPWithRTP)
  : OnDemandServerMediaSubsession(mediaSubsession.parentSession().envir(), True/*reuseFirstSource*/,
				  initialPortNum, multiplexRTCPWithRTP),
    fClientMediaSubsession(mediaSubsession), fCodecName(strDup(mediaSubsession.codecName())),
    fNext(NULL), fHaveSetupStream(False) {
}

UsageEnvironment& operator<<(UsageEnvironment& env, const MyServerMediaSubsession& psmss) { // used for debugging
  return env << "MyServerMediaSubsession[" << psmss.url() << "," << psmss.codecName() << "]";
}

MyServerMediaSubsession::~MyServerMediaSubsession() {
  if (verbosityLevel() > 0) {
    envir() << *this << "::~MyServerMediaSubsession()\n";
  }

  delete[] (char*)fCodecName;
}

FramedSource* MyServerMediaSubsession::createNewStreamSource(unsigned clientSessionId, unsigned& estBitrate) {
  MyServerMediaSession* const sms = (MyServerMediaSession*)fParentSession;

  if (verbosityLevel() > 0) {
    envir() << *this << "::createNewStreamSource(session id " << clientSessionId << ")\n";
  }

  if (fClientMediaSubsession.readSource() == NULL) {
    if (sms->fTranscodingTable == NULL || !sms->fTranscodingTable->weWillTranscode("audio", "MPA-ROBUST")) fClientMediaSubsession.receiveRawMP3ADUs(); // hack for proxying MPA-ROBUST streams
    if (sms->fTranscodingTable == NULL || !sms->fTranscodingTable->weWillTranscode("video", "JPEG")) fClientMediaSubsession.receiveRawJPEGFrames(); // hack for proxying JPEG/RTP streams.
    fClientMediaSubsession.initiate();
    if (verbosityLevel() > 0) {
      envir() << "\tInitiated: " << *this << "\n";
    }

    if (fClientMediaSubsession.readSource() != NULL) {
      if (sms->fTranscodingTable != NULL) {
	char* outputCodecName;
	FramedFilter* transcoder
	  = sms->fTranscodingTable->lookupTranscoder(fClientMediaSubsession, outputCodecName);
	if (transcoder != NULL) {
	  fClientMediaSubsession.addFilter(transcoder);
	  delete[] (char*)fCodecName; fCodecName = outputCodecName;
	}
      }

      FramedFilter* normalizerFilter = sms->fPresentationTimeSessionNormalizer
	->createNewPresentationTimeSubsessionNormalizer(fClientMediaSubsession.readSource(),
							fClientMediaSubsession.rtpSource(),
							fCodecName);
      fClientMediaSubsession.addFilter(normalizerFilter);

      // Some data sources require a 'framer' object to be added, before they can be fed into
      // a "RTPSink".  Adjust for this now:
      if (strcmp(fCodecName, "H264") == 0) {
	fClientMediaSubsession.addFilter(H264VideoStreamDiscreteFramer
					 ::createNew(envir(), fClientMediaSubsession.readSource()));
      } else if (strcmp(fCodecName, "H265") == 0) {
	fClientMediaSubsession.addFilter(H265VideoStreamDiscreteFramer
					 ::createNew(envir(), fClientMediaSubsession.readSource()));
      } else if (strcmp(fCodecName, "MP4V-ES") == 0) {
	fClientMediaSubsession.addFilter(MPEG4VideoStreamDiscreteFramer
					 ::createNew(envir(), fClientMediaSubsession.readSource(),
						     True/* leave PTs unmodified*/));
      } else if (strcmp(fCodecName, "MPV") == 0) {
	fClientMediaSubsession.addFilter(MPEG1or2VideoStreamDiscreteFramer
					 ::createNew(envir(), fClientMediaSubsession.readSource(),
						     False, 5.0, True/* leave PTs unmodified*/));
      } else if (strcmp(fCodecName, "DV") == 0) {
	fClientMediaSubsession.addFilter(DVVideoStreamFramer
					 ::createNew(envir(), fClientMediaSubsession.readSource(),
						     False, True/* leave PTs unmodified*/));
      }
    }

    if (fClientMediaSubsession.rtcpInstance() != NULL) {
      fClientMediaSubsession.rtcpInstance()->setByeHandler(subsessionByeHandler, this);
    }
  }

  MyRTSPClient* const proxyRTSPClient = sms->fMyRTSPClient;
  if (clientSessionId != 0) {
    if (!fHaveSetupStream) {
      Boolean queueWasEmpty = proxyRTSPClient->fSetupQueueHead == NULL;
      if (queueWasEmpty) {
	proxyRTSPClient->fSetupQueueHead = this;
	proxyRTSPClient->fSetupQueueTail = this;
      } else {
	// Add ourself to the "RTSPClient"s 'SETUP queue' (if we're not already on it):
	MyServerMediaSubsession* psms;
	for (psms = proxyRTSPClient->fSetupQueueHead; psms != NULL; psms = psms->fNext) {
	  if (psms == this) break;
	}
	if (psms == NULL) {
	    proxyRTSPClient->fSetupQueueTail->fNext = this;
	    proxyRTSPClient->fSetupQueueTail = this;
	}
      }

      // Hack: If there's already a pending "SETUP" request, don't send this track's "SETUP" right away, because
      // the server might not properly handle 'pipelined' requests.  Instead, wait until after previous "SETUP" responses come back.
      if (queueWasEmpty) {
	proxyRTSPClient->sendSetupCommand(fClientMediaSubsession, ::continueAfterSETUP,
					  False, proxyRTSPClient->fStreamRTPOverTCP, False, proxyRTSPClient->auth());
	++proxyRTSPClient->fNumSetupsDone;
	fHaveSetupStream = True;
      }
    } else {
      // This is a "SETUP" from a new client.  We know that there are no other currently active clients (otherwise we wouldn't
      // have been called here), so we know that the substream was previously "PAUSE"d.  Send "PLAY" downstream once again,
      // to resume the stream:
      if (!proxyRTSPClient->fLastCommandWasPLAY) { // so that we send only one "PLAY"; not one for each subsession
	proxyRTSPClient->sendPlayCommand(fClientMediaSubsession.parentSession(), ::continueAfterPLAY, -1.0f/*resume from previous point*/,
					 -1.0f, 1.0f, proxyRTSPClient->auth());
	proxyRTSPClient->fLastCommandWasPLAY = True;
      }
    }
  }

  estBitrate = fClientMediaSubsession.bandwidth();
  if (estBitrate == 0) estBitrate = 50; // kbps, estimate
  return fClientMediaSubsession.readSource();
}

void MyServerMediaSubsession::closeStreamSource(FramedSource* inputSource) {
  if (verbosityLevel() > 0) {
    envir() << *this << "::closeStreamSource()\n";
  }
  // Because there's only one input source for this 'subsession' (regardless of how many downstream clients are proxying it),
  // we don't close the input source here.  (Instead, we wait until *this* object gets deleted.)
  // However, because (as evidenced by this function having been called) we no longer have any clients accessing the stream,
  // then we "PAUSE" the downstream proxied stream, until a new client arrives:
  if (fHaveSetupStream) {
    MyServerMediaSession* const sms = (MyServerMediaSession*)fParentSession;
    MyRTSPClient* const proxyRTSPClient = sms->fMyRTSPClient;
    if (proxyRTSPClient->fLastCommandWasPLAY) { // so that we send only one "PAUSE"; not one for each subsession
      if (fParentSession->referenceCount() > 1) {
	// There are other client(s) still streaming other subsessions of this stream.
	// Therefore, we don't send a "PAUSE" for the whole stream, but only for the sub-stream:
	proxyRTSPClient->sendPauseCommand(fClientMediaSubsession, NULL, proxyRTSPClient->auth());
      } else {
	// Normal case: There are no other client still streaming (parts of) this stream.
	// Send a "PAUSE" for the whole stream.
	proxyRTSPClient->sendPauseCommand(fClientMediaSubsession.parentSession(), NULL, proxyRTSPClient->auth());
	proxyRTSPClient->fLastCommandWasPLAY = False;
      }
    }
  }
}

RTPSink* MyServerMediaSubsession
::createNewRTPSink(Groupsock* rtpGroupsock, unsigned char rtpPayloadTypeIfDynamic, FramedSource* inputSource) {
  if (verbosityLevel() > 0) {
    envir() << *this << "::createNewRTPSink()\n";
  }

  // Create (and return) the appropriate "RTPSink" object for our codec:
  // (Note: The configuration string might not be correct if a transcoder is used. FIX!) #####
  RTPSink* newSink;
  if (strcmp(fCodecName, "AC3") == 0 || strcmp(fCodecName, "EAC3") == 0) {
    newSink = AC3AudioRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic,
					 fClientMediaSubsession.rtpTimestampFrequency()); 
#if 0 // This code does not work; do *not* enable it:
  } else if (strcmp(fCodecName, "AMR") == 0 || strcmp(fCodecName, "AMR-WB") == 0) {
    Boolean isWideband = strcmp(fCodecName, "AMR-WB") == 0;
    newSink = AMRAudioRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic,
					 isWideband, fClientMediaSubsession.numChannels());
#endif
  } else if (strcmp(fCodecName, "DV") == 0) {
    newSink = DVVideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic);
  } else if (strcmp(fCodecName, "GSM") == 0) {
    newSink = GSMAudioRTPSink::createNew(envir(), rtpGroupsock);
  } else if (strcmp(fCodecName, "H263-1998") == 0 || strcmp(fCodecName, "H263-2000") == 0) {
    newSink = H263plusVideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic,
					      fClientMediaSubsession.rtpTimestampFrequency()); 
  } else if (strcmp(fCodecName, "H264") == 0) {
    newSink = H264VideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic,
					  fClientMediaSubsession.fmtp_spropparametersets());
  } else if (strcmp(fCodecName, "H265") == 0) {
    newSink = H265VideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic,
					  fClientMediaSubsession.fmtp_spropvps(),
					  fClientMediaSubsession.fmtp_spropsps(),
					  fClientMediaSubsession.fmtp_sproppps());
  } else if (strcmp(fCodecName, "JPEG") == 0) {
    newSink = SimpleRTPSink::createNew(envir(), rtpGroupsock, 26, 90000, "video", "JPEG",
				       1/*numChannels*/, False/*allowMultipleFramesPerPacket*/, False/*doNormalMBitRule*/);
  } else if (strcmp(fCodecName, "MP4A-LATM") == 0) {
    newSink = MPEG4LATMAudioRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic,
					       fClientMediaSubsession.rtpTimestampFrequency(),
					       fClientMediaSubsession.fmtp_config(),
					       fClientMediaSubsession.numChannels());
  } else if (strcmp(fCodecName, "MP4V-ES") == 0) {
    newSink = MPEG4ESVideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic,
					     fClientMediaSubsession.rtpTimestampFrequency(),
					     fClientMediaSubsession.attrVal_unsigned("profile-level-id"),
					     fClientMediaSubsession.fmtp_config()); 
  } else if (strcmp(fCodecName, "MPA") == 0) {
    newSink = MPEG1or2AudioRTPSink::createNew(envir(), rtpGroupsock);
  } else if (strcmp(fCodecName, "MPA-ROBUST") == 0) {
    newSink = MP3ADURTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic);
  } else if (strcmp(fCodecName, "MPEG4-GENERIC") == 0) {
    newSink = MPEG4GenericRTPSink::createNew(envir(), rtpGroupsock,
					     rtpPayloadTypeIfDynamic, fClientMediaSubsession.rtpTimestampFrequency(),
					     fClientMediaSubsession.mediumName(),
					     fClientMediaSubsession.attrVal_str("mode"),
					     fClientMediaSubsession.fmtp_config(), fClientMediaSubsession.numChannels());
  } else if (strcmp(fCodecName, "MPV") == 0) {
    newSink = MPEG1or2VideoRTPSink::createNew(envir(), rtpGroupsock);
  } else if (strcmp(fCodecName, "OPUS") == 0) {
    newSink = SimpleRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic,
				       48000, "audio", "OPUS", 2, False/*only 1 Opus 'packet' in each RTP packet*/);
  } else if (strcmp(fCodecName, "T140") == 0) {
    newSink = T140TextRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic);
  } else if (strcmp(fCodecName, "THEORA") == 0) {
    newSink = TheoraVideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic,
					    fClientMediaSubsession.fmtp_config()); 
  } else if (strcmp(fCodecName, "VORBIS") == 0) {
    newSink = VorbisAudioRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic,
					    fClientMediaSubsession.rtpTimestampFrequency(), fClientMediaSubsession.numChannels(),
					    fClientMediaSubsession.fmtp_config()); 
  } else if (strcmp(fCodecName, "VP8") == 0) {
    newSink = VP8VideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic);
  } else if (strcmp(fCodecName, "VP9") == 0) {
    newSink = VP9VideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic);
  } else if (strcmp(fCodecName, "AMR") == 0 || strcmp(fCodecName, "AMR-WB") == 0) {
    // Proxying of these codecs is currently *not* supported, because the data received by the "RTPSource" object is not in a
    // form that can be fed directly into a corresponding "RTPSink" object.
    if (verbosityLevel() > 0) {
      envir() << "\treturns NULL (because we currently don't support the proxying of \""
	      << fClientMediaSubsession.mediumName() << "/" << fCodecName << "\" streams)\n";
    }
    return NULL;
  } else if (strcmp(fCodecName, "QCELP") == 0 ||
	     strcmp(fCodecName, "H261") == 0 ||
	     strcmp(fCodecName, "H263-1998") == 0 || strcmp(fCodecName, "H263-2000") == 0 ||
	     strcmp(fCodecName, "X-QT") == 0 || strcmp(fCodecName, "X-QUICKTIME") == 0) {
    // This codec requires a specialized RTP payload format; however, we don't yet have an appropriate "RTPSink" subclass for it:
    if (verbosityLevel() > 0) {
      envir() << "\treturns NULL (because we don't have a \"RTPSink\" subclass for this RTP payload format)\n";
    }
    return NULL;
  } else {
    // This codec is assumed to have a simple RTP payload format that can be implemented just with a "SimpleRTPSink":
    Boolean allowMultipleFramesPerPacket = True; // by default
    Boolean doNormalMBitRule = True; // by default
    // Some codecs change the above default parameters:
    if (strcmp(fCodecName, "MP2T") == 0) {
      doNormalMBitRule = False; // no RTP 'M' bit
    }
    newSink = SimpleRTPSink::createNew(envir(), rtpGroupsock,
				       rtpPayloadTypeIfDynamic, fClientMediaSubsession.rtpTimestampFrequency(),
				       fClientMediaSubsession.mediumName(), fCodecName,
				       fClientMediaSubsession.numChannels(), allowMultipleFramesPerPacket, doNormalMBitRule);
  }

  // Because our relayed frames' presentation times are inaccurate until the input frames have been RTCP-synchronized,
  // we temporarily disable RTCP "SR" reports for this "RTPSink" object:
  newSink->enableRTCPReports() = False;

  // Also tell our "PresentationTimeSubsessionNormalizer" object about the "RTPSink", so it can enable RTCP "SR" reports later:
  PresentationTimeSubsessionNormalizer* ssNormalizer;
  if (strcmp(fCodecName, "H264") == 0 ||
      strcmp(fCodecName, "H265") == 0 ||
      strcmp(fCodecName, "MP4V-ES") == 0 ||
      strcmp(fCodecName, "MPV") == 0 ||
      strcmp(fCodecName, "DV") == 0) {
    // There was a separate 'framer' object in front of the "PresentationTimeSubsessionNormalizer", so go back one object to get it:
    ssNormalizer = (PresentationTimeSubsessionNormalizer*)(((FramedFilter*)inputSource)->inputSource());
  } else {
    ssNormalizer = (PresentationTimeSubsessionNormalizer*)inputSource;
  }
  ssNormalizer->setRTPSink(newSink);

  return newSink;
}

Groupsock* MyServerMediaSubsession::createGroupsock(struct in_addr const& addr, Port port) {
  MyServerMediaSession* parentSession = (MyServerMediaSession*)fParentSession;
  return parentSession->createGroupsock(addr, port);
}

RTCPInstance* MyServerMediaSubsession
::createRTCP(Groupsock* RTCPgs, unsigned totSessionBW, /* in kbps */
	     unsigned char const* cname, RTPSink* sink) {
  MyServerMediaSession* parentSession = (MyServerMediaSession*)fParentSession;
  return parentSession->createRTCP(RTCPgs, totSessionBW, cname, sink);
}

void MyServerMediaSubsession::subsessionByeHandler(void* clientData) {
  ((MyServerMediaSubsession*)clientData)->subsessionByeHandler();
}

void MyServerMediaSubsession::subsessionByeHandler() {
  if (verbosityLevel() > 0) {
    envir() << *this << ": received RTCP \"BYE\".  (The back-end stream has ended.)\n";
  }

  // This "BYE" signals that our input source has (effectively) closed, so pass this onto the front-end clients:
  fHaveSetupStream = False; // hack to stop "PAUSE" getting sent by:
  if (fClientMediaSubsession.readSource() != NULL) {
    fClientMediaSubsession.readSource()->handleClosure();
  }

  // And then treat this as if we had lost connection to the back-end server,
  // and can reestablish streaming from it only by sending another "DESCRIBE":
  MyServerMediaSession* const sms = (MyServerMediaSession*)fParentSession;
  MyRTSPClient* const proxyRTSPClient = sms->fMyRTSPClient;
  proxyRTSPClient->scheduleReset();
}


//////////// PresentationTimeSessionNormalizer and PresentationTimeSubsessionNormalizer implementations //////////
//
//// PresentationTimeSessionNormalizer:
//
//PresentationTimeSessionNormalizer::PresentationTimeSessionNormalizer(UsageEnvironment& env)
//  : Medium(env),
//    fSubsessionNormalizers(NULL), fMasterSSNormalizer(NULL) {
//}
//
//PresentationTimeSessionNormalizer::~PresentationTimeSessionNormalizer() {
//  while (fSubsessionNormalizers != NULL) {
//    Medium::close(fSubsessionNormalizers);
//  }
//}
//
//PresentationTimeSubsessionNormalizer* PresentationTimeSessionNormalizer
//::createNewPresentationTimeSubsessionNormalizer(FramedSource* inputSource, RTPSource* rtpSource,
//						char const* codecName) {
//  fSubsessionNormalizers
//    = new PresentationTimeSubsessionNormalizer(*this, inputSource, rtpSource, codecName, fSubsessionNormalizers);
//  return fSubsessionNormalizers;
//}
//
//void PresentationTimeSessionNormalizer
//::normalizePresentationTime(PresentationTimeSubsessionNormalizer* ssNormalizer,
//			    struct timeval& toPT, struct timeval const& fromPT) {
//  Boolean const hasBeenSynced = ssNormalizer->fRTPSource->hasBeenSynchronizedUsingRTCP();
//
//  if (!hasBeenSynced) {
//    // If "fromPT" has not yet been RTCP-synchronized, then it was generated by our own receiving code, and thus
//    // is already aligned with 'wall-clock' time.  Just copy it 'as is' to "toPT":
//    toPT = fromPT;
//  } else {
//    if (fMasterSSNormalizer == NULL) {
//      // Make "ssNormalizer" the 'master' subsession - meaning that its presentation time is adjusted to align with 'wall clock'
//      // time, and the presentation times of other subsessions (if any) are adjusted to retain their relative separation with
//      // those of the master:
//      fMasterSSNormalizer = ssNormalizer;
//
//      struct timeval timeNow;
//      gettimeofday(&timeNow, NULL);
//
//      // Compute: fPTAdjustment = timeNow - fromPT
//      fPTAdjustment.tv_sec = timeNow.tv_sec - fromPT.tv_sec;
//      fPTAdjustment.tv_usec = timeNow.tv_usec - fromPT.tv_usec;
//      // Note: It's OK if one or both of these fields underflows; the result still works out OK later.
//    }
//
//    // Compute a normalized presentation time: toPT = fromPT + fPTAdjustment
//    toPT.tv_sec = fromPT.tv_sec + fPTAdjustment.tv_sec - 1;
//    toPT.tv_usec = fromPT.tv_usec + fPTAdjustment.tv_usec + MILLION;
//    while (toPT.tv_usec > MILLION) { ++toPT.tv_sec; toPT.tv_usec -= MILLION; }
//
//    // Because "ssNormalizer"s relayed presentation times are accurate from now on, enable RTCP "SR" reports for its "RTPSink":
//    RTPSink* const rtpSink = ssNormalizer->fRTPSink;
//    if (rtpSink != NULL) { // sanity check; should always be true
//      rtpSink->enableRTCPReports() = True;
//    }
//  }
//}
//
//void PresentationTimeSessionNormalizer
//::removePresentationTimeSubsessionNormalizer(PresentationTimeSubsessionNormalizer* ssNormalizer) {
//  // Unlink "ssNormalizer" from the linked list (starting with "fSubsessionNormalizers"):
//  if (fSubsessionNormalizers == ssNormalizer) {
//    fSubsessionNormalizers = fSubsessionNormalizers->fNext;
//  } else {
//    PresentationTimeSubsessionNormalizer** ssPtrPtr = &(fSubsessionNormalizers->fNext);
//    while (*ssPtrPtr != ssNormalizer) ssPtrPtr = &((*ssPtrPtr)->fNext);
//    *ssPtrPtr = (*ssPtrPtr)->fNext;
//  }
//}
//
//// PresentationTimeSubsessionNormalizer:
//
//PresentationTimeSubsessionNormalizer
//::PresentationTimeSubsessionNormalizer(PresentationTimeSessionNormalizer& parent, FramedSource* inputSource, RTPSource* rtpSource,
//				       char const* codecName, PresentationTimeSubsessionNormalizer* next)
//  : FramedFilter(parent.envir(), inputSource),
//    fParent(parent), fRTPSource(rtpSource), fRTPSink(NULL), fCodecName(codecName), fNext(next) {
//}
//
//PresentationTimeSubsessionNormalizer::~PresentationTimeSubsessionNormalizer() {
//  fParent.removePresentationTimeSubsessionNormalizer(this);
//}
//
//void PresentationTimeSubsessionNormalizer::afterGettingFrame(void* clientData, unsigned frameSize,
//							     unsigned numTruncatedBytes,
//							     struct timeval presentationTime,
//							     unsigned durationInMicroseconds) {
//  ((PresentationTimeSubsessionNormalizer*)clientData)
//    ->afterGettingFrame(frameSize, numTruncatedBytes, presentationTime, durationInMicroseconds);
//}
//
//void PresentationTimeSubsessionNormalizer::afterGettingFrame(unsigned frameSize,
//							     unsigned numTruncatedBytes,
//							     struct timeval presentationTime,
//							     unsigned durationInMicroseconds) {
//  // This filter is implemented by passing all frames through unchanged, except that "fPresentationTime" is changed:
//  fFrameSize = frameSize;
//  fNumTruncatedBytes = numTruncatedBytes;
//  fDurationInMicroseconds = durationInMicroseconds;
//
//  fParent.normalizePresentationTime(this, fPresentationTime, presentationTime);
//
//  // Hack for JPEG/RTP proxying.  Because we're proxying JPEG by just copying the raw JPEG/RTP payloads, without interpreting them,
//  // we need to also 'copy' the RTP 'M' (marker) bit from the "RTPSource" to the "RTPSink":
//  if (fRTPSource->curPacketMarkerBit() && strcmp(fCodecName, "JPEG") == 0) ((SimpleRTPSink*)fRTPSink)->setMBitOnNextPacket();
//
//  // Complete delivery:
//  FramedSource::afterGetting(this);
//}
//
//void PresentationTimeSubsessionNormalizer::doGetNextFrame() {
//  fInputSource->getNextFrame(fTo, fMaxSize, afterGettingFrame, this, FramedSource::handleClosure, this);
//}
