#pragma once
#include "backend.hpp"
namespace forma::mail {
Message parseMimeMessage(std::string raw, std::string id);
}
