FROM gcc:15@sha256:eb5e64286ad47187b32bda75c6b6916166ecb41da56559b8b09e0001748d2cce
COPY . /app
WORKDIR /app
RUN g++ -O3 -std=c++17 -DNDEBUG -pthread -o /app/b_index /app/main.cpp
ENTRYPOINT ["/app/b_index"]
