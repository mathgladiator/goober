// What a directory holds on the disk, each file counted once however many names it has
// (limits.cpp): the logs directory's rotated files and their archive queue are hard links.
#pragma once

#include <cstdint>
#include <filesystem>

namespace goober::host {

// Regular files under `root` (symlinks left out), a file with several hard links counted once.
int64_t bytes_on_disk(const std::filesystem::path& root);

// The archive queue's .log files (logs/archive/) whose numbered twin in logs/ is gone, so they
// hold bytes nothing else does: a queued file still linked to its rotated name adds nothing.
int64_t archive_only_bytes(const std::filesystem::path& archive);

}  // namespace goober::host
