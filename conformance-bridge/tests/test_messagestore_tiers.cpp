// Host-side test for LXMF::MessageStore two-tier (hot + archive) behavior.
//
// Validates:
// 1. cull_conversation_to_hot moves files from hot → archive after the
//    HOT_MESSAGES_PER_CONVERSATION threshold, leaving the in-memory hash
//    list intact.
// 2. load_message falls back to the archive filesystem when a message
//    has been moved off the hot tier.
// 3. Hard-cap eviction at MAX_MESSAGES_PER_CONVERSATION drops the oldest
//    hash from BOTH tiers and from the in-memory list.
// 4. Without an archive filesystem set, cull deletes from hot (bounded
//    in-flash storage, no historical scrollback).
//
// No hardware required; uses POSIX-backed temp dirs as the two
// filesystems.

#include <LXMF/MessageStore.h>
#include <LXMF/LXMessage.h>
#include <Bytes.h>
#include <Utilities/OS.h>

#include <microStore/Adapters/PosixFileSystem.h>
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
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using LXMF::MessageStore;
using LXMF::LXMessage;
using LXMF::HOT_MESSAGES_PER_CONVERSATION;
using LXMF::MAX_MESSAGES_PER_CONVERSATION;
using RNS::Bytes;

// ---------- prefixing FileSystem ----------
//
// The microStore PosixFileSystem stores a basepath but never actually
// uses it — paths are passed straight through to ::open() etc. That's
// fine on ESP32 where LittleFS is mounted at root, but for host tests
// we need every path the MessageStore generates ("/m/<hash>.j") to
// land under a writable temp dir. Wrap PosixFileSystem and prefix.

