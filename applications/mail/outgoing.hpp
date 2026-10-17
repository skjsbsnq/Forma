#pragma once
#include "backend.hpp"
namespace forma::mail {
std::string buildOutgoing(const Account& account, const Outgoing& message);
std::vector<std::string> recipientAddresses(const Outgoing& message);
}  // namespace forma::mail
