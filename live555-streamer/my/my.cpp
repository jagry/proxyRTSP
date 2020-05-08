#include <sys/types.h>
#include <sys/stat.h>
#include <thread>

#include <liveMedia.hh>
#include <BasicUsageEnvironment.hh>
#include <GroupsockHelper.hh>


#include <iostream>



extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavutil/imgutils.h>
    #include <libavutil/opt.h>
    #include <libswscale/swscale.h>
}


//#include <LiveServerMediaSubsession.h>



//extern "C"{
//    #include <libavformat/avformat.h>
//    #include <libavformat/avio.h>
//    #include  <libavcodec/avcodec.h>
//    #include  <libavformat/avformat.h>
//    #include <libavutil/imgutils.h> //for av_image_alloc only
//    #include <libavutil/opt.h>
//}

//#include <stdlib.h>     /* realloc, free, exit, NULL */


static AVCodecContext *c = NULL;
static AVFrame *frame;
static AVPacket pkt;
static FILE *file;
struct SwsContext *sws_context = NULL;

static void ffmpeg_encoder_set_frame_yuv_from_rgb(uint8_t *rgb) {
    const int in_linesize[1] = { 3 * c->width };
    sws_context = sws_getCachedContext(sws_context,
                                       c->width, c->height, AV_PIX_FMT_RGB24,
                                       c->width, c->height, AV_PIX_FMT_YUV420P,
                                       0, 0, 0, 0);
    sws_scale(sws_context, (const uint8_t * const *)&rgb, in_linesize, 0,
              c->height, frame->data, frame->linesize);
}

uint8_t* generate_rgb(int width, int height, int pts, uint8_t *rgb) {
    int x, y, cur;
    rgb = static_cast<uint8_t *>(realloc(rgb, 3 * sizeof(uint8_t) * height * width));
//    rgb = realloc(rgb, 3 * sizeof(uint8_t) * height * width);
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            cur = 3 * (y * width + x);
            rgb[cur + 0] = 0;
            rgb[cur + 1] = 0;
            rgb[cur + 2] = 0;
            if ((frame->pts / 25) % 2 == 0) {
                if (y < height / 2) {
                    if (x < width / 2) {
                        /* Black. */
                    } else {
                        rgb[cur + 0] = 255;
                    }
                } else {
                    if (x < width / 2) {
                        rgb[cur + 1] = 255;
                    } else {
                        rgb[cur + 2] = 255;
                    }
                }
            } else {
                if (y < height / 2) {
                    rgb[cur + 0] = 255;
                    if (x < width / 2) {
                        rgb[cur + 1] = 255;
                    } else {
                        rgb[cur + 2] = 255;
                    }
                } else {
                    if (x < width / 2) {
                        rgb[cur + 1] = 255;
                        rgb[cur + 2] = 255;
                    } else {
                        rgb[cur + 0] = 255;
                        rgb[cur + 1] = 255;
                        rgb[cur + 2] = 255;
                    }
                }
            }
        }
    }
    return rgb;
}

/* Allocate resources and write header data to the output file. */
void ffmpeg_encoder_start(const char *filename, enum AVCodecID codec_id, int fps, int width, int height) {
    AVCodec *codec;
    int ret;

    codec = avcodec_find_encoder(codec_id);
    if (!codec) {
        fprintf(stderr, "Codec not found\n");
        exit(1);
    }
    c = avcodec_alloc_context3(codec);
    if (!c) {
        fprintf(stderr, "Could not allocate video codec context\n");
        exit(1);
    }
    c->bit_rate = 400000;
    c->width = width;
    c->height = height;
    c->time_base.num = 1;
    c->time_base.den = fps;
    c->keyint_min = 600;
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    if (codec_id == AV_CODEC_ID_H264)
        av_opt_set(c->priv_data, "preset", "slow", 0);
    if (avcodec_open2(c, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        exit(1);
    }
    file = fopen(filename, "wb");
    if (!file) {
        fprintf(stderr, "Could not open %s\n", filename);
        exit(1);
    }
    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(1);
    }
    frame->format = c->pix_fmt;
    frame->width  = c->width;
    frame->height = c->height;
    ret = av_image_alloc(frame->data, frame->linesize, c->width, c->height, c->pix_fmt, 32);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate raw picture buffer\n");
        exit(1);
    }
}

/*
Write trailing data to the output file
and free resources allocated by ffmpeg_encoder_start.
*/
void ffmpeg_encoder_finish(void) {
    uint8_t endcode[] = { 0, 0, 1, 0xb7 };
    int got_output, ret;
    do {
        fflush(stdout);
        ret = avcodec_encode_video2(c, &pkt, NULL, &got_output);
        if (ret < 0) {
            fprintf(stderr, "Error encoding frame\n");
            exit(1);
        }
        if (got_output) {
            fwrite(pkt.data, 1, pkt.size, file);
            av_packet_unref(&pkt);
        }
    } while (got_output);
    fwrite(endcode, 1, sizeof(endcode), file);
    fclose(file);
    avcodec_close(c);
    av_free(c);
    av_freep(&frame->data[0]);
    av_frame_free(&frame);
}