namespace test_fs {

class PrefixedFileImpl : public microStore::FileImpl {
private:
    int _fd;
    bool _closed;
public:
    PrefixedFileImpl(int fd) : microStore::FileImpl(), _fd(fd), _closed(false) {}
    ~PrefixedFileImpl() override { if (!_closed) close(); }
    const char* name() const override { return ""; }
    size_t size() const override {
        struct stat st; ::fstat(_fd, &st); return st.st_size;
    }
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
    bool _fail_index_writes;
    std::string fp(const char* path) const { return _prefix + path; }
    void mkparents(const std::string& path) const {
        // Ensure parent directories exist so ::open(O_CREAT) succeeds.
        size_t p = 0;
        while ((p = path.find('/', p + 1)) != std::string::npos) {
            std::string dir = path.substr(0, p);
            ::mkdir(dir.c_str(), 0755);
        }
    }
public:
    PrefixedFSImpl(const std::string& prefix, bool fail_index_writes = false)
        : _prefix(prefix), _fail_index_writes(fail_index_writes) {
        ::mkdir(prefix.c_str(), 0755);
    }
    bool init(bool reformatOnFail = true) override {
        (void)reformatOnFail;
        return true;
    }
    bool format() override { return false; }
    microStore::File open(const char* path, microStore::File::Mode mode,
                          const bool create = false) override {
        if (_fail_index_writes && mode != microStore::File::ModeRead &&
            std::string(path).find("/conv.") == 0) {
            return {};
        }
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
    bool exists(const char* path) override {
        struct stat st; return ::stat(fp(path).c_str(), &st) == 0;
    }
    bool remove(const char* path) override { return ::unlink(fp(path).c_str()) == 0; }
    bool rename(const char* a, const char* b) override {
        return ::rename(fp(a).c_str(), fp(b).c_str()) == 0;
    }
    bool mkdir(const char* path) override {
        std::string full = fp(path);
        mkparents(full);
        return ::mkdir(full.c_str(), 0755) == 0 || errno == EEXIST;
    }
    bool rmdir(const char* path) override { return ::rmdir(fp(path).c_str()) == 0; }
    size_t size(const char* path) override {
        struct stat st; if (::stat(fp(path).c_str(), &st) != 0) return 0;
        return st.st_size;
    }
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
    PrefixedFS(const std::string& prefix, bool fail_index_writes = false)
        : microStore::FileSystem(new PrefixedFSImpl(prefix, fail_index_writes)) {}
};

}  // namespace test_fs

// ---------- test scaffolding ----------

static int g_failures = 0;

#define EXPECT_EQ(a, b, msg) \
    do { auto _a = (a); auto _b = (b); \
         if (!(_a == _b)) { \
             std::cerr << "FAIL " << msg << ": expected " << _b \
                       << ", got " << _a \
                       << " at " __FILE__ ":" << __LINE__ << "\n"; \
             ++g_failures; \
         } else { \
             std::cout << "  ok: " << msg << " = " << _a << "\n"; \
         } \
       } while (0)

#define EXPECT_TRUE(cond, msg) \
    do { if (!(cond)) { \
             std::cerr << "FAIL " << msg \
                       << " at " __FILE__ ":" << __LINE__ << "\n"; \
             ++g_failures; \
         } else { \
             std::cout << "  ok: " << msg << "\n"; \
         } \
       } while (0)

// ---------- synthetic LXMessage construction ----------
//
// MessageStore::save_message reads message.hash(), .destination_hash(),
// .source_hash(), .incoming(), .timestamp(), .state(), .content(),
// .packed(). We construct a minimal-valid set of LXMF wire bytes and
// pass them through LXMessage::unpack_from_bytes(skip_signature=true)
// — that populates the right fields and computes a deterministic
// _hash without needing any real Identity / signing key.
//
// LXMF wire format (per LXMessage::unpack_from_bytes):
//   16 bytes destination_hash
//   16 bytes source_hash
//   64 bytes signature (we fill with zeros; skip_signature=true ignores)
//   N  bytes msgpack payload: [timestamp, title, content, fields]

static Bytes make_msgpack_payload(double timestamp,
                                  const std::string& title,
                                  const std::string& content) {
    Bytes p;
    // arr_size 4
    p.append((uint8_t)0x94);
    // timestamp: float64 (msgpack 0xcb + big-endian 8 bytes)
    p.append((uint8_t)0xcb);
    union { double d; uint64_t u; } cv;
    cv.d = timestamp;
    for (int i = 7; i >= 0; --i) {
        p.append((uint8_t)((cv.u >> (i * 8)) & 0xff));
    }
    // title: bin8 (0xc4 + len + data)
    p.append((uint8_t)0xc4);
    p.append((uint8_t)title.size());
    p.append((const uint8_t*)title.data(), title.size());
    // content: bin8 if <256, else bin16
    if (content.size() < 256) {
        p.append((uint8_t)0xc4);
        p.append((uint8_t)content.size());
    } else {
        p.append((uint8_t)0xc5);
        p.append((uint8_t)((content.size() >> 8) & 0xff));
        p.append((uint8_t)(content.size() & 0xff));
    }
    p.append((const uint8_t*)content.data(), content.size());
    // fields: empty map
    p.append((uint8_t)0x80);
    return p;
}

static LXMessage make_test_message(const Bytes& dest_hash,
                                   const Bytes& src_hash,
                                   double timestamp,
                                   const std::string& content,
                                   bool incoming) {
    Bytes raw;
    raw.append(dest_hash.data(), 16);
    raw.append(src_hash.data(), 16);
    // 64 zero bytes for signature
    for (int i = 0; i < 64; ++i) raw.append((uint8_t)0);
    Bytes payload = make_msgpack_payload(timestamp, "t", content);
    raw.append(payload.data(), payload.size());

    LXMessage m = LXMessage::unpack_from_bytes(raw, LXMF::Type::Message::DIRECT, true);
    m.incoming(incoming);
    return m;
}

// ---------- helpers ----------

static int count_files_in_messages_dir(const std::string& base) {
    // Hot path layout from MessageStore::get_message_path is "/m/<12chars>.j"
    // Hot filesystem has basepath = ""; archive fs has basepath = ""
    // and uses prefix /lxmf-archive/m/. We just count files in either
    // /<basepath>/m or /<basepath>/lxmf-archive/m as the caller requests.
    std::string dir = base + "/m";
    DIR* d = opendir(dir.c_str());
    if (!d) return 0;
    int n = 0;
    struct dirent* ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        ++n;
    }
    closedir(d);
    return n;
}

static void rmrf(const std::string& path) {
    // Naive but sufficient for /tmp test dirs.
    std::string cmd = "rm -rf '" + path + "'";
    (void)system(cmd.c_str());
}

// ---------- tests ----------

