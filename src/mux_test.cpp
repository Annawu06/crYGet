#include "media_process.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#if __has_include("mux_test_config.hpp")
#include "mux_test_config.hpp"
#endif
#ifndef CRYGET_TEST_FIXTURE_DIR
#define CRYGET_TEST_FIXTURE_DIR "tests/fixtures"
#endif
int main(){
    auto folder=std::filesystem::temp_directory_path()/std::filesystem::u8path("crYGet 合并 空格 ' & "+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try{
        std::filesystem::create_directory(folder);
        const auto fixtures=std::filesystem::u8path(CRYGET_TEST_FIXTURE_DIR);
        std::filesystem::copy_file(fixtures/"video.mp4",folder/"video.mp4");
        std::filesystem::copy_file(fixtures/"audio.m4a",folder/"audio.m4a");
        std::atomic<bool> stop{false};
        cryget::merge_media(folder/"video.mp4",folder/"audio.m4a",folder/"merged.mp4",stop);
        if(std::filesystem::file_size(folder/"merged.mp4")<100)throw std::runtime_error("Merged output is empty");
        std::ifstream input(folder/"merged.mp4",std::ios::binary);
        const auto begin=std::istreambuf_iterator<char>{input};
        const auto end=std::istreambuf_iterator<char>{};
        const std::string bytes(begin,end);input.close();
        if(bytes.find("vide")==std::string::npos||bytes.find("soun")==std::string::npos)throw std::runtime_error("Merged MP4 is missing an audio or video track");
        bool rejected=false;
        try{cryget::merge_media(folder/"missing.mp4",folder/"audio.m4a",folder/"bad.mp4",stop);}catch(const std::runtime_error&){rejected=true;}
        if(!rejected)throw std::runtime_error("Invalid media input was accepted");
        stop=true;rejected=false;
        try{cryget::merge_media(folder/"video.mp4",folder/"audio.m4a",folder/"canceled.mp4",stop);}catch(const std::runtime_error&){rejected=true;}
        if(!rejected||std::filesystem::exists(folder/"canceled.mp4"))throw std::runtime_error("Canceled merge created output");
        std::filesystem::remove_all(folder);std::cout<<"In-process MP4 remux tests passed\n";
    }catch(const std::exception& error){std::filesystem::remove_all(folder);std::cerr<<error.what()<<'\n';return 1;}
}
