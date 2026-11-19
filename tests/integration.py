"""Independent file-format and process-level checks using Python's standard library."""
import os
from pathlib import Path
import pty
import select
import socket
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

BINARY = str(Path(sys.argv.pop(1)).resolve())


def run(*args, expected=0):
    result = subprocess.run([BINARY, *map(str, args)], capture_output=True, timeout=10)
    if result.returncode != expected:
        raise AssertionError(f"exit={result.returncode}, stderr={result.stderr.decode()}")
    return result


def records(directory):
    decoded = []
    for path in sorted(directory.glob("*.rlog")):
        raw = path.read_bytes()
        if raw[:8] != b"RSRLOG1\n":
            raise AssertionError("bad magic")
        position = 8
        while position < len(raw):
            sequence, timestamp, stream, length = struct.unpack_from("<QQII", raw, position)
            end = position + 24 + length
            checksum, = struct.unpack_from("<I", raw, end)
            if zlib.crc32(raw[position:end]) != checksum:
                raise AssertionError("CRC differs from independent decoder")
            decoded.append((sequence, timestamp, stream, raw[position + 24:end]))
            position = end + 4
        if position != len(raw):
            raise AssertionError("partial record")
    return decoded


class RecorderTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_multisensor_rotation_and_sequences(self):
        output = self.root / "capture"
        result = run("record", output, 100)
        self.assertIn(b"queue_capacity=8", result.stdout)
        self.assertEqual(len(list(output.glob("*.rlog"))), 13)
        self.assertEqual(len(list(output.glob("*.partial"))), 0)
        data = records(output)
        self.assertEqual(len(data), 400)
        for stream, size in enumerate((4096, 512, 48, 64)):
            samples = [r for r in data if r[2] == stream]
            self.assertEqual([r[0] for r in samples], list(range(100)))
            self.assertEqual([r[1] for r in samples], sorted(r[1] for r in samples))
            self.assertTrue(all(len(r[3]) == size and r[3] == bytes([r[0] % 251]) * size
                                for r in samples))
        self.assertIn(b"verified_records=400", run("verify", output).stdout)

    def test_existing_directory_protected(self):
        output = self.root / "capture"
        run("record", output, 1)
        original = (output / "000000.rlog").read_bytes()
        run("record", output, 1, expected=1)
        self.assertEqual(original, (output / "000000.rlog").read_bytes())

    def test_corruption_detected(self):
        output = self.root / "capture"
        run("record", output, 2)
        path = output / "000000.rlog"
        data = bytearray(path.read_bytes())
        data[32] ^= 1
        path.write_bytes(data)
        self.assertIn(b"CRC32", run("verify", output, expected=1).stderr)

    def test_truncation_recovery_reports_prefix(self):
        output = self.root / "capture"
        run("record", output, 2)
        path = output / "000000.rlog"
        raw = path.read_bytes()[:-1]
        path.write_bytes(raw)
        report = run("recover", path, expected=2)
        self.assertIn(b"valid_records=7", report.stdout)
        self.assertEqual(raw, path.read_bytes())
        run("verify", output, expected=1)

    def test_storage_failure_wakes_workers(self):
        output = self.root / "failed"
        run("record", output, 10000, 35, expected=1)
        self.assertEqual(len(list(output.glob("*.rlog"))), 1)
        partial = next(output.glob("*.partial"))
        self.assertIn(b"valid_records=3", run("recover", partial).stdout)
        run("verify", output, expected=1)

    def start_source(self, *args):
        process = subprocess.Popen([BINARY, *map(str, args)],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(lambda: self.stop_process(process))
        ready, _, _ = select.select([process.stdout], [], [], 5)
        self.assertTrue(ready, "source failed to announce readiness")
        self.assertTrue(process.stdout.readline().startswith(b"ready"), "source failed to start")
        return process

    @staticmethod
    def stop_process(process):
        if process.poll() is None:
            process.kill()
        process.communicate(timeout=5)

    def test_udp_sensor_ingest(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        output = self.root / "udp"
        process = self.start_source("udp", output, port, 3)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
            for i in range(3):
                sender.sendto(struct.pack("<BQQ", 2, i, 100 + i) + b"imu",
                              ("127.0.0.1", port))
        _, error = process.communicate(timeout=8)
        self.assertEqual(process.returncode, 0, error)
        self.assertEqual(records(output), [(i, 100 + i, 2, b"imu") for i in range(3)])

    def test_serial_pty_ingest(self):
        master, slave = pty.openpty()
        try:
            output = self.root / "serial"
            process = self.start_source("serial", output, os.ttyname(slave), 3)
            for i in range(3):
                os.write(master, f"{i + 100},0.1,0.2,9.8\n".encode())
            _, error = process.communicate(timeout=8)
            self.assertEqual(process.returncode, 0, error)
            self.assertEqual([r[:3] for r in records(output)],
                             [(i, i + 100, 2) for i in range(3)])
        finally:
            os.close(master)
            os.close(slave)

    def test_rejects_invalid_source_and_numeric_input(self):
        run("record", self.root / "bad", -1, expected=1)
        run("record", self.root / "bad", 0, expected=1)
        run("serial", self.root / "bad", "/does-not-exist", 1, expected=1)

    def test_rejects_malformed_udp(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        process = self.start_source("udp", self.root / "bad-udp", port, 1)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
            sender.sendto(b"x", ("127.0.0.1", port))
        _, error = process.communicate(timeout=8)
        self.assertEqual(process.returncode, 1)
        self.assertIn(b"Invalid UDP", error)

    def test_rejects_malformed_serial(self):
        master, slave = pty.openpty()
        try:
            process = self.start_source("serial", self.root / "bad-serial", os.ttyname(slave), 1)
            os.write(master, b"100,,0.1,0.2,9.8\n")
            _, error = process.communicate(timeout=8)
            self.assertEqual(process.returncode, 1)
            self.assertIn(b"four serial CSV fields", error)
        finally:
            os.close(master)
            os.close(slave)

    def test_rejects_oversized_stored_payload_length(self):
        output = self.root / "capture"
        run("record", output, 1)
        path = output / "000000.rlog"
        data = bytearray(path.read_bytes())
        data[28:32] = (0xffffffff).to_bytes(4, "little")
        path.write_bytes(data)
        self.assertIn(b"Invalid record length", run("verify", output, expected=1).stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
