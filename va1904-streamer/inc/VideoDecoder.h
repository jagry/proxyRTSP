/* ---------------------------------------------------------------------------
** This software is in the public domain, furnished "as is", without technical
** support, and with no warranty, express or implied, as to its usefulness for
** any purpose.
**
** VideoDecoder.h
**
** -------------------------------------------------------------------------*/

#pragma once

#include <string.h>
#include <vector>

#include "api/video/i420_buffer.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "modules/video_coding/h264_sprop_parameter_sets.h"

#include <iomanip>
#include <sstream>


class VideoDecoder : public webrtc::DecodedImageCallback {
    private:
        class Frame
        {
            public:
                Frame(): m_timestamp_ms(0) {}
                Frame(std::vector<uint8_t> && content, uint64_t timestamp_ms, webrtc::VideoFrameType frameType) : m_content(content), m_timestamp_ms(timestamp_ms), m_frameType(frameType) {}
            
                std::vector<uint8_t>   m_content;
                uint64_t               m_timestamp_ms;
                webrtc::VideoFrameType m_frameType;
                int64_t                created;
        };

    public:
        class RectItem {
        public:
            int objectId;
            float left;
            float top;
            float right;
            float bottom;
            std::string personName;
            float score;
            int age;
            std::string gender;
            std::string emotion;
            bool foundPerson;
        };

        class RectResult {
        public:
            std::vector<RectItem> rects;
            std::string dateStr;

        };

        VideoDecoder(rtc::VideoBroadcaster& broadcaster, const std::map<std::string,std::string> & opts, bool wait) :
                not_found(0),
                m_broadcaster(broadcaster),
                m_stop(false),
                m_has_meta(false),
                m_wait(wait),
                m_previmagets(0),
                m_prevts(0),
                m_newts(0),
                m_newts_prev(0){
        }

        virtual ~VideoDecoder() {
        }

        void DecoderThread() 
        {
            unsigned int counter = 0;
            while (!m_stop) {
                std::unique_lock<std::mutex> mlock(m_queuemutex);
                while (m_queue.empty())
                {
                    m_queuecond.wait(mlock);
                }
                Frame frame = m_queue.front();
                m_queue.pop();		
                mlock.unlock();
                
//                RTC_LOG(LS_VERBOSE) << "VideoDecoder::DecoderThread size:" << frame.m_content.size() << " ts:" << frame.m_timestamp_ms;
                uint8_t* data = frame.m_content.data();
                ssize_t size = frame.m_content.size();
                
                if (size) {
                    webrtc::EncodedImage input_image(data, size, size);		
                    input_image._frameType = frame.m_frameType;
                    input_image._completeFrame = true;
                    counter += 1;
                    input_image.SetTimestamp(counter); // store time in ms that overflow the 32bits

                    m_last_date = fromTs(frame.m_timestamp_ms);

                    RTC_LOG(LS_VERBOSE) << "!!!!!!!!!!!! SSSSSSSSSS "  << m_last_date ;

                    m_newts = frame.created;
//                    m_newts = std::chrono::high_resolution_clock::now().time_since_epoch().count()/1000/1000;
//                    m_newts = frame.m_timestamp_ms;

                    int res = m_decoder->Decode(input_image, false, frame.m_timestamp_ms);
                    if (res != WEBRTC_VIDEO_CODEC_OK) {
                        RTC_LOG(LS_ERROR) << "VideoDecoder::DecoderThread failure:" << res;
                    }
                }
            }
        }

        void Start()
        {
            RTC_LOG(INFO) << "VideoDecoder::start";
            m_stop = false;
            m_decoderthread = std::thread(&VideoDecoder::DecoderThread, this);
        }

        void Stop()
        {
            RTC_LOG(INFO) << "VideoDecoder::stop";
            m_stop = true;
            Frame frame;			
            {
                std::unique_lock<std::mutex> lock(m_queuemutex);
                m_queue.push(frame);
            }
            m_queuecond.notify_all();
            m_decoderthread.join();
        }

