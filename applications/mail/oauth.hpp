#pragma once
#include <atomic>
#include "backend.hpp"

namespace forma::mail::oauth {
// Explicit endpoints are for local protocol tests only. Application callers use
// fixed provider endpoints and cannot override them through account settings.
struct Endpoints {
    std::string authorize, token, device, scopes;
};
Endpoints endpoints(const Account& account);
void authorize(Account& account, std::atomic<bool>& cancelled, const Backend::ShowAuth& show,
               const Endpoints* testing = nullptr);
void refresh(Account& account, std::atomic<bool>& cancelled, const Endpoints* testing = nullptr);
// Keyring storage is optional. Returns a user-facing warning on storage failure.
std::string remember(const Account& account);
void forget(const Account& account);
}  // namespace forma::mail::oauth