/*
Encode one frame from an RGB24 input and save it to the output file.
Must be called after ffmpeg_encoder_start, and ffmpeg_encoder_finish
must be called after the last call to this function.
*/
void ffmpeg_encoder_encode_frame(uint8_t *rgb) {
    int ret, got_output;
    ffmpeg_encoder_set_frame_yuv_from_rgb(rgb);
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;
    if (frame->pts == 1) {
        frame->key_frame = 1;
        frame->pict_type = AV_PICTURE_TYPE_I;
    } else {
        frame->key_frame = 0;
        frame->pict_type = AV_PICTURE_TYPE_P;
    }
    ret = avcodec_encode_video2(c, &pkt, frame, &got_output);
    if (ret < 0) {
        fprintf(stderr, "Error encoding frame\n");
        exit(1);
    }
    if (got_output) {
        fwrite(pkt.data, 1, pkt.size, file);
        av_packet_unref(&pkt);
    }
}

/* Represents the main loop of an application which generates one frame per loop. */
static void encode_example(const char *filename, enum AVCodecID codec_id) {
    int pts;
    int width = 320;
    int height = 240;
    uint8_t *rgb = NULL;
    ffmpeg_encoder_start(filename, codec_id, 25, width, height);
    for (pts = 0; pts < 100000; pts++) {
        frame->pts = pts;
        rgb = generate_rgb(width, height, pts, rgb);
        ffmpeg_encoder_encode_frame(rgb);
    }
    ffmpeg_encoder_finish();
}


void write_to_file() {
    char const* inputFileName = "/tmp/fifo";


    mkfifo(inputFileName, 0777);

    avcodec_register_all();
//    encode_example("tmp.h264", AV_CODEC_ID_H264);
    encode_example(inputFileName, AV_CODEC_ID_H264);
//    encode_example("tmp.mpg", AV_CODEC_ID_MPEG1VIDEO);
}


UsageEnvironment* env;
char const* inputFileName = "/tmp/fifo";
//char const* inputFileName = "test.264";
H264VideoStreamFramer* videoSource;
RTPSink* videoSink;

void play(); // forward



void multi_cast() {


    // Begin by setting up our usage environment:
    TaskScheduler* scheduler = BasicTaskScheduler::createNew();
    env = BasicUsageEnvironment::createNew(*scheduler);

    // Create 'groupsocks' for RTP and RTCP:
    struct in_addr destinationAddress;
    destinationAddress.s_addr = chooseRandomIPv4SSMAddress(*env);
    // Note: This is a multicast address.  If you wish instead to stream
    // using unicast, then you should use the "testOnDemandRTSPServer"
    // test program - not this test program - as a model.

    const unsigned short rtpPortNum = 18888;
    const unsigned short rtcpPortNum = rtpPortNum+1;
    const unsigned char ttl = 255;

    const Port rtpPort(rtpPortNum);
    const Port rtcpPort(rtcpPortNum);

    Groupsock rtpGroupsock(*env, destinationAddress, rtpPort, ttl);
    rtpGroupsock.multicastSendOnly(); // we're a SSM source
    Groupsock rtcpGroupsock(*env, destinationAddress, rtcpPort, ttl);
    rtcpGroupsock.multicastSendOnly(); // we're a SSM source

    // Create a 'H264 Video RTP' sink from the RTP 'groupsock':
    OutPacketBuffer::maxSize = 100000;
    videoSink = H264VideoRTPSink::createNew(*env, &rtpGroupsock, 96);

    // Create (and start) a 'RTCP instance' for this RTP sink:
    const unsigned estimatedSessionBandwidth = 500; // in kbps; for RTCP b/w share
    const unsigned maxCNAMElen = 100;
    unsigned char CNAME[maxCNAMElen+1];
    gethostname((char*)CNAME, maxCNAMElen);
    CNAME[maxCNAMElen] = '\0'; // just in case
    RTCPInstance* rtcp
            = RTCPInstance::createNew(*env, &rtcpGroupsock,
                                      estimatedSessionBandwidth, CNAME,
                                      videoSink, NULL /* we're a server */,
                                      True /* we're a SSM source */);
    // Note: This starts RTCP running automatically

    RTSPServer* rtspServer = RTSPServer::createNew(*env, 8554);
    if (rtspServer == NULL) {
        *env << "Failed to create RTSP server: " << env->getResultMsg() << "\n";
        exit(1);
    }
    ServerMediaSession* sms
            = ServerMediaSession::createNew(*env, "testStream", inputFileName,
                                            "Session streamed by \"testH264VideoStreamer\"",
                                            True /*SSM*/);
    sms->addSubsession(PassiveServerMediaSubsession::createNew(*videoSink, rtcp));
    rtspServer->addServerMediaSession(sms);

    char* url = rtspServer->rtspURL(sms);
    *env << "Play this stream using the URL \"" << url << "\"\n";
    delete[] url;

    // Start the streaming:
    *env << "Beginning streaming...\n";
    play();

    env->taskScheduler().doEventLoop(); // does not return

}



