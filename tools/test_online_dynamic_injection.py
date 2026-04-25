#!/usr/bin/env python3
"""
Generate minimal online manifests plus .bz2 packet-group templates and run
ChipletNetworkSim.exe.
"""

from __future__ import annotations

import json
import subprocess
from pathlib import Path

from create_netrace import NetraceHeader, NetracePacket, PacketType


def write_group_template(path: Path, packets: list[dict], num_nodes: int = 16) -> None:
    header = NetraceHeader(
        benchmark_name=path.stem[:29],
        num_nodes=num_nodes,
        num_cycles=1,
        num_packets=len(packets),
        notes="online packet-group template",
    )
    raw_path = path.with_suffix("")
    with raw_path.open("wb") as f:
        f.write(header.to_bytes())
        for packet in packets:
            f.write(
                NetracePacket(
                    cycle=0,
                    packet_id=packet["packet_id"],
                    src=packet["src"],
                    dst=packet["dst"],
                    pkt_type=PacketType.CustomSize,
                    addr=packet.get("addr", 0),
                    custom_size=packet["size_bytes"],
                ).to_bytes()
            )

    import bz2

    with raw_path.open("rb") as f_in, bz2.open(path, "wb") as f_out:
        f_out.write(f_in.read())
    raw_path.unlink()


def build_manifest() -> dict:
    return {
        "num_inputs": 2,
        "num_resources": 2,
        "template_groups": [
            {"group_id": 0, "bz2_path": "group_a.bz2"},
            {"group_id": 1, "bz2_path": "group_b.bz2"},
        ],
        "phases": [
            {
                "phase_id": 0,
                "input_id": 0,
                "layer_id": 0,
                "compute_latency_cycles": 2,
                "dep_phase_ids": [],
                "resource_ids": [0, 1],
                "group_id": 0,
            },
            {
                "phase_id": 1,
                "input_id": 0,
                "layer_id": 1,
                "compute_latency_cycles": 3,
                "dep_phase_ids": [0],
                "resource_ids": [0, 1],
                "group_id": 1,
            },
            {
                "phase_id": 2,
                "input_id": 1,
                "layer_id": 0,
                "compute_latency_cycles": 1,
                "dep_phase_ids": [],
                "resource_ids": [0, 1],
                "group_id": 0,
            },
            {
                "phase_id": 3,
                "input_id": 1,
                "layer_id": 1,
                "compute_latency_cycles": 2,
                "dep_phase_ids": [2],
                "resource_ids": [0, 1],
                "group_id": 1,
            },
        ],
    }


def build_ini(workload_rel: str) -> str:
    return f"""[Network]
topology = MultiChipMesh
routing_algorithm = XY
k_node = 2
k_chip = 2
buffer_size = 20
vc_number = 2
d2d_IF = off_chip_serial

[Workload]
traffic = online_workload

[Simulation]
threads = 1
pause_on_first_injection = true
pause_on_input_done = true
interactive_pause = false

[Files]
workload_file = {workload_rel}
output_file = ./output/output_online_dynamic.csv
log_file = ./output/log_online_dynamic.txt
"""


def main() -> int:
    repo_root = Path(__file__).resolve().parents[3]
    sim_root = repo_root / "chip" / "chiplet-network-sim"
    test_dir = sim_root / "input" / "online_dynamic_test"
    test_dir.mkdir(parents=True, exist_ok=True)

    manifest_path = test_dir / "minimal_online_manifest.json"
    ini_path = test_dir / "minimal_online_workload.ini"
    exe_path = sim_root / "ChipletNetworkSim.exe"

    write_group_template(
        test_dir / "group_a.bz2",
        [
            {"packet_id": 0, "src": 0, "dst": 15, "size_bytes": 64},
            {"packet_id": 1, "src": 1, "dst": 14, "size_bytes": 64},
        ],
    )
    write_group_template(
        test_dir / "group_b.bz2",
        [
            {"packet_id": 0, "src": 15, "dst": 0, "size_bytes": 128},
            {"packet_id": 1, "src": 14, "dst": 1, "size_bytes": 128},
        ],
    )

    manifest = build_manifest()
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    ini_path.write_text(build_ini(manifest_path.relative_to(sim_root).as_posix()), encoding="utf-8")

    print("Generated manifest:")
    print(json.dumps(manifest, indent=2))
    print(f"\nGenerated ini: {ini_path}")

    if not exe_path.exists():
        print(f"\nExecutable not found: {exe_path}")
        return 1

    result = subprocess.run(
        [str(exe_path), str(ini_path.relative_to(sim_root).as_posix())],
        cwd=str(sim_root),
        text=True,
        capture_output=True,
    )

    print("\n=== STDOUT ===")
    print(result.stdout)
    if result.stderr:
        print("\n=== STDERR ===")
        print(result.stderr)

    output_json = sim_root / "output" / "output_online_dynamic.json"
    if output_json.exists():
        print("\n=== RESULT JSON ===")
        print(output_json.read_text(encoding="utf-8"))

    print(f"\nExit code: {result.returncode}")
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
