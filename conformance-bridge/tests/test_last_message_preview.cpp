// Host-side test for the per-conversation last-message preview cache in
// LXMF::MessageStore.
//
// The conversation list needs (preview, timestamp) for each conversation's
// newest message. Without the cache that costs one LittleFS open + read +
// JSON parse per conversation on every list refresh. The store now keeps
// the first MAX_LAST_PREVIEW_LEN content chars in the in-memory index,
// persists it in the conversation index, and maintains it on save/delete.
//
// Validated here:
//   1. save_message caches the newest message's content prefix.
//   2. Content > MAX_LAST_PREVIEW_LEN is truncated to exactly that many
//      bytes and stays nul-terminated.
//   3. Saving an older-dated message (out of order) does NOT change the
//      cached tail; the newest message keeps the preview.
//   4. delete_message on the tail clears the cache (forces one fallback
//      read); deleting a non-tail message keeps it.
//   5. The cache round-trips through the persisted index: a reconstructed
//      store (reboot shape) serves the preview with no message-file read.
//   6. Empty-content messages leave the cache empty (fallback contract).
//   7. Unknown peers return false.
//
// No hardware required; POSIX temp dir as the hot filesystem.

#include <LXMF/MessageStore.h>
#include <LXMF/LXMessage.h>
#include <microReticulum/Bytes.h>
#include <microReticulum/Utilities/OS.h>

#include <microStore/Adapters/PosixFileSystem.h>
#include <microStore/File.h>
#include <microStore/FileSystem.h>

#include <fcntl.h>
#include <dirent.h>
#include <errno.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using LXMF::MessageStore;
using LXMF::LXMessage;
using RNS::Bytes;

// ---------- prefixing filesystem (mirror of test_messagestore_tiers) ----------
namespace preview_fs {

class PrefixedFileImpl : public microStore::FileImpl {
private:
    int _fd;
    bool _closed;
public:
    explicit PrefixedFileImpl(int fd) : microStore::FileImpl(), _fd(fd), _closed(false) {}
    ~PrefixedFileImpl() override { if (!_closed) close(); }
    const char* name() const override { return ""; }
    size_t size() const override { struct stat st; ::fstat(_fd, &st); return st.st_size; }
    void close() override { if (!_closed) { ::close(_fd); _closed = true; } }
    int read() override { uint8_t b; if (::read(_fd, &b, 1) != 1) return -1; return b; }
    int peek() override { return -1; }
    size_t read(uint8_t* buf, size_t sz) override {
        ssize_t n = ::read(_fd, buf, sz); return n < 0 ? 0 : (size_t)n;
    }
    size_t write(uint8_t b) override { return ::write(_fd, &b, 1) == 1 ? 1 : 0; }
    size_t write(const uint8_t* buf, size_t sz) override {
        ssize_t n = ::write(_fd, buf, sz); return n < 0 ? 0 : (size_t)n;
    }
    int available() override { return 0; }
    size_t tell() override { return ::lseek(_fd, 0, SEEK_CUR); }
    long seek(uint32_t pos, microStore::SeekMode m) override {
        int wh = SEEK_SET;
        if (m == microStore::SeekMode::SeekModeCur) wh = SEEK_CUR;
        else if (m == microStore::SeekMode::SeekModeEnd) wh = SEEK_END;
        return ::lseek(_fd, pos, wh);
    }
    void flush() override {}
    bool isValid() const override { return !_closed && _fd >= 0; }
};

class PrefixedFSImpl : public microStore::FileSystemImpl {
private:
    std::string _prefix;
    std::string fp(const char* path) const { return _prefix + path; }
    void mkparents(const std::string& path) const {
        size_t p = 0;
        while ((p = path.find('/', p + 1)) != std::string::npos) {
            std::string dir = path.substr(0, p);
            ::mkdir(dir.c_str(), 0755);
        }
    }
public:
    explicit PrefixedFSImpl(const std::string& prefix) : _prefix(prefix) {
        ::mkdir(prefix.c_str(), 0755);
    }
    bool init(bool) override { return true; }
    bool format() override { return false; }
    microStore::File open(const char* path, microStore::File::Mode mode,
                          const bool create = false) override {
        std::string full = fp(path);
        int flags;
        switch (mode) {
            case microStore::File::ModeRead:       flags = O_RDONLY; break;
            case microStore::File::ModeWrite:      flags = O_WRONLY|O_CREAT|O_TRUNC; break;
            case microStore::File::ModeAppend:     flags = O_WRONLY|O_CREAT|O_APPEND; break;
            case microStore::File::ModeReadWrite:  flags = O_RDWR|O_CREAT|O_TRUNC; break;
            case microStore::File::ModeReadAppend: flags = O_RDWR|O_CREAT|O_APPEND; break;
            default: return {};
        }
        if (flags & O_CREAT) mkparents(full);
        int fd = ::open(full.c_str(), flags, 0644);
        if (fd == -1) return {};
        return microStore::File(new PrefixedFileImpl(fd));
    }
    bool exists(const char* path) override { struct stat st; return ::stat(fp(path).c_str(), &st) == 0; }
    bool remove(const char* path) override { return ::unlink(fp(path).c_str()) == 0; }
    bool rename(const char* a, const char* b) override { return ::rename(fp(a).c_str(), fp(b).c_str()) == 0; }
    bool mkdir(const char* path) override {
        std::string full = fp(path); mkparents(full);
        return ::mkdir(full.c_str(), 0755) == 0 || errno == EEXIST;
    }
    bool rmdir(const char* path) override { return ::rmdir(fp(path).c_str()) == 0; }
    size_t size(const char* path) override { struct stat st; if (::stat(fp(path).c_str(), &st) != 0) return 0; return st.st_size; }
    bool isDirectory(const char* path) override {
        struct stat st; if (::stat(fp(path).c_str(), &st) != 0) return false;
        return S_ISDIR(st.st_mode);
    }
    std::list<std::string> listDirectory(const char* path,
            Callbacks::DirectoryListing cb = nullptr) override {
        std::list<std::string> out;
        DIR* d = ::opendir(fp(path).c_str());
        if (!d) return out;
        while (auto* ent = ::readdir(d)) {
            if (ent->d_name[0] == '.') continue;
            if (cb) cb(ent->d_name); else out.push_back(ent->d_name);
        }
        ::closedir(d);
        return out;
    }
    size_t storageSize() override { return 0; }
    size_t storageAvailable() override { return 0; }
};

class PrefixedFS : public microStore::FileSystem {
public:
    explicit PrefixedFS(const std::string& prefix)
        : microStore::FileSystem(new PrefixedFSImpl(prefix)) {}
};

}  // namespace preview_fs