        std::vector< std::vector<uint8_t> > getInitFrames(const std::string & codec, const char* sdp) {
            std::vector< std::vector<uint8_t> > frames;

            if (codec == "H264") {
                const char* pattern="sprop-parameter-sets=";
                const char* sprop=strstr(sdp, pattern);
                if (sprop)
                {
                    std::string sdpstr(sprop+strlen(pattern));
                    size_t pos = sdpstr.find_first_of(" ;\r\n");
                    if (pos != std::string::npos)
                    {
                        sdpstr.erase(pos);
                    }
                    webrtc::H264SpropParameterSets sprops;
                    if (sprops.DecodeSprop(sdpstr))
                    {
                        std::vector<uint8_t> sps;
                        sps.insert(sps.end(), H26X_marker, H26X_marker+sizeof(H26X_marker));
                        sps.insert(sps.end(), sprops.sps_nalu().begin(), sprops.sps_nalu().end());
                        frames.push_back(sps);

                        std::vector<uint8_t> pps;
                        pps.insert(pps.end(), H26X_marker, H26X_marker+sizeof(H26X_marker));
                        pps.insert(pps.end(), sprops.pps_nalu().begin(), sprops.pps_nalu().end());
                        frames.push_back(pps);
                    }
                    else
                    {
                        RTC_LOG(WARNING) << "Cannot decode SPS:" << sprop;
                    }
                }
            }

            return frames;
        }

        void createDecoder(const std::string & codec) {
            if (codec == "H264") {
                    m_decoder=m_factory.CreateVideoDecoder(webrtc::SdpVideoFormat(cricket::kH264CodecName));
                    webrtc::VideoCodec codec_settings;
                    codec_settings.codecType = webrtc::VideoCodecType::kVideoCodecH264;
                    m_decoder->InitDecode(&codec_settings,2);
                    m_decoder->RegisterDecodeCompleteCallback(this);
            } else if (codec == "VP9") {
                m_decoder=m_factory.CreateVideoDecoder(webrtc::SdpVideoFormat(cricket::kVp9CodecName));
                webrtc::VideoCodec codec_settings;
                codec_settings.codecType = webrtc::VideoCodecType::kVideoCodecVP9;
                m_decoder->InitDecode(&codec_settings,2);
                m_decoder->RegisterDecodeCompleteCallback(this);	                
            }
        }

        void destroyDecoder() {
            m_decoder.reset(NULL);
        }

        bool hasDecoder() {
            return (m_decoder.get() != NULL);
        }

        void PostMeta(RectResult rectResult) {
//            return;
            m_has_meta = true;
            {
                std::unique_lock<std::mutex> lock(m_metaqueuemutex);
//                m_metaqueue.push(rectResult);
                m_metamap[rectResult.dateStr] = rectResult;
            }
        }

        void PostFrame(std::vector<uint8_t>&& content, uint64_t ts, webrtc::VideoFrameType frameType) {

			Frame frame(std::move(content), ts, frameType);
			frame.created = std::chrono::high_resolution_clock::now().time_since_epoch().count()/1000/1000;
			{
				std::unique_lock<std::mutex> lock(m_queuemutex);
				m_queue.push(frame);
			}
			m_queuecond.notify_all();
        }

