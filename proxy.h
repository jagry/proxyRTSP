#ifndef _MY_PROXY_SERVER_MEDIA_SESSION_HH
#define _MY_PROXY_SERVER_MEDIA_SESSION_HH

#ifndef _SERVER_MEDIA_SESSION_HH
#include "ServerMediaSession.hh"
#endif
#ifndef _MEDIA_SESSION_HH
#include "MediaSession.hh"
#endif
#ifndef _RTSP_CLIENT_HH
#include "RTSPClient.hh"
#endif
#ifndef _MEDIA_TRANSCODING_TABLE_HH
#include "MediaTranscodingTable.hh"
#endif

// A subclass of "RTSPClient", used to refer to the particular "ProxyServerMediaSession" object being used.
// It is used only within the implementation of "ProxyServerMediaSession", but is defined here, in case developers wish to
// subclass it.

class MyRTSPClient: public RTSPClient {
public:
  MyRTSPClient(class MyServerMediaSession& ourServerMediaSession, char const* rtspURL,
                  char const* username, char const* password,
                  portNumBits tunnelOverHTTPPortNum, int verbosityLevel, int socketNumToServer);
  virtual ~MyRTSPClient();

  void continueAfterDESCRIBE(char const* sdpDescription);
  void continueAfterLivenessCommand(int resultCode, Boolean serverSupportsGetParameter);
  void continueAfterSETUP(int resultCode);
  void continueAfterPLAY(int resultCode);
  void scheduleReset();

private:
  void reset();
  int connectToServer(int socketNum, portNumBits remotePortNum);

  Authenticator* auth() { return fOurAuthenticator; }

  void scheduleLivenessCommand();
  static void sendLivenessCommand(void* clientData);
  void doReset();
  static void doReset(void* clientData);

  void scheduleDESCRIBECommand();
  static void sendDESCRIBE(void* clientData);
  void sendDESCRIBE();

  static void subsessionTimeout(void* clientData);
  void handleSubsessionTimeout();

private:
  friend class MyServerMediaSession;
  friend class MyServerMediaSubsession;
  MyServerMediaSession& fOurServerMediaSession;
  char* fOurURL;
  Authenticator* fOurAuthenticator;
  Boolean fStreamRTPOverTCP;
  class MyServerMediaSubsession *fSetupQueueHead, *fSetupQueueTail;
  unsigned fNumSetupsDone;
  unsigned fNextDESCRIBEDelay; // in seconds
  Boolean fServerSupportsGetParameter, fLastCommandWasPLAY, fDoneDESCRIBE;
  TaskToken fLivenessCommandTask, fDESCRIBECommandTask, fSubsessionTimerTask, fResetTask;
};


typedef MyRTSPClient*
createNewMyRTSPClientFunc(MyServerMediaSession& ourServerMediaSession,
			     char const* rtspURL,
			     char const* username, char const* password,
			     portNumBits tunnelOverHTTPPortNum, int verbosityLevel,
			     int socketNumToServer);
MyRTSPClient*
defaultCreateNewMyRTSPClientFunc(MyServerMediaSession& ourServerMediaSession,
				    char const* rtspURL,
				    char const* username, char const* password,
				    portNumBits tunnelOverHTTPPortNum, int verbosityLevel,
				    int socketNumToServer);

class MyServerMediaSession: public ServerMediaSession {
public:
  static MyServerMediaSession* createNew(UsageEnvironment& env,
					    GenericMediaServer* ourMediaServer, // Note: We can be used by just one server
					    char const* inputStreamURL, // the "rtsp://" URL of the stream we'll be proxying
					    char const* streamName = NULL,
					    char const* username = NULL, char const* password = NULL,
					    portNumBits tunnelOverHTTPPortNum = 0,
					        // for streaming the *proxied* (i.e., back-end) stream
					    int verbosityLevel = 0,
					    int socketNumToServer = -1,
					    MediaTranscodingTable* transcodingTable = NULL);
      // Hack: "tunnelOverHTTPPortNum" == 0xFFFF (i.e., all-ones) means: Stream RTP/RTCP-over-TCP, but *not* using HTTP
      // "verbosityLevel" == 1 means display basic proxy setup info; "verbosityLevel" == 2 means display RTSP client protocol also.
      // If "socketNumToServer" is >= 0, then it is the socket number of an already-existing TCP connection to the server.
      //      (In this case, "inputStreamURL" must point to the socket's endpoint, so that it can be accessed via the socket.)

  virtual ~MyServerMediaSession();

  char const* url() const;

