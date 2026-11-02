# PLANK shared Client core

This maintained fork supplies the desktop Clients' input queue, media frame
assembly, audio timestamps, callback lifecycle and platform threading helpers.
Networking and authenticated encryption belong to PLANK's native Kyber/QUIC
transport. There is no ENet, GameStream socket implementation, MbedTLS or
OpenSSL dependency in this library. The parent Client still needs TLS for its
HTTPS control connection.

The transport must be connected before `LiStartConnection`. That function
starts the negotiated media/input workers; it does not resolve names or probe
network reachability again. See the parent repository's `protocol/` directory
for the current wire contracts and supported platforms.

Configure the component tests with `PLANK_BUILD_TESTS=ON` and
`PLANK_TRANSPORT_DIR` pointing at the parent's `protocol/plank-transport`.
The parent repository owns application packaging and hardware qualification.

This project derives from Moonlight common-C. Upstream attribution, license
notices and Git history are retained; the former GameStream behavior is not
part of the PLANK product.
