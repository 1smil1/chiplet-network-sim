# Chiplet Network Sim

CNSim is a cycle-accurate, packet-parallel simulator for chiplet networks.

## Windows build

Run from the project root:

```bat
rebuild_auto.bat --cnsim
```

This is the only CNSim Windows build entry. It uses Ninja and Strawberry GCC
(`C:\Strawberry\c\bin\g++.exe`), incrementally builds Release from current sources
in `build_mingw`, and copies it to the canonical runtime path
`chip\chiplet-network-sim\ChipletNetworkSim.exe`, which the main workflow uses.
Building only a binary under a build directory does not update that runtime copy.
Standalone `rebuild_noc_sim.bat` and `rebuild_noc_sim_mingw.bat` were removed.
Do not run binaries from old build directories; the main workflow needs no
`--sim-exe` override after this build succeeds.

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
