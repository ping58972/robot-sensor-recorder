#include "recorder.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <unistd.h>

namespace capture {
namespace {

constexpr std::array<std::uint8_t, 8> magic{'R','S','R','L','O','G','1','\n'};
constexpr std::size_t max_payload = 65536;

void put(std::vector<std::uint8_t>& bytes, std::uint64_t value, unsigned width)
{
    for (unsigned byte = 0; byte < width; ++byte)
        bytes.push_back(static_cast<std::uint8_t>(value >> (byte * 8)));
}

std::uint64_t get(const std::uint8_t* bytes, unsigned width)
{
    std::uint64_t value = 0;
    for (unsigned byte = 0; byte < width; ++byte)
        value |= static_cast<std::uint64_t>(bytes[byte]) << (byte * 8);
    return value;
}

std::uint32_t crc32(const std::vector<std::uint8_t>& bytes)
{
    std::uint32_t crc = 0xffffffffU;
    for (auto byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

void write_all(int fd, const std::uint8_t* data, std::size_t size)
{
    while (size > 0) {
        const auto written = ::write(fd, data, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) throw std::system_error(errno, std::generic_category(), "write");
        data += written;
        size -= static_cast<std::size_t>(written);
    }
}

} // namespace

std::uint64_t number(const std::string& text, std::uint64_t maximum)
{
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value > maximum)
        throw std::invalid_argument("Invalid numeric argument.");
    return value;
}

std::uint64_t monotonic_ns()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

SegmentWriter::SegmentWriter(const std::filesystem::path& directory, std::size_t limit)
    : directory_(directory), limit_(limit)
{
    if (limit == 0) throw std::invalid_argument("Segment limit must be positive.");
    if (!std::filesystem::create_directory(directory))
        throw std::runtime_error("Output directory already exists.");
}

SegmentWriter::~SegmentWriter()
{
    if (fd_ >= 0) ::close(fd_);
}

void SegmentWriter::open_segment()
{
    std::ostringstream name;
    name << std::setw(6) << std::setfill('0') << segment_++;
    partial_ = directory_ / (name.str() + ".partial");
    committed_ = directory_ / (name.str() + ".rlog");
    fd_ = ::open(partial_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd_ < 0) throw std::system_error(errno, std::generic_category(), "open segment");
    write_all(fd_, magic.data(), magic.size());
}

void SegmentWriter::append(const Record& record)
{
    if (finished_) throw std::logic_error("Recording already finished.");
    if (record.stream > 3 || record.payload.empty() || record.payload.size() > max_payload)
        throw std::invalid_argument("Invalid stream or payload size.");
    if (fd_ < 0) open_segment();

    std::vector<std::uint8_t> bytes;
    bytes.reserve(28 + record.payload.size());
    put(bytes, record.sequence, 8);
    put(bytes, record.timestamp_ns, 8);
    put(bytes, record.stream, 4);
    put(bytes, record.payload.size(), 4);
    bytes.insert(bytes.end(), record.payload.begin(), record.payload.end());
    const auto crc = crc32(bytes);
    put(bytes, crc, 4);
    write_all(fd_, bytes.data(), bytes.size());
    if (++in_segment_ == limit_) commit();
}

void SegmentWriter::commit()
{
    if (fd_ < 0) return;
    if (::fsync(fd_) != 0) throw std::system_error(errno, std::generic_category(), "fsync segment");
    const int descriptor = fd_;
    fd_ = -1;
    if (::close(descriptor) != 0) throw std::system_error(errno, std::generic_category(), "close segment");
    std::filesystem::rename(partial_, committed_);
    const int directory = ::open(directory_.c_str(), O_RDONLY | O_CLOEXEC);
    if (directory < 0) throw std::system_error(errno, std::generic_category(), "open directory");
    const int result = ::fsync(directory);
    const int error = errno;
    ::close(directory);
    if (result != 0) throw std::system_error(error, std::generic_category(), "fsync directory");
    in_segment_ = 0;
}

void SegmentWriter::finish()
{
    commit();
    finished_ = true;
}

Scan scan_file(const std::filesystem::path& file)
{
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open recording.");
    Scan result;
    auto reject = [&](const std::string& error) {
        result.complete = false;
        result.error = error;
        return result;
    };

    std::array<std::uint8_t, 8> header{};
    input.read(reinterpret_cast<char*>(header.data()), header.size());
    if (header != magic || !input) return reject("Invalid or incomplete file header.");
    result.valid_bytes = magic.size();

    while (true) {
        std::vector<std::uint8_t> bytes(24);
        input.read(reinterpret_cast<char*>(bytes.data()), 24);
        if (input.gcount() == 0 && input.eof()) return result;
        if (!input) return reject("Truncated record header.");
        const auto length = get(bytes.data() + 20, 4);
        if (get(bytes.data() + 16, 4) > 3 || length == 0 || length > max_payload)
            return reject("Invalid record length or stream.");
        bytes.resize(24 + static_cast<std::size_t>(length));
        input.read(reinterpret_cast<char*>(bytes.data() + 24), static_cast<std::streamsize>(length));
        std::array<std::uint8_t, 4> checksum{};
        input.read(reinterpret_cast<char*>(checksum.data()), checksum.size());
        if (!input) return reject("Truncated record payload.");
        if (get(checksum.data(), 4) != crc32(bytes)) return reject("CRC32 mismatch.");
        ++result.records;
        result.valid_bytes += bytes.size() + checksum.size();
    }
}

std::uint64_t verify_directory(const std::filesystem::path& directory)
{
    std::uint64_t records = 0;
    std::size_t segments = 0;
    for (const auto& file : std::filesystem::directory_iterator(directory)) {
        if (file.path().extension() == ".partial")
            throw std::runtime_error("Uncommitted segment found. Inspect it with recover.");
        if (file.path().extension() != ".rlog") continue;
        const auto scan = scan_file(file.path());
        if (!scan.complete) throw std::runtime_error(file.path().filename().string() + ": " + scan.error);
        records += scan.records;
        ++segments;
    }
    if (segments == 0) throw std::runtime_error("No committed segments found.");
    return records;
}

} // namespace capture
