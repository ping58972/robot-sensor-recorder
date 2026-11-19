# Robot Sensor Recorder

A C++20 device-side recording prototype for robotics data collection. Four producer threads simulate video, audio, IMU, and device-status sources, feed a bounded queue, and share one segmented storage writer. UDP and serial adapters also accept externally supplied samples.

Robotics datasets depend on complete, readable recordings. This project makes the storage path inspectable through sequence numbers, timestamps, CRC32 checksums, explicit segment commits, and fault-injection tests.

## Relevance to robotics data infrastructure

MaxInsights describes a Physical AI data pipeline with capture, structured outputs, and quality verification. This project targets the recording and integrity responsibilities in its Systems Software & Middleware Engineer role. [Company website](https://www.maxinsights.ai/)

| Role requirement | Implemented evidence |
| --- | --- |
| C++, Linux, CMake | Reusable C++20 library, GCC builds on Ubuntu 24.04, native macOS build, and Docker test image. |
| Video/audio/IMU/status organization | Four typed stream IDs with per-source sequence numbers and monotonic nanosecond timestamps in a documented binary container. |
| Buffering and thread management | Fixed-capacity producer-consumer queue, condition variables, four producer threads, one storage thread, cooperative close, and worker error propagation. |
| Storage and integrity | Segment rotation, per-record CRC32 over metadata and payload, fsync, rename-based finalization, independent Python decoder, and valid-prefix inspection. |
| Linux network/serial interfaces | Loopback UDP socket adapter and termios serial adapter with poll-based deadlines. |
| Troubleshooting and module ownership | Injected storage failure, truncated/corrupted fixtures, source-disconnection handling, repeatable tests, and a design/test guide. |

## Build and run

Requires CMake 3.20+, GCC 12+ or a C++20-capable Clang, and Python 3.10+ for tests. Linux and macOS provide the POSIX interfaces used here.

~~~sh
git clone https://github.com/ping58972/robot-sensor-recorder.git
cd robot-sensor-recorder
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure -V
~~~

Create a new output directory by recording 100 samples from each simulated source.

~~~sh
./build/sensor-recorder record /tmp/robot-capture-demo 100
./build/sensor-recorder verify /tmp/robot-capture-demo
~~~

Expected verification output is `verified_records=400`. The recorder prints queue capacity and observed peak queue depth. An existing output directory produces an error, so use a fresh directory for each run.

### Reproducible Ubuntu/GCC environment

~~~sh
docker build -t robot-sensor-recorder .
docker run --rm robot-sensor-recorder
~~~

The image builds the project and runs the queue checks plus eleven process-level tests. Container runs include a serial pseudo-terminal and real loopback UDP sockets.

## Architecture

~~~text
video fixture ----\
audio fixture -----+--> bounded queue (8 records) --> storage thread
IMU fixture -------+                                |
status fixture ---/                                 v
                                          32-record segments
                                          .partial -> .rlog
~~~

Each producer owns its source sequence and constructs a record before pushing it. The consumer owns the file descriptors and all writes. A full queue makes producers wait. Closing the queue wakes blocked producers and consumers, and queued records remain available for draining. If the storage worker fails, it closes the queue, wakes producers, joins all threads, and returns an error.

Each synthetic source emits `N` samples. Total records are `4 × N`. At `N = 100`, there are `400` records, giving `ceil(400 / 32) = 13` segments. Fixture payload sizes are 4,096 bytes for video, 512 for audio, 48 for IMU, and 64 for status.

## File format and inspection

All integers use little-endian byte order. Each file starts with the eight-byte magic `RSRLOG1\n`.

| Record field | Bytes |
| --- | --- |
| Sequence within the source | 8 |
| Timestamp in nanoseconds | 8 |
| Stream ID, 0 through 3 | 4 |
| Payload length, 1 through 65,536 | 4 |
| Opaque payload | Variable |
| CRC32 of the preceding record metadata and payload | 4 |

Stream IDs map to video, audio, IMU, and status in that order. Synthetic timestamps use the process monotonic clock. UDP and serial timestamps retain the source-provided values.

A segment is written as `.partial`. Finalization fsyncs the file, closes it, renames it to `.rlog`, and fsyncs the directory. A crash can leave an inspectable partial file.

~~~sh
# Inject a storage-worker failure after 35 accepted writes.
./build/sensor-recorder record /tmp/robot-failed-demo 100 35

# Inspect the three valid records in the unfinished second segment.
./build/sensor-recorder recover /tmp/robot-failed-demo/000001.partial
~~~

`recover` reports the verified record count and byte boundary of the valid prefix. It preserves the input bytes. A truncated or corrupt record returns exit code 2. `verify` accepts committed segments with valid checksums and rejects a directory containing unfinished segments.

## UDP and serial sources

~~~sh
./build/sensor-recorder udp /tmp/robot-udp-demo 58080 3
./build/sensor-recorder serial /tmp/robot-serial-demo /dev/ttyUSB0 3
~~~

The UDP wire format is one byte of stream ID, eight bytes of sequence, eight bytes of timestamp, then a nonempty payload. Integers are little-endian. The receiver binds to `127.0.0.1`. UDP delivery retains the transport's possible losses and reordering. Source sequence values remain available for downstream gap analysis.

The serial adapter configures raw 115,200-baud input and accepts four CSV fields per line, `timestamp_ns,x,y,z`. It records those bytes as IMU payloads. Both adapters impose a five-second total input deadline and report malformed data.

The [integration test](tests/integration.py) contains reproducible UDP and pseudo-terminal senders.

## Validation and benchmark

Validation on 2026-11-18 covered macOS ARM64 with Apple Clang and Ubuntu 24.04 ARM64 with GCC 13 in Docker. Both CTest targets passed, including eleven Python integration cases and the queue lifecycle executable. AddressSanitizer and UndefinedBehaviorSanitizer checks also passed on macOS.

~~~sh
python3 tests/benchmark.py build/sensor-recorder
docker run --rm robot-sensor-recorder python3 tests/benchmark.py /opt/build/sensor-recorder
~~~

A captured Linux Docker run wrote 4,000 synthetic records in 0.226709 seconds, totaling 4,833,000 file bytes across 125 segments. That is 20.33 MiB/s including process startup, synthetic generation, CRC32, rotation, and fsync. A separate verification pass took 0.034121 seconds. These are single-run fixture measurements on a shared Apple Silicon host.

## Source guide and engineering scope

- [Recording API and queue](include/recorder.hpp)
- [Binary encoding, storage, and verification](src/recording.cpp)
- [Threaded synthetic capture](src/main.cpp)
- [UDP and serial adapters](src/sources.cpp)
- [Design decisions, acceptance checks, and next steps](docs/design.md)

The fixture uses deterministic opaque byte payloads. Physical camera/audio integration requires acquisition and codec adapters. Capture synchronization, power-loss certification, and production disk-failure handling require additional hardware testing. CRC32 checks accidental corruption. Complete-segment deletion requires a future session manifest to detect.

### Resume bullet

Built a C++20 multi-sensor recorder with bounded producer-consumer buffering, segmented storage, CRC32 verification, UDP/serial input, and fault-injection tests validated on Ubuntu/GCC and macOS.

Created by [Ping](https://github.com/ping58972).