static void test_messages_survive_store_reconstruction() {
    std::cout << "\n=== test_messages_survive_store_reconstruction ===\n";

    std::string hot_root = "/tmp/mstore_reboot_test";
    rmrf(hot_root);
    mkdir(hot_root.c_str(), 0755);

    test_fs::PrefixedFS hot_fs(hot_root);
    RNS::Utilities::OS::register_filesystem(hot_fs);

    Bytes dest_hash; for (int i = 0; i < 16; ++i) dest_hash.append((uint8_t)0xAA);
    Bytes src_hash;  for (int i = 0; i < 16; ++i) src_hash.append((uint8_t)0xEE);
    Bytes outgoing_dest; for (int i = 0; i < 16; ++i) outgoing_dest.append((uint8_t)0xDD);
    LXMessage incoming = make_test_message(dest_hash, src_hash,
                                           1700000000.0, "incoming-survives", true);
    LXMessage outgoing = make_test_message(outgoing_dest, dest_hash,
                                           1700000001.0, "outgoing-survives", false);

    {
        MessageStore before_reboot("/lxmf");
        EXPECT_TRUE(before_reboot.save_message(incoming),
                    "incoming message and index commit before reboot");
        EXPECT_TRUE(before_reboot.save_message(outgoing),
                    "outgoing message and index commit before reboot");
    }

    {
        MessageStore after_reboot("/lxmf");
        auto conversations = after_reboot.get_conversations();
        EXPECT_EQ((int)conversations.size(), 2,
                  "incoming and outgoing conversations restored");
        auto incoming_messages = after_reboot.get_messages_for_conversation(src_hash);
        auto outgoing_messages = after_reboot.get_messages_for_conversation(outgoing_dest);
        EXPECT_EQ((int)incoming_messages.size(), 1,
                  "incoming message hash restored");
        EXPECT_EQ((int)outgoing_messages.size(), 1,
                  "outgoing message hash restored");
        if (!incoming_messages.empty()) {
            LXMessage loaded = after_reboot.load_message(incoming_messages.front());
            EXPECT_EQ(loaded.hash().toHex(), incoming.hash().toHex(),
                      "incoming payload restored after store reconstruction");
        }
        if (!outgoing_messages.empty()) {
            LXMessage loaded = after_reboot.load_message(outgoing_messages.front());
            EXPECT_EQ(loaded.hash().toHex(), outgoing.hash().toHex(),
                      "outgoing payload restored after store reconstruction");
        }
    }

    RNS::Utilities::OS::deregister_filesystem();
    rmrf(hot_root);
}

static void test_index_write_failure_is_reported() {
    std::cout << "\n=== test_index_write_failure_is_reported ===\n";

    std::string hot_root = "/tmp/mstore_index_failure_test";
    rmrf(hot_root);
    mkdir(hot_root.c_str(), 0755);

    test_fs::PrefixedFS failing_fs(hot_root, true);
    RNS::Utilities::OS::register_filesystem(failing_fs);

    {
        MessageStore store("/lxmf");
        Bytes dest_hash; for (int i = 0; i < 16; ++i) dest_hash.append((uint8_t)0xAA);
        Bytes src_hash;  for (int i = 0; i < 16; ++i) src_hash.append((uint8_t)0xEF);
        LXMessage message = make_test_message(dest_hash, src_hash,
                                              1700000000.0, "must-not-lie", true);

        EXPECT_TRUE(!store.save_message(message),
                    "save_message fails when durable index commit fails");
        EXPECT_EQ((int)store.get_conversations().size(), 0,
                  "failed index commit rolls back in-memory conversation");
        EXPECT_EQ(count_files_in_messages_dir(hot_root), 0,
                  "failed index commit removes orphaned message payload");
    }

    RNS::Utilities::OS::deregister_filesystem();
    rmrf(hot_root);
}

static void test_interrupted_index_commit_recovers_previous_index() {
    std::cout << "\n=== test_interrupted_index_commit_recovers_previous_index ===\n";

    std::string hot_root = "/tmp/mstore_index_recovery_test";
    rmrf(hot_root);
    mkdir(hot_root.c_str(), 0755);

    test_fs::PrefixedFS hot_fs(hot_root);
    RNS::Utilities::OS::register_filesystem(hot_fs);

    Bytes dest_hash; for (int i = 0; i < 16; ++i) dest_hash.append((uint8_t)0xAA);
    Bytes src_hash;  for (int i = 0; i < 16; ++i) src_hash.append((uint8_t)0xF0);
    LXMessage message = make_test_message(dest_hash, src_hash,
                                          1700000000.0, "recover-index", true);

    {
        MessageStore store("/lxmf");
        EXPECT_TRUE(store.save_message(message), "seed index before interrupted commit");
    }

    std::string index_path = hot_root + "/conv.json";
    std::string backup_path = hot_root + "/conv.bak";
    EXPECT_TRUE(::rename(index_path.c_str(), backup_path.c_str()) == 0,
                "simulate power loss after index moved to backup");

    {
        MessageStore recovered("/lxmf");
        auto messages = recovered.get_messages_for_conversation(src_hash);
        EXPECT_EQ((int)messages.size(), 1,
                  "backup index restored during next initialization");
    }

    EXPECT_TRUE(::access(index_path.c_str(), F_OK) == 0,
                "committed index exists after recovery");
    EXPECT_TRUE(::access(backup_path.c_str(), F_OK) != 0,
                "stale backup removed after recovery");

    RNS::Utilities::OS::deregister_filesystem();
    rmrf(hot_root);
}

