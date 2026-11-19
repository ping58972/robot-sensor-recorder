#include "recorder.hpp"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

namespace capture {
namespace {

struct Descriptor {
    int fd;
    explicit Descriptor(int value) : fd(value) {
        if (fd < 0) throw std::runtime_error("Cannot open sensor source.");
    }
    ~Descriptor() { ::close(fd); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
};

void await_input(int fd, std::uint64_t deadline)
{
    for (;;) {
        const auto now = monotonic_ns();
        if (now >= deadline) throw std::runtime_error("Sensor input deadline expired.");
        const auto remaining = (deadline - now) / 1000000;
        pollfd event{fd, POLLIN, 0};
        const auto ready = ::poll(&event, 1, static_cast<int>(remaining + 1));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) throw std::runtime_error("Sensor input deadline expired.");
        if (event.revents & POLLIN) return;
        throw std::runtime_error("Sensor source disconnected.");
    }
}

std::uint64_t decode64(const std::uint8_t* bytes)
{
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index)
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
    return value;
}

} // namespace

void record_udp(const std::filesystem::path& directory, std::uint16_t port, std::size_t count)
{
    Descriptor socket(::socket(AF_INET, SOCK_DGRAM, 0));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(socket.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        throw std::runtime_error("Cannot bind UDP port.");
    SegmentWriter writer(directory);
    std::cout << "ready udp " << port << std::endl;
    const auto deadline = monotonic_ns() + 5000000000ULL;
    for (std::size_t index = 0; index < count; ++index) {
        await_input(socket.fd, deadline);
        std::array<std::uint8_t, 65536> packet{};
        const auto size = ::recv(socket.fd, packet.data(), packet.size(), 0);
        if (size < 18 || packet[0] > 3)
            throw std::runtime_error("Invalid UDP sensor packet.");
        Record record{decode64(packet.data() + 1), decode64(packet.data() + 9), packet[0],
            std::vector<std::uint8_t>(packet.begin() + 17, packet.begin() + size)};
        writer.append(record);
    }
    writer.finish();
    std::cout << "recorded_packets=" << count << '\n';
}

void record_serial(const std::filesystem::path& directory, const std::string& device, std::size_t count)
{
    Descriptor serial(::open(device.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC));
    termios original{};
    if (::tcgetattr(serial.fd, &original) != 0) throw std::runtime_error("Device is not a terminal.");
    auto settings = original;
    ::cfmakeraw(&settings);
    ::cfsetispeed(&settings, B115200);
    ::cfsetospeed(&settings, B115200);
    if (::tcsetattr(serial.fd, TCSANOW, &settings) != 0) throw std::runtime_error("Cannot configure serial device.");
    struct Restore {
        int fd;
        termios settings;
        ~Restore() { ::tcsetattr(fd, TCSANOW, &settings); }
    } restore{serial.fd, original};

    SegmentWriter writer(directory);
    std::cout << "ready serial" << std::endl;
    const auto deadline = monotonic_ns() + 5000000000ULL;
    std::string line;
    std::size_t received = 0;
    while (received < count) {
        await_input(serial.fd, deadline);
        char byte = 0;
        const auto size = ::read(serial.fd, &byte, 1);
        if (size < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (size <= 0) throw std::runtime_error("Serial device closed.");
        if (byte == '\n') {
            if (std::count(line.begin(), line.end(), ',') != 3)
                throw std::runtime_error("Expected four serial CSV fields.");
            auto fields = line;
            for (auto& character : fields) if (character == ',') character = ' ';
            std::istringstream values(fields);
            std::string timestamp, extra;
            double x = 0, y = 0, z = 0;
            if (!(values >> timestamp >> x >> y >> z) || (values >> extra) ||
                !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
                throw std::runtime_error("Invalid serial IMU line.");
            writer.append(Record{received++, number(timestamp, UINT64_MAX), 2,
                std::vector<std::uint8_t>(line.begin(), line.end())});
            line.clear();
        } else {
            if (line.size() == 255) throw std::runtime_error("Serial line too long.");
            line += byte;
        }
    }
    writer.finish();
    std::cout << "recorded_lines=" << count << '\n';
}

} // namespace capture
