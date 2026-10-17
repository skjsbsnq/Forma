#pragma once
#include "backend.hpp"
namespace forma::mail::storage {
void saveDrafts(const std::string& directory, const std::vector<Draft>& drafts);
std::vector<Draft> loadDrafts(const std::string& directory);
void saveCache(const std::string& directory, const Account& account, const std::string& folder,
               const std::vector<Message>& messages);
std::vector<Message> loadCache(const std::string& directory, const Account& account, const std::string& folder);
void clearAccountCache(const std::string& directory, const Account& account);
}  // namespace forma::mail::storage
