# Образ bakery: сборка в debian:trixie, запуск в debian:trixie-slim от непривилегированного пользователя.
# База, резервные копии и всё состояние - в томе /data. Пример запуска: docker-compose.yml.
FROM debian:trixie AS build
RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential cmake libsqlite3-dev libssl-dev nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX=/usr \
    && cmake --build /build -j"$(nproc)" \
    && DESTDIR=/out cmake --install /build --strip

FROM debian:trixie-slim
# tzdata: время документов и копий по расписанию берётся из TZ
RUN apt-get update \
    && apt-get install -y --no-install-recommends libsqlite3-0 libssl3t64 tzdata \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --home-dir /data --shell /usr/sbin/nologin bakery \
    && install -d -o bakery -g bakery -m 0750 /data
COPY --from=build /out/usr/bin/bakery /usr/bin/bakery
COPY --from=build /out/usr/share/bakery /usr/share/bakery
COPY --from=build /out/usr/share/doc/bakery /usr/share/doc/bakery
USER bakery
VOLUME /data
EXPOSE 8080
ENTRYPOINT ["/usr/bin/bakery", "--db", "/data/bakery.db", "--host", "0.0.0.0", "--port", "8080"]
