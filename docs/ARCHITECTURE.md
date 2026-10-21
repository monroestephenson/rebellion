# Architecture

A scrape flows through these stages:

```
URL
 │  rbl_url_parse / resolve (src/url.c, libcurl URL API)
 ▼
SSRF guard  (src/net.c — private/loopback/link-local/CGNAT refused)
 │
 ▼
robots.txt gate  (src/robots.c — cached per origin for 1 h, path+query rules,
 │                Crawl-delay honored, capped at 30 s)
 │
 ▼
per-host rate limit  (src/client.c)
 │
 ├── HTTP engine (src/http.c, libcurl)
 │      ├─ redirects followed manually; every hop re-checked by the guard
 │      ├─ size cap, binary bodies rejected
 │      └─ charset detection + iconv → UTF-8 (src/charset.c)
 │
 └── Browser engine (src/browser.c)
        └─ launch Chromium → DevTools over raw-socket WebSocket →
           Page.navigate → poll readyState → outerHTML
 │
 ▼  (UTF-8 HTML)
HTML parse  (src/html.c): tokenizer + forgiving tree builder
 │             entity decode → UTF-8 (src/entities.c)
 ▼
 ├─ metadata: title / description / lang   (src/extract.c)
 ├─ main-content heuristic (readability-ish density scoring)
 └─ link collection (resolved, deduped)
 │
 ▼
HTML tree → Markdown  (src/markdown.c)
 │
 ▼
rbl_doc  (src/scrape.c) ──▶ Markdown | JSON | screenshot
```

## Engine selection (`RBL_ENGINE_AUTO`)

1. Fetch over HTTP.
2. If the response is non-HTML, return it verbatim.
3. Build a document from the HTML.
4. If it looks JS-gated (challenge markers, 403/429/503) or the extracted
   content is tiny, and Chromium is available, re-render with the browser.

The heuristic lives in `needs_browser()` in `src/scrape.c`.

## The HTML parser

Not a full HTML5 tree-construction algorithm — a pragmatic tokenizer plus a
stack-based tree builder (`src/html.c`) that:

- treats `script`/`style`/`textarea`/`title` as raw-text elements,
- knows the void elements,
- applies the common implicit-close rules (`<p>`, `<li>`, table cells, …),
- decodes entities in text and attribute values,
- caps nesting depth to stay safe on hostile input.

This keeps the code small while handling the messy markup real sites emit.

## The DevTools client

`src/browser.c` launches Chromium with `--remote-debugging-port=0`, reads the
chosen port from `DevToolsActivePort`, finds a page target via the
`/json/list` HTTP endpoint, then speaks the WebSocket protocol directly over a
localhost TCP socket (handshake + masked client frames + frame reassembly),
because the system libcurl is built without WebSocket support. Commands used:
`Page.enable`, `Runtime.enable`, `Page.navigate`, `Runtime.evaluate`
(readyState, outerHTML, location.href, the navigation's `responseStatus`),
`Page.captureScreenshot` and the `Fetch` domain.

`Fetch.enable` pauses every request the page makes; the client checks its host
against the SSRF guard and continues or fails it (`Fetch.requestPaused`), and
answers proxy authentication challenges with the configured credentials
(`Fetch.authRequired`) — Chromium ignores credentials in `--proxy-server`.

**Process lifecycle.** The forked child starts a new process group and
becomes a tiny supervisor: it starts Chromium as its own child and watches a "lifeline"
pipe whose write end only the library's process holds. If that process dies
for any reason, the pipe reports EOF and the supervisor stops Chromium,
deletes a throwaway profile and exits, so browsers are never orphaned. Live
browsers per client are bounded by `max_concurrent_browsers` (a condition
variable gate); sessions on a persistent profile are serialized.

## The API server

`src/server.c` is a thread-per-connection HTTP/1.1 server (one request per
connection) over a single shared `rbl_client`. It parses requests with a
bounded reader (header/body caps, a read deadline, chunked bodies,
`Expect: 100-continue`), decodes the JSON body with `src/jsonval.c`, and maps
library statuses to HTTP codes and stable `code` strings. See `docs/API.md`.

## Charset strategy

German legacy pages often declare ISO-8859-1/15 or Windows-1252, or declare
nothing. `src/charset.c` resolves the encoding from the Content-Type header,
then an HTML `<meta>` sniff, then falls back to a Latin-1 mapping if the bytes
are not valid UTF-8 — so text is never turned into mojibake. Numeric character
references in the 0x80–0x9F range are remapped the way browsers do.
