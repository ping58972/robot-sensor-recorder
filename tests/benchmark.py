"""Report wall-clock throughput for synthetic capture, including fsync and verification."""
import json
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1]).resolve())
samples = int(sys.argv[2]) if len(sys.argv) > 2 else 1000
with tempfile.TemporaryDirectory() as folder:
    target = Path(folder) / "capture"
    start = time.perf_counter()
    subprocess.run([binary, "record", str(target), str(samples)], check=True, capture_output=True)
    capture_seconds = time.perf_counter() - start
    start = time.perf_counter()
    subprocess.run([binary, "verify", str(target)], check=True, capture_output=True)
    verify_seconds = time.perf_counter() - start
    total = sum(path.stat().st_size for path in target.glob("*.rlog"))
    print(json.dumps({
        "platform": platform.platform(), "records": samples * 4,
        "file_bytes": total, "capture_seconds": round(capture_seconds, 6),
        "verify_seconds": round(verify_seconds, 6),
        "capture_mib_per_second": round(total / 1048576 / capture_seconds, 2),
        "includes": "synthetic generation, CRC32, segment rotation, fsync and process startup"
    }, indent=2))
