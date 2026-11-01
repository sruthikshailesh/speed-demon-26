
# Stage 1: Compile
FROM gcc:15@sha256:eb5e64286ad47187b32bda75c6b6916166ecb41da56559b8b09e0001748d2cce AS builder

WORKDIR /app

COPY src/main.cpp /app/main.cpp

RUN g++ -O3 -std=c++17 -DNDEBUG -pthread -static -s \
    -o /app/b_index /app/main.cpp

# Stage 2: Runtime
FROM debian:13-slim@sha256:62bd423aeee912639ca8ee681c2b762cb3e25137e0a92b20be455898a9f034ad

COPY --from=builder /app/b_index /app/b_index

ENTRYPOINT ["/app/b_index"]
