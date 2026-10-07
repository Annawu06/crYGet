#pragma once

#include <filesystem>
#include <string>

namespace cryget {

std::filesystem::path log_path();
void log_event(const std::string& stage, const std::string& detail) noexcept;

} // namespace cryget
