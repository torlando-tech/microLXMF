#pragma once

#include <microStore/File.h>
#include <microStore/FileSystem.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <list>
#include <string>
#include <utility>

namespace bridge {

class PrefixedFileImpl : public microStore::FileImpl {
public:
    explicit PrefixedFileImpl(int fd) : _fd(fd) {}
    ~PrefixedFileImpl() override { close(); }

    const char* name() const override { return ""; }
    size_t size() const override {
        struct stat st {};
        return ::fstat(_fd, &st) == 0 ? static_cast<size_t>(st.st_size) : 0;
    }
    void close() override {
        if (!_closed && _fd >= 0) ::close(_fd);
        _closed = true;
    }
    int read() override {
        uint8_t value = 0;
        return ::read(_fd, &value, 1) == 1 ? value : -1;
    }
    int peek() override { return -1; }
    size_t read(uint8_t* buffer, size_t size) override {
        const ssize_t count = ::read(_fd, buffer, size);
        return count < 0 ? 0 : static_cast<size_t>(count);
    }
    size_t write(uint8_t value) override {
        return ::write(_fd, &value, 1) == 1 ? 1 : 0;
    }
    size_t write(const uint8_t* buffer, size_t size) override {
        const ssize_t count = ::write(_fd, buffer, size);
        return count < 0 ? 0 : static_cast<size_t>(count);
    }
    int available() override { return 0; }
    size_t tell() override { return static_cast<size_t>(::lseek(_fd, 0, SEEK_CUR)); }
    long seek(uint32_t position, microStore::SeekMode mode) override {
        int origin = SEEK_SET;
        if (mode == microStore::SeekMode::SeekModeCur) origin = SEEK_CUR;
        if (mode == microStore::SeekMode::SeekModeEnd) origin = SEEK_END;
        return ::lseek(_fd, position, origin);
    }
    void flush() override {}
    bool isValid() const override { return !_closed && _fd >= 0; }

private:
    int _fd = -1;
    bool _closed = false;
};

class PrefixedFileSystemImpl : public microStore::FileSystemImpl {
public:
    explicit PrefixedFileSystemImpl(std::string prefix) : _prefix(std::move(prefix)) {
        ::mkdir(_prefix.c_str(), 0700);
    }

    bool init(bool reformatOnFail = true) override {
        (void)reformatOnFail;
        return true;
    }
    bool format() override { return false; }
    microStore::File open(const char* path, microStore::File::Mode mode,
                          bool create = false) override {
        (void)create;
        const std::string full = full_path(path);
        int flags = 0;
        switch (mode) {
            case microStore::File::ModeRead: flags = O_RDONLY; break;
            case microStore::File::ModeWrite: flags = O_WRONLY | O_CREAT | O_TRUNC; break;
            case microStore::File::ModeAppend: flags = O_WRONLY | O_CREAT | O_APPEND; break;
            case microStore::File::ModeReadWrite: flags = O_RDWR | O_CREAT | O_TRUNC; break;
            case microStore::File::ModeReadAppend: flags = O_RDWR | O_CREAT | O_APPEND; break;
            default: return {};
        }
        if (flags & O_CREAT) create_parents(full);
        const int fd = ::open(full.c_str(), flags, 0600);
        return fd < 0 ? microStore::File{} : microStore::File(new PrefixedFileImpl(fd));
    }
    bool exists(const char* path) override {
        struct stat st {};
        return ::stat(full_path(path).c_str(), &st) == 0;
    }
    bool remove(const char* path) override { return ::unlink(full_path(path).c_str()) == 0; }
    bool rename(const char* from, const char* to) override {
        return ::rename(full_path(from).c_str(), full_path(to).c_str()) == 0;
    }
    bool mkdir(const char* path) override {
        const std::string full = full_path(path);
        create_parents(full);
        return ::mkdir(full.c_str(), 0700) == 0 || errno == EEXIST;
    }
    bool rmdir(const char* path) override { return ::rmdir(full_path(path).c_str()) == 0; }
    size_t size(const char* path) override {
        struct stat st {};
        return ::stat(full_path(path).c_str(), &st) == 0 ? static_cast<size_t>(st.st_size) : 0;
    }
    bool isDirectory(const char* path) override {
        struct stat st {};
        return ::stat(full_path(path).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    }
    std::list<std::string> listDirectory(
        const char* path, Callbacks::DirectoryListing callback = nullptr) override {
        std::list<std::string> entries;
        DIR* directory = ::opendir(full_path(path).c_str());
        if (!directory) return entries;
        while (dirent* entry = ::readdir(directory)) {
            if (entry->d_name[0] == '.') continue;
            if (callback) callback(entry->d_name);
            else entries.emplace_back(entry->d_name);
        }
        ::closedir(directory);
        return entries;
    }
    size_t storageSize() override { return 0; }
    size_t storageAvailable() override { return 0; }

private:
    std::string full_path(const char* path) const {
        if (!path || !*path) return _prefix;
        return path[0] == '/' ? _prefix + path : _prefix + "/" + path;
    }
    static void create_parents(const std::string& path) {
        size_t slash = 0;
        while ((slash = path.find('/', slash + 1)) != std::string::npos) {
            ::mkdir(path.substr(0, slash).c_str(), 0700);
        }
    }

    std::string _prefix;
};

class PrefixedFileSystem : public microStore::FileSystem {
public:
    explicit PrefixedFileSystem(const std::string& prefix)
        : microStore::FileSystem(new PrefixedFileSystemImpl(prefix)) {}
};

}  // namespace bridge
