// Conformance test: MessageStore message-metadata cache
//
// The chat screen reads load_message_metadata() for every displayed
// message; on SPI flash each disk read costs ~hundreds of ms. The
// store now caches metadata in a bounded in-memory table so re-reads
// (conversation reopen, background page fill, pagination) are O(1).
//
// Behavior under test:
//   1. First read loads from disk (valid, correct fields).
//   2. Second read returns identical fields (cache hit, same values).
//   3. Cache is keyed by hash: interleaved reads of different messages
//      all return their own content/timestamp/incoming.
//   4. update_message_state() keeps the cache in sync: a re-read after
//      the state write reports the new state (no stale PENDING).
//   5. delete_message() evicts: a re-read after deletion reports
//      invalid (no phantom cached entry).
//   6. Content truncation: a message longer than the display cap
//      returns full content on first read, then the capped prefix on
//      cached reads (the chat UI renders at most that many chars).
//
// No hardware required; POSIX temp dir as the hot filesystem.

#include <LXMF/MessageStore.h>
#include <LXMF/LXMessage.h>
#include <microReticulum/Bytes.h>
#include <microReticulum/Utilities/OS.h>

#include <microStore/File.h>
#include <microStore/FileSystem.h>

#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <list>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using LXMF::MessageStore;
using LXMF::LXMessage;
using RNS::Bytes;

