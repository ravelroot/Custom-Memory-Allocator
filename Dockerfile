FROM debian:bookworm-slim AS native
RUN apt-get update && apt-get install -y --no-install-recommends build-essential cmake && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY include include
COPY src src
COPY tools tools
COPY tests tests
COPY traces traces
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"$(nproc)" && cd build && ctest --output-on-failure
RUN mkdir -p /out/lib /out/bin && cp -L build/libfavo.so build/libfavo_preload.so /out/lib/ && cp build/favo /out/bin/

FROM node:22-bookworm-slim AS web
WORKDIR /web
COPY web/package.json web/package-lock.json ./
RUN npm ci
COPY web ./
RUN npm run build

FROM python:3.13-slim-bookworm
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1 \
    FAVO_LIBRARY=/opt/favo/lib/libfavo.so \
    FAVO_WEB_DIR=/opt/favo/web \
    FAVO_DATABASE=/data/favo.db \
    FAVO_HOST=0.0.0.0 \
    FAVO_PORT=8080
RUN useradd --system --uid 10001 --home-dir /opt/favo favo && mkdir -p /data /opt/favo/lib /opt/favo/bin && chown favo /data
COPY api/requirements.txt /tmp/requirements.txt
RUN pip install --no-cache-dir -r /tmp/requirements.txt
COPY --from=native /out/lib/ /opt/favo/lib/
COPY --from=native /out/bin/ /opt/favo/bin/
COPY --from=web /web/dist /opt/favo/web
COPY api/favo_api /opt/favo/api/favo_api
COPY traces /opt/favo/traces
ENV PATH="/opt/favo/bin:${PATH}" PYTHONPATH=/opt/favo/api
USER favo
WORKDIR /opt/favo
EXPOSE 8080
VOLUME ["/data"]
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s CMD python -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8080/api/health')"
CMD ["python", "-m", "favo_api"]
