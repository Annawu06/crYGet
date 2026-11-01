#pragma once

#include <cstddef>
#include <vector>

namespace cryget {
// List order is download priority. Completed/failed/removed entries do not
// occupy slots; a worker stopping for reprioritization remains eligible.
template<class Job, class Eligible>
std::vector<Job> priority_jobs(const std::vector<Job>& jobs, std::size_t limit, Eligible eligible) {
    std::vector<Job> result;
    for (const auto& job : jobs) {
        if (result.size() == limit) break;
        if (eligible(job)) result.push_back(job);
    }
    return result;
}
} // namespace cryget