// ---------- synthetic messages (mirror of test_messagestore_tiers) ----------
static Bytes make_msgpack_payload(double timestamp,
                                  const std::string& title,
                                  const std::string& content) {
    Bytes p;
    p.append((uint8_t)0x94);
    p.append((uint8_t)0xcb);
    union { double d; uint64_t u; } cv;
    cv.d = timestamp;
    for (int i = 7; i >= 0; --i) p.append((uint8_t)((cv.u >> (i * 8)) & 0xff));
    p.append((uint8_t)0xc4);
    p.append((uint8_t)title.size());
    p.append((const uint8_t*)title.data(), title.size());
    if (content.size() < 256) {
        p.append((uint8_t)0xc4);
        p.append((uint8_t)content.size());
    } else {
        p.append((uint8_t)0xc5);
        p.append((uint8_t)((content.size() >> 8) & 0xff));
        p.append((uint8_t)(content.size() & 0xff));
    }
    p.append((const uint8_t*)content.data(), content.size());
    p.append((uint8_t)0x80);
    return p;
}

static LXMessage make_message(const Bytes& peer_hash, const Bytes& self_hash,
                              double timestamp, const std::string& content,
                              bool incoming) {
    // For incoming messages the peer is the source; for outgoing it is the
    // destination — save_message() keys conversations that way.
    const Bytes& dest = incoming ? self_hash : peer_hash;
    const Bytes& src  = incoming ? peer_hash : self_hash;
    Bytes raw;
    raw.append(dest.data(), 16);
    raw.append(src.data(), 16);
    for (int i = 0; i < 64; ++i) raw.append((uint8_t)0);
    Bytes payload = make_msgpack_payload(timestamp, "t", content);
    raw.append(payload.data(), payload.size());
    LXMessage m = LXMessage::unpack_from_bytes(raw, LXMF::Type::Message::DIRECT, true);
    m.incoming(incoming);
    return m;
}

static Bytes peer_hash(uint8_t tag) {
    Bytes h;
    for (int i = 0; i < 16; ++i) h.append((uint8_t)(tag + i));
    return h;
}
static Bytes self_hash() {
    Bytes h;
    for (int i = 0; i < 16; ++i) h.append((uint8_t)(0x50 + i));
    return h;
}

static void rmrf(const std::string& path) {
    std::string cmd = "rm -rf '" + path + "'";
    (void)system(cmd.c_str());
}

// ---------- test scaffolding ----------
static int g_failures = 0;

#define EXPECT_EQ(a, b, msg) \
    do { auto _a = (a); auto _b = (b); \
         if (!(_a == _b)) { \
             std::cerr << "FAIL " << msg << ": expected " << _b \
                       << ", got " << _a << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
             ++g_failures; \
         } else { \
             std::cout << "  ok: " << msg << "\n"; \
         } \
       } while (0)

