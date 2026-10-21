# rebellion

**A Firecrawl-style web scraper, written in C.**

`rebellion` turns web pages into clean Markdown (plus structured metadata and
links), from a CLI, a small C library, and an HTTP+JSON API server that any
other service (Kotlin, Go, Python, ...) can call. It has two engines:

- **HTTP engine** — a fast [libcurl](https://curl.se/libcurl/) fetch with
  HTTP/2, brotli/gzip/zstd, a cookie jar, and browser-like headers.
- **Browser engine** — a real headless **Chromium** driven over the Chrome
  DevTools Protocol. It executes JavaScript, so single-page apps and pages
  that build their content (or a "please enable JavaScript" gate) in the
  browser render properly, and it returns the live DOM. It does not solve
  CAPTCHAs or other bot challenges (see *Responsible use*).

An **`auto`** mode fetches over HTTP first and escalates to the browser only
when the response looks JS-gated or empty — fast by default, robust when it
needs to be.

> rebellion is built with care for **niche German sites**. Many of them —
> Volksbank/Raiffeisenbank portals, registries, classifieds — still serve
> legacy encodings (ISO-8859-1 / ISO-8859-15 / Windows-1252) and localized
> markup. rebellion detects the charset from headers *and* `<meta>` tags,
> converts everything to UTF-8, and decodes HTML entities, so `ä ö ü ß €`
> come out intact.

---

## Features

- **Clean Markdown** output: headings, bold/italic, links (resolved to
  absolute URLs), images, ordered/unordered/nested lists, blockquotes,
  fenced code, and GitHub-style tables.
- **Main-content extraction**: a readability-style heuristic drops nav,
  headers, footers, asides, forms, scripts and styles and keeps the article
  body (toggle with `--full-page-content`).
- **Structured data**: title, description, language, and deduplicated
  absolute links — available as JSON.
- **Robust charset handling** for legacy German pages (→ UTF-8).
- **Headless-browser rendering** via the pre-detected Chromium, including
  optional PNG **screenshots**.
- **Interactive browser sessions**: drive a live page step by step — click,
  fill forms, submit, scroll, wait for elements, run JS — and read back the
  rendered DOM / Markdown / screenshots.
- **Persistent authenticated profiles**: reuse a Chromium user-data-dir so
  cookies and logins survive across runs.
- **Proxy rotation**: a configurable proxy pool (round-robin / random /
  sticky-per-host) applied to both the HTTP and browser engines.
- **HTTP API server** (`rebellion-server`): `/v1/scrape`, `/v1/crawl`,
  `/v1/map` with Firecrawl-style JSON, API keys, and a Docker image — see
  [docs/API.md](docs/API.md) and the [Kotlin client](examples/kotlin).
- **Crawling** (same-site BFS with depth/limit and include/exclude regex) and
  **mapping** (fast URL discovery from `sitemap.xml` + homepage links).
- **Polite by default**: obeys `robots.txt`, honors `Crawl-delay`, and
  rate-limits per host. (All overridable — see *Responsible use*.)
- **Safe by default**: refuses loopback/private/cloud-metadata targets
  (SSRF guard) on every redirect and every browser request.
- Small, dependency-light: just **libcurl** and the C library's `iconv`.

## Build

Requirements: a C11 compiler, CMake ≥ 3.16, and libcurl development headers.

```sh
# Debian/Ubuntu
sudo apt-get install -y build-essential cmake libcurl4-openssl-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build        # run the unit tests
```

The binaries are `build/rebellion` (CLI) and `build/rebellion-server` (HTTP
API); the static library is `build/librebellion.a`. Linux is the supported
platform.

Build options: `-DBUILD_SHARED_LIBS=ON` (shared library with SONAME),
`-DREBELLION_BUILD_TESTS=OFF`, `-DREBELLION_BUILD_CLI=OFF`,
`-DREBELLION_BUILD_SERVER=OFF`,
`-DREBELLION_WERROR=ON`, `-DREBELLION_HARDEN=ON` (default: stack protector,
`_FORTIFY_SOURCE`, RELRO/BIND_NOW), `-DREBELLION_FUZZ=ON` (see `fuzz/`).

### Installing & linking

```sh
cmake --install build --prefix /usr/local
```

This installs the headers, the library, a man page, a **pkg-config** file and
a **CMake package config**. Consume it either way:

```sh
# pkg-config
cc my.c $(pkg-config --cflags --libs librebellion) -o my
```

```cmake
# CMake
find_package(rebellion REQUIRED)
target_link_libraries(myapp PRIVATE rebellion::rebellion)
```

For the **browser engine** you also need a Chromium or Chrome binary.
rebellion looks for it automatically (`chromium`, `chromium-browser`,
`google-chrome`, and the Playwright layout at `/opt/pw-browsers/chromium`);
point `$REBELLION_CHROMIUM` (or `$CHROME_PATH`) at a binary to choose one.

## CLI usage

```sh
# Scrape a page to Markdown (auto engine)
rebellion scrape https://www.vr.de

# Force the headless browser (JavaScript-rendered pages / SPAs)
rebellion scrape https://www.mediamarkt.de --browser

# Structured JSON, written to a file
rebellion scrape https://www.volksbank.de --json -o page.json

# Keep the whole page instead of just the main content
rebellion scrape https://example.de --full-page-content

# Screenshot (implies the browser engine)
rebellion scrape https://example.de --screenshot shot.png

# Crawl one site, writing a Markdown file per page
rebellion crawl https://example.de --limit 25 --depth 2 --out-dir ./out

# Only follow certain paths
rebellion crawl https://example.de --include '/blog/' --exclude '/tag/'

# Discover URLs fast (sitemaps + homepage links)
rebellion map https://example.de

# Browser actions: interact before capturing (imply --browser, run in order)
rebellion scrape https://shop.de --click '#accept-cookies' --scroll-bottom
rebellion scrape https://app.de --fill '#q=laptop' --submit '#search-form' \
  --wait-for '.results'

# Proxy rotation + a persistent (authenticated) profile
rebellion scrape https://example.de \
  --proxy http://user:pass@p1:3128 --proxy http://user:pass@p2:3128 \
  --proxy-strategy sticky --profile ~/.rebellion/profile
```

Run `rebellion --help` for the full option list.

## HTTP API (for other services)

`rebellion-server` wraps the library in a small HTTP+JSON service, so a
Kotlin/Java, Go, Python or Node service can scrape without linking C:

```sh
rebellion-server --api-key "$KEY"            # listens on 127.0.0.1:8080

curl -sS http://127.0.0.1:8080/v1/scrape \
  -H "Authorization: Bearer $KEY" -H 'Content-Type: application/json' \
  -d '{"url":"https://www.berliner-volksbank.de/"}'
# {"success":true,"data":{"url":"...","statusCode":200,"engine":"http",
#   "title":"Privatkunden - Berliner Volksbank","language":"de","markdown":"...","links":[...]}}
```

Endpoints: `POST /v1/scrape` (with optional browser `actions` and
`screenshot`), `POST /v1/crawl`, `POST /v1/map`, `GET /health`. Errors carry
a machine-readable `code` (`blocked`, `robots_denied`, `timeout`, `busy`,
...). The full reference is in [docs/API.md](docs/API.md).

From Kotlin, with the client in [`examples/kotlin`](examples/kotlin)
(`java.net.http` + kotlinx.serialization, no other dependencies):

```kotlin
val client = RebellionClient("http://rebellion:8080", apiKey = System.getenv("REBELLION_API_KEY"))
val doc = client.scrape(ScrapeRequest("https://www.volksbank.de/"))
println("${doc.title}: ${doc.markdown.length} chars of Markdown")
```

### Docker

```sh
docker build -t rebellion .
docker run --rm -p 8080:8080 -e REBELLION_API_KEY=change-me rebellion
```

The image (Debian slim + Chromium, ~300 MB compressed) runs the server as an
unprivileged user under `tini`, listens on `$PORT` (default 8080) and has a
`/health` healthcheck.

### Engines

| Mode              | Flag        | What it does                                        |
|-------------------|-------------|-----------------------------------------------------|
| `auto` (default)  | `--engine auto` | HTTP first; escalate to the browser if gated/empty |
| `http`            | `--http`    | libcurl only — fastest, no JavaScript               |
| `browser`         | `--browser` | headless Chromium — executes JavaScript              |

## Library usage

```c
#include <rebellion/rebellion.h>
#include <stdio.h>

int main(void) {
    rbl_client *c = rbl_client_new(NULL);            /* sensible defaults */

    rbl_scrape_opts o;
    rbl_scrape_opts_init(&o);
    o.engine = RBL_ENGINE_AUTO;

    rbl_status st;
    rbl_doc *d = rbl_scrape_ex(c, "https://www.vr.de", &o, &st);
    if (d) {
        printf("title: %s\n", rbl_doc_title(d));
        printf("lang:  %s\n", rbl_doc_language(d));
        puts(rbl_doc_markdown(d));
        rbl_doc_free(d);
    } else {
        fprintf(stderr, "failed: %s\n", rbl_strerror(st));
    }
    rbl_client_free(c);
    return 0;
}
```

Compile and link against the build tree:

```sh
cc my.c -Iinclude -Lbuild -lrebellion -lcurl -lpthread -o my
```

See [`examples/`](examples/) for a complete program, and
[`include/rebellion/rebellion.h`](include/rebellion/rebellion.h) for the full
API (scrape, crawl, map, JSON serialization, screenshots).

### Interactive sessions, proxies & authenticated profiles

```c
/* A persistent profile keeps cookies/logins across runs. */
rbl_client_config cfg;
rbl_client_config_init(&cfg);
cfg.browser_profile_dir = "/var/lib/myapp/profile";
rbl_client *c = rbl_client_new(&cfg);

/* A rotating proxy pool (used by both engines). */
const char *pool[] = {"http://user:pass@p1:3128", "socks5h://p2:1080"};
rbl_client_set_proxies(c, pool, 2, RBL_PROXY_STICKY_HOST);

/* Drive a live page: search a form, wait, capture clean Markdown. */
rbl_status st;
rbl_session *s = rbl_session_open(c, NULL, &st);
rbl_session_goto(s, "https://app.example/search", -1);
rbl_session_fill(s, "input[name=q]", "laptops");
rbl_session_submit(s, "form#search");
rbl_session_wait_for(s, ".results", 10000);
rbl_session_scroll_to_bottom(s);

rbl_doc *d = rbl_session_scrape(s, NULL);     /* markdown/links/metadata */
puts(rbl_doc_markdown(d));
rbl_doc_free(d);
rbl_session_close(s);
rbl_client_free(c);
```

## How it works

```
 URL ──▶ robots.txt check ──▶ rate-limit ──▶ ┌─ HTTP engine (libcurl) ─┐
                                             │  charset → UTF-8        │
                                             └─ Browser engine (CDP) ──┘
                                                        │
                                                 rendered HTML
                                                        │
                        tokenizer + forgiving tree builder (src/html.c)
                                                        │
                      ┌──────────────┬──────────────────┴───────────┐
                   metadata     main-content heuristic          link set
                      │              │                               │
                      └────────▶ HTML → Markdown (src/markdown.c) ◀──┘
```

Everything is plain C11. The parser is a pragmatic tokenizer + a forgiving
tree builder (it copes with unclosed `<p>`/`<li>`, raw `<script>`/`<style>`,
void elements, comments and entities) rather than a full HTML5 state machine,
which keeps it small while handling real-world markup. The DevTools client
speaks WebSocket over a raw localhost socket, so there is no dependency on a
WebSocket-enabled libcurl build.

## Production notes

**Thread safety.** A `rbl_client` is safe to share across threads: libcurl's
shared cookie/DNS/TLS state is protected by per-class mutexes, global init
runs once under `pthread_once`, and per-host rate-limiting is locked. Create
one client and reuse it; each `rbl_scrape`/`rbl_crawl` call uses its own curl
easy handle. (A single `rbl_doc` is not meant to be mutated from two threads
at once, but distinct calls returning distinct docs are fine.)

**Security / SSRF.** By default rebellion refuses to fetch loopback, private
(RFC1918 / ULA), link-local (including the `169.254.169.254` cloud-metadata
address), CGNAT and unspecified/multicast targets — covering literal hosts
(including spellings like `127.1` or `localhost.`), DNS names that resolve
into those ranges, every redirect hop, and every request the headless
browser makes (`RBL_ERR_BLOCKED`).
Transfers are restricted to `http`/`https` so a redirect cannot pivot to
`file://`. TLS verification is always on. Set `allow_private_networks`
(`--allow-private`) to scrape internal hosts you control. See
[SECURITY.md](SECURITY.md).

**Resource bounds.** Response bodies are capped (`max_response_bytes`,
default 32 MiB → `RBL_ERR_TOO_LARGE`), DevTools frames are bounded, and
parser nesting is limited, so hostile input can't exhaust memory or blow the
stack. Binary responses (PDF, images) are rejected with
`RBL_ERR_UNSUPPORTED_CONTENT` instead of being parsed. Per-request and
connect timeouts, redirect caps and transient-failure retries with backoff
are configurable.

**Browsers and memory.** At most `max_concurrent_browsers` (default 4)
headless Chromium instances run at once per client; further requests wait
and then fail with `RBL_ERR_BUSY`. Each browser is started under a tiny
supervisor process that kills it (and removes its throwaway profile) if your
process exits or crashes, so no orphaned Chromium is left behind.

**How much RAM does it use?** Measured on Linux x86-64:

| What                                          | Memory |
|-----------------------------------------------|--------|
| `rebellion-server`, idle                      | ~13 MB RSS |
| `rebellion-server`, 16 concurrent HTTP scrapes | < 30 MB RSS peak |
| one headless Chromium render (Volksbank homepage) | ~325 MB PSS (heavy pages: up to ~700 MB) |
| browser supervisor                            | ~1 MB |

So the HTTP engine is nearly free, and memory is dominated by browsers: plan
for roughly `50 MB + max_browsers × 500 MB`.

## Responsible use

rebellion is a tool for **authorized** data collection: content you own,
public pages you may access, APIs and sites whose terms permit it, research
and archiving within the law.

- `robots.txt` is **respected by default**. `--no-robots` exists for cases
  where you have permission that the file does not express (e.g. your own
  staging site); use it deliberately.
- The browser engine renders pages the way a normal browser does; it does
  **not** solve CAPTCHAs or bot challenges, defeat login walls, or bypass
  access controls, and you should not use it to. If a site blocks automated
  access, ask the operator for access or an API.
- Be polite: keep the default per-host delay (or raise it with `--delay`),
  honor `Crawl-delay`, and don't hammer a site.
- Respect each site's terms of service, applicable law (incl. the GDPR for
  personal data), and copyright. You are responsible for how you use it.

