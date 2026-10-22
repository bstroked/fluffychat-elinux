#include <cctype>
#include <clocale>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include "nlohmann/json.hpp"
#include "i18n.h"

using json = nlohmann::json;

// The Dart UI rewrites this file every ~1s while AppLifecycleState.resumed.
// Recents SIGSTOP's the process, so Flutter lifecycle and fcntl locks both
// lie: mtime freezes. Older than this is "not foreground" and we still post.
static const int kHeartbeatMaxAgeSec = 3;

static bool heartbeat_fresh(const std::string &path)
{
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0)
        return false;
    const time_t now = time(nullptr);
    if (now < st.st_mtime)
        return false;
    return (now - st.st_mtime) <= kHeartbeatMaxAgeSec;
}

static void add_lock_dir(std::vector<std::string> &paths, const std::string &dir)
{
    if (dir.empty())
        return;
    const char *names[] = {"/foreground.lock", "/running.lock"};
    for (const char *name : names) {
        paths.push_back(dir + name);
        if (dir.size() < 18 || dir.compare(dir.size() - 18, 18, "fluffychat.notkit") != 0)
            paths.push_back(dir + "/fluffychat.notkit" + std::string(name));
    }
}

static bool app_is_foreground()
{
    std::vector<std::string> paths;
    if (const char *xdg = std::getenv("XDG_RUNTIME_DIR"))
        add_lock_dir(paths, xdg);
    add_lock_dir(paths, std::string("/run/user/") + std::to_string(getuid()));
    if (const char *cache = std::getenv("XDG_CACHE_HOME"))
        add_lock_dir(paths, cache);
    if (const char *home = std::getenv("HOME"))
        add_lock_dir(paths, std::string(home) + "/.cache/fluffychat.notkit");
    for (const auto &path : paths) {
        if (heartbeat_fresh(path))
            return true;
    }
    return false;
}

// Every field below is optional in the Matrix push gateway payload: the
// "event_id_only" pusher format omits nearly all of them, "tweaks" is absent
// unless a push rule sets one, and non-message events (reactions, membership,
// calls) carry no content.body. Reading them unguarded throws, and a helper
// that dies produces no notification at all — so look everything up defensively
// and fall back to a generic card.
static std::string str_at(const json &obj, const char *key)
{
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string())
        return "";
    return it->get<std::string>();
}

static const json &obj_at(const json &obj, const char *key)
{
    static const json empty = json::object();
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_object())
        return empty;
    return *it;
}

// Written by ContactBook (fluffychat/lib/utils/contact_book.dart). The helper
// cannot read the app's SharedPreferences, so this is its only view of the
// phone address book.
static json load_contact_names()
{
    std::vector<std::string> paths;
    if (const char *cache = std::getenv("XDG_CACHE_HOME")) {
        std::string dir(cache);
        if (dir.size() < 17 || dir.compare(dir.size() - 17, 17, "fluffychat.notkit") != 0)
            dir += "/fluffychat.notkit";
        paths.push_back(dir + "/contact_names.json");
    }
    if (const char *home = std::getenv("HOME"))
        paths.push_back(std::string(home) + "/.cache/fluffychat.notkit/contact_names.json");
    for (const auto &path : paths) {
        std::ifstream f(path);
        if (!f)
            continue;
        json book = json::parse(f, nullptr, false);
        if (!book.is_discarded() && book.is_object())
            return book;
    }
    return json::object();
}