static int g_failures = 0;
#define EXPECT_TRUE(cond, msg)                                              \
    do {                                                                    \
        if (cond) {                                                         \
            std::cout << "  ok: " << msg << "\n";                           \
        } else {                                                            \
            std::cout << "FAIL " << msg << " at " << __FILE__ << ":"        \
                      << __LINE__ << "\n";                                  \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

// ---------- prefixing filesystem (mirror of test_messagestore_tiers) ----------
namespace meta_fs {

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
    void mkparents(const std::string& full) const {
        size_t p = 0;
        while ((p = full.find('/', p + 1)) != std::string::npos) {
            std::string dir = full.substr(0, p);
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

}  // namespace meta_fs

// ---------- synthetic messages (mirror of test_last_message_preview) ----------
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

static Bytes peer_hash(uint32_t v) {
    Bytes b;
    for (int i = 0; i < 16; ++i) b.append((uint8_t)(v >> (i * 8)));
    return b;
}

static Bytes self_hash() { return peer_hash(0x5E5E5E5E); }

static void rmrf(const std::string& path) { ::system(("rm -rf '" + path + "'").c_str()); }

// ---------- tests ----------

static void test_first_read_and_cache_hit() {
    std::cout << "\n=== test_first_read_and_cache_hit ===\n";
    std::string root = "/tmp/mstore_meta_cache_test1";
    rmrf(root);
    ::mkdir(root.c_str(), 0755);
    meta_fs::PrefixedFS hot_fs(root);
    RNS::Utilities::OS::register_filesystem(hot_fs);

    Bytes peer = peer_hash(0x11), self = self_hash();
    {
        MessageStore store("/lxmf");
        LXMessage m = make_message(peer, self, 1700000000.0, "hello cache", true);
        EXPECT_TRUE(store.save_message(m), "save message");

        // First read: from disk. (State reflects the saved message.state();
        // the cache contract is that a re-read returns the SAME value, not a
        // specific one.)
        MessageStore::MessageMetadata meta = store.load_message_metadata(m.hash());
        EXPECT_TRUE(meta.valid, "first read valid");
        EXPECT_TRUE(meta.content == "hello cache", "first read content");
        EXPECT_TRUE(meta.incoming, "first read incoming");

        // Second read: cache hit, identical fields.
        MessageStore::MessageMetadata meta2 = store.load_message_metadata(m.hash());
        EXPECT_TRUE(meta2.valid, "cached read valid");
        EXPECT_TRUE(meta2.content == "hello cache", "cached read content");
        EXPECT_TRUE(meta2.incoming, "cached read incoming");
        EXPECT_TRUE((long long)meta2.timestamp == 1700000000LL, "cached timestamp");
        EXPECT_TRUE(meta2.state == meta.state, "cached state matches first read");
    }
    RNS::Utilities::OS::deregister_filesystem();
    rmrf(root);
}

static void test_per_hash_keying() {
    std::cout << "\n=== test_per_hash_keying ===\n";
    std::string root = "/tmp/mstore_meta_cache_test2";
    rmrf(root);
    ::mkdir(root.c_str(), 0755);
    meta_fs::PrefixedFS hot_fs(root);
    RNS::Utilities::OS::register_filesystem(hot_fs);

    Bytes peer = peer_hash(0x22), self = self_hash();
    {
        MessageStore store("/lxmf");
        LXMessage m1 = make_message(peer, self, 1700000001.0, "first", true);
        LXMessage m2 = make_message(peer, self, 1700000002.0, "second", false);
        LXMessage m3 = make_message(peer, self, 1700000003.0, "third", true);
        EXPECT_TRUE(store.save_message(m1), "save 1");
        EXPECT_TRUE(store.save_message(m2), "save 2");
        EXPECT_TRUE(store.save_message(m3), "save 3");

        // Warm each.
        for (int i = 0; i < 3; ++i) {
            store.load_message_metadata(i == 0 ? m1.hash()
                                  : i == 1 ? m2.hash() : m3.hash());
        }
        // Interleaved re-reads must each return their own data.
        MessageStore::MessageMetadata a = store.load_message_metadata(m1.hash());
        MessageStore::MessageMetadata b = store.load_message_metadata(m2.hash());
        MessageStore::MessageMetadata c = store.load_message_metadata(m3.hash());
        EXPECT_TRUE(a.content == "first" && a.incoming, "m1 own data");
        EXPECT_TRUE(b.content == "second" && !b.incoming, "m2 own data");
        EXPECT_TRUE(c.content == "third" && c.incoming, "m3 own data");
    }
    RNS::Utilities::OS::deregister_filesystem();
    rmrf(root);
}

static void test_state_update_keeps_cache_synced() {
    std::cout << "\n=== test_state_update_keeps_cache_synced ===\n";
    std::string root = "/tmp/mstore_meta_cache_test3";
    rmrf(root);
    ::mkdir(root.c_str(), 0755);
    meta_fs::PrefixedFS hot_fs(root);
    RNS::Utilities::OS::register_filesystem(hot_fs);

    Bytes peer = peer_hash(0x33), self = self_hash();
    {
        MessageStore store("/lxmf");
        LXMessage m = make_message(peer, self, 1700000000.0, "ping", false);
        EXPECT_TRUE(store.save_message(m), "save outgoing");

        MessageStore::MessageMetadata meta = store.load_message_metadata(m.hash());
        EXPECT_TRUE(meta.valid, "initial read valid");

        // Delivery confirmed: persist state, then re-read must be DELIVERED
        // (not the stale PENDING from the cache).
        EXPECT_TRUE(store.update_message_state(m.hash(),
                    LXMF::Type::Message::DELIVERED), "state update");
        MessageStore::MessageMetadata meta2 = store.load_message_metadata(m.hash());
        EXPECT_TRUE(meta2.valid, "re-read valid");
        EXPECT_TRUE(meta2.state == static_cast<int>(LXMF::Type::Message::DELIVERED),
                    "cached state is DELIVERED after update");
        EXPECT_TRUE(meta2.content == "ping", "content intact after state update");
    }
    RNS::Utilities::OS::deregister_filesystem();
    rmrf(root);
}

static void test_delete_evicts_cache() {
    std::cout << "\n=== test_delete_evicts_cache ===\n";
    std::string root = "/tmp/mstore_meta_cache_test4";
    rmrf(root);
    ::mkdir(root.c_str(), 0755);
    meta_fs::PrefixedFS hot_fs(root);
    RNS::Utilities::OS::register_filesystem(hot_fs);

    Bytes peer = peer_hash(0x44), self = self_hash();
    {
        MessageStore store("/lxmf");
        LXMessage m1 = make_message(peer, self, 1700000001.0, "one", true);
        LXMessage m2 = make_message(peer, self, 1700000002.0, "two", true);
        EXPECT_TRUE(store.save_message(m1), "save 1");
        EXPECT_TRUE(store.save_message(m2), "save 2");
        store.load_message_metadata(m1.hash());  // warm
        store.load_message_metadata(m2.hash());  // warm

        EXPECT_TRUE(store.delete_message(m2.hash()), "delete m2");
        MessageStore::MessageMetadata gone = store.load_message_metadata(m2.hash());
        EXPECT_TRUE(!gone.valid, "deleted message is not served from cache");
        // Sibling entry untouched.
        MessageStore::MessageMetadata kept = store.load_message_metadata(m1.hash());
        EXPECT_TRUE(kept.valid && kept.content == "one", "sibling entry intact");
    }
    RNS::Utilities::OS::deregister_filesystem();
    rmrf(root);
}

static void test_long_content_truncation_contract() {
    std::cout << "\n=== test_long_content_truncation_contract ===\n";
    std::string root = "/tmp/mstore_meta_cache_test5";
    rmrf(root);
    ::mkdir(root.c_str(), 0755);
    meta_fs::PrefixedFS hot_fs(root);
    RNS::Utilities::OS::register_filesystem(hot_fs);

    Bytes peer = peer_hash(0x55), self = self_hash();
    const std::string long_content(1500, 'x');
    {
        MessageStore store("/lxmf");
        LXMessage m = make_message(peer, self, 1700000000.0, long_content, true);
        EXPECT_TRUE(store.save_message(m), "save long");

        MessageStore::MessageMetadata first = store.load_message_metadata(m.hash());
        EXPECT_TRUE(first.valid, "first read valid");
        EXPECT_TRUE(first.content.size() == long_content.size(),
                    "first read has FULL content");

        // Cached reads are capped at the display cap (the chat UI renders
        // at most that many chars per bubble; full content is only needed
        // by the explicit full-message view, which is a rare action).
        MessageStore::MessageMetadata second = store.load_message_metadata(m.hash());
        EXPECT_TRUE(second.valid, "cached read valid");
        const size_t cap = MessageStore::MESSAGE_METADATA_MAX_CONTENT;
        EXPECT_TRUE(second.content.size() == cap, "cached content capped");
        EXPECT_TRUE(second.content == long_content.substr(0, cap),
                    "cached content is the capped prefix");

        // load_message_content() is the rare full-view path: it bypasses
        // the (capped) metadata cache and returns the FULL stored content
        // even after the metadata read has warmed the cache.
        std::string full = store.load_message_content(m.hash());
        EXPECT_TRUE(full.size() == long_content.size(),
                    "full view returns uncapped content after cache warm");
        EXPECT_TRUE(full == long_content, "full view content is exact");

        // Unknown hash: empty string, no crash.
        std::string missing = store.load_message_content(peer_hash(0x99));
        EXPECT_TRUE(missing.empty(), "unknown hash returns empty");
    }
    RNS::Utilities::OS::deregister_filesystem();
    rmrf(root);
}

static void test_persistence_across_reconstruction() {
    std::cout << "\n=== test_persistence_across_reconstruction ===\n";
    // Reboot shape: the process-lifetime cache is empty, so a fresh store
    // must load from disk correctly (cache must not hide a disk miss on a
    // cold process — and must not poison a later warm process).
    std::string root = "/tmp/mstore_meta_cache_test6";
    rmrf(root);
    ::mkdir(root.c_str(), 0755);
    meta_fs::PrefixedFS hot_fs(root);
    RNS::Utilities::OS::register_filesystem(hot_fs);

    Bytes peer = peer_hash(0x66), self = self_hash();
    {
        MessageStore store("/lxmf");
        LXMessage m = make_message(peer, self, 1700000000.0, "reboot text", true);
        EXPECT_TRUE(store.save_message(m), "save before reboot");
        EXPECT_TRUE(store.load_message_metadata(m.hash()).valid, "warm read");
        // "Reboot".
    }
    {
        MessageStore store("/lxmf");
        MessageStore::MessageMetadata meta = store.load_message_metadata(
            make_message(peer, self, 1700000000.0, "reboot text", true).hash());
        EXPECT_TRUE(meta.valid, "cold read valid from disk");
        EXPECT_TRUE(meta.content == "reboot text", "cold read content");
    }
    RNS::Utilities::OS::deregister_filesystem();
    rmrf(root);
}

int main() {
    std::cout << "MessageStore metadata cache tests\n";
    try {
        test_first_read_and_cache_hit();
        test_per_hash_keying();
        test_state_update_keeps_cache_synced();
        test_delete_evicts_cache();
        test_long_content_truncation_contract();
        test_persistence_across_reconstruction();
    } catch (const std::exception& e) {
        std::cerr << "FATAL: uncaught exception: " << e.what() << "\n";
        return 2;
    }
    if (g_failures > 0) {
        std::cerr << g_failures << " FAILURES\n";
        return 1;
    }
    std::cout << "MessageStore metadata cache: passed\n";
    return 0;
}
