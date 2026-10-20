# Changelog

All notable changes to this project are documented here. The format is based
on [Keep a Changelog](https://keepachangelog.com/), and the project aims to
follow [Semantic Versioning](https://semver.org/) from 1.0 onward.

## [Unreleased]

### Added
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
