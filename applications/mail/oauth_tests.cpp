#include <curl/curl.h>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include "oauth.hpp"
using namespace forma::mail;
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
    const char* base = std::getenv("FORMA_OAUTH_TEST_BASE");
    if (!base) return 2;
    oauth::Endpoints endpoints{std::string(base) + "/authorize", std::string(base) + "/token",
                               std::string(base) + "/device", "test-mail offline_access"};
    Account account;
    account.provider = "google";
    account.clientId = "local-test-client";
    account.username = account.address = "local@example.invalid";
    std::atomic<bool> cancel = false;
    std::jthread browser;
    bool googlePrompt = false;
    oauth::authorize(
        account, cancel,
        [&](const Backend::AuthPrompt& prompt) {
            check(prompt.url.find("code_challenge_method=S256") != prompt.url.npos, "PKCE prompt");
            check(prompt.code.empty(), "Google not device code");
            googlePrompt = true;
            browser = std::jthread([url = prompt.url] {
                auto* c = curl_easy_init();
                curl_easy_setopt(c, CURLOPT_URL, url.c_str());
                curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
                curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
                curl_easy_setopt(c, CURLOPT_CAINFO, std::getenv("SSL_CERT_FILE"));
                curl_easy_setopt(
                    c, CURLOPT_WRITEFUNCTION, +[](char*, size_t n, size_t m, void*) -> size_t { return n * m; });
                auto code = curl_easy_perform(c);
                curl_easy_cleanup(c);
                if (code != CURLE_OK) std::cerr << "test browser failure\n";
            });
        },
        &endpoints);
    check(googlePrompt && account.accessToken == "local-access" && account.refreshToken == "local-refresh",
          "Google authorization exchange");
    if (browser.joinable()) browser.join();
    account.tokenExpires = 0;
    oauth::refresh(account, cancel, &endpoints);
    check(account.accessToken == "local-refreshed" && account.refreshToken == "local-rotated",
          "refresh token rotation");
    Account ms;
    ms.provider = "microsoft";
    ms.clientId = "local-ms-client";
    ms.username = "ms@example.invalid";
    bool code = false;
    oauth::authorize(
        ms, cancel,
        [&](const Backend::AuthPrompt& prompt) {
            check(prompt.url.starts_with("https://"), "device HTTPS");
            check(prompt.code == "TEST-CODE", "device user code");
            code = true;
        },
        &endpoints);
    check(code && ms.accessToken == "local-access", "Microsoft pending flow");
    cancel = true;
    ms.accessToken.clear();
    ms.refreshToken.clear();
    ms.username = "cancelled@example.invalid";
    bool rejected = false;
    try {
        oauth::authorize(ms, cancel, [](const auto&) {}, &endpoints);
    } catch (...) {
        rejected = true;
    }
    check(rejected, "cancel OAuth");
    std::cout << "OAuth PKCE/state rejection/token exchange/rotation/device polling/cancellation passed\n";
}
