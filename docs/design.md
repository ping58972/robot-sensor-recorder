# Recording module design and test procedures

## Ownership and lifecycle

The CLI selects a source. Synthetic capture creates four producers and one writer. Producers own payload allocations until transfer into the queue. The bounded queue moves records to the writer and applies backpressure. The storage thread owns segment creation and commit.

The queue's close operation is idempotent. Close wakes all waiters. Accepted items can be drained. A storage exception closes the queue and propagates after thread joins. A producer exception also ends the run with an error after accepted records drain.

## Acceptance checks

| Requirement | Test |
| --- | --- |
| Preserve all fixture samples and each stream's sequence order | Record 100 samples per source, independently decode 400 records, check payload sizes and values. |
| Enforce bounded buffering and release waiters | Fill a one-slot queue, prove the second push waits, close, and verify both producer and consumer can terminate. |
| Rotate at 32 records and finalize segments | Check 13 committed files for 400 records and absence of partial files. |
| Detect data corruption | Flip a stored payload bit and require CRC verification failure. |
| Handle truncated capture | Remove the final byte, require seven valid records in an eight-record fixture, and confirm inspection leaves bytes intact. |
| Exit after storage failure | Fail after 35 writes in a large capture, check the committed first segment and inspect the partial second segment. |
| Ingest UDP | Send three timestamped datagrams over loopback and independently decode the resulting records. |
| Ingest serial | Send three IMU lines through a pseudo-terminal and verify timestamps and stream IDs. |
| Protect prior capture | Attempt to reuse an output directory and verify the original bytes remain intact. |

## Storage decisions

Metadata is explicitly encoded with shifts, which avoids native struct padding and host-endian coupling. CRC32 covers sequence, timestamp, stream, payload size, and payload. Payload length is bounded before allocating during inspection.

Commit uses file fsync, rename, and directory fsync. Inspectable partial segments expose the valid byte prefix after interruption. Platform and storage-device behavior determines the strength of power-loss durability.

The default queue permits eight waiting records. Each of four producers and the consumer can additionally hold an in-flight record. Larger camera frames can use an adjusted bounded payload policy or a shared buffer pool.

## Current boundaries and next iterations

1. Add a session manifest to audit segment continuity and whole-session completeness.
2. Add timestamp skew checks and source clock calibration.
3. Add physical V4L2/ALSA or device-SDK source adapters and encoded media metadata.
4. Inject short writes and fsync failures through a storage interface.
5. Add TCP framing for a reliable network transport and a recorder adapter for the companion shared-memory bus.
6. Run prolonged capture and power-interruption tests on the target Linux device.

## Troubleshooting

An existing-directory error requires a fresh capture path. A serial configuration error requires a terminal device with appropriate OS permissions. A source deadline indicates fewer samples arrived than requested. CRC failure identifies a damaged record. A `.partial` file indicates an unfinished segment and can be inspected with the recovery command.

For Linux syscall inspection, run the CLI under `strace -f -e trace=write,fsync,rename,openat`. For memory checks, configure a Debug build with `-fsanitize=address,undefined -fno-omit-frame-pointer`, build, and execute CTest.