static std::string to_lower(std::string text)
{
    for (auto &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

static std::string digits_of(const std::string &text)
{
    std::string digits;
    for (char c : text) {
        if (c >= '0' && c <= '9')
            digits += c;
    }
    return digits;
}

static bool is_lid(const std::string &value)
{
    const std::string lower = to_lower(value);
    return lower.find("whatsapp_lid") != std::string::npos ||
           lower.find("_lid-") != std::string::npos;
}

// Same lookup keys as ContactBook._keys.
static std::vector<std::string> number_keys(const std::string &raw)
{
    std::vector<std::string> keys;
    std::string text = to_lower(raw);
    const auto at = text.find(':');
    if (!text.empty() && text[0] == '@' && at != std::string::npos && at > 0)
        text = text.substr(1, at - 1);
    for (const char *prefix : {"whatsapp_", "signal_", "telegram_", "sms_"}) {
        const std::string p(prefix);
        if (text.compare(0, p.size(), p) == 0) {
            text = text.substr(p.size());
            break;
        }
    }
    if (text.compare(0, 3, "lid") == 0)
        return keys;
    const std::string digits = digits_of(text);
    if (digits.size() < 7)
        return keys;
    keys.push_back(digits);
    const auto first = digits.find_first_not_of('0');
    const std::string stripped = first == std::string::npos ? "" : digits.substr(first);
    if (stripped.size() >= 7 && stripped != digits)
        keys.push_back(stripped);
    for (size_t n : {10, 9, 8}) {
        if (digits.size() > n)
            keys.push_back(digits.substr(digits.size() - n));
    }
    for (size_t n : {9, 8}) {
        if (stripped.size() > n)
            keys.push_back(stripped.substr(stripped.size() - n));
    }
    return keys;
}

static std::string lookup_number(const json &numbers, const std::string &value)
{
    if (value.empty())
        return "";
    for (const auto &key : number_keys(value)) {
        const std::string name = str_at(numbers, key.c_str());
        if (!name.empty())
            return name;
    }
    return "";
}

static bool looks_like_phone(const std::string &value)
{
    if (value.empty() || is_lid(value))
        return false;
    if (digits_of(value).size() >= 7)
        return true;
    const std::string lower = to_lower(value);
    for (const char *prefix : {"whatsapp_", "signal_", "telegram_", "sms_"}) {
        if (lower.find(prefix) != std::string::npos)
            return true;
    }
    return false;
}

// Mirrors ContactBook.resolve: a name the user assigned, then the address
// book by displayed number or MXID digits, then the last real bridge name.
static std::string contact_sender(const json &book, const std::string &mxid,
                                  const std::string &display)
{
    if (!mxid.empty()) {
        const std::string assigned = str_at(obj_at(book, "users"), mxid.c_str());
        if (!assigned.empty())
            return assigned;
    }
    const json &numbers = obj_at(book, "numbers");
    std::string found;
    if (looks_like_phone(display))
        found = lookup_number(numbers, display);
    if (found.empty())
        found = lookup_number(numbers, mxid);
    if (found.empty() && !mxid.empty() && looks_like_phone(display))
        found = str_at(obj_at(book, "remembered"), mxid.c_str());
    return found.empty() ? display : found;
}

int main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");
    textdomain(GETTEXT_DOMAIN.c_str());

    if (argc < 3) {
        std::cerr << "usage: push <input.json> <output.json>" << std::endl;
        return 1;
    }

    std::ifstream f1(argv[1]);
    std::ofstream f2(argv[2]);
    json js1 = json::parse(f1, nullptr, false);
    json js2;

    if (js1.is_discarded()) {
        std::cerr << "push: cannot parse input" << std::endl;
        return 1;
    }

    const json &message = obj_at(js1, "message");
    std::string mtype = str_at(message, "type");
    std::string room_name = str_at(message, "room_name");
    std::string sender = str_at(message, "sender_display_name");
    const std::string sender_id = str_at(message, "sender");
    if (!sender.empty() || !sender_id.empty())
        sender = contact_sender(load_contact_names(), sender_id, sender);
    std::string room_id = str_at(message, "room_id");
    std::string content_body = str_at(obj_at(message, "content"), "body");

    std::string summary = "";
    std::string body = "";
    std::string icon = "";

    int unread_count = 0;
    bool popup = false;

    if (!room_name.empty())
    {
        summary = room_name;
    }
    else if (!sender.empty())
    {
        summary = sender;
    }
    else
    {
        // event_id_only payloads name neither room nor sender.
        summary = "FluffyChat";
    }

    // No plaintext to show for encrypted rooms, and none either when the
    // payload carries no body at all.
    if (mtype == "m.room.encrypted" || content_body.empty()) {
        body = N_("New Message");
    }
    else {
        body = content_body;
    }
    if (!sender.empty() && summary != sender) {
        body = sender + ": " + body;
    }

    if (room_name.empty()) {
        icon = "contact";
    }
    else {
        icon = "contact-group";
    }

    if(summary != "" && body != ""){
        popup = true;
    }

    const json &counts = obj_at(message, "counts");
    auto unread = counts.find("unread");
    if (unread != counts.end() && unread->is_number_integer()) {
        unread_count = unread->get<int>();
    }

    // Badge-only updates have no event. Showing a generic "New Message" card
    // for those is what produced a second, empty notification next to the
    // real one. Stay silent only while a fresh resumed heartbeat is on disk.
    // Recents SIGSTOP's the UI — it does not /sync — so a stale or missing
    // file means we still post.
    const std::string event_id = str_at(message, "event_id");
    const bool count_only = event_id.empty() && content_body.empty();
    const bool foreground = app_is_foreground();
    const bool show_card = popup && !count_only && !foreground;
    if (foreground) {
        std::cerr << "push: silent (foreground heartbeat fresh)" << std::endl;
    } else if (show_card) {
        std::cerr << "push: posting notification" << std::endl;
    }

    if (show_card && !room_id.empty()) {
        js2["notification"]["card"]["actions"] = {"fluffychat://" + room_id};
        // Tag by room so the app can dismiss a room's notifications once it is
        // read, via the postal service's ClearPersistentList.
        js2["notification"]["tag"] = room_id;
    }
    if (show_card) {
        js2["notification"]["card"]["summary"] = summary;
        js2["notification"]["card"]["body"] = body;
        js2["notification"]["card"]["icon"] = icon;
        js2["notification"]["card"]["persist"] = true;
        js2["notification"]["card"]["popup"] = true;
    }

    js2["notification"]["emblem-counter"]["count"] = unread_count;
    js2["notification"]["emblem-counter"]["visible"] = unread_count > 0;

    // Recents cards must chime even when Matrix omitted tweaks.sound. Postal
    // plays this on the alert slider, not media.
    js2["notification"]["sound"] = show_card;
    js2["notification"]["vibrate"] = show_card;
    f2 << js2;

    return 0;
}
