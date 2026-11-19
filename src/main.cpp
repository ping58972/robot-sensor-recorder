#include "recorder.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <limits>
#include <thread>

namespace {

void generate(const std::filesystem::path& directory, std::size_t count, std::size_t fail_after)
{
    capture::SegmentWriter writer(directory);
    capture::BoundedQueue<capture::Record> queue(8);
    std::mutex errors_mutex;
    std::exception_ptr error;
    auto fail = [&] {
        {
            std::lock_guard lock(errors_mutex);
            if (!error) error = std::current_exception();
        }
        queue.close();
    };

    std::uint64_t written = 0;
    {
        std::jthread consumer([&] {
            try {
                while (auto record = queue.pop()) {
                    if (written == fail_after) throw std::runtime_error("Injected storage failure.");
                    writer.append(*record);
                    ++written;
                }
                writer.finish();
            } catch (...) { fail(); }
        });
        std::array<std::jthread, 4> producers;
        constexpr std::array<std::size_t, 4> payload_sizes{4096, 512, 48, 64};
        try {
            for (std::uint32_t stream = 0; stream < producers.size(); ++stream) {
                producers[stream] = std::jthread([&, stream] {
                    try {
                        for (std::size_t index = 0; index < count; ++index) {
                            capture::Record record{index, capture::monotonic_ns(), stream,
                                std::vector<std::uint8_t>(payload_sizes[stream],
                                    static_cast<std::uint8_t>(index % 251))};
                            if (!queue.push(std::move(record))) return;
                        }
                    } catch (...) { fail(); }
                });
            }
        } catch (...) {
            fail();
        }
        for (auto& producer : producers)
            if (producer.joinable()) producer.join();
        queue.close();
    }
    if (error) std::rethrow_exception(error);
    std::cout << "records=" << written << " queue_capacity=8 peak_queued=" << queue.peak() << '\n';
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        if (argc >= 4 && std::string(argv[1]) == "record" && (argc == 4 || argc == 5)) {
            const auto count = capture::number(argv[3], 100000);
            if (count == 0) throw std::invalid_argument("Record count must be positive.");
            const auto fail_after = argc == 5 ? capture::number(argv[4], 400000) :
                std::numeric_limits<std::size_t>::max();
            generate(argv[2], count, fail_after);
        } else if (argc == 3 && std::string(argv[1]) == "verify") {
            const auto count = capture::verify_directory(argv[2]);
            std::cout << "verified_records=" << count << '\n';
        } else if (argc == 3 && std::string(argv[1]) == "recover") {
            const auto scan = capture::scan_file(argv[2]);
            std::cout << "valid_records=" << scan.records << " valid_prefix_bytes=" << scan.valid_bytes
                      << " complete=" << (scan.complete ? "true" : "false") << '\n';
            if (!scan.complete) { std::cerr << scan.error << '\n'; return 2; }
        } else if (argc == 5 && (std::string(argv[1]) == "udp" || std::string(argv[1]) == "serial")) {
            const auto count = capture::number(argv[4], 100000);
            if (count == 0) throw std::invalid_argument("Record count must be positive.");
            if (std::string(argv[1]) == "udp") {
                const auto port = capture::number(argv[3], 65535);
                if (port == 0) throw std::invalid_argument("Use a nonzero UDP port.");
                capture::record_udp(argv[2], static_cast<std::uint16_t>(port), count);
            } else {
                capture::record_serial(argv[2], argv[3], count);
            }
        } else {
            std::cerr << "Usage: sensor-recorder record DIR SAMPLES_PER_STREAM [FAIL_AFTER]\n"
                         "       sensor-recorder verify DIR\n"
                         "       sensor-recorder recover FILE\n"
                         "       sensor-recorder udp DIR PORT PACKETS\n"
                         "       sensor-recorder serial DIR DEVICE LINES\n";
            return 2;
        }
    } catch (const std::exception& error) {
        std::cerr << "sensor-recorder: " << error.what() << '\n';
        return 1;
    }
}
