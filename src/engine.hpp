#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace strata {
namespace fs = std::filesystem;
constexpr std::uint32_t max_frame = 16U * 1024U * 1024U;
inline void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
inline std::uint32_t crc(const std::string &bytes) {
    std::uint32_t value = 0xffffffffU;
    for (unsigned char byte : bytes) {
        value ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            value = (value >> 1U) ^ ((0U - (value & 1U)) & 0xedb88320U);
    }
    return ~value;
}
inline void number(std::string &out, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        out.push_back(static_cast<char>((value >> (8U * i)) & 255U));
}
struct Reader {
    const std::string &bytes;
    std::size_t pos = 0;
    std::uint64_t number(unsigned width) {
        require(width <= 8 && width <= bytes.size() - pos, "truncated number");
        std::uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i)
            value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[pos++]))
                     << (8U * i);
        return value;
    }
    std::string take(std::size_t size) {
        require(size <= bytes.size() - pos, "truncated bytes");
        auto out = bytes.substr(pos, size);
        pos += size;
        return out;
    }
};
inline void point(const char *name) {
    const char *requested = std::getenv("STRATA_FAILPOINT");
    if (requested && std::string(requested) == name)
        std::_Exit(86);
}
inline void sync_file(const fs::path &path) {
#ifdef _WIN32
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(file != INVALID_HANDLE_VALUE, "sync open failed");
    const bool ok = FlushFileBuffers(file) != 0;
    CloseHandle(file);
    require(ok, "file sync failed");
#else
    int file = ::open(path.c_str(), O_RDWR);
    require(file >= 0, "sync open failed");
    int ok = ::fsync(file);
    ::close(file);
    require(ok == 0, "file sync failed");
#endif
}
inline void sync_dir(const fs::path &path) {
#ifdef _WIN32
    (void)path;
#else
    int file = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
    require(file >= 0, "directory open failed");
    int ok = ::fsync(file);
    ::close(file);
    require(ok == 0, "directory sync failed");
#endif
}
inline void write_file(const fs::path &path, const std::string &bytes, bool append = false) {
    {
        std::ofstream stream(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
        require(static_cast<bool>(stream), "write open failed");
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        require(static_cast<bool>(stream), "write failed");
    }
    sync_file(path);
}
inline std::string frame(const std::string &payload) {
    require(!payload.empty() && payload.size() <= max_frame, "frame limit");
    std::string out;
    number(out, payload.size(), 4);
    out += payload;
    number(out, crc(payload), 4);
    return out;
}
inline std::vector<std::string> frames(const fs::path &path, bool recover) {
    std::ifstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream), "missing referenced file");
    std::vector<std::string> out;
    std::uintmax_t good = 0;
    for (;;) {
        char header[4];
        stream.read(header, 4);
        const auto count = stream.gcount();
        if (count == 0)
            break;
        if (count != 4) {
            require(recover, "torn segment header");
            break;
        }
        std::string head(header, 4);
        Reader r{head};
        auto length = r.number(4);
        require(length > 0 && length <= max_frame, "invalid frame length");
        std::string payload(static_cast<std::size_t>(length), '\0');
        stream.read(payload.data(), static_cast<std::streamsize>(length));
        if (stream.gcount() != static_cast<std::streamsize>(length)) {
            require(recover, "torn segment payload");
            break;
        }
        stream.read(header, 4);
        if (stream.gcount() != 4) {
            require(recover, "torn segment checksum");
            break;
        }
        std::string tail(header, 4);
        Reader c{tail};
        require(c.number(4) == crc(payload), "CRC mismatch");
        out.push_back(std::move(payload));
        good += length + 8;
    }
    require(!stream.bad(), "read failed");
    stream.close();
    if (recover && good != fs::file_size(path)) {
        fs::resize_file(path, good);
        sync_file(path);
    }
    return out;
}
class Lock {
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int handle = -1;
#endif
  public:
    explicit Lock(const fs::path &path) {
#ifdef _WIN32
        handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        require(handle != INVALID_HANDLE_VALUE, "database already locked");
#else
        handle = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
        require(handle >= 0, "lock open failed");
        if (::flock(handle, LOCK_EX | LOCK_NB) != 0) {
            ::close(handle);
            handle = -1;
            throw std::runtime_error("database already locked");
        }
#endif
    }
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;
    ~Lock() {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
#else
        if (handle >= 0)
            ::close(handle);
#endif
    }
};
struct Mutation {
    std::string key;
    std::optional<std::string> value;
};
struct Version {
    std::uint64_t sequence;
    std::optional<std::string> value;
};
using Index = std::map<std::string, std::vector<Version>>;
inline std::string encode(std::uint64_t sequence, const std::vector<Mutation> &batch) {
    require(!batch.empty() && batch.size() <= 100000, "batch count limit");
    std::set<std::string> keys;
    std::string out = "B1";
    number(out, sequence, 8);
    number(out, batch.size(), 4);
    for (const auto &mutation : batch) {
        require(mutation.key.size() <= 65536 &&
                    (!mutation.value || mutation.value->size() <= 1048576),
                "key/value limit");
        require(keys.insert(mutation.key).second, "duplicate key in batch");
        number(out, mutation.key.size(), 4);
        number(out, mutation.value ? mutation.value->size() : 0xffffffffU, 4);
        out += mutation.key;
        if (mutation.value)
            out += *mutation.value;
    }
    require(out.size() <= max_frame, "batch byte limit");
    return out;
}
inline std::pair<std::uint64_t, std::vector<Mutation>> decode(const std::string &bytes) {
    Reader r{bytes};
    require(r.take(2) == "B1", "unsupported batch");
    auto sequence = r.number(8);
    auto count = r.number(4);
    require(sequence > 0 && count > 0 && count <= 100000, "invalid batch metadata");
    std::vector<Mutation> batch;
    for (std::uint64_t i = 0; i < count; ++i) {
        auto keysize = r.number(4);
        auto size = r.number(4);
        require(keysize <= 65536 && (size == 0xffffffffU || size <= 1048576),
                "invalid key/value size");
        auto key = r.take(static_cast<std::size_t>(keysize));
        std::optional<std::string> value;
        if (size != 0xffffffffU)
            value = r.take(static_cast<std::size_t>(size));
        batch.push_back({std::move(key), std::move(value)});
    }
    require(r.pos == bytes.size(), "trailing batch bytes");
    (void)encode(sequence, batch);
    return {sequence, std::move(batch)};
}
class Engine {
    fs::path directory;
    Lock lock;
    std::uint64_t wal = 1, next_file = 2, last = 0;
    std::vector<std::uint64_t> segments;
    bool poisoned = false;
    Index index;
    std::vector<std::string> memory;
    fs::path file(std::uint64_t id, const char *suffix) const {
        return directory / (std::to_string(id) + suffix);
    }
    static fs::path prepare(const fs::path &path) {
        fs::create_directories(path);
        return path;
    }
    void publish(const std::vector<std::uint64_t> &ids, std::uint64_t active) {
        std::string payload = "SM01";
        number(payload, active, 8);
        number(payload, ids.size(), 4);
        for (auto id : ids)
            number(payload, id, 8);
        auto temp = directory / "MANIFEST.tmp";
        auto target = directory / "MANIFEST";
        write_file(temp, frame(payload));
        point("manifest_synced");
#ifdef _WIN32
        require(MoveFileExW(temp.c_str(), target.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
                "manifest replace failed");
#else
        fs::rename(temp, target);
#endif
        sync_dir(directory);
        point("manifest_published");
    }
    void install(const std::string &payload, std::set<std::uint64_t> &sequences) {
        auto [sequence, batch] = decode(payload);
        require(sequences.insert(sequence).second, "duplicate sequence");
        last = std::max(last, sequence);
        for (auto &mutation : batch)
            index[mutation.key].push_back({sequence, std::move(mutation.value)});
    }
    void segment(std::uint64_t id, const std::vector<std::string> &batches) {
        std::map<std::pair<std::string, std::uint64_t>, Mutation> sorted;
        for (const auto &payload : batches) {
            auto [sequence, mutations] = decode(payload);
            for (auto &mutation : mutations) {
                auto key = mutation.key;
                require(sorted.emplace(std::make_pair(key, sequence), std::move(mutation)).second,
                        "duplicate segment version");
            }
        }
        std::string bytes;
        for (const auto &[identity, mutation] : sorted)
            bytes += frame(encode(identity.second, {mutation}));
        write_file(file(id, ".sst"), bytes);
        sync_dir(directory);
        point("segment_synced");
    }
    std::vector<std::string> read_segment(std::uint64_t id) const {
        std::map<std::uint64_t, std::vector<Mutation>> batches;
        std::optional<std::pair<std::string, std::uint64_t>> previous;
        auto records = frames(file(id, ".sst"), false);
        require(!records.empty(), "empty segment");
        for (const auto &payload : records) {
            auto [sequence, mutations] = decode(payload);
            require(mutations.size() == 1, "invalid sorted record");
            auto identity = std::make_pair(mutations.front().key, sequence);
            require(!previous || *previous < identity, "segment order violation");
            previous = identity;
            batches[sequence].push_back(std::move(mutations.front()));
        }
        std::vector<std::string> out;
        for (const auto &[seq, mutations] : batches)
            out.push_back(encode(seq, mutations));
        return out;
    }

  public:
    explicit Engine(const fs::path &path) : directory(prepare(path)), lock(directory / "LOCK") {
        for (const auto &entry : fs::directory_iterator(directory)) {
            const auto stem = entry.path().stem().string();
            if ((entry.path().extension() == ".wal" || entry.path().extension() == ".sst") &&
                !stem.empty() && stem.find_first_not_of("0123456789") == std::string::npos) {
                auto id = std::stoull(stem);
                require(id < std::numeric_limits<std::uint64_t>::max() - 1, "file ID exhausted");
                next_file = std::max(next_file, static_cast<std::uint64_t>(id + 1));
            }
        }
        if (!fs::exists(directory / "MANIFEST")) {
            // Never reinterpret an existing unmanifested store as an empty one.
            require(next_file == 2 && !fs::exists(file(1, ".wal")),
                    "missing manifest in nonempty store");
            write_file(file(wal, ".wal"), "");
            sync_dir(directory);
            publish({}, wal);
        }
        auto records = frames(directory / "MANIFEST", false);
        require(records.size() == 1, "invalid manifest frames");
        Reader r{records.front()};
        require(r.take(4) == "SM01", "unsupported manifest");
        wal = r.number(8);
        auto count = r.number(4);
        require(wal > 0 && count <= 100000, "invalid manifest");
        std::set<std::uint64_t> ids{wal};
        for (std::uint64_t i = 0; i < count; ++i) {
            auto id = r.number(8);
            require(id > 0 && ids.insert(id).second, "duplicate manifest file");
            segments.push_back(id);
        }
        require(r.pos == records.front().size(), "manifest trailing bytes");
        std::set<std::uint64_t> sequences;
        for (auto id : segments)
            for (const auto &batch : read_segment(id))
                install(batch, sequences);
        memory = frames(file(wal, ".wal"), true);
        std::uint64_t previous = last;
        for (const auto &batch : memory) {
            auto sequence = decode(batch).first;
            require(sequence > previous, "WAL sequence regression");
            install(batch, sequences);
            previous = sequence;
        }
        for (auto &[key, versions] : index) {
            (void)key;
            std::sort(versions.begin(), versions.end(),
                      [](const auto &a, const auto &b) { return a.sequence < b.sequence; });
        }
    }
    std::uint64_t sequence() const {
        require(!poisoned, "engine must be reopened after I/O failure");
        return last;
    }
    std::uint64_t batch(const std::vector<Mutation> &mutations) {
        require(!poisoned, "engine must be reopened after I/O failure");
        require(last < std::numeric_limits<std::uint64_t>::max(), "sequence exhausted");
        auto payload = encode(last + 1, mutations);
        poisoned = true;
        write_file(file(wal, ".wal"), frame(payload), true);
        point("wal_synced");
        ++last;
        for (const auto &m : mutations)
            index[m.key].push_back({last, m.value});
        memory.push_back(payload);
        poisoned = false;
        return last;
    }
    std::optional<std::string> get(const std::string &key, std::uint64_t at) const {
        require(!poisoned, "engine must be reopened after I/O failure");
        require(at <= last, "future snapshot");
        auto found = index.find(key);
        if (found == index.end())
            return {};
        for (auto it = found->second.rbegin(); it != found->second.rend(); ++it)
            if (it->sequence <= at)
                return it->value;
        return {};
    }
    std::vector<std::pair<std::string, std::string>>
    scan(const std::string &lower, const std::string &upper, std::uint64_t at) const {
        require(!poisoned, "engine must be reopened after I/O failure");
        require(at <= last && lower <= upper, "invalid scan");
        std::vector<std::pair<std::string, std::string>> out;
        for (auto it = index.lower_bound(lower); it != index.end() && it->first < upper; ++it) {
            auto value = get(it->first, at);
            if (value)
                out.emplace_back(it->first, *value);
        }
        return out;
    }
    void flush() {
        require(!poisoned, "engine must be reopened after I/O failure");
        if (memory.empty())
            return;
        require(next_file < std::numeric_limits<std::uint64_t>::max() - 2, "file ID exhausted");
        poisoned = true;
        auto segment_id = next_file++;
        auto new_wal = next_file++;
        segment(segment_id, memory);
        write_file(file(new_wal, ".wal"), "");
        sync_dir(directory);
        point("new_wal_synced");
        auto updated = segments;
        updated.push_back(segment_id);
        publish(updated, new_wal);
        auto old = wal;
        wal = new_wal;
        segments = std::move(updated);
        memory.clear();
        fs::remove(file(old, ".wal"));
        sync_dir(directory);
        poisoned = false;
    }
    void compact() {
        require(!poisoned, "engine must be reopened after I/O failure");
        if (segments.size() < 2)
            return;
        require(next_file < std::numeric_limits<std::uint64_t>::max() - 1, "file ID exhausted");
        poisoned = true;
        std::map<std::uint64_t, std::vector<Mutation>> batches;
        for (auto id : segments)
            for (const auto &payload : read_segment(id)) {
                auto [sequence, mutations] = decode(payload);
                batches.emplace(sequence, std::move(mutations));
            }
        std::vector<std::string> merged;
        for (const auto &[seq, mutations] : batches)
            merged.push_back(encode(seq, mutations));
        auto id = next_file++;
        segment(id, merged);
        publish({id}, wal);
        auto old = segments;
        segments = {id};
        for (auto prior : old) {
            fs::remove(file(prior, ".sst"));
        }
        sync_dir(directory);
        poisoned = false;
    }
};
} // namespace strata
