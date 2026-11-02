# Protocol and native input queue regression tests

The input/queue tests run the actual common-C worker with an in-memory sender.
They do not open sockets, access input devices, or connect to a Host.
Production builds leave `PLANK_BUILD_TESTS=OFF` (the default).

The clipboard ABI checks compile the same source as C11 and C++17. Enabling
tests therefore requires a C++ compiler; production builds remain C-only.
These two executables use headers only, without linking the common-C runtime.
All assertions are compile-time checks and remain active in Release builds.

From this repository, with `PLANK_ROOT` pointing to a current PLANK root checkout:

```sh
cmake -S . -B build/input-tests \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_SHARED_LIBS=OFF \
  -DPLANK_BUILD_TESTS=ON \
  -DPLANK_TRANSPORT_DIR="$PLANK_ROOT/protocol/plank-transport"
cmake --build build/input-tests --parallel
ctest --test-dir build/input-tests --output-on-failure
ctest --test-dir build/input-tests --repeat until-fail:100 --output-on-failure
```

For Clang AddressSanitizer/UndefinedBehaviorSanitizer, use a separate build
directory, add `-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++`, and set
both `CMAKE_C_FLAGS` and `CMAKE_CXX_FLAGS` to
`-fsanitize=address,undefined -fno-omit-frame-pointer`.

- `clipboard-wire-abi.c` / `.cpp`: the 32-byte packed header, every field
  offset, magic/version/flags, text/chunk limits, restoration of both caller
  and native packing, and the 32 + 8,160 = 8,192-byte input payload bound
  against the actual transport header. Run these with
  `ctest --test-dir build/input-tests -R clipboard-abi --output-on-failure`.
- `native-input-wire.c`: coordinates, button/key barriers, modifiers, scrolling,
  150-move saturation followed by releases, and pen tip/button/motion barriers.
  The original ordering/saturation fixture is from Christopher Noellert's
  PLANK integration PR #4 (`tests/session/native-input-wire.c`, blob
  `e3721d1ecc9562e36f601420d30a2f66e56b2a32`); it lives beside the input worker
  here so these regressions can be tested without a whole Client build.
- `queue-ownership.c`: empty, matching/nonmatching heads, barriers, full-queue
  replacement, link/count integrity, shutdown/drain, and concurrent replacement
  while the consumer inspects and acquires ownership.
- `native-input-replacement.c`: deterministic interleaving after the pen worker
  inspects a pending absolute position, while the producer replaces its holder
  and recycles it into a subsequent pen event. Both pen events and the absolute
  position must survive. Linux static-library tests use ELF linker wrappers,
  without runtime test hooks or changing any queue data.

The replacement fixture wraps only `LbqPollQueueElementIf` and requires
`absolute=1 pen=2`. Apple linkers do not support ELF `--wrap`; Mac package builds
run the portable wire/tool-barrier and concurrent queue-ownership fixtures
against their actual common-C library. Linux also runs the deterministic
replacement fixture. No runtime test hooks or historical worker are required.

Standalone tests are not full Linux/macOS Client or physical Wacom acceptance.
