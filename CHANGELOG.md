# Changelog

All notable changes to this project are documented here. The format is based
on [Keep a Changelog](https://keepachangelog.com/), and the project aims to
follow [Semantic Versioning](https://semver.org/) from 1.0 onward.

## [Unreleased]

### Added
- **HTTP API server** (`rebellion-server`): `POST /v1/scrape` (with browser
  `actions` and screenshots), `/v1/crawl`, `/v1/map`, `GET /health`;
  Firecrawl-style JSON, API keys (`--api-key` / `$REBELLION_API_KEY`),
  machine-readable error codes, bounded connections/bodies, graceful drain on
  SIGTERM, `$PORT` support. Reference: `docs/API.md`.
- **Docker image** (Debian slim + Chromium, non-root, `tini`, healthcheck).
- **Kotlin client example** (`examples/kotlin`), exercised in CI against a
  live server.
- Browser **supervisor**: Chromium is started under a tiny watchdog that
  kills it and removes its throwaway profile if the owning process exits or
  crashes, so no orphaned browsers are left behind.
- `max_response_bytes`, `max_concurrent_browsers` (→ `RBL_ERR_BUSY`),
  `RBL_ERR_TOO_LARGE`, `RBL_ERR_UNSUPPORTED_CONTENT` for binary bodies.
- `rbl_session_status_code`, `rbl_session_last_error`, `rbl_session_capture`.
- CLI: `--full-page` (full-page screenshots), strict validation of numeric
  options and `--proxy`/`--proxy-strategy`.
- Tests: end-to-end runs through a local authenticating proxy (rotation,
  stickiness, 407s, SSRF), persistent-profile cookies, the browser gate, SSRF
  classification unit tests; fuzzers for robots.txt and charset/link paths.

### Changed
- The API server sends no CORS headers unless `--cors-origin` is given,
  requires `Content-Type: application/json`, and — when bound to loopback
  without an API key — rejects requests for non-loopback `Host` names
  (DNS rebinding).
- Redirects are followed manually so the SSRF guard vets every hop; with an
  environment proxy, targets are resolved and checked locally per hop.
- The SSRF guard also recognizes `localhost.`/`127.0.0.1.` and legacy IPv4
  spellings (`127.1`, `0x7f.1`, `2130706433`).
- robots.txt matching uses path + query, Allow wins ties (RFC 9309), and the
  cache is per origin with a one-hour TTL; `map` reads `Sitemap:` lines and
  keeps non-default ports.
- `map` sitemap/homepage fetches respect the per-host delay.
- Links are cleaned like a browser does before resolving (surrounding
  whitespace, line-wrapped hrefs, backslashes).
- Chromium launch waits up to 45 s for DevTools (cold containers).

### Fixed
- **Main-content extraction dropped headlines and intros.** Every `<header>`
  and `<footer>` was treated as site chrome, so a product page's
  `<header><h1>…</h1><h2>…</h2><p>intro</p></header>` inside `<main>` vanished
  (while its image and CTA survived), as did footnotes in a content
  `<footer>`. Header/footer now count as chrome only at page level (the ARIA
  banner/contentinfo rule), ARIA `banner`/`contentinfo`/`navigation` roles
  are honored, and when the best text block does not contain the page's
  `<h1>`, the headline block in front of it is kept too (not when it is
  mostly links, and never a linked logo).
- `$REBELLION_CHROMIUM` / `$CHROME_PATH` were ignored unless a path was also
  passed in the config; an explicitly configured path is now authoritative.
- Browser engine: real HTTP status codes, Chrome error pages are no longer
  returned as content, proxy credentials work via the DevTools Fetch domain.
- `rebellion map --json` now escapes URLs (valid JSON for any URL).
- Markdown: no stack overflow on deep `<pre>`/`<code>` nesting; inline code
  and fences sized to their content.
- JSON request parser: no over-reads on truncated escapes.
- `crawl` reports why the seed failed and rejects invalid regexes.

### Added (sessions, proxies, profiles)
- **Interactive browser sessions** (`rbl_session_*`): open a long-lived
  headless-browser session and drive it — `goto`, `click`, `fill`, `submit`,
  `scroll`/`scroll_to_bottom`, `wait_for`, `wait_ms`, `eval` — then read back
  the live DOM (`rbl_session_html`), a screenshot, or a full document
  (`rbl_session_scrape`). CLI: ordered `--wait-for/--click/--fill/--submit/
  --scroll-bottom/--wait` actions on `scrape`.
- **Persistent authenticated profiles**: `browser_profile_dir` /
  `--profile <dir>` reuses a Chromium user-data-dir so cookies and logins
  persist across runs (throwaway profiles are still the default and are
  cleaned up).
- **Proxy rotation**: `rbl_client_set_proxies()` installs a pool with
  round-robin / random / sticky-per-host selection, applied to both the HTTP
  engine (`CURLOPT_PROXY`) and the browser (`--proxy-server`). CLI: repeatable
  `--proxy` plus `--proxy-strategy`.
- The one-shot `rbl_browser_render` is now implemented on top of sessions.

### Fixed
- base64 decode no longer relies on signed-integer overflow (UBSan clean).
- Chromium no longer forces loopback targets through the configured proxy, so
  local/internal hosts render correctly.

### Added (earlier in this cycle)
- **Security / SSRF guard** (on by default): refuse loopback, private,
  link-local, CGNAT and unspecified/multicast targets — both literal hosts
  and DNS names that resolve into them (via a libcurl pre-request hook that
  also covers redirects). Configurable with `allow_private_networks`
  (`--allow-private`). New error `RBL_ERR_BLOCKED`.
- **Protocol allow-list**: transfers and redirects restricted to http/https.
- **Thread safety**: `rbl_client` is now safe to share across threads —
  libcurl share locks are wired with per-data-class mutexes, global init runs
  under `pthread_once`, and the browser's CDP message-id counter is
  per-connection.
- **Packaging**: installs a pkg-config file (`librebellion.pc`, relocatable),
  a CMake package config (`find_package(rebellion)` → `rebellion::rebellion`),
  a man page, and supports `-DBUILD_SHARED_LIBS=ON` with a proper SONAME.
- **Hardening build flags** (`-fstack-protector-strong`,
  `-fstack-clash-protection`, `_FORTIFY_SOURCE=2`, RELRO/BIND_NOW) via
  `REBELLION_HARDEN` (default on).
- **Hermetic integration test**: an in-process HTTP server exercises scrape,
  charset conversion, robots.txt, redirects, link extraction, map, crawl and
  the SSRF guard without touching the network.
- **Fuzzing**: a libFuzzer target over the HTML parser + Markdown renderer
  (`fuzz/`), behind `-DREBELLION_FUZZ=ON`.
- Docs: `SECURITY.md`, `CONTRIBUTING.md`, `docs/ARCHITECTURE.md`, man page,
  `.clang-format`, `.editorconfig`.

### Changed
- Removed all `system()` shell-outs: the browser profile is cleaned with
  `nftw`, and the CLI creates `--out-dir` with a native `mkdir -p`.
- `crawl` now adopts the host it actually lands on when the seed redirects to
  a different domain.

## [0.1.0] — initial

- CLI (`rebellion`) + library (`librebellion`) with `scrape`, `crawl`, `map`.
- HTTP engine (libcurl) and headless-Chromium engine (DevTools Protocol over
  a raw-socket WebSocket), plus an `auto` mode.
- Pragmatic HTML parser, readability-style main-content extraction, and an
  HTML→Markdown converter.
- German-aware charset handling (ISO-8859-1/15, Windows-1252 → UTF-8) and
  HTML entity decoding.
- robots.txt parsing with Crawl-delay, per-host rate limiting, JSON output,
  screenshots.
- Unit tests, CMake build, GitHub Actions CI.
