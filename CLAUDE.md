# CLAUDE.md

Guidance for working in this repository.

## What this is

`rebellion` is a Firecrawl-style web scraper in C: a CLI (`rebellion`), an
HTTP+JSON API server (`rebellion-server`, see `docs/API.md`) and a static
library (`librebellion.a`) that turn web pages into clean Markdown plus
structured metadata/links. Two engines: a libcurl HTTP fetch and a headless
Chromium renderer driven over the DevTools Protocol.

## Build / test / run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure      # unit + e2e + API smoke tests

./build/rebellion scrape <url>                   # try it
./build/rebellion scrape <url> --browser         # headless Chromium path
```

Sanitizer build (what CI runs):

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -g -O1"
cmake --build build-asan -j && ctest --test-dir build-asan
```

CI (`.github/workflows/ci.yml`) builds with `-DREBELLION_WERROR=ON` on gcc and
clang, so keep the tree warning-clean under `-Wall -Wextra -Wshadow`.

## Conventions

- C11. Public API lives in `include/rebellion/rebellion.h` and is prefixed
  `rbl_`; internal headers sit next to their `.c` in `src/`.
- Ownership is explicit: functions that return heap memory say so; the caller
  frees. The growable `rbl_buf` and `rbl_strset` helpers are in `src/util.c`.
- Everything is UTF-8 internally. New parsing paths must go through
  `rbl_to_utf8` / the entity decoder so German legacy encodings keep working.
- Add a unit test under `tests/` for any new pure function (see the existing
  `test_*.c`; they register via `tests/test.h`).

## Module map

See the "Project layout" section of `README.md`. Orchestration is in
`src/scrape.c` (`rbl_scrape_ex`), crawling/mapping in `src/crawl.c`.

Interactive browser sessions (`rbl_session_*`: goto/click/fill/submit/scroll/
wait/eval/screenshot) live in `src/browser.c`; the one-shot `rbl_browser_render`
is built on top of them. Proxy-pool selection is in `src/client.c`
(`rbl_client_pick_proxy`), wired into `src/http.c` (CURLOPT_PROXY) and the
browser launch (`--proxy-server`). Persistent profiles come from
`browser_profile_dir` (not deleted on close). Each Chromium runs under a
small supervisor (`supervise_chromium` in `src/browser.c`) that kills it if
our process dies; code in that forked child must stay async-signal-safe.
The SSRF guard lives in `src/net.c` and is applied in `src/http.c` (every
redirect hop), `src/scrape.c`/`src/crawl.c` and the browser's Fetch
interception. The API server is `src/server.c`; its contract is tested by
`tests/server_smoke.sh`. Out of scope by policy: CAPTCHA/anti-bot solving.

## Notes

- The headless engine needs a Chromium binary; detection is in `src/client.c`
  (`$REBELLION_CHROMIUM` overrides). The DevTools client is a raw-socket
  WebSocket in `src/browser.c` because the system libcurl has no WS support.
- Be conservative with the network politeness defaults (`robots.txt`,
  per-host delay). They are deliberate — see "Responsible use" in the README.