static void test_cull_to_hot_with_archive() {
    std::cout << "\n=== test_cull_to_hot_with_archive ===\n";

    std::string hot_root = "/tmp/mstore_hot_test";
    std::string archive_root = "/tmp/mstore_arc_test";
    rmrf(hot_root); rmrf(archive_root);
    mkdir(hot_root.c_str(), 0755);
    mkdir(archive_root.c_str(), 0755);

    // Hot FS — register globally so MessageStore + Utilities::OS use it.
    test_fs::PrefixedFS hot_fs(hot_root);
    RNS::Utilities::OS::register_filesystem(hot_fs);

    // Archive FS — passed to MessageStore via set_archive_filesystem.
    test_fs::PrefixedFS archive_fs(archive_root);

    MessageStore store("/lxmf");
    store.set_archive_filesystem(archive_fs, "/lxmf-archive");

    // Synthetic peer hashes — same source for all so messages land in
    // a single conversation.
    Bytes dest_hash; for (int i = 0; i < 16; ++i) dest_hash.append((uint8_t)0xAA);
    Bytes src_hash;  for (int i = 0; i < 16; ++i) src_hash.append((uint8_t)0xBB);

    // Save 75 messages — exceeds HOT_MESSAGES_PER_CONVERSATION (50) by 25.
    const int total = 75;
    for (int i = 0; i < total; ++i) {
        std::string content = "msg-" + std::to_string(i);
        LXMessage m = make_test_message(dest_hash, src_hash,
                                        1700000000.0 + i, content, true);
        bool ok = store.save_message(m);
        EXPECT_TRUE(ok, "save_message #" + std::to_string(i));
    }

    int hot_files = count_files_in_messages_dir(hot_root);
    int arc_files = count_files_in_messages_dir(archive_root + "/lxmf-archive");

    std::cout << "  hot files: " << hot_files << "\n";
    std::cout << "  archive files: " << arc_files << "\n";

    EXPECT_EQ(hot_files, (int)HOT_MESSAGES_PER_CONVERSATION,
              "hot bounded to HOT_MESSAGES_PER_CONVERSATION");
    EXPECT_EQ(arc_files, total - (int)HOT_MESSAGES_PER_CONVERSATION,
              "archive holds the overflow");

    // Load an archived (oldest) message — must succeed via fallback.
    auto conv_messages = store.get_messages_for_conversation(src_hash);
    EXPECT_EQ((int)conv_messages.size(), total,
              "in-memory hash list intact after cull");

    Bytes oldest_hash = conv_messages.front();
    LXMessage loaded = store.load_message(oldest_hash);
    EXPECT_EQ(loaded.hash().toHex(), oldest_hash.toHex(),
              "load_message hits archive fallback for oldest");

    // Newest is in hot — also loadable.
    Bytes newest_hash = conv_messages.back();
    LXMessage newest = store.load_message(newest_hash);
    EXPECT_EQ(newest.hash().toHex(), newest_hash.toHex(),
              "load_message reads newest from hot");

    RNS::Utilities::OS::deregister_filesystem();
    rmrf(hot_root); rmrf(archive_root);
}

