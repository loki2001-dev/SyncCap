# OpenAPI server image. build context: repository root
#   docker build -f server/Dockerfile -t synccap-server .
FROM ubuntu:24.04 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake ninja-build ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY server ./server
COPY 3rdparty/spdlog ./3rdparty/spdlog
COPY 3rdparty/nlohmann/include ./3rdparty/nlohmann/include
COPY 3rdparty/cpp_httplib ./3rdparty/cpp_httplib
COPY 3rdparty/paho_mqtt_c/paho.mqtt.c-1.3.16 ./3rdparty/paho_mqtt_c/paho.mqtt.c-1.3.16
COPY 3rdparty/libuv_src/libuv-1.51.0 ./3rdparty/libuv_src/libuv-1.51.0
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DSYNCCAP_BUILD_COLLECTOR=OFF -DSYNCCAP_BUILD_TESTS=OFF -DSYNCCAP_BUILD_SERVER=ON -DSYNCCAP_BUILD_DASHBOARD=OFF \
 && cmake --build build --target synccap_server

FROM ubuntu:24.04
RUN useradd --system --create-home --home-dir /app synccap
WORKDIR /app
COPY --from=build /src/build/synccap_server /app/synccap_server
COPY server/openapi.yaml /app/server/openapi.yaml
COPY config/server.json /app/config/server.json
# containers listen on all interfaces of their own network namespace; the host port mapping decides exposure
ENV SYNCCAP_SERVER_BIND=0.0.0.0 \
    SYNCCAP_MQTT_HOST=mosquitto \
    SYNCCAP_LOG_DIR=/app/logs
USER synccap
EXPOSE 8080
HEALTHCHECK --interval=15s --timeout=3s --retries=3 CMD ["/bin/sh", "-c", "exec 3<>/dev/tcp/127.0.0.1/8080"]
ENTRYPOINT ["/app/synccap_server", "/app/config/server.json"]
