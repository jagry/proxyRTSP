/* ---------------------------------------------------------------------------
** This software is in the public domain, furnished "as is", without technical
** support, and with no warranty, express or implied, as to its usefulness for
** any purpose.
**
** livevideosource.h
**
** -------------------------------------------------------------------------*/

#pragma once

#include <string.h>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>

#include "environment.h"

#include "libyuv/video_common.h"
#include "libyuv/convert.h"

#include "media/base/codec.h"
#include "media/base/video_common.h"
#include "media/base/video_broadcaster.h"
#include "media/engine/internal_decoder_factory.h"

#include "common_video/h264/h264_common.h"
#include "common_video/h264/sps_parser.h"
#include "modules/video_coding/h264_sprop_parameter_sets.h"

#include "api/video_codecs/video_decoder.h"

#include "VideoDecoder.h"

#include <regex>
#include <iomanip>
#include <sstream>

// RTSPConnection

template <typename T>
class LiveVideoSource : public rtc::VideoSourceInterface<webrtc::VideoFrame>, public T::Callback
{
public:
    LiveVideoSource(const std::string &uri, const std::map<std::string, std::string> &opts, bool wait) :
        m_env(m_stop),
        meta_counter(0),
        video_counter(0),
	    m_liveclient(m_env, this, uri.c_str(), opts, rtc::LogMessage::GetLogToDebug()<=2),
	    m_decoder(m_broadcaster, opts, wait) {
            this->Start();
    }
    virtual ~LiveVideoSource() {
            this->Stop();
    }

    void Start()
    {
        RTC_LOG(INFO) << "LiveVideoSource::Start";
        m_capturethread = std::thread(&LiveVideoSource::CaptureThread, this);
        m_decoder.Start();
    }
    void Stop()
    {
        RTC_LOG(INFO) << "LiveVideoSource::stop";
        m_env.stop();
        m_capturethread.join();
        m_decoder.Stop();
    }
    bool IsRunning() { return (m_stop == 0); }

    void CaptureThread()
    {
        m_env.mainloop();
    }

    // overide T::Callback
    virtual bool onNewSession(const char *id, const char *media, const char *codec, const char *sdp)
    {

        if ( strcmp(media, "application") == 0) {
            m_codec[id] = codec;
            return true;
        }

        bool success = false;
        if (strcmp(media, "video") == 0)
        {
            RTC_LOG(INFO) << "LiveVideoSource::onNewSession " << media << "/" << codec << " " << sdp;

            if ( (strcmp(codec, "H264") == 0)
               || (strcmp(codec, "JPEG") == 0)
               || (strcmp(codec, "VP9") == 0) )
            {
                m_codec[id] = codec;
                success = true;
            }

            if (success) 
            {
                struct timeval presentationTime;
                timerclear(&presentationTime);

                std::vector<std::vector<uint8_t>> initFrames = m_decoder.getInitFrames(codec, sdp);
                for (auto frame : initFrames)
                {
                    onData(id, frame.data(), frame.size(), presentationTime);
                }
            }
        }
        return success;
    }

    std::string getDateFromXml(const char* data) {
        std::regex re("UtcTime=\"(.*?)\"");
        std::smatch match;
        std::string result;
        std::string inputStr(data);

        if (std::regex_search(inputStr, match, re) && match.size() > 1) {
            result = match.str(1);
        } else {
            result = "------";
        }
        return result;
    }

    std::string time_in_HH_MM_SS_MMM()
    {
        using namespace std::chrono;

        // get current time
        auto now = system_clock::now();

        // get number of milliseconds for the current second
        // (remainder after division into seconds)
        auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

        // convert to std::time_t in order to convert to std::tm (broken time)
        auto timer = system_clock::to_time_t(now);

        // convert to broken time
        std::tm bt = *std::localtime(&timer);

        std::ostringstream oss;

        oss << std::put_time(&bt, "%H:%M:%S"); // HH:MM:SS
        oss << '.' << std::setfill('0') << std::setw(3) << ms.count();

        return oss.str();
    }

    std::string fromTs(int64_t ts)
    {
        using namespace std::chrono;

        std::chrono::milliseconds dur(ts);
        time_point<system_clock> now(dur);


        // get current time
        //auto now = system_clock::now();

        // get number of milliseconds for the current second
        // (remainder after division into seconds)
        auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

        // convert to std::time_t in order to convert to std::tm (broken time)
        auto timer = system_clock::to_time_t(now);

        // convert to broken time
        std::tm bt = *std::gmtime(&timer);
        //std::tm bt = *std::localtime(&timer);


        std::ostringstream oss;

        oss << std::put_time(&bt, "%Y-%m-%d %H:%M:%S"); // HH:MM:SS
        oss << '.' << std::setfill('0') << std::setw(3) << ms.count();

        return oss.str();
    }