  char describeCompletedFlag;
    // initialized to 0; set to 1 when the back-end "DESCRIBE" completes.
    // (This can be used as a 'watch variable' in "doEventLoop()".)
  Boolean describeCompletedSuccessfully() const { return fClientMediaSession != NULL; }
    // This can be used - along with "describeCompletedFlag" - to check whether the back-end "DESCRIBE" completed *successfully*.

protected:
  MyServerMediaSession(UsageEnvironment& env, GenericMediaServer* ourMediaServer,
			  char const* inputStreamURL, char const* streamName,
			  char const* username, char const* password,
			  portNumBits tunnelOverHTTPPortNum, int verbosityLevel,
			  int socketNumToServer,
			  MediaTranscodingTable* transcodingTable,
			  createNewMyRTSPClientFunc* ourCreateNewMyRTSPClientFunc
			  = defaultCreateNewMyRTSPClientFunc,
			  portNumBits initialPortNum = 6970,
			  Boolean multiplexRTCPWithRTP = False);

  virtual Groupsock* createGroupsock(struct in_addr const& addr, Port port);
  virtual RTCPInstance* createRTCP(Groupsock* RTCPgs, unsigned totSessionBW, /* in kbps */
				   unsigned char const* cname, RTPSink* sink);

  virtual Boolean allowProxyingForSubsession(MediaSubsession const& mss);
  // By default, this function always returns True.  However, a subclass may redefine this
  // if it wishes to restrict which subsessions of a stream get proxied - e.g., if it wishes
  // to proxy only video tracks, but not audio (or other) tracks.

protected:
  GenericMediaServer* fOurMediaServer;
  MyRTSPClient* fMyRTSPClient;
  MediaSession* fClientMediaSession;

private:
  friend class MyRTSPClient;
  friend class MyServerMediaSubsession;
  void continueAfterDESCRIBE(char const* sdpDescription);
  void resetDESCRIBEState(); // undoes what was done by "contineAfterDESCRIBE()"

private:
  int fVerbosityLevel;
  class PresentationTimeSessionNormalizer* fPresentationTimeSessionNormalizer;
  createNewMyRTSPClientFunc* fCreateNewMyRTSPClientFunc;
  MediaTranscodingTable* fTranscodingTable;
  portNumBits fInitialPortNum;
  Boolean fMultiplexRTCPWithRTP;
};


////////// PresentationTimeSessionNormalizer and PresentationTimeSubsessionNormalizer definitions //////////

/*class MyPresentationTimeSubsessionNormalizer: public FramedFilter {
public:
  void setRTPSink(RTPSink* rtpSink) { fRTPSink = rtpSink; }

private:
  friend class MyPresentationTimeSessionNormalizer;
  MyPresentationTimeSubsessionNormalizer(MyPresentationTimeSessionNormalizer& parent, FramedSource* inputSource, RTPSource* rtpSource,
				       char const* codecName, MyPresentationTimeSubsessionNormalizer* next);
      // called only from within "PresentationTimeSessionNormalizer"
  virtual ~MyPresentationTimeSubsessionNormalizer();

  static void afterGettingFrame(void* clientData, unsigned frameSize,
                                unsigned numTruncatedBytes,
                                struct timeval presentationTime,
                                unsigned durationInMicroseconds);
  void afterGettingFrame(unsigned frameSize,
			 unsigned numTruncatedBytes,
			 struct timeval presentationTime,
			 unsigned durationInMicroseconds);

private: // redefined virtual functions:
  virtual void doGetNextFrame();

private:
  MyPresentationTimeSessionNormalizer& fParent;
  RTPSource* fRTPSource;
  RTPSink* fRTPSink;
  char const* fCodecName;
  MyPresentationTimeSubsessionNormalizer* fNext;
};

class MyPresentationTimeSessionNormalizer: public Medium {
public:
  MyPresentationTimeSessionNormalizer(UsageEnvironment& env);
  virtual ~MyPresentationTimeSessionNormalizer();

  MyPresentationTimeSubsessionNormalizer*
  createNewPresentationTimeSubsessionNormalizer(FramedSource* inputSource, RTPSource* rtpSource, char const* codecName);

private: // called only from within "~PresentationTimeSubsessionNormalizer":
  friend class MyPresentationTimeSubsessionNormalizer;
  void normalizePresentationTime(MyPresentationTimeSubsessionNormalizer* ssNormalizer,
				 struct timeval& toPT, struct timeval const& fromPT);
  void removePresentationTimeSubsessionNormalizer(MyPresentationTimeSubsessionNormalizer* ssNormalizer);

private:
  MyPresentationTimeSubsessionNormalizer* fSubsessionNormalizers;
  MyPresentationTimeSubsessionNormalizer* fMasterSSNormalizer; // used for subsessions that have been RTCP-synced

  struct timeval fPTAdjustment; // Added to (RTCP-synced) subsession presentation times to 'normalize' them with wall-clock time.
};*/


#endif
