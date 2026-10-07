#include "media_process.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifndef CRYGET_TEST_FIXTURE_DIR
#define CRYGET_TEST_FIXTURE_DIR "tests/fixtures"
#endif
int main(){
    if(cryget::find_ffmpeg().empty()){std::cout<<"FFmpeg unavailable; skipping merge integration test\n";return 77;}
    auto folder=std::filesystem::temp_directory_path()/std::filesystem::u8path("crYGet 合并 空格 ' & "+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try{
        std::filesystem::create_directory(folder);
        std::filesystem::copy_file(std::filesystem::path(CRYGET_TEST_FIXTURE_DIR)/"video.mp4",folder/"video.mp4");
        std::filesystem::copy_file(std::filesystem::path(CRYGET_TEST_FIXTURE_DIR)/"audio.m4a",folder/"audio.m4a");
        std::atomic<bool> stop{false};
        cryget::merge_media(folder/"video.mp4",folder/"audio.m4a",folder/"merged.mp4",stop);
        if(std::filesystem::file_size(folder/"merged.mp4")<100)throw std::runtime_error("Merged output is empty");
        std::ifstream input(folder/"merged.mp4",std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(input)),{});input.close();
        if(bytes.find("vide")==std::string::npos||bytes.find("soun")==std::string::npos)throw std::runtime_error("Merged MP4 is missing an audio or video track");
        bool rejected=false;
        try{cryget::merge_media(folder/"missing.mp4",folder/"audio.m4a",folder/"bad.mp4",stop);}catch(const std::runtime_error&){rejected=true;}
        if(!rejected)throw std::runtime_error("Invalid media input was accepted");
        stop=true;rejected=false;
        try{cryget::merge_media(folder/"video.mp4",folder/"audio.m4a",folder/"canceled.mp4",stop);}catch(const std::runtime_error&){rejected=true;}
        if(!rejected||std::filesystem::exists(folder/"canceled.mp4"))throw std::runtime_error("Canceled merge created output");
        std::filesystem::remove_all(folder);std::cout<<"FFmpeg merge tests passed\n";
    }catch(const std::exception& error){std::filesystem::remove_all(folder);std::cerr<<error.what()<<'\n';return 1;}
}
