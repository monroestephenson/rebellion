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
  with `RBL_ERR_BLOCKED`. Both literal hosts and DNS names that resolve into
  those ranges are covered — the latter via a libcurl pre-request hook that
  inspects the resolved peer IP, so it also applies across redirects. Set
  `allow_private_networks` (`--allow-private`) to scrape internal hosts you
  control. (When an outbound proxy is configured, the peer-IP check is
  skipped because the peer is the proxy; egress policy is then the proxy's.)
- **Protocol allow-list.** Transfers are restricted to `http`/`https` for
  both the initial request and any redirect target, so a crafted `Location:`
  cannot pivot to `file://`, `gopher://`, etc.
- **Bounded input.** Response bodies and WebSocket frames are capped
  (256 MiB) and the HTML parser caps nesting depth, limiting memory blowups
  and deep recursion on hostile input.
- **No shell-outs.** The library does not invoke a shell; temporary browser
  profiles are removed with `nftw`, not `system("rm -rf")`.
- **TLS verification is always on** (`CURLOPT_SSL_VERIFYPEER/HOST`); there is
  no option to disable it.
- **Fuzzing.** `fuzz/fuzz_html.c` exercises the parser + Markdown renderer
  under libFuzzer (see `fuzz/CMakeLists.txt`).
- **Sanitizers in CI.** The test suite (including the hermetic HTTP
  integration test) runs under AddressSanitizer + UndefinedBehaviorSanitizer.

## Responsible use

rebellion is intended for authorized data collection. It respects
`robots.txt` by default and does not solve CAPTCHAs or bypass authentication
or access controls. You are responsible for complying with each site's terms,
copyright, and applicable law (including the GDPR for personal data).
