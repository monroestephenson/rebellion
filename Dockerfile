# rebellion-server: the HTTP+JSON scraping API, with headless Chromium.
#
#   docker build -t rebellion .
#   docker run --rm -p 8080:8080 -e REBELLION_API_KEY=change-me rebellion
#
# Size the container for its browsers: ~15 MB for the server plus a few
# hundred MB per concurrent Chromium (see --max-browsers, default 4).

FROM debian:bookworm-slim AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential cmake libcurl4-openssl-dev ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DREBELLION_BUILD_TESTS=OFF \
 && cmake --build build -j"$(nproc)"

FROM debian:bookworm-slim
# chromium: the browser engine. tini: PID 1 that reaps Chromium's helper
# processes. curl: the HEALTHCHECK. Fonts so screenshots render text.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      libcurl4 ca-certificates chromium tini curl \
      fonts-liberation fonts-dejavu-core \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --create-home --uid 10001 rebellion
COPY --from=build /src/build/rebellion /src/build/rebellion-server /usr/local/bin/
ENV REBELLION_CHROMIUM=/usr/bin/chromium \
    PORT=8080
USER rebellion
WORKDIR /home/rebellion
EXPOSE 8080
# Change the port with -e PORT=..., not --port, so the healthcheck follows.
HEALTHCHECK --interval=30s --timeout=5s --start-period=5s \
  CMD curl -fsS --noproxy '*' "http://127.0.0.1:${PORT}/health" || exit 1
ENTRYPOINT ["/usr/bin/tini", "--", "rebellion-server"]
CMD ["--host", "0.0.0.0"]
