#!/bin/sh
# tests/run_batch1_selftest.sh -- build + run the headless Batch-1 v3 corrective selftest.
# Pure g++ build: no engine, no Windows, no UE. Exit 0 = all checks pass.
# (The DLL itself builds only in the repo's Windows CI; this gate is the pre-CI correctness bar.)
set -e
cd "$(dirname "$0")/.."   # -> src/votv-coop
g++ -std=c++20 -I include \
    tests/batch1_corrective_selftest.cpp \
    src/coop/items/save_record_wire.cpp \
    src/coop/interactables/signal_wire.cpp \
    -o /tmp/batch1_selftest
exec /tmp/batch1_selftest