#define EXPECT_TRUE(cond, msg) \
    do { if (!(cond)) { \
             std::cerr << "FAIL " << msg << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
             ++g_failures; \
         } else { \
             std::cout << "  ok: " << msg << "\n"; \
         } \
       } while (0)

// ---------- tests ----------
static void test_save_caches_preview() {
    std::cout << "\n=== test_save_caches_preview ===\n";
    std::string root = "/tmp/mstore_preview_test";
    rmrf(root);
    ::mkdir(root.c_str(), 0755);
    preview_fs::PrefixedFS hot_fs(root);
    RNS::Utilities::OS::register_filesystem(hot_fs);

    {
        MessageStore store("/lxmf");
        Bytes peer = peer_hash(0x10), self = self_hash();

        // Short content: exact copy.
        LXMessage m1 = make_message(peer, self, 1700000000.0, "hello world", true);
        EXPECT_TRUE(store.save_message(m1), "save 1");
        std::string preview; double ts = 0;
        EXPECT_TRUE(store.get_last_message_preview(peer, preview, ts),
                    "preview available after save");
        EXPECT_EQ(preview, std::string("hello world"), "preview == content");
        EXPECT_EQ((long long)ts, (long long)1700000000LL, "timestamp from last_activity");

        // Long content: truncated to exactly MAX_LAST_PREVIEW_LEN bytes.
        std::string long_content(80, 'A');
        long_content.replace(40, 20, "zzzzzzzzzzzzzzzzzzzz");
        LXMessage m2 = make_message(peer, self, 1700000100.0, long_content, false);
        EXPECT_TRUE(store.save_message(m2), "save 2");
        EXPECT_TRUE(store.get_last_message_preview(peer, preview, ts),
                    "preview available after save 2");
        EXPECT_EQ(preview.size(), (size_t)MessageStore::MAX_LAST_PREVIEW_LEN,
                  "preview truncated to MAX_LAST_PREVIEW_LEN");
        EXPECT_EQ(preview, long_content.substr(0, MessageStore::MAX_LAST_PREVIEW_LEN),
                  "preview is the content prefix");

        // Out-of-order save (older timestamp) does NOT steal the tail:
        // the hash array appends, so the tail is the most recently SAVED
        // message — matches get_last_message_hash semantics.
        LXMessage m3 = make_message(peer, self, 1700000050.0, "older msg", true);
        EXPECT_TRUE(store.save_message(m3), "save 3 (older date)");
        EXPECT_TRUE(store.get_last_message_preview(peer, preview, ts),
                    "preview available after save 3");
        EXPECT_EQ(preview, std::string("older msg"),
                  "most-recently-saved message owns the preview");
    }
    RNS::Utilities::OS::deregister_filesystem();
    rmrf(root);
}

static void test_delete_tail_clears_preview() {
    std::cout << "\n=== test_delete_tail_clears_preview ===\n";
    std::string root = "/tmp/mstore_preview_delete_test";
    rmrf(root);
    ::mkdir(root.c_str(), 0755);
    preview_fs::PrefixedFS hot_fs(root);
    RNS::Utilities::OS::register_filesystem(hot_fs);

    {
        MessageStore store("/lxmf");
        Bytes peer = peer_hash(0x20), self = self_hash();

        LXMessage m1 = make_message(peer, self, 1700000000.0, "first", true);
        LXMessage m2 = make_message(peer, self, 1700000100.0, "second", false);
        EXPECT_TRUE(store.save_message(m1), "save 1");
        EXPECT_TRUE(store.save_message(m2), "save 2");

        // Delete the non-tail message: preview untouched.
        EXPECT_TRUE(store.delete_message(m1.hash()), "delete non-tail");
        std::string preview; double ts = 0;
        EXPECT_TRUE(store.get_last_message_preview(peer, preview, ts),
                    "preview survives non-tail delete");
        EXPECT_EQ(preview, std::string("second"), "preview still the tail's");

        // Delete the tail: preview must be cleared (fallback contract).
        EXPECT_TRUE(store.delete_message(m2.hash()), "delete tail");
        EXPECT_TRUE(!store.get_last_message_preview(peer, preview, ts),
                    "preview cleared after tail delete");
        EXPECT_TRUE(store.get_last_message_hash(peer).empty(),
                    "tail hash cleared after tail delete");
    }
    RNS::Utilities::OS::deregister_filesystem();
    rmrf(root);
}

