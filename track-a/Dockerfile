# Speed Demon 2026 — Track A (Ingest)
# Build offline: docker build --network=none -t speed-demon-track-a .
# Both images are on the official allowlist and pinned by digest.

FROM gcc:15@sha256:eb5e64286ad47187b32bda75c6b6916166ecb41da56559b8b09e0001748d2cce AS build

WORKDIR /build
COPY src/main.cpp ./main.cpp

# Fully static: the runtime image needs no compiler, no libstdc++, no network.
RUN g++ -O3 -std=c++17 -DNDEBUG -static -s -o /build/solution /build/main.cpp


FROM debian:13-slim@sha256:62bd423aeee912639ca8ee681c2b762cb3e25137e0a92b20be455898a9f034ad

COPY --from=build /build/solution /app/solution

WORKDIR /app
USER 65534:65534
ENTRYPOINT ["/app/solution"]
