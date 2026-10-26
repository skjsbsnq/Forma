#pragma once

#include <string>
#include <vector>

namespace finder {

/// Only offer extraction when every selected item is an archive outside the trash.
bool canExtractArchives(const std::vector<std::string>& paths);
/// An empty destination extracts each archive into its own parent directory.
/// Ark owns progress, passwords, overwrite prompts and extraction errors.
std::string extractArchives(const std::vector<std::string>& paths, const std::string& destination = {});

}  // namespace finder
