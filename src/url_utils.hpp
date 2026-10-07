#pragma once
#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace cryget {
inline std::string path_utf8(const std::filesystem::path& path) {
    const auto value=path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()),value.size());
}

inline std::string url_decode(const std::string& input) {
    std::string out;
    auto hex = [](char c) -> int { if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return -1; };
    for(size_t i=0;i<input.size();++i) {
        if(input[i]=='%' && i+2<input.size() && hex(input[i+1])>=0 && hex(input[i+2])>=0) {
            out += static_cast<char>(hex(input[i+1])*16+hex(input[i+2])); i+=2;
        } else out += input[i]=='+' ? ' ' : input[i];
    }
    return out;
}
inline std::string url_encode(const std::string& input) {
    static const char digits[]="0123456789ABCDEF";
    std::string out;
    for(unsigned char c:input) {
        if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') out+=c;
        else {out+='%';out+=digits[c>>4];out+=digits[c&15];}
    }
    return out;
}
inline std::string query_value(const std::string& query, const std::string& key) {
    size_t start=0;
    while(start<query.size()) {
        auto end=query.find('&',start);if(end==std::string::npos)end=query.size();
        auto eq=query.find('=',start);
        if(eq<end && url_decode(query.substr(start,eq-start))==key) return url_decode(query.substr(eq+1,end-eq-1));
        start=end+1;
    }
    return {};
}
inline std::string url_parameter(const std::string& url,const std::string& key) {
    auto q=url.find('?');if(q==std::string::npos)return {};
    auto hash=url.find('#',q);
    return query_value(url.substr(q+1,hash==std::string::npos?hash:hash-q-1),key);
}
inline std::string set_url_parameter(const std::string& url,const std::string& key,const std::string& value) {
    const auto hash=url.find('#');
    const auto end=hash==std::string::npos?url.size():hash;
    const auto q=url.find('?');
    std::string out=url.substr(0,q<end?q:end)+"?";
    if(q<end) {
        size_t start=q+1;
        while(start<end) {
            auto next=url.find('&',start);if(next==std::string::npos||next>end)next=end;
            auto eq=url.find('=',start);if(eq>next)eq=next;
            if(url_decode(url.substr(start,eq-start))!=key) out+=url.substr(start,next-start)+"&";
            start=next+1;
        }
    }
    out+=url_encode(key)+"="+url_encode(value);
    if(hash!=std::string::npos)out+=url.substr(hash);
    return out;
}
} // namespace cryget
