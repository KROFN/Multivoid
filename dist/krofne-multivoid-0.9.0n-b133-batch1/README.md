# dist/krofne-multivoid-0.9.0n-b133-batch1

This directory is the drop point for the fork DLL. **The local (Linux) environment cannot
produce it** — the build is Windows/MSVC-only by design (see `BUILDING.md`: VS BuildTools,
vcpkg manifest with protobuf 3.21.12 `x64-windows-static`, DX11/DX12 SDKs). No source was
changed to force a local compile.

## How to produce the DLL (repo-documented path)

Fork/push this branch (`krofne/b133-playability-batch1`) and run the repo's own CI lane:

1. Push the branch to a GitHub fork with Actions enabled.
2. Actions -> **build** -> **Run workflow** (workflow_dispatch) on the branch.
   The lane uses `VOTV-MP/Multivoid/.github/workflows/build-core.yml@main` (upstream build
   recipe) against YOUR commit.
3. Download the `multivoid-ci-<sha12>` artifact.
4. Copy `xinput1_3.dll` + the payload DLL into this directory, next to this README.

Expected payload filename (load-bearing — the xinput proxy scans `multivoid-*.dll` and loads
the highest build number; the build number is parsed from `kProtocolVersion`):

    multivoid-0.9.0n-2133.dll

Protocol 2133 = the fork-local identifier (see `src/votv-coop/include/coop/net/protocol.h`,
"KROFNE FORK PROTOCOL"). It is intentionally above any stock value, so this DLL wins the
proxy's highest-build scan over a stock b133 install; run ONE DLL per game copy (duplicates
pop the mod's own "MOD INSTALL PROBLEM" dialog).

Compatibility: stock-b133 peers <-> this fork = clean handshake reject (ParseHeader version
mismatch); fork <-> fork = compatible; game build = VotV Alpha 0.9.0n (unchanged).
