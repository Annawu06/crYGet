#include "media_process.hpp"
#include "diagnostics.hpp"
#include "url_utils.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace cryget {
namespace {
using Bytes=std::vector<std::uint8_t>;
struct Box {
    std::uint64_t start,size,header;
    std::string type;
    std::uint64_t end() const {return start+size;}
};
struct MediaRange {std::uint64_t first,last,output_first;};

[[noreturn]] void invalid(const std::string& why){throw std::runtime_error("Unsupported or damaged MP4: "+why);}
std::uint64_t get(const Bytes& d,std::uint64_t p,unsigned n){
    if(n>8||p>d.size()||n>d.size()-p)invalid("truncated box");
    std::uint64_t v=0;for(unsigned i=0;i<n;++i)v=(v<<8)|d[static_cast<std::size_t>(p+i)];return v;
}
void put(Bytes& d,std::uint64_t p,std::uint64_t v,unsigned n){
    if(n>8||p>d.size()||n>d.size()-p||(n<8&&v>=(std::uint64_t{1}<<(n*8))))invalid("MP4 field is too large");
    for(unsigned i=0;i<n;++i)d[static_cast<std::size_t>(p+n-1-i)]=static_cast<std::uint8_t>(v>>(8*i));
}
void append(Bytes& d,std::uint64_t v,unsigned n){auto p=d.size();d.resize(p+n);put(d,p,v,n);}
Box parse(const Bytes& d,std::uint64_t p,std::uint64_t limit){
    if(limit>d.size()||p>limit||limit-p<8)invalid("incomplete box header");
    Box b{p,get(d,p,4),8,std::string(d.begin()+static_cast<std::ptrdiff_t>(p+4),d.begin()+static_cast<std::ptrdiff_t>(p+8))};
    if(b.size==1){b.header=16;b.size=get(d,p+8,8);}
    else if(b.size==0)b.size=limit-p;
    if(b.size<b.header||b.size>limit-p)invalid("invalid box length");
    return b;
}
std::vector<Box> children(const Bytes& d,const Box& parent){
    std::vector<Box> out;auto p=parent.start+parent.header;
    while(p<parent.end()){auto b=parse(d,p,parent.end());out.push_back(b);p=b.end();}
    return out;
}
Box child(const Bytes& d,const Box& parent,const std::string& type){
    for(auto b:children(d,parent))if(b.type==type)return b;
    invalid("missing "+type+" box");
}
Bytes slice(const Bytes& d,const Box& b){
    return Bytes(d.begin()+static_cast<std::ptrdiff_t>(b.start),d.begin()+static_cast<std::ptrdiff_t>(b.end()));
}
Bytes boxed(const std::string& type,const Bytes& payload){
    if(type.size()!=4||payload.size()>std::numeric_limits<std::uint32_t>::max()-8)invalid("metadata box is too large");
    Bytes out;out.reserve(payload.size()+8);append(out,payload.size()+8,4);
    out.insert(out.end(),type.begin(),type.end());out.insert(out.end(),payload.begin(),payload.end());return out;
}
unsigned version(const Bytes& d,const Box& b){
    if(b.size<b.header+4)invalid("truncated full box");
    return d[static_cast<std::size_t>(b.start+b.header)];
}
std::uint64_t scale(std::uint64_t value,std::uint32_t from,std::uint32_t to){
    if(!from||!to)invalid("zero movie timescale");
    const long double result=static_cast<long double>(value)*to/from;
    if(result>static_cast<long double>(std::numeric_limits<std::uint64_t>::max()-1))invalid("movie duration is too large");
    return static_cast<std::uint64_t>(std::round(result));
}
std::uint32_t timescale(const Bytes& d,const Box& mvhd){
    auto v=version(d,mvhd);if(v>1)invalid("movie header version");
    return static_cast<std::uint32_t>(get(d,mvhd.start+mvhd.header+(v?20:12),4));
}
std::uint64_t duration(const Bytes& d,const Box& mvhd){
    auto v=version(d,mvhd);return get(d,mvhd.start+mvhd.header+(v?24:16),v?8:4);
}
Box track(const Bytes& d,const Box& moov,const std::string& handler){
    for(auto trak:children(d,moov))if(trak.type=="trak"){
        auto hdlr=child(d,child(d,trak,"mdia"),"hdlr");
        auto p=hdlr.start+hdlr.header+8;
        if(p+4>hdlr.end())invalid("truncated handler");
        if(std::string(d.begin()+static_cast<std::ptrdiff_t>(p),d.begin()+static_cast<std::ptrdiff_t>(p+4))==handler)return trak;
    }
    invalid("missing "+handler+" track");
}
std::uint64_t offset_after_copy(std::uint64_t old,const std::vector<MediaRange>& ranges){
    for(const auto& r:ranges)if(old>=r.first&&old<r.last)return r.output_first+old-r.first;
    invalid("audio sample is outside an mdat box");
}
Bytes rewrite_audio(const Bytes& d,const Box& b,const std::vector<MediaRange>& ranges,
                    std::uint32_t id,std::uint32_t from,std::uint32_t to){
    if(b.type=="stco"||b.type=="co64"){
        if(version(d,b)!=0)invalid("chunk offset version");
        auto count=get(d,b.start+b.header+4,4);unsigned width=b.type=="stco"?4:8;
        if(b.size<b.header+8||count>(b.size-b.header-8)/width)invalid("truncated chunk offsets");
        Bytes payload(d.begin()+static_cast<std::ptrdiff_t>(b.start+b.header),
                      d.begin()+static_cast<std::ptrdiff_t>(b.start+b.header+8));
        for(std::uint64_t i=0;i<count;++i)append(payload,offset_after_copy(get(d,b.start+b.header+8+i*width,width),ranges),8);
        return boxed("co64",payload);
    }
    if(b.type=="tkhd"){
        Bytes out=slice(d,b);auto local=parse(out,0,out.size());auto v=version(out,local);
        if(v>1)invalid("track header version");
        put(out,local.header+(v?20:12),id,4);
        auto p=local.header+(v?28:20);put(out,p,scale(get(out,p,v?8:4),from,to),v?8:4);
        return out;
    }
    if(b.type=="elst"){
        Bytes out=slice(d,b);auto local=parse(out,0,out.size());auto v=version(out,local);
        if(v>1)invalid("edit list version");
        auto count=get(out,local.header+4,4);unsigned width=v?8:4,entry=v?20:12;
        if(local.size<local.header+8||count>(local.size-local.header-8)/entry)invalid("truncated edit list");
        for(std::uint64_t i=0;i<count;++i){
            auto p=local.header+8+i*entry;put(out,p,scale(get(out,p,width),from,to),width);
        }
        return out;
    }
    if(b.type=="trak"||b.type=="mdia"||b.type=="minf"||b.type=="stbl"||b.type=="edts"){
        Bytes payload;
        for(auto c:children(d,b)){auto part=rewrite_audio(d,c,ranges,id,from,to);payload.insert(payload.end(),part.begin(),part.end());}
        return boxed(b.type,payload);
    }
    return slice(d,b);
}
Bytes combine_moov(const Bytes& v,const Bytes& a,const std::vector<MediaRange>& ranges,bool fragmented){
    auto vm=parse(v,0,v.size()),am=parse(a,0,a.size());
    bool video_mvex=false,audio_mvex=false;
    for(auto b:children(v,vm))if(b.type=="mvex")video_mvex=true;
    for(auto b:children(a,am))if(b.type=="mvex")audio_mvex=true;
    if(fragmented&&!video_mvex&&!audio_mvex)invalid("missing fragment defaults");
    auto vmh=child(v,vm,"mvhd"),amh=child(a,am,"mvhd");
    auto vt=track(v,vm,"vide"),at=track(a,am,"soun");
    auto from=timescale(a,amh),to=timescale(v,vmh);
    auto vtk=child(v,vt,"tkhd"),atk=child(a,at,"tkhd");
    auto vv=version(v,vtk),av=version(a,atk);
    if(vv>1||av>1)invalid("track header version");
    auto id=get(v,vtk.start+vtk.header+(vv?20:12),4);
    if(id>std::numeric_limits<std::uint32_t>::max()-2)invalid("track ID is too large");
    auto audio_duration=scale(get(a,atk.start+atk.header+(av?28:20),av?8:4),from,to);
    auto total=std::max(duration(v,vmh),audio_duration);
    Bytes audio_defaults;
    if(audio_mvex){
        auto amvex=child(a,am,"mvex");
        for(auto entry:children(a,amvex))if(entry.type=="trex"){
            auto part=slice(a,entry);
            auto local=parse(part,0,part.size());
            put(part,local.header+4,id+1,4);
            audio_defaults.insert(audio_defaults.end(),part.begin(),part.end());
        }
        if(audio_defaults.empty())invalid("missing audio fragment defaults");
    }
    Bytes payload;
    for(auto b:children(v,vm)){
        Bytes part;
        if(b.type=="trak"){
            if(b.start!=vt.start)continue;
            part=slice(v,b);
        }else if(b.type=="mvex"&&fragmented){
            Bytes content;
            for(auto entry:children(v,b)){
                auto part=slice(v,entry);content.insert(content.end(),part.begin(),part.end());
            }
            content.insert(content.end(),audio_defaults.begin(),audio_defaults.end());
            part=boxed("mvex",content);
        }else if(b.type=="mvhd"){
            part=slice(v,b);auto local=parse(part,0,part.size());auto vers=version(part,local);
            if(vers>1)invalid("movie header version");
            put(part,local.header+(vers?24:16),total,vers?8:4);
            put(part,part.size()-4,id+2,4);
        }else part=slice(v,b);
        payload.insert(payload.end(),part.begin(),part.end());
        if(b.start==vt.start){
            auto added=rewrite_audio(a,at,ranges,static_cast<std::uint32_t>(id+1),from,to);
            payload.insert(payload.end(),added.begin(),added.end());
        }
    }
    if(fragmented&&!video_mvex){
        auto mvex=boxed("mvex",audio_defaults);
        payload.insert(payload.end(),mvex.begin(),mvex.end());
    }
    return boxed("moov",payload);
}
std::uint64_t file_size(std::ifstream& in){
    in.seekg(0,std::ios::end);auto p=in.tellg();
    if(p<0)invalid("cannot read media size");
    in.seekg(0);return static_cast<std::uint64_t>(p);
}
std::vector<Box> top_boxes(std::ifstream& in,std::uint64_t size){
    std::vector<Box> out;
    for(std::uint64_t p=0;p<size;){
        if(size-p<8)invalid("incomplete top-level box");
        std::array<unsigned char,16> h{};
        in.clear();in.seekg(static_cast<std::streamoff>(p));in.read(reinterpret_cast<char*>(h.data()),8);
        if(!in)invalid("cannot read MP4 header");
        std::uint64_t n=(std::uint64_t(h[0])<<24)|(std::uint64_t(h[1])<<16)|(std::uint64_t(h[2])<<8)|h[3];
        Box b{p,n,8,std::string(reinterpret_cast<const char*>(h.data()+4),4)};
        if(n==0&&b.type=="mdat")invalid("mdat with unknown length");
        if(n==1){
            in.read(reinterpret_cast<char*>(h.data()+8),8);if(!in)invalid("truncated extended box");
            b.header=16;b.size=0;for(unsigned i=8;i<16;++i)b.size=(b.size<<8)|h[i];
        }else if(n==0)b.size=size-p;
        if(b.size<b.header||b.size>size-p)invalid("invalid top-level box length");
        out.push_back(b);p=b.end();
    }
    return out;
}
Box top(const std::vector<Box>& boxes,const std::string& type){
    for(auto b:boxes)if(b.type==type)return b;
    invalid("missing "+type+" box");
}
Bytes metadata(std::ifstream& in,const Box& b){
    if(b.size>128*1024*1024)invalid("metadata exceeds 128 MiB");
    Bytes d(static_cast<std::size_t>(b.size));in.clear();in.seekg(static_cast<std::streamoff>(b.start));
    in.read(reinterpret_cast<char*>(d.data()),static_cast<std::streamsize>(d.size()));
    if(!in)invalid("cannot read movie metadata");
    return d;
}
std::uint64_t output_position(std::ofstream& out){
    auto p=out.tellp();
    if(p<0)throw std::runtime_error("Cannot write combined MP4");
    return static_cast<std::uint64_t>(static_cast<std::streamoff>(p));
}
std::uint64_t shifted(std::uint64_t value,std::uint64_t old_start,std::uint64_t new_start){
    if(new_start>=old_start){
        const auto delta=new_start-old_start;
        if(value>std::numeric_limits<std::uint64_t>::max()-delta)invalid("fragment offset is too large");
        return value+delta;
    }
    const auto delta=old_start-new_start;
    if(value<delta)invalid("fragment offset is invalid");
    return value-delta;
}
void patch_fragment(Bytes& bytes,std::uint32_t old_id,std::uint32_t new_id,
                    std::uint64_t old_start,std::uint64_t new_start,std::uint32_t sequence){
    auto root=parse(bytes,0,bytes.size());
    if(root.type!="moof")invalid("expected movie fragment");
    auto mfhd=child(bytes,root,"mfhd");
    put(bytes,mfhd.start+mfhd.header+4,sequence,4);
    bool found=false;
    for(auto traf:children(bytes,root))if(traf.type=="traf"){
        auto tfhd=child(bytes,traf,"tfhd");
        auto id=get(bytes,tfhd.start+tfhd.header+4,4);
        if(id!=old_id)invalid("unexpected fragment track");
        put(bytes,tfhd.start+tfhd.header+4,new_id,4);
        const auto flags=get(bytes,tfhd.start+tfhd.header,4)&0xffffff;
        if(flags&1){
            const auto p=tfhd.start+tfhd.header+8;
            put(bytes,p,shifted(get(bytes,p,8),old_start,new_start),8);
        }
        found=true;
    }
    if(!found)invalid("fragment has no track");
}
void patch_sidx(Bytes& bytes,std::uint32_t id){
    auto root=parse(bytes,0,bytes.size());
    put(bytes,root.header+4,id,4);
}
void copy(std::ifstream& in,std::ofstream& out,std::uint64_t start,std::uint64_t count,const std::atomic<bool>& canceled){
    in.clear();in.seekg(static_cast<std::streamoff>(start));std::array<char,1024*1024> buf{};
    while(count){
        if(canceled.load())throw std::runtime_error("Download canceled");
        auto n=static_cast<std::streamsize>(std::min<std::uint64_t>(count,buf.size()));
        in.read(buf.data(),n);if(in.gcount()!=n)invalid("cannot read media data");
        out.write(buf.data(),n);if(!out)throw std::runtime_error("Cannot write combined MP4");
        count-=static_cast<std::uint64_t>(n);
    }
}
void copy_fragments(std::ifstream& input,std::ofstream& output,const std::vector<Box>& boxes,
                    std::uint32_t old_id,std::uint32_t new_id,std::uint32_t& sequence,
                    const std::atomic<bool>& canceled){
    bool found=false;
    for(auto box:boxes){
        if(box.type=="ftyp"||box.type=="moov"||box.type=="mfra")continue;
        if(box.type=="moof"){
            auto bytes=metadata(input,box);
            patch_fragment(bytes,old_id,new_id,box.start,output_position(output),sequence++);
            output.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
            found=true;
        }else if(box.type=="sidx"&&old_id!=new_id){
            auto bytes=metadata(input,box);
            patch_sidx(bytes,new_id);
            output.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
        }else copy(input,output,box.start,box.size,canceled);
        if(!output)throw std::runtime_error("Cannot write combined MP4");
    }
    if(!found)invalid("missing movie fragments");
}
std::vector<MediaRange> copy_audio_data(std::ifstream& audio,std::ofstream& out,
                                        const std::vector<Box>& boxes,const std::atomic<bool>& canceled){
    std::vector<MediaRange> ranges;
    for(auto b:boxes)if(b.type=="mdat"){
        auto dest=output_position(out);
        copy(audio,out,b.start,b.size,canceled);
        ranges.push_back({b.start+b.header,b.end(),dest+b.header});
    }
    if(ranges.empty())invalid("missing audio mdat box");
    return ranges;
}
void merge_impl(const std::filesystem::path& vp,const std::filesystem::path& ap,
                const std::filesystem::path& op,const std::atomic<bool>& canceled){
    std::ifstream video(vp,std::ios::binary),audio(ap,std::ios::binary);
    if(!video||!audio)throw std::runtime_error("Cannot open downloaded media");
    auto vs=file_size(video),as=file_size(audio);
    auto vb=top_boxes(video,vs),ab=top_boxes(audio,as);
    top(vb,"ftyp");top(ab,"ftyp");
    const auto vf=std::any_of(vb.begin(),vb.end(),[](const Box& b){return b.type=="moof";});
    const auto af=std::any_of(ab.begin(),ab.end(),[](const Box& b){return b.type=="moof";});
    auto vm=top(vb,"moov"),am=top(ab,"moov");
    auto v=metadata(video,vm),a=metadata(audio,am);
    std::ofstream out(op,std::ios::binary|std::ios::trunc);
    if(!out)throw std::runtime_error("Cannot create combined MP4");
    if(vf){
        auto vroot=parse(v,0,v.size()),aroot=parse(a,0,a.size());
        auto vt=track(v,vroot,"vide"),at=track(a,aroot,"soun");
        auto vtk=child(v,vt,"tkhd"),atk=child(a,at,"tkhd");
        auto vv=version(v,vtk),av=version(a,atk);
        if(vv>1||av>1)invalid("track header version");
        auto video_id=static_cast<std::uint32_t>(get(v,vtk.start+vtk.header+(vv?20:12),4));
        auto audio_id=static_cast<std::uint32_t>(get(a,atk.start+atk.header+(av?20:12),4));
        if(video_id>std::numeric_limits<std::uint32_t>::max()-2)invalid("track ID is too large");
        auto ftyp=top(vb,"ftyp");
        copy(video,out,ftyp.start,ftyp.size,canceled);
        std::vector<MediaRange> ranges;
        if(!af)ranges=copy_audio_data(audio,out,ab,canceled);
        auto moov=combine_moov(v,a,ranges,true);
        out.write(reinterpret_cast<const char*>(moov.data()),static_cast<std::streamsize>(moov.size()));
        std::uint32_t sequence=1;
        copy_fragments(video,out,vb,video_id,video_id,sequence,canceled);
        if(af)copy_fragments(audio,out,ab,audio_id,video_id+1,sequence,canceled);
        return;
    }
    copy(video,out,0,vs,canceled);
    out.seekp(static_cast<std::streamoff>(vm.start+4));out.write("free",4);
    out.seekp(0,std::ios::end);
    std::vector<MediaRange> ranges;
    if(!af)ranges=copy_audio_data(audio,out,ab,canceled);
    auto moov=combine_moov(v,a,ranges,af);
    if(canceled.load())throw std::runtime_error("Download canceled");
    out.write(reinterpret_cast<const char*>(moov.data()),static_cast<std::streamsize>(moov.size()));
    if(af){
        auto vroot=parse(v,0,v.size()),aroot=parse(a,0,a.size());
        auto vt=track(v,vroot,"vide"),at=track(a,aroot,"soun");
        auto vtk=child(v,vt,"tkhd"),atk=child(a,at,"tkhd");
        auto vv=version(v,vtk),av=version(a,atk);
        if(vv>1||av>1)invalid("track header version");
        auto video_id=static_cast<std::uint32_t>(get(v,vtk.start+vtk.header+(vv?20:12),4));
        auto audio_id=static_cast<std::uint32_t>(get(a,atk.start+atk.header+(av?20:12),4));
        std::uint32_t sequence=1;
        copy_fragments(audio,out,ab,audio_id,video_id+1,sequence,canceled);
    }
    if(!out)throw std::runtime_error("Cannot finish combined MP4");
}
} // namespace
void merge_media(const std::filesystem::path& video,const std::filesystem::path& audio,
                 const std::filesystem::path& output,const std::atomic<bool>& canceled){
    if(canceled.load())throw std::runtime_error("Download canceled");
    if(std::filesystem::exists(output))throw std::runtime_error("Combined output already exists");
    log_event("merge.start","output="+path_utf8(output));
    try{merge_impl(video,audio,output,canceled);}
    catch(...){std::error_code ignored;std::filesystem::remove(output,ignored);throw;}
    log_event("merge.complete","output="+path_utf8(output));
}
} // namespace cryget