static void test_cull_without_archive_deletes() {
    std::cout << "\n=== test_cull_without_archive_deletes ===\n";

    std::string hot_root = "/tmp/mstore_hot_only_test";
    rmrf(hot_root);
    mkdir(hot_root.c_str(), 0755);

    test_fs::PrefixedFS hot_fs2(hot_root);
    RNS::Utilities::OS::register_filesystem(hot_fs2);

    MessageStore store("/lxmf");
    // No set_archive_filesystem — cull should DELETE older messages.

    Bytes dest_hash; for (int i = 0; i < 16; ++i) dest_hash.append((uint8_t)0xAA);
    Bytes src_hash;  for (int i = 0; i < 16; ++i) src_hash.append((uint8_t)0xCC);

    const int total = 60;
    for (int i = 0; i < total; ++i) {
        LXMessage m = make_test_message(dest_hash, src_hash,
                                        1700000000.0 + i,
                                        "msg-" + std::to_string(i), true);
        store.save_message(m);
    }

    int hot_files = count_files_in_messages_dir(hot_root);
    EXPECT_EQ(hot_files, (int)HOT_MESSAGES_PER_CONVERSATION,
              "hot bounded by cull-delete with no archive");

    RNS::Utilities::OS::deregister_filesystem();
    rmrf(hot_root);
}

static void test_hard_cap_eviction() {
    std::cout << "\n=== test_hard_cap_eviction ===\n";

    std::string hot_root = "/tmp/mstore_hardcap_hot_test";
    std::string archive_root = "/tmp/mstore_hardcap_arc_test";
    rmrf(hot_root); rmrf(archive_root);
    mkdir(hot_root.c_str(), 0755);
    mkdir(archive_root.c_str(), 0755);

    test_fs::PrefixedFS hot_fs3(hot_root);
    test_fs::PrefixedFS archive_fs3(archive_root);
    RNS::Utilities::OS::register_filesystem(hot_fs3);

    MessageStore store("/lxmf");
    store.set_archive_filesystem(archive_fs3, "/lxmf-archive");

    Bytes dest_hash; for (int i = 0; i < 16; ++i) dest_hash.append((uint8_t)0xAA);
    Bytes src_hash;  for (int i = 0; i < 16; ++i) src_hash.append((uint8_t)0xDD);

    // Save MAX + 5 messages. The first 5 must be evicted entirely
    // (dropped from index, file gone from BOTH tiers).
    const int total = (int)MAX_MESSAGES_PER_CONVERSATION + 5;
    for (int i = 0; i < total; ++i) {
        LXMessage m = make_test_message(dest_hash, src_hash,
                                        1700000000.0 + i,
                                        "msg-" + std::to_string(i), true);
        store.save_message(m);
    }

    // After hard-cap eviction, the in-memory index holds at most
    // MAX_MESSAGES_PER_CONVERSATION hashes.
    auto conv_messages = store.get_messages_for_conversation(src_hash);
    EXPECT_EQ((int)conv_messages.size(),
              (int)MAX_MESSAGES_PER_CONVERSATION,
              "in-memory list capped at MAX after hard-cap eviction");

    // Hot should still be bounded at HOT_MESSAGES_PER_CONVERSATION.
    int hot_files = count_files_in_messages_dir(hot_root);
    EXPECT_EQ(hot_files, (int)HOT_MESSAGES_PER_CONVERSATION,
              "hot still bounded under hard-cap eviction");

    // Archive size = MAX - HOT = 256 - 50 = 206 (NOT total - HOT,
    // because hard-cap evicts from both tiers).
    int arc_files = count_files_in_messages_dir(archive_root + "/lxmf-archive");
    EXPECT_EQ(arc_files,
              (int)(MAX_MESSAGES_PER_CONVERSATION - HOT_MESSAGES_PER_CONVERSATION),
              "archive bounded at MAX-HOT after hard-cap eviction "
              "(evicted-oldest cleaned up too)");

    RNS::Utilities::OS::deregister_filesystem();
    rmrf(hot_root); rmrf(archive_root);
}

int main() {
    std::cout << "MessageStore tier tests\n";
    std::cout << "HOT_MESSAGES_PER_CONVERSATION = "
              << HOT_MESSAGES_PER_CONVERSATION << "\n";
    std::cout << "MAX_MESSAGES_PER_CONVERSATION = "
              << MAX_MESSAGES_PER_CONVERSATION << "\n";

    try {
        test_messages_survive_store_reconstruction();
        test_index_write_failure_is_reported();
        test_interrupted_index_commit_recovers_previous_index();
        test_cull_without_archive_deletes();
        test_cull_to_hot_with_archive();
        test_hard_cap_eviction();
    } catch (const std::exception& e) {
        std::cerr << "FATAL: uncaught exception: " << e.what() << "\n";
        return 2;
    }

    if (g_failures == 0) {
        std::cout << "\nAll tier tests passed.\n";
        return 0;
    }
    std::cerr << "\n" << g_failures << " failure(s).\n";
    return 1;
}
