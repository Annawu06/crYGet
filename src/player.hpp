#pragma once
#include <atomic>
#include <memory>
#include <string>

namespace cryget {
struct PlayerResult { std::string signature, n; };
class PlayerSolver {
public:
    // The parser and player both run without filesystem, network or process APIs.
    static std::string prepare(const std::string& source, const std::atomic<bool>* canceled = nullptr);
    explicit PlayerSolver(const std::string& prepared, const std::atomic<bool>* canceled = nullptr);
    ~PlayerSolver();
    PlayerSolver(const PlayerSolver&) = delete;
    PlayerSolver& operator=(const PlayerSolver&) = delete;
    PlayerResult solve(const std::string& signature, const std::string& n);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
std::string resolve_media_url(const std::string& direct, const std::string& cipher, PlayerSolver* solver);
} // namespace cryget
