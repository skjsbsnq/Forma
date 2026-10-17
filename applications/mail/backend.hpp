#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mayonnaise/mayonnaise.hpp>
#include <string>
#include <thread>
#include <vector>

namespace forma::mail {
struct Attachment {
    std::string name, mime, data;
    std::string cid;  // Content-ID of an inline image, without angle brackets.
};
struct Message {
    std::string id, folder = "INBOX", sender, address, subject, preview, body, date;
    bool unread = false, star = false;
    std::vector<Attachment> attachments;
    // Inline images referenced by cid: in the HTML body (multipart/related parts).
    std::vector<Attachment> inlineImages;
};
struct Folder {
    std::string id, title;
    int unread = 0;
};
struct Account {
    std::string name, address, username, password, imapHost, smtpHost;
    int imapPort = 993, smtpPort = 465;
    bool imapImplicitTLS = true, smtpImplicitTLS = true, rememberPassword = false;
    // password, google or microsoft. Tokens never enter ordinary settings files.
    std::string provider = "password", clientId, clientSecret, tenant = "common";
    std::string accessToken, refreshToken;
    std::int64_t tokenExpires = 0;
};
struct Draft {
    std::string id, to, subject, body;
    std::string cc, bcc;
    std::vector<Attachment> attachments;
};
struct Outgoing {
    std::string to, subject, body;
    std::string cc, bcc;
    std::vector<Attachment> attachments;
};
// All public calls and callbacks run on the UI thread. Network operations run on
// one cancellable worker. A callback's empty string means success.
class Backend {
public:
    using Done = std::function<void(const std::string&)>;
    Backend(mayo::App& app, mayo::Settings& settings);
    ~Backend();
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;
    const Account& account() const { return account_; }
    const std::vector<Folder>& folders() const { return folders_; }
    const std::vector<Message>& messages() const { return messages_; }
    const std::vector<Draft>& drafts() const { return drafts_; }
    const std::string& currentFolder() const { return folder_; }
    bool connected() const { return connected_; }
    bool busy() const { return busy_; }
    bool demo() const { return demo_; }
    const std::string& warning() const { return warning_; }
    const std::string& draftError() const { return draftError_; }
    // Private configuration directory (where the offline cache and drafts live).
    const std::string& settingsDirectory() const { return settings_.directory(); }
    // Explicit demo state: never confused with a connected account.
    void useDemo();
    void connect(Account account, Done done);
    struct AuthPrompt {
        std::string url, code, message;
    };
    using ShowAuth = std::function<void(const AuthPrompt&)>;
    // Google uses PKCE+loopback; Microsoft uses its device authorization flow.
    void signIn(Account account, ShowAuth show, Done done);
    void cancel() { cancelled_ = true; }
    void loadMore(Done done);
    int messageLimit() const { return messageLimit_; }
    void disconnect();
    void sync(Done done);
    void loadFolder(const std::string& id, Done done);
    void setRead(const std::string& id, bool read, Done done);
    void setStar(const std::string& id, bool star, Done done);
    void move(const std::string& id, const std::string& folder, Done done);
    // Irreversible deletion is deliberately absent; move to Trash instead.
    void send(Outgoing message, Done done);
    std::string saveDraft(Draft draft);
    void removeDraft(const std::string& id);
    // Tests MIME parsing without a server or account.
    static Message parseMessage(std::string raw, std::string id);
    static std::string makeOutgoing(const Account& account, const Outgoing& message);

private:
    mayo::App& app_;
    mayo::Settings& settings_;
    Account account_;
    std::vector<Folder> folders_;
    std::vector<Message> messages_;
    std::vector<Draft> drafts_;
    std::string folder_ = "INBOX", warning_, draftError_;
    bool connected_ = false, busy_ = false, demo_ = true, readOnly_ = false, draftStoreHealthy_ = true;
    std::atomic<bool> cancelled_{false}, shuttingDown_{false};
    int messageLimit_ = 40;
    std::jthread worker_;
    void run(std::function<std::string()> work, std::function<void()> apply, Done done);
    void saveProfile();
    void updateUnread();
    void loadDrafts();
    void persistDrafts();
    void saveCurrentCache();
};
}  // namespace forma::mail
