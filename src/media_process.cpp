#include "media_process.hpp"
#include "diagnostics.hpp"
#include "url_utils.hpp"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

namespace cryget {
namespace {
std::string av_error(int code) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code,message,sizeof(message));
    return message;
}

struct Input {
    AVFormatContext* format=nullptr;
    int stream=-1;
    ~Input(){if(format)avformat_close_input(&format);}
    Input(const Input&)=delete;
    Input& operator=(const Input&)=delete;
    Input()=default;
    Input(Input&& other) noexcept:format(std::exchange(other.format,nullptr)),stream(other.stream){}
    Input& operator=(Input&& other) noexcept {
        if(this!=&other){if(format)avformat_close_input(&format);format=std::exchange(other.format,nullptr);stream=other.stream;}
        return *this;
    }
};

struct Output {
    AVFormatContext* format=nullptr;
    bool opened=false;
    bool finished=false;
    std::filesystem::path path;
    ~Output(){
        if(format){if(opened&&!(format->oformat->flags&AVFMT_NOFILE))avio_closep(&format->pb);avformat_free_context(format);}
        if(!finished&&!path.empty()){std::error_code ignored;std::filesystem::remove(path,ignored);}
    }
    Output(const Output&)=delete;
    Output& operator=(const Output&)=delete;
    Output()=default;
};

Input open_input(const std::filesystem::path& path,AVMediaType type) {
    Input input;
    const auto filename=path_utf8(path);
    int rc=avformat_open_input(&input.format,filename.c_str(),nullptr,nullptr);
    if(rc<0)throw std::runtime_error("Cannot open downloaded media: "+av_error(rc));
    rc=avformat_find_stream_info(input.format,nullptr);
    if(rc<0)throw std::runtime_error("Cannot read downloaded media streams: "+av_error(rc));
    input.stream=av_find_best_stream(input.format,type,-1,-1,nullptr,0);
    if(input.stream<0)throw std::runtime_error(type==AVMEDIA_TYPE_VIDEO?"Downloaded file has no video stream":"Downloaded file has no audio stream");
    return input;
}

AVStream* make_stream(AVFormatContext* output,const AVStream* input) {
    AVStream* stream=avformat_new_stream(output,nullptr);
    if(!stream)throw std::runtime_error("Cannot allocate MP4 output stream");
    const int rc=avcodec_parameters_copy(stream->codecpar,input->codecpar);
    if(rc<0)throw std::runtime_error("Cannot copy media stream settings: "+av_error(rc));
    stream->codecpar->codec_tag=0;
    stream->time_base=input->time_base;
    return stream;
}

bool read_stream_packet(AVFormatContext* input,int stream,AVPacket* packet) {
    for(;;) {
        const int rc=av_read_frame(input,packet);
        if(rc==AVERROR_EOF)return false;
        if(rc<0)throw std::runtime_error("Cannot read downloaded media packet: "+av_error(rc));
        if(packet->stream_index==stream)return true;
        av_packet_unref(packet);
    }
}

int64_t packet_time(const AVPacket* packet,const AVStream* stream) {
    const int64_t stamp=packet->dts!=AV_NOPTS_VALUE?packet->dts:packet->pts;
    return stamp==AV_NOPTS_VALUE?INT64_MAX:av_rescale_q(stamp,stream->time_base,AV_TIME_BASE_Q);
}

void write_packet(Output& output,AVPacket* packet,const AVStream* input,AVStream* destination) {
    av_packet_rescale_ts(packet,input->time_base,destination->time_base);
    packet->stream_index=destination->index;
    packet->pos=-1;
    const int rc=av_interleaved_write_frame(output.format,packet);
    av_packet_unref(packet);
    if(rc<0)throw std::runtime_error("Cannot write combined MP4: "+av_error(rc));
}
} // namespace

void merge_media(const std::filesystem::path& video,const std::filesystem::path& audio,
                 const std::filesystem::path& output_path,const std::atomic<bool>& canceled) {
    if(canceled.load())throw std::runtime_error("Download canceled");
    if(std::filesystem::exists(output_path))throw std::runtime_error("Combined output already exists");
    log_event("merge.start","output="+path_utf8(output_path));
    Input video_input=open_input(video,AVMEDIA_TYPE_VIDEO);
    Input audio_input=open_input(audio,AVMEDIA_TYPE_AUDIO);
    AVStream* video_source=video_input.format->streams[video_input.stream];
    AVStream* audio_source=audio_input.format->streams[audio_input.stream];
    Output output;
    const auto filename=path_utf8(output_path);
    int rc=avformat_alloc_output_context2(&output.format,nullptr,"mp4",filename.c_str());
    if(rc<0||!output.format)throw std::runtime_error("Cannot create combined MP4: "+(rc<0?av_error(rc):std::string("unsupported output")));
    output.path=output_path;
    AVStream* video_target=make_stream(output.format,video_source);
    AVStream* audio_target=make_stream(output.format,audio_source);
    if(!(output.format->oformat->flags&AVFMT_NOFILE)) {
        rc=avio_open(&output.format->pb,filename.c_str(),AVIO_FLAG_WRITE);
        if(rc<0)throw std::runtime_error("Cannot create combined MP4: "+av_error(rc));
        output.opened=true;
    }
    AVDictionary* options=nullptr;
    av_dict_set(&options,"movflags","+faststart",0);
    rc=avformat_write_header(output.format,&options);
    av_dict_free(&options);
    if(rc<0)throw std::runtime_error("Cannot initialize combined MP4: "+av_error(rc));

    AVPacket* video_packet=av_packet_alloc();
    AVPacket* audio_packet=av_packet_alloc();
    if(!video_packet||!audio_packet){av_packet_free(&video_packet);av_packet_free(&audio_packet);throw std::runtime_error("Cannot allocate media packet buffers");}
    bool has_video=read_stream_packet(video_input.format,video_input.stream,video_packet);
    bool has_audio=read_stream_packet(audio_input.format,audio_input.stream,audio_packet);
    while(has_video||has_audio) {
        if(canceled.load()) {
            av_packet_free(&video_packet);av_packet_free(&audio_packet);
            throw std::runtime_error("Download canceled");
        }
        const bool take_video=has_video&&(!has_audio||packet_time(video_packet,video_source)<=packet_time(audio_packet,audio_source));
        if(take_video) {
            write_packet(output,video_packet,video_source,video_target);
            has_video=read_stream_packet(video_input.format,video_input.stream,video_packet);
        } else {
            write_packet(output,audio_packet,audio_source,audio_target);
            has_audio=read_stream_packet(audio_input.format,audio_input.stream,audio_packet);
        }
    }
    av_packet_free(&video_packet);av_packet_free(&audio_packet);
    rc=av_write_trailer(output.format);
    if(rc<0)throw std::runtime_error("Cannot finish combined MP4: "+av_error(rc));
    output.finished=true;
    log_event("merge.complete","output="+path_utf8(output_path));
}
} // namespace cryget
