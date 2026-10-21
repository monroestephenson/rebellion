# rebellion HTTP API

`rebellion-server` exposes the scraper as a small HTTP+JSON service, so any
language can use it: Kotlin/Java, Go, Python, Node, a shell script with
`curl`. The request and response shapes follow Firecrawl's (`url`,
`onlyMainContent`, `markdown`, `links`, ...).

```sh
rebellion-server --api-key "$KEY"                         # 127.0.0.1:8080
docker run -p 8080:8080 -e REBELLION_API_KEY="$KEY" rebellion   # see Dockerfile
```

| Method | Path         | Purpose                                   | Auth |
|--------|--------------|-------------------------------------------|------|
| GET    | `/health`    | liveness + whether a browser is available | no   |
| POST   | `/v1/scrape` | one page → Markdown, metadata, links      | yes  |
| POST   | `/v1/crawl`  | breadth-first crawl of one site           | yes  |
| POST   | `/v1/map`    | fast URL discovery (sitemaps + links)     | yes  |

## Conventions

- **Auth.** When the server has an API key (`--api-key` or
  `$REBELLION_API_KEY`), send `Authorization: Bearer <key>` or
  `X-API-Key: <key>`. `/health` never needs it.
- **Requests** are `POST` with `Content-Type: application/json` (anything
  else is `415`). Unknown fields are ignored.
- **Responses** are JSON. Success: `{"success": true, ...}`. Failure:
  `{"success": false, "code": "<machine code>", "error": "<message>"}` with a
  matching HTTP status (table below).
- **The target's status is data, not an error.** Scraping a page that answers
  `404` returns HTTP `200` with `"statusCode": 404` in the document.
- **One request per connection** (`Connection: close`). Standard HTTP clients
  handle this transparently.
- Scrapes can take a while (a browser render is typically 1–5 s; a crawl is
  limited by the per-host politeness delay). Use a client timeout of a few
  minutes for crawls.

## GET /health

```json
{"status":"ok","service":"rebellion","version":"0.1.0","browser":true}
```

`browser` is `false` when no Chromium was found; requests needing it then fail
with `501 unsupported`.

## POST /v1/scrape

```sh
curl -sS http://127.0.0.1:8080/v1/scrape \
  -H "Authorization: Bearer $KEY" -H 'Content-Type: application/json' \
  -d '{"url":"https://www.berliner-volksbank.de/","engine":"auto"}'
```

| Field             | Type    | Default | Notes |
|-------------------|---------|---------|-------|
| `url`             | string  | —       | required; `http`/`https` only |
| `engine`          | string  | `auto`  | `auto` (HTTP, escalate to the browser when the page looks JS-gated or empty), `http`, `browser` |
| `onlyMainContent` | bool    | `true`  | keep the article body and its headline; drop site navigation, the site header/footer and asides |
| `includeHtml`     | bool    | `false` | add the cleaned HTML as `html` |
| `includeLinks`    | bool    | `true`  | absolute, de-duplicated links |
| `respectRobots`   | bool    | `true`  | obey robots.txt (`403 robots_denied` otherwise) |
| `timeoutMs`       | number  | 30 s HTTP / 45 s browser | per-URL budget, clamped to 1000–300000 |
| `waitAfterLoadMs` | number  | 800     | browser: extra settle time after load, 0–60000 |
| `userAgent`       | string  | Chrome  | no control characters |
| `acceptLanguage`  | string  | `de-DE,de;q=0.9,en;q=0.8` | |
| `screenshot`      | bool    | `false` | PNG as a `data:` URL (browser) |
| `fullPage`        | bool    | `false` | full-page instead of viewport screenshot |
| `actions`         | array   | —       | browser actions run in order before capture (implies the browser) |

**Actions** (at most 50). Each is an object with a `type`:

| `type`          | Fields                       | Effect |
|-----------------|------------------------------|--------|
| `click`         | `selector`                   | click the first match |
| `fill`          | `selector`, `value`          | type into an input/textarea |
| `submit`        | `selector`                   | submit the form (or the form of a field/button) |
| `wait_for`      | `selector`, `ms` (optional)  | wait until the selector matches (default: request timeout) |
| `scroll`        | `x`, `y` (default `0`, `600`) | scroll by an offset |
| `scroll_bottom` | —                            | scroll to the bottom (lazy loading) |
| `wait`          | `ms` (0–60000)               | pause |

Any action may carry `"optional": true` (e.g. a cookie banner that is not
always shown): its failure is then ignored. Otherwise a failing action ends
the request with `422 action_failed`. A navigation that the private-network
guard stops (e.g. a click that leads to `http://10.0.0.1/`) is always fatal:
`403 blocked`.

```json
{"url": "https://shop.example/",
 "actions": [
   {"type": "click", "selector": "#accept-cookies", "optional": true},
   {"type": "fill", "selector": "input[name=q]", "value": "Laptop"},
   {"type": "submit", "selector": "form#search"},
   {"type": "wait_for", "selector": ".results", "ms": 10000}
 ],
 "screenshot": true}
```

**Response**

```json
{"success": true,
 "data": {
   "url": "https://www.berliner-volksbank.de/",
   "statusCode": 200,
   "engine": "http",
   "title": "Privatkunden - Berliner Volksbank",
   "description": "...",
   "language": "de",
   "markdown": "# ...",
   "links": ["https://www.berliner-volksbank.de/..."],
   "html": "<only with includeHtml>",
   "screenshot": "data:image/png;base64,<only with screenshot>"
 }}
```