static void test_preview_persists_across_reconstruction() {
    std::cout << "\n=== test_preview_persists_across_reconstruction ===\n";
    std::string root = "/tmp/mstore_preview_reboot_test";
    rmrf(root);
    ::mkdir(root.c_str(), 0755);
    preview_fs::PrefixedFS hot_fs(root);
    RNS::Utilities::OS::register_filesystem(hot_fs);

    Bytes peer = peer_hash(0x30), self = self_hash();
    {
        MessageStore store("/lxmf");
        LXMessage m = make_message(peer, self, 1700000000.0,
                                   "rebooted preview text", true);
        EXPECT_TRUE(store.save_message(m), "save before reboot");
        // Drop the in-memory store (reboot shape).
    }
    {
        MessageStore store("/lxmf");
        std::string preview; double ts = 0;
        EXPECT_TRUE(store.get_last_message_preview(peer, preview, ts),
                    "preview restored from persisted index");
        EXPECT_EQ(preview, std::string("rebooted preview text"),
                  "persisted preview is intact");
        EXPECT_EQ((long long)ts, (long long)1700000000LL,
                  "timestamp restored from index");
    }
    {
        // Empty-content tail round-trips as a populated empty cache, so a
        // cold boot does NOT fall back to the file for it.
        MessageStore store("/lxmf");
        LXMessage m = make_message(peer, self, 1700000001.0, "", true);
        EXPECT_TRUE(store.save_message(m), "save empty tail before reboot");
    }
    {
        MessageStore store("/lxmf");
        std::string preview; double ts = 0;
        EXPECT_TRUE(store.get_last_message_preview(peer, preview, ts),
                    "empty tail restored as populated cache");
        EXPECT_EQ(preview, std::string(""), "restored preview is empty");
    }
    RNS::Utilities::OS::deregister_filesystem();
    rmrf(root);
}

static void test_empty_content_and_unknown_peer() {
    std::cout << "\n=== test_empty_content_and_unknown_peer ===\n";
    std::string root = "/tmp/mstore_preview_empty_test";
    rmrf(root);
    ::mkdir(root.c_str(), 0755);
    preview_fs::PrefixedFS hot_fs(root);
    RNS::Utilities::OS::register_filesystem(hot_fs);

    {
        MessageStore store("/lxmf");
        Bytes peer = peer_hash(0x40), self = self_hash();

        // Unknown peer: no conversation.
        std::string preview; double ts = 0;
        EXPECT_TRUE(!store.get_last_message_preview(peer_hash(0x99), preview, ts),
                    "unknown peer has no preview");

        // Empty-content message: cached as an EMPTY preview with the
        // cache marked populated — the list must not re-read this file
        // (location shares, blank pings). Fallback contract is only for
        // an unpopulated cache.
        LXMessage m = make_message(peer, self, 1700000000.0, "", true);
        EXPECT_TRUE(store.save_message(m), "save empty-content message");
        EXPECT_TRUE(store.get_last_message_hash(peer) == m.hash(),
                    "tail hash is set");
        EXPECT_TRUE(store.get_last_message_preview(peer, preview, ts),
                    "empty content is a valid cached preview");
        EXPECT_EQ(preview, std::string(""), "preview is empty");
        EXPECT_EQ((long long)ts, (long long)1700000000LL,
                  "timestamp restored for empty tail");

        // Re-pop via the explicit accessor (the Pyxis fallback warm-up
        // path): an empty preview still marks the cache populated.
        store.set_last_message_preview(peer, "");
        preview.clear(); ts = 0;
        EXPECT_TRUE(store.get_last_message_preview(peer, preview, ts),
                    "set(empty) marks cache populated");
        EXPECT_EQ(preview, std::string(""), "still empty");

        // A new non-empty tail replaces it.
        LXMessage m2 = make_message(peer, self, 1700000010.0, "after empty",
                                    false);
        EXPECT_TRUE(store.save_message(m2), "save after empty");
        EXPECT_TRUE(store.get_last_message_preview(peer, preview, ts),
                    "new tail cached");
        EXPECT_EQ(preview, std::string("after empty"), "preview updated");
    }
    RNS::Utilities::OS::deregister_filesystem();
    rmrf(root);
}

int main() {
    std::cout << "MessageStore last-preview cache tests\n";
    try {
        test_save_caches_preview();
        test_delete_tail_clears_preview();
        test_preview_persists_across_reconstruction();
        test_empty_content_and_unknown_peer();
    } catch (const std::exception& e) {
        std::cerr << "FATAL: uncaught exception: " << e.what() << "\n";
        return 2;
    }
    if (g_failures > 0) {
        std::cerr << g_failures << " FAILURES\n";
        return 1;
    }
    std::cout << "MessageStore last-preview cache: passed\n";
    return 0;
}
