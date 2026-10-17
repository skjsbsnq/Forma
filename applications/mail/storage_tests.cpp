#include <sys/stat.h>
#include <unistd.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include "mime.hpp"
#include "outgoing.hpp"
#include "storage.hpp"
using namespace forma::mail;
template <class F>
void rejects(F f) {
    bool rejected = false;
    try {
        f();
    } catch (...) {
        rejected = true;
    }
    assert(rejected);
}
int main() {
    char tmp[] = "/tmp/forma-storage-XXXXXX";
    auto p = mkdtemp(tmp);
    assert(p);
    std::string d = p;
    Account a;
    a.address = "first@example.com";
    a.imapHost = "imap.example.com";
    a.username = "first";
    a.name = "中文姓名";
    a.accessToken = "DO-NOT-SAVE";
    Draft draft;
    draft.id = "一";
    draft.to = "Name <one@example.com>; two@example.com";
    draft.cc = "three@example.com";
    draft.bcc = "secret@example.com";
    draft.subject = "中文主题";
    draft.body = "正文";
    draft.attachments.push_back({"测试.png", "image/png", std::string("\0\1binary", 8)});
    storage::saveDrafts(d, {draft});
    auto ds = storage::loadDrafts(d);
    assert(ds.size() == 1 && ds[0].bcc == draft.bcc && ds[0].attachments[0].data == draft.attachments[0].data);
    struct stat s{};
    stat(d.c_str(), &s);
    assert((s.st_mode & 0777) == 0700);
    stat((d + "/drafts.json").c_str(), &s);
    assert((s.st_mode & 0777) == 0600);
    Message msg;
    msg.id = "1";
    msg.body = "缓存";
    msg.attachments = draft.attachments;
    storage::saveCache(d, a, "../../INBOX", {msg});
    assert(storage::loadCache(d, a, "../../INBOX").size() == 1);
    Account b = a;
    b.username = "second";
    assert(storage::loadCache(d, b, "../../INBOX").empty());
    for (auto& f : std::filesystem::directory_iterator(d)) {
        std::ifstream in(f.path());
        std::string text((std::istreambuf_iterator<char>(in)), {});
        assert(text.find(a.accessToken) == std::string::npos);
    }
    Outgoing out;
    out.to = draft.to;
    out.cc = draft.cc;
    out.bcc = draft.bcc;
    out.subject = std::string(160, 'a') + draft.subject;
    out.body = draft.body;
    out.attachments = draft.attachments;
    assert(recipientAddresses(out).size() == 4);
    auto mime = buildOutgoing(a, out);
    assert(mime.find("Bcc:") == std::string::npos && mime.find("secret@example.com") == std::string::npos);
    assert(mime.find("filename*0*=UTF-8''%E6") != std::string::npos);
    assert(mime.find("Cc: three@example.com") != std::string::npos);
    out.cc = "one@example.com";
    assert(recipientAddresses(out).size() == 3);
    out.subject = "bad\r\nBcc: leak@example.com";
    rejects([&] { buildOutgoing(a, out); });
    out.subject = "okay";
    out.to = "evil@example.com\r\nDATA";
    rejects([&] { recipientAddresses(out); });
    out.to = "bad..local@example.com";
    rejects([&] { recipientAddresses(out); });
    auto parsed = parseMimeMessage(mime, "roundtrip");
    assert(parsed.body == draft.body && parsed.attachments.size() == 1);
    assert(parsed.attachments[0].name == draft.attachments[0].name &&
           parsed.attachments[0].data == draft.attachments[0].data);
    assert(parsed.subject == std::string(160, 'a') + draft.subject && parsed.sender == a.name);
    std::string alternative =
        "Subject: =?UTF-8?B?5Lit5paH?=\r\n =?UTF-8?B?5Li76aKY?=\r\nContent-Type: multipart/alternative; "
        "boundary=alt\r\n\r\n--alt\r\nContent-Type: text/plain; charset=utf-8\r\n\r\nplain "
        "body\r\n--alt\r\nContent-Type: text/html; charset=utf-8\r\n\r\n<b>duplicated html</b>\r\n--alt--\r\n";
    auto alt = parseMimeMessage(alternative, "alt");
    assert(alt.body == "plain body" && alt.subject == "中文主题");
    auto html = parseMimeMessage(
        "Content-Type: text/html; charset=utf-8\r\n\r\n<head>hidden</head><script>evil()</script><p>&#20013;&#x6587; "
        "&amp; <a href=\"https://example.com\">link</a></p><style>hidden</style>",
        "html");
    assert(html.body == "中文 & link (https://example.com)");
    auto filename = parseMimeMessage(
        "Content-Type: application/octet-stream\r\nContent-Disposition: attachment; "
        "filename*=UTF-8''%E4%B8%AD.txt\r\nContent-Transfer-Encoding: base64\r\n\r\nYWJj\r\n",
        "file");
    assert(filename.attachments[0].name == "中.txt" && filename.attachments[0].data == "abc");
    auto related = parseMimeMessage(
        "Content-Type: multipart/related; boundary=r\r\n\r\n--r\r\nContent-Type: text/html\r\n\r\nhello<img "
        "src=cid:photo>\r\n--r\r\nContent-Type: image/png\r\nContent-ID: <photo>\r\nContent-Transfer-Encoding: "
        "base64\r\n\r\nYWJj\r\n--r--\r\n",
        "related");
    // The inline cid image becomes a replaceable placeholder, carried as the
    // attachment plus the U+2028-delimited record the reader view resolves.
    assert(related.body.starts_with("hello") && related.body.find("img:photo") != std::string::npos &&
           related.attachments.empty() && related.inlineImages.size() == 1 &&
           related.inlineImages[0].cid == "photo");
    rejects([&] { parseMimeMessage("Content-Type: text/plain; charset=nonsense-charset\r\n\r\na", "bad"); });
    rejects([&] { parseMimeMessage("Content-Transfer-Encoding: base64\r\n\r\n@@@=", "bad"); });
    rejects([&] { parseMimeMessage("Content-Transfer-Encoding: quoted-printable\r\n\r\n=XY", "bad"); });
    auto bad = draft;
    bad.subject = std::string("bad\0text", 8);
    rejects([&] { storage::saveDrafts(d, {bad}); });
    assert(storage::loadDrafts(d).at(0).subject == draft.subject);
    std::ofstream(d + "/drafts.json") << "{broken";
    rejects([&] { storage::loadDrafts(d); });
    assert(std::filesystem::file_size(d + "/drafts.json") == 7);
    storage::clearAccountCache(d, a);
    assert(storage::loadCache(d, a, "../../INBOX").empty());
    std::filesystem::remove_all(d);
    std::cout << "storage and outgoing tests passed\n";
}