`url` is the final URL after redirects; `engine` is the engine that produced
the content. `title`, `description`, `language`, `html` and `screenshot` are
omitted when absent.

## POST /v1/crawl

```json
{"url": "https://example.de/", "limit": 25, "depth": 2,
 "include": "/produkte/", "exclude": "\\.pdf$",
 "scrapeOptions": {"engine": "http", "onlyMainContent": true}}
```

| Field             | Default | Notes |
|-------------------|---------|-------|
| `url`             | —       | seed URL |
| `limit`           | 25      | pages to return, 1 to the server's `--crawl-cap` (default 100) |
| `depth`           | 2       | link depth from the seed, 0–10 |
| `sameHostOnly`    | `true`  | stay on the seed's site (the landed host after redirects) |
| `allowSubdomains` | `true`  | treat `shop.example.de` as the same site as `www.example.de` |
| `include`/`exclude` | —     | POSIX extended regexes on the full URL, ≤ 512 chars |
| `scrapeOptions`   | —       | the scrape fields above (no `actions`); when omitted, top-level scrape fields are used |

Response: `{"success": true, "data": [<document>, ...], "count": n}`. Pages
that fail are skipped; the request fails only when nothing could be fetched
(e.g. `403 robots_denied` for a disallowed seed).

## POST /v1/map

```json
{"url": "https://example.de/", "limit": 5000}
```

Response: `{"success": true, "links": ["https://example.de/a", ...], "count": n}`.
URLs come from the sitemaps declared in robots.txt, `/sitemap.xml` (sitemap
indexes are expanded one level) and the homepage's same-site links. `limit`
is 1–100000.

## Errors

| HTTP | `code`                  | Meaning |
|------|-------------------------|---------|
| 400  | `bad_request`           | missing/invalid field, bad regex, malformed framing |
| 400  | `bad_json`              | body is not a JSON object |
| 400  | `bad_url`               | not an `http`/`https` URL |
| 401  | `unauthorized`          | missing or wrong API key |
| 403  | `blocked`               | target is loopback/private/link-local/metadata (start with `--allow-private` for internal hosts you control) |
| 403  | `robots_denied`         | robots.txt disallows the URL |
| 404  | `not_found`             | unknown endpoint |
| 405  | `method_not_allowed`    | not GET/POST/OPTIONS |
| 413  | `body_too_large`        | request body over `--max-body` |
| 413  | `headers_too_large`     | request headers over 64 KiB |
| 415  | `unsupported_media_type`| request body not sent as `application/json` |
| 415  | `unsupported_content`   | target is binary (PDF, image, ...) |
| 421  | `host_not_allowed`      | see *Security* below |
| 422  | `action_failed`         | a non-optional browser action failed |
| 501  | `unsupported`           | needs a browser, none installed |
| 502  | `network`               | DNS/connect/TLS failure reaching the target |
| 502  | `too_many_redirects`    | |
| 502  | `response_too_large`    | target body over the 32 MiB cap |
| 502  | `render_failed`         | the headless browser failed |
| 503  | `busy`                  | too many connections, or no browser slot freed in time |
| 504  | `timeout`               | the target did not answer in time |

`502`/`503`/`504` are worth retrying with backoff; `4xx` are not.

## Security

- **Bind address.** The default is `127.0.0.1`. Bind `0.0.0.0` (as the Docker
  image does) only behind an API key or a private network.
- **Private networks.** The scraper refuses loopback, RFC 1918, link-local
  (including `169.254.169.254`), CGNAT and multicast targets, both as
  literals and via DNS, on every redirect hop and for every request the
  browser makes. `--allow-private` lifts this for trusted deployments.
- **Browsers.** CORS is off unless `--cors-origin` is given, and POST bodies
  must be `application/json`, so a web page cannot quietly drive the API from
  a user's browser. A loopback server without an API key additionally answers
  only requests addressed to `localhost`/`127.0.0.1`/`[::1]` (`421`
  otherwise), which defeats DNS-rebinding attacks.

## Sizing

The server itself is small: about 13 MB RSS idle and under 30 MB while
serving 16 concurrent HTTP-engine scrapes. Each live headless Chromium costs
a few hundred MB depending on the page (≈325 MB PSS measured rendering the
Berliner Volksbank homepage; heavy pages can reach 700 MB), plus about 1 MB
for its supervisor process. Plan for

    memory ≈ 50 MB + max_browsers × 500 MB

and set `--max-browsers` (default 4) to what the machine can hold; requests
beyond it wait for a free slot and then fail with `503 busy`.

## Kotlin

A complete, dependency-light client (`java.net.http` +
kotlinx.serialization) lives in [`examples/kotlin`](../examples/kotlin):

```kotlin
val client = RebellionClient("http://rebellion:8080", apiKey = System.getenv("REBELLION_API_KEY"))

val doc = client.scrape(ScrapeRequest("https://www.volksbank.de/", engine = "auto"))
println("${doc.statusCode} ${doc.title}\n${doc.markdown}")

val urls = client.map("https://www.volksbank.de/", limit = 500)

try {
    client.scrape(ScrapeRequest("https://example.de/", actions = listOf(Action.click("#accept", optional = true))))
} catch (e: RebellionException) {
    if (e.code == "busy" || e.httpStatus >= 502) { /* retry later */ }
}
```

Run the example against a server with
`cd examples/kotlin && gradle run --args="https://example.com/ http://127.0.0.1:8080"`.