## Project layout

```
include/rebellion/rebellion.h   public API
src/
  util.c        buffers, string helpers, hash set, base64, timing
  client.c      client lifecycle, Chromium detection, rate limiting,
                proxy pool, browser concurrency gate
  url.c         URL parse/resolve (libcurl URL API)
  net.c         SSRF guard: private-address classification
  charset.c     charset detection + iconv conversion to UTF-8
  entities.c    HTML entity decoding → UTF-8
  html.c        tokenizer + forgiving DOM tree builder
  extract.c     metadata, main-content heuristic, link collection
  markdown.c    HTML tree → GitHub-flavored Markdown
  http.c        libcurl fetch
  browser.c     headless Chromium via DevTools Protocol (raw-socket WS)
  robots.c      robots.txt fetch/parse/eval + crawl-delay
  json.c        JSON escaping + shallow parsing
  jsonval.c     JSON parser for API requests
  scrape.c      orchestration, rbl_doc, JSON serialization
  crawl.c       crawl (BFS) + map (sitemap/link discovery)
  main.c        CLI
  server.c      rebellion-server (HTTP+JSON API)
tests/          unit + hermetic end-to-end tests (local origin and proxy),
                server_smoke.sh for the API
fuzz/           libFuzzer targets (HTML/charset/links, JSON, robots.txt)
examples/       C library example, Kotlin API client
docs/           API reference, architecture, man pages
Dockerfile      rebellion-server + Chromium image
```

## License

MIT — see [LICENSE](LICENSE).
