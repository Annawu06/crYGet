#pragma once
#include <atomic>
#include <filesystem>
namespace cryget {
void merge_media(const std::filesystem::path& video,const std::filesystem::path& audio,
                 const std::filesystem::path& output,const std::atomic<bool>& canceled);
}
