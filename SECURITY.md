# Security Policy

## Reporting a vulnerability

Please report security issues privately via GitHub Security Advisories
("Report a vulnerability" on the repository's **Security** tab) rather than
opening a public issue. We aim to acknowledge reports within a few days.

Include, where possible: a description, affected version/commit, a minimal
reproduction, and the impact you observed.

## Supported versions

This project is pre-1.0; fixes land on `main`. Pin a commit for reproducible
builds.

## Security posture

rebellion fetches and parses untrusted content, so it is built defensively:

- **SSRF guard (on by default).** Requests to loopback, RFC1918/ULA private
  ranges, link-local (incl. the `169.254.169.254` cloud metadata address),
  CGNAT (`100.64.0.0/10`), and unspecified/multicast addresses are refused
  with `RBL_ERR_BLOCKED`. Covered: literal hosts (including alternative
  spellings such as `127.1`, `2130706433`, `0x7f.1`, `localhost.`), DNS
  names that resolve into those ranges (a libcurl pre-request hook inspects
  the connected peer IP), and every redirect hop (redirects are followed
  manually so each hop can be checked). In the browser engine every request the page
  makes is intercepted over the DevTools Fetch domain and checked the same
  way, including navigations triggered by clicks or scripts. Set
  `allow_private_networks` (`--allow-private`) to scrape internal hosts you
  control.
- **Proxies.** With an explicit proxy pool, names are resolved by the proxy,
  so only literal hosts are checked locally; egress policy is then the
  proxy's. With a proxy from the environment (`HTTPS_PROXY`, ...), which
  some hosts bypass via `NO_PROXY`, targets are additionally resolved and
  checked locally on every hop. Proxy URLs are validated and credentials are
  passed to Chromium through the DevTools protocol, never on its command
  line.
- **Protocol allow-list.** Transfers are restricted to `http`/`https` for
  both the initial request and any redirect target, so a crafted `Location:`
  cannot pivot to `file://`, `gopher://`, etc.
- **Bounded input.** Response bodies are capped (`max_response_bytes`,
  default 32 MiB), DevTools frames are bounded, binary content is rejected
  instead of parsed, and the HTML parser and JSON parser cap nesting depth,
  limiting memory blowups and deep recursion on hostile input.
- **Header injection.** User-Agent / Accept-Language values containing
  control characters are rejected (`RBL_ERR_INVALID_ARG`).
- **No shell-outs.** The library does not invoke a shell; temporary browser
  profiles are removed with `nftw`. Chromium is started with `execv` with
  every inherited descriptor closed, under a small supervisor that kills it
  if the owning process dies.
- **Headless browser.** Chromium runs with `--no-sandbox` (needed in most
  containers), so treat the machine running it as exposed to the pages it
  renders: run it in a container or VM, as an unprivileged user. Its
  DevTools endpoint listens on an ephemeral `127.0.0.1` port for the
  lifetime of the browser; other local users on the same host could connect
  to it, which is another reason to give rebellion its own container/user.
- **TLS verification is always on** (`CURLOPT_SSL_VERIFYPEER/HOST`); there is
  no option to disable it.
- **Fuzzing.** libFuzzer targets cover charset detection/conversion, the
  HTML parser, link resolution and the Markdown renderer (`fuzz_html`), the
  API's JSON parser (`fuzz_json`) and robots.txt evaluation (`fuzz_robots`).
- **Sanitizers in CI.** The test suite — including hermetic end-to-end tests
  with a local origin and an authenticating proxy, and the API server smoke
  test — runs under AddressSanitizer + UndefinedBehaviorSanitizer.

## API server (`rebellion-server`)

- Binds `127.0.0.1` by default and warns when bound elsewhere without an
  API key. API keys are compared in constant time.
- CORS is off unless `--cors-origin` is set, and POST bodies must be
  `application/json`, so web pages cannot drive the API from a visitor's
  browser. A loopback-bound server without an API key only answers requests
  whose `Host` is `localhost`/`127.0.0.1`/`[::1]`, defeating DNS rebinding.
- Requests are bounded: 64 KiB of headers, `--max-body` (2 MiB) of body, a
  30 s read deadline (slowloris), `--max-conns` concurrent connections,
  ambiguous framing (`Content-Length` with `Transfer-Encoding`) is rejected,
  and every connection carries one request.

## Known deviations

- **robots.txt that cannot be fetched** (network error, 4xx or 5xx) is
  treated as "allow all". RFC 9309 asks crawlers to treat 5xx as "disallow
  all"; we do not, because bot-protection front-ends commonly answer 503 for
  every path, which would make such sites unscrapeable even where their
  robots.txt allows it. Robots files are cached per origin for one hour.

## Responsible use

rebellion is intended for authorized data collection. It respects
`robots.txt` by default and does not solve CAPTCHAs or bypass authentication
or access controls. You are responsible for complying with each site's terms,
copyright, and applicable law (including the GDPR for personal data).
