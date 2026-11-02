# PLANK Host protocol headers

This Host-only branch retains the protocol declarations consumed by the Linux
Host: `src/Limelight.h` and `src/plank.h`.

There is no compiled library, transport implementation or recursive dependency.
The CMake target `plank_host_protocol` is header-only. PLANK's native KyProto
transport handles network traffic; do not restore the retired GameStream
transport, ENet or Reed-Solomon implementations here.

The shared Client uses its separate maintained branch. Do not substitute this
Host-only header tree for the Client's active common-C implementation.

Retained headers preserve their original wire definitions and upstream license.
Historical builds are not a supported maintenance requirement. Git history is
retained for attribution, not as a reason to keep unused code in current builds.
