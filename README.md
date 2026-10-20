# rebellion

**A Firecrawl-style web scraper, written in C.**

`rebellion` turns web pages into clean Markdown (plus structured metadata and
links), from a single CLI binary and a small C library. It has two engines:

- **HTTP engine** — a fast [libcurl](https://curl.se/libcurl/) fetch with
  HTTP/2, brotli/gzip/zstd, a cookie jar, and browser-like headers.
- **Browser engine** — a real headless **Chromium** driven over the Chrome
  DevTools Protocol. Because it is an actual browser, it executes JavaScript
  and transparently clears the interstitial JS challenges that front many
  sites (e.g. Cloudflare's "checking your browser" page), returning the live,
  rendered DOM.

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
- **Crawling** (same-site BFS with depth/limit and include/exclude regex) and
  **mapping** (fast URL discovery from `sitemap.xml` + homepage links).
- **Polite by default**: obeys `robots.txt`, honors `Crawl-delay`, and
  rate-limits per host. (All overridable — see *Responsible use*.)
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

The binary is `build/rebellion`; the static library is `build/librebellion.a`.

For the **browser engine** you also need a Chromium or Chrome binary.
rebellion looks for it automatically (including the Playwright layout at
`/opt/pw-browsers/chromium`); override with `$REBELLION_CHROMIUM` or
`--browser` plus a path via `$REBELLION_CHROMIUM`.

## CLI usage

```sh
# Scrape a page to Markdown (auto engine)
rebellion scrape https://www.vr.de

# Force the headless browser (handles JS challenges / SPAs)
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
```

Run `rebellion --help` for the full option list.

### Engines

| Mode              | Flag        | What it does                                        |
|-------------------|-------------|-----------------------------------------------------|
| `auto` (default)  | `--engine auto` | HTTP first; escalate to the browser if gated/empty |
| `http`            | `--http`    | libcurl only — fastest, no JavaScript               |
| `browser`         | `--browser` | headless Chromium — executes JS, clears JS gates    |

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

Compile and link:

```sh
cc my.c -Ibuild/../include -Lbuild -lrebellion -lcurl -o my
```

See [`examples/`](examples/) for a complete program, and
[`include/rebellion/rebellion.h`](include/rebellion/rebellion.h) for the full
API (scrape, crawl, map, JSON serialization, screenshots).

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

## Responsible use

rebellion is a tool for **authorized** data collection: content you own,
public pages you may access, APIs and sites whose terms permit it, research
and archiving within the law.

- `robots.txt` is **respected by default**. `--no-robots` exists for cases
  where you have permission that the file does not express (e.g. your own
  staging site); use it deliberately.
- The browser engine clears *automated interstitial JS challenges* the way
  any real browser does; it does **not** solve CAPTCHAs, defeat login walls,
  or bypass access controls, and you should not use it to.
- Be polite: keep the default per-host delay (or raise it with `--delay`),
  honor `Crawl-delay`, and don't hammer a site.
- Respect each site's terms of service, applicable law (incl. the GDPR for
  personal data), and copyright. You are responsible for how you use it.

## Project layout

```
include/rebellion/rebellion.h   public API
src/
  util.c        buffers, string helpers, hash set, base64, timing
  client.c      client lifecycle, Chromium detection, rate limiting
  url.c         URL parse/resolve (libcurl URL API)
  charset.c     charset detection + iconv conversion to UTF-8
  entities.c    HTML entity decoding → UTF-8
  html.c        tokenizer + forgiving DOM tree builder
  extract.c     metadata, main-content heuristic, link collection
  markdown.c    HTML tree → GitHub-flavored Markdown
  http.c        libcurl fetch
  browser.c     headless Chromium via DevTools Protocol (raw-socket WS)
  robots.c      robots.txt fetch/parse/eval + crawl-delay
  json.c        JSON escaping + shallow parsing
  scrape.c      orchestration, rbl_doc, JSON serialization
  crawl.c       crawl (BFS) + map (sitemap/link discovery)
  main.c        CLI
tests/          unit tests (markdown, charset, entities, url, robots, html)
```

## License

MIT — see [LICENSE](LICENSE).
