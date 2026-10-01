# Chiplet Network Sim

CNSim is a cycle-accurate, packet-parallel simulator for chiplet networks.

## Windows build

Run from the project root:

```bat
chip\chiplet-network-sim\rebuild_noc_sim_mingw.bat
```

This reuses `build_mingw`, uses `C:\Strawberry\c\bin\g++.exe`, and produces
`chip\chiplet-network-sim\build_mingw\ChipletNetworkSim.exe`. Pass that path
explicitly as `--sim-exe` in DSE communication runs. The old Visual Studio
`rebuild_noc_sim.bat` entry point was removed to avoid the stale root binary.

## Linux build

```bash
sudo apt install cmake ninja-build build-essential libboost-all-dev libbz2-dev
cd chiplet-network-sim
cmake --preset Release
cmake --build builds/Release
```

If you use CNSim in research, cite Yinxiao Feng, Yuchen Wei, Dong Xiang, and
Kaisheng Ma, *Evaluating Chiplet-based Large-Scale Interconnection Networks via
Cycle-Accurate Packet-Parallel Simulation*, USENIX ATC 2024.