		// overide webrtc::DecodedImageCallback
		virtual int32_t Decoded(webrtc::VideoFrame& decodedImage) override {
            int64_t ts = std::chrono::high_resolution_clock::now().time_since_epoch().count()/1000/1000;

            //kRGB24
            rtc::scoped_refptr<webrtc::I420BufferInterface> i420_buffer = decodedImage.video_frame_buffer()->ToI420();

            int width_ = i420_buffer->width();
            int height_ = i420_buffer->height();

            std::unique_ptr<uint8_t[]> res_rgb_buffer2(new uint8_t[width_ * height_ * 4]);

            int r1 = libyuv::ConvertFromI420(
                    i420_buffer->DataY(), i420_buffer->StrideY(), i420_buffer->DataU(),
                    i420_buffer->StrideU(), i420_buffer->DataV(), i420_buffer->StrideV(),
                    res_rgb_buffer2.get(), 0, width_, height_,
                    libyuv::FOURCC_ABGR
            );
            //FOURCC_24BG
//            m_newts
            {
                int64_t d = 100;

                if (m_newts_prev != 0) {
                    d = m_newts - m_newts_prev;
//                    d = 0;
                }
                m_newts_prev = m_newts;
//                RTC_LOG(LS_VERBOSE) << "WAIT--------"  << "" << m_newts << "----" << d  << "";
//                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }

            while (true) {
                int64_t curr_ts = std::chrono::high_resolution_clock::now().time_since_epoch().count() / 1000 / 1000;
                if (curr_ts - m_newts >= 300) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

//            RTC_LOG(LS_VERBOSE) << "!!!!!!!!!!!!!!!!!!!!!!!!FOUND[" << rectResult.isFound << "] score:" << rectResult.score;

//            while(true) {
//                m_newts
//            }


            int64_t q_counter;
            {
                std::unique_lock<std::mutex> mlock(m_queuemutex);
                q_counter = m_queue.size();

            }

//            if (false) {
            if (m_has_meta) {

                int64_t sz;
                RectResult rectResult;

                bool found = false;

                bool exitLoop = false;

                int64_t start_ts = std::chrono::high_resolution_clock::now().time_since_epoch().count()/1000/1000;

                {
                    std::unique_lock <std::mutex> lock(m_metaqueuemutex);
                    RTC_LOG(LS_VERBOSE) << "!!!!!!!!!!>>>>>>>>>>>>>>>" << m_metamap.size() << " " << not_found << " " << q_counter;
                    if (m_metamap.find(m_last_date) != m_metamap.end()) {
                        rectResult = m_metamap.at(m_last_date);
                        found = rectResult.rects.size() > 0;
                        m_metamap.erase(m_last_date);
                    } else {
                        not_found++;
                    }
                }


//                while (!exitLoop) {
//                    {
//                        std::unique_lock <std::mutex> lock(m_metaqueuemutex);
////                        sz = m_metaqueue.size();
//
//                        while (!m_metaqueue.empty()) {
//                            rectResult = m_metaqueue.front();
//
//                            if (rectResult.dateStr >= m_last_date) {
//                                if (rectResult.dateStr == m_last_date) {
////                            RTC_LOG(LS_VERBOSE) << "!!!!!!!!!!!!!!!!!!!!!!!!FOUND[" << rectResult.isFound << "] score:" << rectResult.score;
//                                    found = rectResult.rects.size() > 0;
//                                    lastResult = rectResult;
//                                    m_metaqueue.pop();
//                                } else {
//                                    RTC_LOG(LS_VERBOSE) << "!!!!!!!!!!!!WWWWWWWWWWWWWWWWWWWWWWWWW " << rectResult.dateStr  <<"(" << m_last_date <<")";
//                                };
//                                exitLoop = true;
//                                break;
//                            }
//                            m_metaqueue.pop();
//                        }
////                    RTC_LOG(LS_VERBOSE) << "!!!!!!!!!!!!!!!!!!!!!!!![" << found << "] TS:" << m_last_date << "QUEUE: "<< rectResult.dateStr;
//                    }
//                    int64_t end_ts = std::chrono::high_resolution_clock::now().time_since_epoch().count()/1000/1000;
//                    if (end_ts - start_ts > 5000) {
//                        m_has_meta = false;
//                        break;
//                    }
//
//                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
//                }


                if (found) {
                    m_last_found = 3;
                } else {
                    m_last_found--;
                }

                if (m_last_found > 0) {
//                    rectResult = lastResult;
//                    found = true;
                } else {
                    m_last_found = -1;
                }

                RTC_LOG(LS_VERBOSE) << "!!!!!!!!!!!!eeeeeeeeeeeee " << rectResult.rects.size() << " " << rectResult.dateStr ;
                if (found) {

//                    for (int i=0;i<rectResult.rects.size();i++) {
                    for (auto frame : rectResult.rects) {

                        int color = 0;


                        if (frame.foundPerson) {
                            color = 1;
                        }

                        int x1 = ((frame.left + 1) / 2) * width_;
                        int y1 = (1 - (frame.top + 1) / 2) * height_;

                        int x2 = ((frame.right + 1) / 2) * width_;
                        int y2 = (1 - (frame.bottom + 1) / 2) * height_;

                        for (int i = x1; i < x2; i++) {
                            int p = y1 * width_ * 4 + i * 4 + color;
                            res_rgb_buffer2.get()[p] = 255;

                            p = (y1 + 1) * width_ * 4 + i * 4 + color;
                            res_rgb_buffer2.get()[p] = 255;


                            int p2 = y2 * width_ * 4 + i * 4 + color;
                            res_rgb_buffer2.get()[p2] = 255;

                            p2 = (y2 - 1) * width_ * 4 + i * 4 + color;
                            res_rgb_buffer2.get()[p2] = 255;
                        }

                        for (int i = y1; i < y2; i++) {
                            int p = i * width_ * 4 + x1 * 4 + color;
                            res_rgb_buffer2.get()[p] = 255;

                            p = i * width_ * 4 + (x1 + 1) * 4 + color;
                            res_rgb_buffer2.get()[p] = 255;


                            p = i * width_ * 4 + (x2) * 4 + color;
                            res_rgb_buffer2.get()[p] = 255;

                            p = i * width_ * 4 + (x2 - 1) * 4 + color;
                            res_rgb_buffer2.get()[p] = 255;
                        }
                    }

//                0 - синий
//                1 - green
//                FOURCC_ABGR
                }
            } else {
                RTC_LOG(LS_VERBOSE) << "!!!!!!!!!!!!AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA " ;
            }

            rtc::scoped_refptr<webrtc::I420Buffer> res_i420_buffer =
                    webrtc::I420Buffer::Create(width_, height_);


            int ret = libyuv::ConvertToI420(
                    res_rgb_buffer2.get(), 0, res_i420_buffer.get()->MutableDataY(),
                    res_i420_buffer.get()->StrideY(), res_i420_buffer.get()->MutableDataU(),
                    res_i420_buffer.get()->StrideU(), res_i420_buffer.get()->MutableDataV(),
                    res_i420_buffer.get()->StrideV(), 0, 0, width_, height_,
                    res_i420_buffer->width(), res_i420_buffer->height(), libyuv::kRotate0,
                    libyuv::FOURCC_ABGR);

            decodedImage.set_video_frame_buffer(res_i420_buffer);

            // waiting
            if ( (m_wait) && (m_prevts != 0) ) {
                int64_t periodSource = decodedImage.timestamp() - m_previmagets;
                int64_t periodDecode = ts-m_prevts;
                    
                int64_t delayms = periodSource-periodDecode;
                if ( (delayms > 0) && (delayms < 1000) ) {
                    RTC_LOG(LS_VERBOSE) << "VideoDecoder::Decoded interframe decode:" << periodDecode << " source:" << periodSource  << "total:" << delayms;
//                    std::this_thread::sleep_for(std::chrono::milliseconds(delayms));
                }
            }                        

            m_broadcaster.OnFrame(decodedImage);

	        m_previmagets = decodedImage.timestamp();
	        m_prevts = std::chrono::high_resolution_clock::now().time_since_epoch().count()/1000/1000;


            return 1;
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
//        std::tm bt = *std::localtime(&timer);

        std::ostringstream oss;

        oss << std::put_time(&bt, "%Y-%m-%dT%H:%M:%S"); // HH:MM:SS
        oss << '.' << std::setfill('0') << std::setw(3) << ms.count();

        return oss.str();
    }

        RectResult                            lastResult;
        int64_t                               m_last_found;
        std::string                           m_last_date;

        rtc::VideoBroadcaster&                m_broadcaster;
        webrtc::InternalDecoderFactory        m_factory;
        std::unique_ptr<webrtc::VideoDecoder> m_decoder;

        std::map<std::string, RectResult>     m_metamap;
        std::queue<RectResult>                m_metaqueue;
        std::mutex                            m_metaqueuemutex;

		std::queue<Frame>                     m_queue;
		std::mutex                            m_queuemutex;
		std::condition_variable               m_queuecond;

		std::thread                           m_decoderthread;     
        bool                                  m_stop;

        bool                                  m_wait;
        int64_t                               m_previmagets;	
        int64_t                               m_prevts;


        int64_t                               m_newts;
        int64_t                               m_newts_prev;
        bool                                  m_has_meta;

        std::atomic<int64_t>                  not_found;

};
