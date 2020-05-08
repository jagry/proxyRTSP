
//std::string TNAME = "AAAAAAAAAA";


int NAME111 = 121212;


class LiveServerMediaSubsession: public OnDemandServerMediaSubsession
{
public:
    static LiveServerMediaSubsession* createNew(UsageEnvironment& env, StreamReplicator* replicator);

protected:
    LiveServerMediaSubsession(UsageEnvironment& env, StreamReplicator* replicator)
            : OnDemandServerMediaSubsession(env, False), m_replicator(replicator) {};

    virtual FramedSource* createNewStreamSource(unsigned clientSessionId, unsigned& estBitrate);
    virtual RTPSink* createNewRTPSink(Groupsock* rtpGroupsock,  unsigned char rtpPayloadTypeIfDynamic, FramedSource* inputSource);

    StreamReplicator * m_replicator;
};


LiveServerMediaSubsession * LiveServerMediaSubsession::createNew(UsageEnvironment& env, StreamReplicator* replicator)
{
    return new LiveServerMediaSubsession(env,replicator);
}

FramedSource* LiveServerMediaSubsession::createNewStreamSource(unsigned clientSessionId, unsigned& estBitrate)
{
    FramedSource* source = m_replicator->createStreamReplica();
    return H264VideoStreamDiscreteFramer::createNew(envir(), source);
}

RTPSink* LiveServerMediaSubsession::createNewRTPSink(Groupsock* rtpGroupsock,  unsigned char rtpPayloadTypeIfDynamic, FramedSource* inputSource)
{
    return H264VideoRTPSink::createNew(envir(), rtpGroupsock,rtpPayloadTypeIfDynamic);
}








class FFmpegH264Source : public FramedSource {
public:
    static FFmpegH264Source* createNew(UsageEnvironment& env, FFmpegH264Encoder * E_Source);
    FFmpegH264Source(UsageEnvironment& env, FFmpegH264Encoder *  E_Source);
    ~FFmpegH264Source();

private:
    static void deliverFrameStub(void* clientData) {((FFmpegH264Source*) clientData)->deliverFrame();};
    virtual void doGetNextFrame();
    void deliverFrame();
    virtual void doStopGettingFrames();
    void onFrame();


private:
    FFmpegH264Encoder * Encoding_Source;
    EventTriggerId m_eventTriggerId;

};






FFmpegH264Source * FFmpegH264Source::createNew(UsageEnvironment& env, FFmpegH264Encoder * E_Source) {
    return new FFmpegH264Source(env, E_Source);
}

FFmpegH264Source::FFmpegH264Source(UsageEnvironment& env, FFmpegH264Encoder * E_Source) : FramedSource(env), Encoding_Source(E_Source)
{
    m_eventTriggerId = envir().taskScheduler().createEventTrigger(FFmpegH264Source::deliverFrameStub);
    std::function<void()> callback1 = std::bind(&FFmpegH264Source::onFrame, this);
    Encoding_Source -> setCallbackFunctionFrameIsReady(callback1);
}

FFmpegH264Source::~FFmpegH264Source()
{

}

void FFmpegH264Source::doStopGettingFrames()
{
    FramedSource::doStopGettingFrames();
}

void FFmpegH264Source::onFrame()
{
    envir().taskScheduler().triggerEvent(m_eventTriggerId, this);
}




void FFmpegH264Source::doGetNextFrame()
{
    deliverFrame();
}

void FFmpegH264Source::deliverFrame()
{
    if (!isCurrentlyAwaitingData()) return; // we're not ready for the data yet

    static uint8_t* newFrameDataStart;
    static unsigned newFrameSize = 0;

    /* get the data frame from the Encoding thread.. */
    if (Encoding_Source->GetFrame(&newFrameDataStart, &newFrameSize)){
        if (newFrameDataStart!=NULL) {
            /* This should never happen, but check anyway.. */
            if (newFrameSize > fMaxSize) {
                fFrameSize = fMaxSize;
                fNumTruncatedBytes = newFrameSize - fMaxSize;
            } else {
                fFrameSize = newFrameSize;
            }

            gettimeofday(&fPresentationTime, NULL);
            memcpy(fTo, newFrameDataStart, fFrameSize);

            //delete newFrameDataStart;
            //newFrameSize = 0;

            Encoding_Source->ReleaseFrame();
        }
        else {
            fFrameSize=0;
            fTo=NULL;
            handleClosure(this);
        }
    }else
    {
        fFrameSize = 0;
    }

    if(fFrameSize>0)
        FramedSource::afterGetting(this);

}