Boolean reuseFirstSource = False;

// To stream *only* MPEG-1 or 2 video "I" frames
// (e.g., to reduce network bandwidth),
// change the following "False" to "True":
Boolean iFramesOnly = False;

static void announceStream(RTSPServer* rtspServer, ServerMediaSession* sms,
                           char const* streamName, char const* inputFileName); // fwd



void single_cast() {
// Begin by setting up our usage environment:
    TaskScheduler* scheduler = BasicTaskScheduler::createNew();
    env = BasicUsageEnvironment::createNew(*scheduler);

    UserAuthenticationDatabase* authDB = NULL;
#ifdef ACCESS_CONTROL
    // To implement client access control to the RTSP server, do the following:
  authDB = new UserAuthenticationDatabase;
  authDB->addUserRecord("username1", "password1"); // replace these with real strings
  // Repeat the above with each <username>, <password> that you wish to allow
  // access to the server.
#endif

//     Create the RTSP server:
    RTSPServer* rtspServer = RTSPServer::createNew(*env, 8554, authDB);
    if (rtspServer == NULL) {
        *env << "Failed to create RTSP server: " << env->getResultMsg() << "\n";
        exit(1);
    }

    char const* descriptionString = "Session streamed by \"testOnDemandRTSPServer\"";

//     Set up each of the possible streams that can be served by the
//     RTSP server.  Each such stream is implemented using a
//     "ServerMediaSession" object, plus one or more
//     "ServerMediaSubsession" objects for each audio/video substream.

//     A H.265 video elementary stream:
    {
        char const* streamName = "h264ESVideoTest";
//        char const* inputFileName = "test.265";

        ServerMediaSession* sms = ServerMediaSession::createNew(*env, streamName, streamName, descriptionString);
        sms->addSubsession(H264VideoFileServerMediaSubsession::createNew(*env, inputFileName, reuseFirstSource));




//        FFmpegH264Source * source = FFmpegH264Source::createNew(*env, m_Encoder);
//        StreamReplicator * inputDevice = StreamReplicator::createNew(*env, source, false);
//
//
//
//        sms->addSubsession(LiveServerMediaSubsession::createNew(*env, inputDevice));
        rtspServer->addServerMediaSession(sms);

        announceStream(rtspServer, sms, streamName, inputFileName);
    }

//     Also, attempt to create a HTTP server for RTSP-over-HTTP tunneling.
//     Try first with the default HTTP port (80), and then with the alternative HTTP
//     port numbers (8000 and 8080).

    if (rtspServer->setUpTunnelingOverHTTP(80) || rtspServer->setUpTunnelingOverHTTP(8000) || rtspServer->setUpTunnelingOverHTTP(8080)) {
        *env << "\n(We use port " << rtspServer->httpServerPortNum() << " for optional RTSP-over-HTTP tunneling.)\n";
    } else {
        *env << "\n(RTSP-over-HTTP tunneling is not available.)\n";
    }

    env->taskScheduler().doEventLoop(); // does not return
}

int main(void) {

//    std::string TNAME1 = "AAAAAAAAAA";

//    *env << "\n!!!!!! !!!!!! !!!!!! !!!!!! !!!!!!\n" << NAME111;

//    std::cout << "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" << NAME111;



    std::thread th1(write_to_file);


//    multi_cast();

    single_cast();


    th1.join();

    return 0;
}


static void announceStream(RTSPServer* rtspServer, ServerMediaSession* sms,
                           char const* streamName, char const* inputFileName) {
    char* url = rtspServer->rtspURL(sms);
    UsageEnvironment& env = rtspServer->envir();
    env << "\n\"" << streamName << "\" stream, from the file \""
        << inputFileName << "\"\n";
    env << "Play this stream using the URL \"" << url << "\"\n";
    delete[] url;
}


void afterPlaying(void* /*clientData*/) {
    *env << "...done reading from file\n";
    videoSink->stopPlaying();
    Medium::close(videoSource);
    // Note that this also closes the input file that this source read from.

    // Start playing once again:
    play();
}

void play() {
    // Open the input file as a 'byte-stream file source':
    ByteStreamFileSource* fileSource
            = ByteStreamFileSource::createNew(*env, inputFileName);
    if (fileSource == NULL) {
        *env << "Unable to open file \"" << inputFileName
             << "\" as a byte-stream file source\n";
        exit(1);
    }

    FramedSource* videoES = fileSource;

    // Create a framer for the Video Elementary Stream:
    videoSource = H264VideoStreamFramer::createNew(*env, videoES);

    // Finally, start playing:
    *env << "Beginning to read from file...\n";
    videoSink->startPlaying(*videoSource, afterPlaying, videoSink);
}