    VideoDecoder::RectResult getRectFromXml(const char* data) {


        std::regex re("<tt:Object ObjectId=\"(.*?)\"><tt:Appearance>"
                      "<tt:Shape><tt:BoundingBox left=\"(.*?)\" top=\"(.*?)\" right=\"(.*?)\" bottom=\"(.*?)\"/></tt:Shape>"
                      "<tt:Class><tt:Type PersonId=\"(.*?)\" Score=\"(.*?)\" Age=\"(.*?)\" Gender=\"(.*?)\" Emotion=\"(.*?)\">(.*?)</tt:Type></tt:Class>"
                      "</tt:Appearance></tt:Object>");

//        std::smatch match;
        std::string inputStr(data);


        VideoDecoder::RectResult rectResult;

        rectResult.dateStr = "";

//      if (std::regex_search(inputStr, match, re) && match.size() > 1) {
        for (std::smatch match; std::regex_search(inputStr, match, re); inputStr = match.suffix()) {

            VideoDecoder::RectItem rectItem;

//            rectItem.isFound = true;
            rectItem.objectId = std::stoi(match.str(1));

            rectItem.left = std::stof(match.str(2));
            rectItem.top = std::stof(match.str(3));
            rectItem.right = std::stof(match.str(4));
            rectItem.bottom = std::stof(match.str(5));
            rectItem.personName = match.str(6);
            rectItem.score = std::stof(match.str(7));

            rectItem.age = std::stoi(match.str(8));

            rectItem.gender = match.str(9);
            rectItem.emotion = match.str(10);
            std::string s = match.str(11);
            rectItem.foundPerson = match.str(11).compare("Found") == 0;

            rectResult.rects.push_back(rectItem);


        }
        return rectResult;
    }

    void proccessMeta(unsigned char *buffer, ssize_t size) {
        char * tmp = new char[size + 1];

        tmp[size] = 0;
        strncpy(tmp, (const char*)buffer, size);

        VideoDecoder::RectResult rect = getRectFromXml((const char*) tmp);

        std::string dateStr = getDateFromXml((const char*) tmp);
        rect.dateStr = dateStr;

        RTC_LOG(LS_VERBOSE) << "!!!!!!!!!!!!!!!xxxxxxxxxxxx " << rect.rects.size() << " " << rect.dateStr;

        //TODO zones
        //TODO delete
        delete []tmp;
        m_decoder.PostMeta(rect);
    }

