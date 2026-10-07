#include "video_core.hpp"
#include "diagnostics.hpp"
#include "json.hpp"
#include "player.hpp"
#include "media_process.hpp"
#include "url_utils.hpp"
#ifdef _WIN32
#include "winhttp_download.hpp"
#else
#include <curl/curl.h>
#endif
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <regex>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace cryget {
namespace {
#ifndef _WIN32
struct CurlDeleter { void operator()(CURL* handle) const { curl_easy_cleanup(handle); } };
using Curl=std::unique_ptr<CURL,CurlDeleter>;
struct Memory { std::string bytes; size_t maximum=12*1024*1024; };
size_t receive_text(char* bytes,size_t size,size_t count,void* context) {
    auto& out=*static_cast<Memory*>(context);const size_t amount=size*count;
    if(amount>out.maximum-out.bytes.size())return 0;
    out.bytes.append(bytes,amount);return amount;
}
struct Writer { FILE* file; int error=0; uint64_t received=0; };
size_t receive_file(char* bytes,size_t size,size_t count,void* context) {
    auto& writer=*static_cast<Writer*>(context);const auto written=std::fwrite(bytes,size,count,writer.file);
    if(written!=count)writer.error=errno;
    writer.received+=written*size;
    return written*size;
}
struct RangeInfo { uint64_t first=0,last=0,total=0; bool valid=false; };
size_t receive_range_header(char* bytes,size_t size,size_t count,void* context) {
    const size_t length=size*count;
    const std::string line(bytes,length);
    const auto colon=line.find(':');
    if(colon==std::string::npos)return length;
    std::string name=line.substr(0,colon);
    std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    if(name=="content-range") {
        unsigned long long first=0,last=0,total=0;
        if(std::sscanf(line.c_str()+colon+1," bytes %llu-%llu/%llu",&first,&last,&total)==3) {
            auto& range=*static_cast<RangeInfo*>(context);
            range={first,last,total,true};
        }
    }
    return length;
}
struct Transfer { const std::atomic<bool>* canceled; const Progress* progress=nullptr; };
int transfer_progress(void* context,curl_off_t total,curl_off_t received,curl_off_t,curl_off_t) {
    auto& t=*static_cast<Transfer*>(context);
    if(t.canceled&&t.canceled->load())return 1;
    if(t.progress&&*t.progress) {
        try {(*t.progress)(static_cast<uint64_t>(std::max<curl_off_t>(0,received)),static_cast<uint64_t>(std::max<curl_off_t>(0,total)));}
        catch(...) {return 1;} // Never unwind through libcurl's C callback.
    }
    return 0;
}
Curl make_request(const std::string& url) {
    Curl request(curl_easy_init());if(!request)throw std::runtime_error("Cannot initialize HTTP");
    curl_easy_setopt(request.get(),CURLOPT_URL,url.c_str());
    curl_easy_setopt(request.get(),CURLOPT_FOLLOWLOCATION,1L);
    curl_easy_setopt(request.get(),CURLOPT_MAXREDIRS,5L);
    curl_easy_setopt(request.get(),CURLOPT_CONNECTTIMEOUT,10L);
    curl_easy_setopt(request.get(),CURLOPT_LOW_SPEED_LIMIT,1024L);
    curl_easy_setopt(request.get(),CURLOPT_LOW_SPEED_TIME,30L);
    curl_easy_setopt(request.get(),CURLOPT_NOSIGNAL,1L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(request.get(),CURLOPT_PROTOCOLS_STR,"https");
    curl_easy_setopt(request.get(),CURLOPT_REDIR_PROTOCOLS_STR,"https");
#else
    curl_easy_setopt(request.get(),CURLOPT_PROTOCOLS,CURLPROTO_HTTPS);
    curl_easy_setopt(request.get(),CURLOPT_REDIR_PROTOCOLS,CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(request.get(),CURLOPT_USERAGENT,"Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/130.0 Safari/537.36");
    curl_easy_setopt(request.get(),CURLOPT_ACCEPT_ENCODING,"");
    return request;
}
#endif
std::string fetch_text(const std::string& url,const std::string& stage,const std::atomic<bool>* canceled) {
#ifdef _WIN32
    std::string page;
    auto result=winhttp_get(url,canceled,[&](const char* bytes,size_t count,uint64_t,uint64_t){
        if(count>12*1024*1024-page.size())throw std::runtime_error("Video information is too large");
        page.append(bytes,count);
    });
    log_event(stage,"http="+std::to_string(result.status)+" bytes="+std::to_string(page.size()));
    return page;
#else
    if(canceled&&canceled->load())throw std::runtime_error("Download canceled");
    auto request=make_request(url);Memory page;Transfer transfer{canceled};
    curl_easy_setopt(request.get(),CURLOPT_TIMEOUT,30L);
    curl_easy_setopt(request.get(),CURLOPT_WRITEFUNCTION,receive_text);
    curl_easy_setopt(request.get(),CURLOPT_WRITEDATA,&page);
    curl_easy_setopt(request.get(),CURLOPT_NOPROGRESS,0L);
    curl_easy_setopt(request.get(),CURLOPT_XFERINFOFUNCTION,transfer_progress);
    curl_easy_setopt(request.get(),CURLOPT_XFERINFODATA,&transfer);
    auto status=curl_easy_perform(request.get());long response=0;
    curl_easy_getinfo(request.get(),CURLINFO_RESPONSE_CODE,&response);
    log_event(stage,"curl="+std::to_string(status)+" http="+std::to_string(response)+" bytes="+std::to_string(page.bytes.size()));
    if(canceled&&canceled->load())throw std::runtime_error("Download canceled");
    if(status!=CURLE_OK)throw std::runtime_error(std::string("Cannot read video information: ")+curl_easy_strerror(status));
    if(response!=200)throw std::runtime_error("YouTube returned HTTP "+std::to_string(response));
    return page.bytes;
#endif
}

std::string fetch_player_response(const std::string& page,const std::string& id,const std::atomic<bool>* canceled) {
    static const std::regex key_pattern(R"re("INNERTUBE_API_KEY"\s*:\s*"([A-Za-z0-9_-]{20,60})")re");
    std::smatch match;
    if(!std::regex_search(page,match,key_pattern))throw std::runtime_error("YouTube player API key was not found");
    const std::string url="https://www.youtube.com/youtubei/v1/player?key="+match[1].str();
    const std::string body="{\"videoId\":\""+id+"\",\"context\":{\"client\":{\"clientName\":\"ANDROID\",\"clientVersion\":\"20.10.38\",\"androidSdkVersion\":35}}}";
#ifdef _WIN32
    std::string response;
    auto result=winhttp_post_json(url,body,canceled,[&](const char* bytes,size_t count,uint64_t,uint64_t){
        if(count>12*1024*1024-response.size())throw std::runtime_error("Player response is too large");
        response.append(bytes,count);
    });
    log_event("video.player.http","http="+std::to_string(result.status)+" bytes="+std::to_string(response.size()));
    return response;
#else
    Curl request=make_request(url);Memory response;Transfer transfer{canceled};
    curl_easy_setopt(request.get(),CURLOPT_TIMEOUT,30L);
    curl_easy_setopt(request.get(),CURLOPT_POSTFIELDS,body.c_str());
    curl_easy_setopt(request.get(),CURLOPT_POSTFIELDSIZE,static_cast<long>(body.size()));
    curl_slist* headers=curl_slist_append(nullptr,"Content-Type: application/json");
    curl_easy_setopt(request.get(),CURLOPT_HTTPHEADER,headers);
    curl_easy_setopt(request.get(),CURLOPT_WRITEFUNCTION,receive_text);
    curl_easy_setopt(request.get(),CURLOPT_WRITEDATA,&response);
    curl_easy_setopt(request.get(),CURLOPT_NOPROGRESS,0L);
    curl_easy_setopt(request.get(),CURLOPT_XFERINFOFUNCTION,transfer_progress);
    curl_easy_setopt(request.get(),CURLOPT_XFERINFODATA,&transfer);
    const auto status=curl_easy_perform(request.get());long http=0;
    curl_easy_getinfo(request.get(),CURLINFO_RESPONSE_CODE,&http);
    curl_slist_free_all(headers);
    log_event("video.player.http","curl="+std::to_string(status)+" http="+std::to_string(http)+" bytes="+std::to_string(response.bytes.size()));
    if(status!=CURLE_OK||http!=200)throw std::runtime_error("Cannot read YouTube player response");
    return response.bytes;
#endif
}
std::string extract_object(const std::string& page,size_t opening) {
    if(opening==std::string::npos||page[opening]!='{')throw std::runtime_error("Video metadata was not found");
    int depth=0;bool in_string=false,escaped=false;
    for(size_t i=opening;i<page.size();++i){char c=page[i];
        if(in_string){if(escaped)escaped=false;else if(c=='\\')escaped=true;else if(c=='"')in_string=false;}
        else if(c=='"')in_string=true;else if(c=='{')++depth;else if(c=='}'&&--depth==0)return page.substr(opening,i-opening+1);
    }
    throw std::runtime_error("Video metadata is incomplete");
}
std::string safe_filename(const std::string& title, const std::string& id) {
    std::string result;
    for (size_t index = 0; index < title.size();) {
        const unsigned char c = static_cast<unsigned char>(title[index]);
        const size_t length = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        if (result.size() + length > 160 || index + length > title.size()) break;
        if (c < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
            c == '\\' || c == '|' || c == '?' || c == '*') result += '_';
        else result.append(title, index, length);
        index += length;
    }
    while (!result.empty() && (result.back() == ' ' || result.back() == '.')) result.pop_back();
    if (result.empty()) result = "video";
    return result + " [" + id + "].mp4";
}

} // namespace

std::filesystem::path checked_folder(const std::string& utf8) {
    log_event("folder.check", utf8);
    if (utf8.empty()) throw std::runtime_error("Choose a download folder");
    std::error_code error;
    std::string expanded = utf8;
    if (expanded[0] == '~' && (expanded.size() == 1 || expanded[1] == '/' || expanded[1] == '\\')) {
#ifdef _WIN32
        const char* home = std::getenv("USERPROFILE");
#else
        const char* home = std::getenv("HOME");
#endif
        if (home) expanded = std::string(home) + expanded.substr(1);
    }
    auto path = std::filesystem::absolute(std::filesystem::u8path(expanded), error);
    if (error) throw std::runtime_error("Invalid download folder: " + error.message());
    path = std::filesystem::weakly_canonical(path, error);
    if (error) throw std::runtime_error("Cannot read download folder: " + error.message());
    const bool directory = std::filesystem::is_directory(path, error);
    if (error) throw std::runtime_error("Cannot access download folder: " + error.message());
    if (!directory)
        throw std::runtime_error("Download folder does not exist: " + utf8);
    log_event("folder.ready", path.u8string());
    return path;
}

namespace {
std::string player_address(std::string path) {
    if(path.rfind("//",0)==0)path="https:"+path;
    else if(path.rfind("/",0)==0)path="https://www.youtube.com"+path;
    if(path.rfind("https://www.youtube.com/s/player/",0)!=0&&path.rfind("https://youtube.com/s/player/",0)!=0)
        throw std::runtime_error("Invalid YouTube player address");
    return path;
}
Format parse_format(const Json& entry) {
    Format f;f.url=entry.get("url").string;f.cipher=entry.get("signatureCipher").string;
    if(f.cipher.empty())f.cipher=entry.get("cipher").string;
    f.mime_type=entry.get("mimeType").string;f.height=static_cast<int>(entry.get("height").number);
    f.bitrate=static_cast<int>(entry.get("bitrate").number);
    f.has_audio=entry.get("audioQuality").is_string()||entry.get("audioSampleRate").is_string();
    int itag=static_cast<int>(entry.get("itag").number);
    f.has_audio=f.has_audio||itag==18||itag==22||itag==37||itag==38;
    try{f.content_length=std::stoull(entry.get("contentLength").string);}catch(const std::exception&){}
    return f;
}
bool challenged(const std::string& url,const std::string& cipher) {
    return !query_value(cipher,"s").empty()||!url_parameter(url.empty()?query_value(cipher,"url"):url,"n").empty();
}
std::string prepared_player(const std::string& url,const std::atomic<bool>* canceled) {
    static std::mutex mutex;static std::map<std::string,std::string> cache;
    const auto address=player_address(url);
    {std::lock_guard lock(mutex);const auto f=cache.find(address);if(f!=cache.end())return f->second;}
    log_event("player.fetch",address);
    auto source=fetch_text(address,"player.http",canceled);
    auto prepared=PlayerSolver::prepare(source,canceled);
    {std::lock_guard lock(mutex);if(cache.size()>=3)cache.erase(cache.begin());cache[address]=prepared;}
    return prepared;
}
struct TemporaryDirectory {
    std::filesystem::path path;
    explicit TemporaryDirectory(const std::filesystem::path& folder) {
        std::random_device random;
        for(int i=0;i<16;++i){auto candidate=folder/(".cryget-"+std::to_string(random())+"-"+std::to_string(random()));std::error_code error;
            if(std::filesystem::create_directory(candidate,error)){path=std::move(candidate);return;}
            if(error&&error!=std::errc::file_exists)throw std::runtime_error("Cannot create temporary download folder: "+error.message());
        }
        throw std::runtime_error("Cannot reserve a temporary download folder");
    }
    ~TemporaryDirectory(){std::error_code ignored;std::filesystem::remove_all(path,ignored);}
};
uint64_t fetch_media(const std::string& url,const std::filesystem::path& path,const std::atomic<bool>& canceled,const Progress& progress,uint64_t expected_length) {
    if(canceled.load())throw std::runtime_error("Download canceled");
    constexpr uint64_t chunk_size=8*1024*1024;
#ifdef _WIN32
    FILE* file=_wfopen(path.c_str(),L"wb");
    if(!file){const int error=errno;log_event("download.file_error",path.u8string()+" errno="+std::to_string(error));
        throw std::runtime_error("Cannot create download file: "+std::error_code(error,std::generic_category()).message());}
    try {
        uint64_t received_total=0,total=expected_length;
        size_t chunks=0;
        for(;;) {
            const uint64_t last=total?std::min(total-1,received_total+chunk_size-1):received_total+chunk_size-1;
            const auto result=winhttp_get_range(url,received_total,last,&canceled,
                [&,base=received_total](const char* bytes,size_t count,uint64_t received,uint64_t){
                    if(std::fwrite(bytes,1,count,file)!=count)throw std::runtime_error("Cannot write download file");
                    if(progress)progress(base+received,total);
                });
            ++chunks;
            if(result.status==200) {
                received_total=result.received;
                total=result.content_length?result.content_length:received_total;
                break;
            }
            if(!result.received||!result.total_length)throw std::runtime_error("Incomplete media byte range");
            total=result.total_length;
            received_total+=result.received;
            if(progress)progress(received_total,total);
            if(received_total>=total)break;
        }
        if(!received_total||received_total!=total)throw std::runtime_error("Incomplete media download");
        if(std::fclose(file)!=0){file=nullptr;throw std::runtime_error("Cannot finish writing the video file");}
        file=nullptr;
        log_event("download.transfer","bytes="+std::to_string(received_total)+" chunks="+std::to_string(chunks));
        return received_total;
    } catch(...) {if(file)std::fclose(file);throw;}
#else
    auto request=make_request(url);
    FILE* file=std::fopen(path.c_str(),"wb");
    if(!file){const int e=errno;log_event("download.file_error",path.u8string()+" errno="+std::to_string(e));throw std::runtime_error("Cannot create download file: "+std::error_code(e,std::generic_category()).message());}
    Writer writer{file};Transfer transfer{&canceled};RangeInfo range;
    curl_easy_setopt(request.get(),CURLOPT_WRITEFUNCTION,receive_file);curl_easy_setopt(request.get(),CURLOPT_WRITEDATA,&writer);
    curl_easy_setopt(request.get(),CURLOPT_HEADERFUNCTION,receive_range_header);curl_easy_setopt(request.get(),CURLOPT_HEADERDATA,&range);
    curl_easy_setopt(request.get(),CURLOPT_NOPROGRESS,0L);curl_easy_setopt(request.get(),CURLOPT_XFERINFOFUNCTION,transfer_progress);curl_easy_setopt(request.get(),CURLOPT_XFERINFODATA,&transfer);
    uint64_t received_total=0,total=expected_length;
    size_t chunks=0;
    try {
        for(;;) {
            const uint64_t last=total?std::min(total-1,received_total+chunk_size-1):received_total+chunk_size-1;
            const std::string bytes=std::to_string(received_total)+"-"+std::to_string(last);
            curl_easy_setopt(request.get(),CURLOPT_RANGE,bytes.c_str());
            writer.received=0;
            range={};
            Progress chunk_progress=[&,base=received_total](uint64_t received,uint64_t){
                if(progress)progress(base+received,total);
            };
            transfer.progress=&chunk_progress;
            const auto status=curl_easy_perform(request.get());long response=0;
            curl_easy_getinfo(request.get(),CURLINFO_RESPONSE_CODE,&response);
            ++chunks;
            if(canceled.load())throw std::runtime_error("Download canceled");
            if(status!=CURLE_OK||writer.error||(response!=200&&response!=206)) {
                log_event("download.transfer.error","offset="+std::to_string(received_total)+" curl="+std::to_string(status)+" http="+std::to_string(response));
                if(status!=CURLE_OK)throw std::runtime_error(std::string("Download failed: ")+curl_easy_strerror(status));
                if(writer.error)throw std::runtime_error("Cannot write download file");
                throw std::runtime_error("Media server returned HTTP "+std::to_string(response));
            }
            if(response==200) {
                if(received_total)throw std::runtime_error("Media server ignored a later byte range");
                received_total=writer.received;
                total=received_total;
                break;
            }
            if(!range.valid||range.first!=received_total||range.last<range.first||
               range.last>last||range.total<=range.last||writer.received!=range.last-range.first+1)
                throw std::runtime_error("Invalid media Content-Range");
            total=range.total;
            received_total+=writer.received;
            if(progress)progress(received_total,total);
            if(received_total>=total)break;
        }
        if(!received_total||received_total!=total)throw std::runtime_error("Incomplete media download");
        if(std::fclose(file)!=0){file=nullptr;throw std::runtime_error("Cannot finish writing the video file");}
        file=nullptr;
        log_event("download.transfer","bytes="+std::to_string(received_total)+" chunks="+std::to_string(chunks));
        return received_total;
    } catch(...) {if(file)std::fclose(file);throw;}
#endif
}
void publish_file(const std::filesystem::path& temporary,const std::filesystem::path& output) {
#ifdef _WIN32
    if(!MoveFileW(temporary.c_str(),output.c_str()))throw std::runtime_error("Cannot save finished video (Windows error "+std::to_string(GetLastError())+")");
#else
    // link() publishes atomically and refuses to replace an existing user file.
    if(link(temporary.c_str(),output.c_str())!=0)throw std::runtime_error("Cannot save finished video: "+std::error_code(errno,std::generic_category()).message());
    std::error_code ignored;std::filesystem::remove(temporary,ignored);
#endif
}
} // namespace

namespace {
Video parse_player_data(const Json& response,const std::string& id,const std::string& page) {
    const auto status=response.get("playabilityStatus").get("status").string;
    if(!status.empty()&&status!="OK") {
        const auto reason=response.get("playabilityStatus").get("reason").string;
        throw std::runtime_error("Video unavailable: "+(reason.empty()?status:reason));
    }
    Video video;video.id=id;video.title=response.get("videoDetails").get("title").string;if(video.title.empty())video.title=id;
    const auto& streaming=response.get("streamingData");
    for(const char* collection:{"formats","adaptiveFormats"})for(const auto& entry:streaming.get(collection).array) {
        if(!entry.get("drmFamilies").array.empty())continue;
        auto f=parse_format(entry);if(f.url.empty()&&f.cipher.empty())continue;
        if(f.mime_type.find("video/mp4")==0)video.formats.push_back(std::move(f));
        else if(f.mime_type.find("audio/mp4")==0)video.audio_formats.push_back(std::move(f));
    }
    std::string player=response.get("assets").get("js").string;
    if(player.empty()) {
        static const std::regex pattern(R"re("(?:jsUrl|PLAYER_JS_URL)"\s*:\s*("(?:\\.|[^"\\])*"))re");
        std::smatch match;if(std::regex_search(page,match,pattern))player=JsonParser(match[1].str()).parse().string;
    }
    if(player.empty()) {
        static const std::regex pattern(R"re(<script[^>]+src="([^" ]*/s/player/[^" ]+\.js)")re");
        std::smatch match;if(std::regex_search(page,match,pattern))player=match[1].str();
    }
    if(!player.empty())video.player_url=player_address(player);
    return video;
}
}

Video parse_watch_page(const std::string& page,const std::string& id) {
    const auto start=page.find("ytInitialPlayerResponse");
    if(start==std::string::npos)throw std::runtime_error("YouTube player data is unavailable");
    const auto opening=page.find('{',start+23);
    return parse_player_data(JsonParser(extract_object(page,opening)).parse(),id,page);
}

Format choose_format(const Video& video,int maximum_height,bool allow_merge) {
    const bool can_merge=allow_merge&&!find_ffmpeg().empty()&&!video.audio_formats.empty();
    std::vector<const Format*> available;
    for(const auto& f:video.formats)if(f.has_audio||can_merge)available.push_back(&f);
    if(available.empty()) {
        if(!video.formats.empty()&&!video.audio_formats.empty())throw std::runtime_error("Install FFmpeg to combine this video's audio and picture");
        throw std::runtime_error("YouTube did not provide a supported MP4 stream for this video");
    }
    const Format* best=nullptr;
    for(const auto* f:available){if(maximum_height>0&&f->height>maximum_height)continue;if(!best||f->height>best->height||(f->height==best->height&&f->bitrate>best->bitrate))best=f;}
    if(!best)best=*std::min_element(available.begin(),available.end(),[](const auto* a,const auto* b){return a->height<b->height;});
    Format result=*best;
    if(!result.has_audio) {
        const auto& audio=*std::max_element(video.audio_formats.begin(),video.audio_formats.end(),[](const auto& a,const auto& b){return a.bitrate<b.bitrate;});
        result.audio_url=audio.url;result.audio_cipher=audio.cipher;result.audio_length=audio.content_length;
    }
    log_event("format.selected","height="+std::to_string(result.height)+" merge="+(result.has_audio?"no":"yes"));
    return result;
}
Video inspect_video(const std::string& id,const std::atomic<bool>* canceled) {
    if(id.size()!=11||!std::all_of(id.begin(),id.end(),[](unsigned char c){return(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-';}))throw std::runtime_error("Invalid YouTube video ID");
    log_event("video.inspect.start",id);
    const auto page=fetch_text("https://www.youtube.com/watch?v="+id,"video.inspect.http",canceled);
    Video video;
    std::string page_error;
    try { video=parse_watch_page(page,id); }
    catch(const std::exception& error) { page_error=error.what(); }
    const bool needs_fallback=!page_error.empty()||video.formats.empty()||
        (find_ffmpeg().empty()&&std::none_of(video.formats.begin(),video.formats.end(),[](const Format& f){return f.has_audio;}));
    if(needs_fallback) {
        try {
            auto response=fetch_player_response(page,id,canceled);
            auto alternative=parse_player_data(JsonParser(response).parse(),id,page);
            if(!alternative.formats.empty())video=std::move(alternative);
            else if(!page_error.empty())throw std::runtime_error(page_error);
        } catch(const std::exception& error) {
            log_event("video.player.fallback_error",error.what());
            if(!page_error.empty())throw std::runtime_error(page_error);
        }
    }
    if(video.formats.empty()&&video.audio_formats.empty())
        throw std::runtime_error("YouTube did not expose downloadable MP4 URLs for this video");
    log_event("video.inspect.ready",id+" video_formats="+std::to_string(video.formats.size())+" audio_formats="+std::to_string(video.audio_formats.size()));return video;
}
std::filesystem::path download_video(const Video& video,const Format& format,const std::filesystem::path& folder,std::atomic<bool>& canceled,Progress progress) {
    const auto target=checked_folder(folder.u8string());
    const auto output=target/std::filesystem::u8path(safe_filename(video.title,video.id));
    log_event("download.start",video.id+" path="+output.u8string()+" height="+std::to_string(format.height));
    if(std::filesystem::exists(output))throw std::runtime_error("Output file already exists");
    std::unique_ptr<PlayerSolver> solver;
    if(challenged(format.url,format.cipher)||challenged(format.audio_url,format.audio_cipher)) {
        if(video.player_url.empty())throw std::runtime_error("YouTube did not provide the player script needed for this video");
        solver=std::make_unique<PlayerSolver>(prepared_player(video.player_url,&canceled),&canceled);
    }
    const auto video_url=resolve_media_url(format.url,format.cipher,solver.get());
    const bool merge=!format.has_audio;
    if(merge&&find_ffmpeg().empty())throw std::runtime_error("FFmpeg is needed to combine this video's audio and picture");
    const auto audio_url=merge?resolve_media_url(format.audio_url,format.audio_cipher,solver.get()):std::string{};
    solver.reset(); // Release the player heap before downloading media.
    TemporaryDirectory temporary(target);auto media=temporary.path/"video.mp4";
    const uint64_t total=format.content_length&&(!merge||format.audio_length)?format.content_length+(merge?format.audio_length:0):0;
    const auto bytes=fetch_media(video_url,media,canceled,[&](uint64_t received,uint64_t reported){if(progress)progress(received,total?total:(merge?0:reported));},format.content_length);
    if(merge) {
        auto audio=temporary.path/"audio.m4a";
        fetch_media(audio_url,audio,canceled,[&](uint64_t received,uint64_t reported){if(progress)progress(bytes+received,total?total:(reported?bytes+reported:0));},format.audio_length);
        auto merged=temporary.path/"merged.mp4";merge_media(media,audio,merged,canceled);media=merged;
    }
    if(canceled.load())throw std::runtime_error("Download canceled");
    try{publish_file(media,output);}catch(const std::exception& error){log_event("download.publish_error",error.what());throw;}
    log_event("download.complete",video.id+" path="+output.u8string());return output;
}
} // namespace cryget
