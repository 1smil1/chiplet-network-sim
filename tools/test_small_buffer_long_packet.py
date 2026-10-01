#!/usr/bin/env python3
"""Regression check: a packet longer than one VC buffer must still complete."""

from __future__ import annotations

import bz2
import json
import os
import subprocess
from pathlib import Path

from create_netrace import NetraceHeader, NetracePacket, PacketType


def write_group(path: Path) -> None:
    raw = path.with_suffix("")
    header = NetraceHeader(
        benchmark_name="small-buffer-long-packet",
        num_nodes=4,
        num_cycles=1,
        num_packets=1,
        notes="long packet with four-flit VC buffer",
    )
    with raw.open("wb") as stream:
        stream.write(header.to_bytes())
        stream.write(NetracePacket(
            cycle=0,
            packet_id=0,
            src=0,
            dst=3,
            pkt_type=PacketType.CustomSize,
            addr=0,
            custom_size=1024,
        ).to_bytes())
    with raw.open("rb") as source, bz2.open(path, "wb") as target:
        target.write(source.read())
    raw.unlink()


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    work = root / "output" / "small_buffer_long_packet_test"
    work.mkdir(parents=True, exist_ok=True)
    group = work / "group.bz2"
    write_group(group)
    manifest = work / "manifest.json"
    manifest.write_text(json.dumps({
        "num_inputs": 1,
        "num_resources": 1,
        "template_groups": [{"group_id": 0, "bz2_path": group.name}],
        "phases": [{
            "phase_id": 0, "input_id": 0, "layer_id": 0,
            "compute_latency_cycles": 1, "dep_phase_ids": [],
            "resource_ids": [0], "group_id": 0,
        }],
    }, indent=2), encoding="utf-8")
    ini = work / "sim.ini"
    ini.write_text(f"""[Network]
topology = SingleChipMesh
routing_algorithm = XY
k_node = 2
k_chip = 1
buffer_size = 4
vc_number = 2

[Workload]
traffic = online_workload

[Simulation]
threads = 1
timeout_threshold = 100
timeout_limit = 20
pause_on_first_injection = false
pause_on_input_done = false

[Files]
workload_file = {manifest.name}
output_file = result.csv
log_file = result.log
""", encoding="utf-8")
    exe = Path(os.environ.get("CNSIM_EXE", root / "ChipletNetworkSim.exe"))
    if not exe.is_file():
        raise FileNotFoundError(exe)
    result = subprocess.run(
        [str(exe), ini.name], cwd=work, capture_output=True,
        text=True, timeout=30,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"long packet did not complete with buffer_size=4:\n{result.stderr}\n{result.stdout}"
        )
    output = work / "result.json"
    data = json.loads(output.read_text(encoding="utf-8"))
    assert int(data["message_arrived"]) == 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