    virtual bool onData(const char *id, unsigned char *buffer, ssize_t size, struct timeval presentationTime) {
        auto tvnow = time_in_HH_MM_SS_MMM();

        int64_t ts = presentationTime.tv_sec;
        ts = ts * 1000 + presentationTime.tv_usec / 1000;

        int res = 0;

        std::string codec = m_codec[id];

        std::string postfix1 = "";

        if (codec == "VND.ONVIF.METADATA") {

            char * tmp = new char[size + 1];
            tmp[size] = 0;
    //        strcpy(tmp, (char *)fReceiveBuffer)
            strncpy(tmp, (const char*)buffer, size);
//            RTC_LOG(LS_VERBOSE) << ">>>>>>>>>>" << tmp << " : " << getDateFromXml((const char*) tmp) << " : " << tvnow;

//            postfix1 = getDateFromXml((const char*) tmp);

            delete []tmp;
        }


//        RTC_LOG(LS_VERBOSE) << "!!!!!!!!!!!!!!!xxxxxxxxxxxx " << rect.rects.size() << " " << rect.dateStr;
        if (codec == "VND.ONVIF.METADATA") {
//            RTC_LOG(LS_VERBOSE) << "@@@@@@@@@LiveVideoSource:onData id:" << id << " size:" << size << " ts:" << ts << " cod:"
//                                << m_codec[id] << " TS : " << fromTs(ts) << " now: " << tvnow << "(" << postfix1 << ")" << buffer;
            proccessMeta(buffer, size);


            meta_counter++;
            return true;
        } else {
//            RTC_LOG(LS_VERBOSE) << "________LiveVideoSource:onData id:" << id << " size:" << size << " ts:" << ts << " cod:"
//                                << m_codec[id] << " TS : " << fromTs(ts) << " now: " << tvnow << "(" << postfix1 << ")";

        }

        RTC_LOG(LS_VERBOSE) << "!!!!!!!!!!!!!!!kkkkkkkkkkkkkkkk " << meta_counter << " " << video_counter;


        if (codec == "H264")
        {
//            RTC_LOG(LS_VERBOSE) << "!!!!!!!!!!!!!!!mmmmmmmmm " << ts;
            video_counter++;


            webrtc::H264::NaluType nalu_type = webrtc::H264::ParseNaluType(buffer[sizeof(H26X_marker)]);
            if (nalu_type == webrtc::H264::NaluType::kSps)
            {
                RTC_LOG(LS_VERBOSE) << "LiveVideoSource:onData SPS";
                m_cfg.clear();
                m_cfg.insert(m_cfg.end(), buffer, buffer + size);

                absl::optional<webrtc::SpsParser::SpsState> sps = webrtc::SpsParser::ParseSps(buffer + sizeof(H26X_marker) + webrtc::H264::kNaluTypeSize, size - sizeof(H26X_marker) - webrtc::H264::kNaluTypeSize);
                if (!sps)
                {
                    RTC_LOG(LS_ERROR) << "cannot parse sps";
                    res = -1;
                }
                else
                {
                    if (m_decoder.hasDecoder())
                    {
                        if ((m_format.width != sps->width) || (m_format.height != sps->height))
                        {
                            RTC_LOG(INFO) << "format changed => set format from " << m_format.width << "x" << m_format.height << " to " << sps->width << "x" << sps->height;
                            m_decoder.destroyDecoder();
                        }
                    }

                    if (!m_decoder.hasDecoder())
                    {
                        int fps = 25;
                        RTC_LOG(INFO) << "LiveVideoSource:onData SPS set format " << sps->width << "x" << sps->height << " fps:" << fps;
                        cricket::VideoFormat videoFormat(sps->width, sps->height, cricket::VideoFormat::FpsToInterval(fps), cricket::FOURCC_I420);
                        m_format = videoFormat;

                        m_decoder.createDecoder(codec);
                    }
                }
            }
            else if (nalu_type == webrtc::H264::NaluType::kPps)
            {
                RTC_LOG(LS_VERBOSE) << "LiveVideoSource:onData PPS";
                m_cfg.insert(m_cfg.end(), buffer, buffer + size);
            }
            else if (nalu_type == webrtc::H264::NaluType::kSei) 
            {
            }            
            else if (m_decoder.hasDecoder())
            {
                webrtc::VideoFrameType frameType = webrtc::VideoFrameType::kVideoFrameDelta;
                std::vector<uint8_t> content;
                if (nalu_type == webrtc::H264::NaluType::kIdr)
                {
                    frameType = webrtc::VideoFrameType::kVideoFrameKey;
                    RTC_LOG(LS_VERBOSE) << "LiveVideoSource:onData IDR";
                    content.insert(content.end(), m_cfg.begin(), m_cfg.end());
                }
                else
                {
//                    RTC_LOG(LS_VERBOSE) << "LiveVideoSource:onData SLICE NALU:" << nalu_type;
                }
                content.insert(content.end(), buffer, buffer + size);
                m_decoder.PostFrame(std::move(content), ts, frameType);
            }
            else
            {
                RTC_LOG(LS_ERROR) << "LiveVideoSource:onData no decoder";
                res = -1;
            }
        }
        else if (codec == "JPEG")
        {
            int32_t width = 0;
            int32_t height = 0;
            if (libyuv::MJPGSize(buffer, size, &width, &height) == 0)
            {
                int stride_y = width;
                int stride_uv = (width + 1) / 2;

                rtc::scoped_refptr<webrtc::I420Buffer> I420buffer = webrtc::I420Buffer::Create(width, height, stride_y, stride_uv, stride_uv);
                const int conversionResult = libyuv::ConvertToI420((const uint8_t *)buffer, size,
                                                                   I420buffer->MutableDataY(), I420buffer->StrideY(),
                                                                   I420buffer->MutableDataU(), I420buffer->StrideU(),
                                                                   I420buffer->MutableDataV(), I420buffer->StrideV(),
                                                                   0, 0,
                                                                   width, height,
                                                                   width, height,
                                                                   libyuv::kRotate0, ::libyuv::FOURCC_MJPG);

                if (conversionResult >= 0)
                {
                    webrtc::VideoFrame frame(I420buffer, 0, ts, webrtc::kVideoRotation_0);
                    m_decoder.Decoded(frame);
                }
                else
                {
                    RTC_LOG(LS_ERROR) << "LiveVideoSource:onData decoder error:" << conversionResult;
                    res = -1;
                }
            }
            else
            {
                RTC_LOG(LS_ERROR) << "LiveVideoSource:onData cannot JPEG dimension";
                res = -1;
            }
        }
        else if (codec == "VP9")
        {
            if (!m_decoder.hasDecoder())
            {
                m_decoder.createDecoder(codec);
            }
            if (m_decoder.hasDecoder())
            {
                webrtc::VideoFrameType frameType = webrtc::VideoFrameType::kVideoFrameKey;
                std::vector<uint8_t> content;
                content.insert(content.end(), buffer, buffer + size);
                m_decoder.PostFrame(std::move(content), ts, frameType);
            }
        }

        return (res == 0);
    }

    // overide rtc::VideoSourceInterface<webrtc::VideoFrame>
    void AddOrUpdateSink(rtc::VideoSinkInterface<webrtc::VideoFrame> *sink, const rtc::VideoSinkWants &wants)
    {
        m_broadcaster.AddOrUpdateSink(sink, wants);
    }

    void RemoveSink(rtc::VideoSinkInterface<webrtc::VideoFrame> *sink)
    {
        m_broadcaster.RemoveSink(sink);
    }

private:
    char        m_stop;
    Environment m_env;

protected:
    T m_liveclient;

private:
    std::thread                        m_capturethread;
    cricket::VideoFormat               m_format;
    std::vector<uint8_t>               m_cfg;
    std::map<std::string, std::string> m_codec;

    rtc::VideoBroadcaster              m_broadcaster;
    VideoDecoder                       m_decoder;

    std::atomic<int64_t> meta_counter;
    std::atomic<int64_t> video_counter;


};
