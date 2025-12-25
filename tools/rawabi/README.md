# RAW ABI test utilities

This folder contains a minimal RAW streaming test suite built on top of the
existing ABI headers (`include/abi/ip-video-raw.h`). It provides
sender/receiver/record/playback helpers plus unit/integration tests.

## Building (CMake)
```bash
cmake -S .. -B ../build -DBUILD_RAWABI_TESTS=ON
cmake --build ../build
ctest --test-dir ../build
```

The tools live under `tools/rawabi/` and are installed in `build/tools/rawabi/`
when using the commands above.

## Tools
### rawabi_sender
Synthetic frame generator and UDP transmitter. Supports color bars, ramps,
checker/slanted edges, and a moving box animation. You can also replay a `.raw`
file (16-bit container) by providing metadata.

Example:
```bash
./build/tools/rawabi/rawabi_sender --dest 127.0.0.1 --port 10000 \
  --width 1280 --height 720 --fps 30 --bit-depth 12 --bayer rg1bg2 \
  --pattern bars --fragment 1400 --loss 1.0 --dup 0.5 --reorder 4
```

### rawabi_receiver
UDP receiver that reassembles FH/FD fragments, validates continuity, and shows
an ISP-lite preview (black level subtraction, WB gains, gamma LUT). View modes:
`mono`, `green`, `half`, `bilinear`.

Example:
```bash
./build/tools/rawabi/rawabi_receiver --port 10000 --view half --black 64 \
  --wb 1.2,1.0,1.5 --gamma 2.2 --record capture
```

### rawabi_record / rawabi_playback
`rawabi_record` captures reassembled frames to disk without preview. Use
`--frames` to limit the capture count.

`rawabi_playback` replays a RAW file with ABI headers over UDP:
```bash
./build/tools/rawabi/rawabi_playback --file frame.raw --width 1280 --height 720 \
  --bit-depth 12 --bayer rg1bg2 --dest 127.0.0.1 --port 10000 --fps 30
```

## Tests
`ctest --test-dir ../build` runs the `rawabi_tests` binary which exercises header
packing, reassembly, reorder handling, and a localhost sender/receiver loop.
