#!/usr/bin/env bash
# End-to-end smoke test for rebellion-server: starts the real binary, serves
# fixture pages from a local Python HTTP server, and checks the API contract.
# Usage: server_smoke.sh <path-to-rebellion-server>
set -u

SERVER_BIN="${1:?usage: server_smoke.sh <rebellion-server>}"
WORK="$(mktemp -d)"
PIDS=()
FAILS=0
CHECKS=0

cleanup() {
    for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

free_port() { python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()'; }

ok()   { CHECKS=$((CHECKS+1)); }
fail() { CHECKS=$((CHECKS+1)); FAILS=$((FAILS+1)); echo "FAIL: $*" >&2; }

# expect_code <expected-http-code> <description> <curl args...>
expect_code() {
    local want="$1" desc="$2"; shift 2
    local got
    got=$(curl -sS --noproxy '*' -o "$WORK/body" -w '%{http_code}' --max-time 120 "$@")
    if [ "$got" = "$want" ]; then ok; else fail "$desc: expected HTTP $want, got $got: $(head -c 300 "$WORK/body")"; fi
}
# body_has <needle> <description>
body_has() { if grep -qF -- "$1" "$WORK/body"; then ok; else fail "$2: '$1' not in $(head -c 300 "$WORK/body")"; fi; }

wait_http() { # url
    for _ in $(seq 1 100); do curl -s --noproxy '*' -o /dev/null "$1" && return 0; sleep 0.1; done
    return 1
}

# ---- fixtures ---------------------------------------------------------------
mkdir -p "$WORK/site"
cat > "$WORK/site/index.html" <<'EOF'
<!DOCTYPE html><html lang="de"><head><meta charset="utf-8"><title>Startseite</title></head>
<body><nav><a href="/nav">NAVLINK</a></nav><article><h1>Grüße aus dem Test</h1>
<p>Ein ausreichend langer Absatz, damit die Hauptinhalts-Erkennung den Artikel
auswählt und die Navigation verwirft. Siehe <a href="/page2.html">Seite zwei</a>.</p>
</article></body></html>
EOF
cat > "$WORK/site/page2.html" <<'EOF'
<html lang="de"><head><title>Zwei</title></head><body><main><h1>Seite zwei</h1>
<p>Noch ein Absatz mit genug Text für die Extraktion des Hauptinhalts hier.</p></main></body></html>
EOF
cat > "$WORK/site/form.html" <<'EOF'
<html><head><title>Formular</title></head><body><h1 id="t">START</h1><input id="q">
<button id="go" onclick="document.getElementById('t').textContent='TREFFER:'+document.getElementById('q').value">Los</button>
</body></html>
EOF
printf '\x89PNG\r\n\x1a\n\0\0\0\rIHDR' > "$WORK/site/image.png"

SITE_PORT=$(free_port)
python3 -m http.server "$SITE_PORT" --bind 127.0.0.1 --directory "$WORK/site" >/dev/null 2>&1 &
PIDS+=($!)
SITE="http://127.0.0.1:$SITE_PORT"
wait_http "$SITE/index.html" || { echo "fixture server did not start" >&2; exit 1; }

# ---- server with --allow-private and an API key -------------------------------
PORT=$(free_port)
"$SERVER_BIN" --port "$PORT" --allow-private --api-key s3cret --quiet >"$WORK/server.log" 2>&1 &
SRV_PID=$!
PIDS+=($SRV_PID)
API="http://127.0.0.1:$PORT"
wait_http "$API/health" || { echo "rebellion-server did not start" >&2; cat "$WORK/server.log" >&2; exit 1; }
AUTH=(-H 'Authorization: Bearer s3cret')
JSON=(-H 'Content-Type: application/json')

expect_code 200 "health needs no key" "$API/health"
body_has '"status":"ok"' "health body"

expect_code 401 "no key" -X POST "${JSON[@]}" -d "{\"url\":\"$SITE/\"}" "$API/v1/scrape"
expect_code 401 "wrong key" -X POST -H 'Authorization: Bearer nope' "${JSON[@]}" -d "{\"url\":\"$SITE/\"}" "$API/v1/scrape"
expect_code 200 "X-API-Key header" -X POST -H 'X-API-Key: s3cret' "${JSON[@]}" -d "{\"url\":\"$SITE/index.html\",\"engine\":\"http\"}" "$API/v1/scrape"

expect_code 200 "scrape" -X POST "${AUTH[@]}" "${JSON[@]}" -d "{\"url\":\"$SITE/index.html\",\"engine\":\"http\"}" "$API/v1/scrape"
body_has '"success":true' "scrape success"
body_has 'Grüße aus dem Test' "scrape markdown (UTF-8)"
body_has '"language":"de"' "scrape language"
if grep -q NAVLINK "$WORK/body"; then fail "nav should be stripped from markdown"; else ok; fi

expect_code 200 "query string ignored in routing" -X POST "${AUTH[@]}" "${JSON[@]}" -d "{\"url\":\"$SITE/index.html\",\"engine\":\"http\"}" "$API/v1/scrape?trace=1"

expect_code 400 "missing url" -X POST "${AUTH[@]}" "${JSON[@]}" -d '{}' "$API/v1/scrape"
body_has '"code":"bad_request"' "missing url code"
expect_code 400 "invalid JSON" -X POST "${AUTH[@]}" "${JSON[@]}" -d '{"url":' "$API/v1/scrape"
body_has '"code":"bad_json"' "bad json code"
expect_code 400 "JSON must be an object" -X POST "${AUTH[@]}" "${JSON[@]}" -d '["x"]' "$API/v1/scrape"
expect_code 400 "unknown engine" -X POST "${AUTH[@]}" "${JSON[@]}" -d "{\"url\":\"$SITE/\",\"engine\":\"warp\"}" "$API/v1/scrape"
expect_code 400 "unknown action" -X POST "${AUTH[@]}" "${JSON[@]}" -d "{\"url\":\"$SITE/\",\"actions\":[{\"type\":\"teleport\"}]}" "$API/v1/scrape"
expect_code 400 "action missing selector" -X POST "${AUTH[@]}" "${JSON[@]}" -d "{\"url\":\"$SITE/\",\"actions\":[{\"type\":\"click\"}]}" "$API/v1/scrape"
expect_code 400 "header injection in userAgent" -X POST "${AUTH[@]}" "${JSON[@]}" -d "{\"url\":\"$SITE/\",\"userAgent\":\"x\\r\\nX-Evil: 1\"}" "$API/v1/scrape"
expect_code 400 "bad URL scheme" -X POST "${AUTH[@]}" "${JSON[@]}" -d '{"url":"file:///etc/passwd","engine":"http"}' "$API/v1/scrape"
expect_code 415 "binary content" -X POST "${AUTH[@]}" "${JSON[@]}" -d "{\"url\":\"$SITE/image.png\",\"engine\":\"http\"}" "$API/v1/scrape"
body_has '"code":"unsupported_content"' "binary content code"
expect_code 502 "unreachable target" -X POST "${AUTH[@]}" "${JSON[@]}" -d '{"url":"http://127.0.0.1:1/","engine":"http","timeoutMs":3000}' "$API/v1/scrape"
expect_code 404 "unknown endpoint" -X POST "${AUTH[@]}" "${JSON[@]}" -d '{}' "$API/v1/nope"
expect_code 405 "wrong method" -X PUT "${AUTH[@]}" "${JSON[@]}" -d '{}' "$API/v1/scrape"

# The body must be declared as JSON (so browsers have to preflight cross-site
# requests), and CORS is off unless --cors-origin is given.
expect_code 415 "form-encoded body refused" -X POST "${AUTH[@]}" -d "{\"url\":\"$SITE/\"}" "$API/v1/scrape"
body_has '"code":"unsupported_media_type"' "415 code"
expect_code 415 "text/plain body refused" -X POST "${AUTH[@]}" -H 'Content-Type: text/plain' -d "{\"url\":\"$SITE/\"}" "$API/v1/scrape"
expect_code 200 "JSON with charset parameter" -X POST "${AUTH[@]}" -H 'Content-Type: application/json; charset=utf-8' -d "{\"url\":\"$SITE/index.html\",\"engine\":\"http\"}" "$API/v1/scrape"
if curl -sS --noproxy '*' -D - -o /dev/null "$API/health" | grep -qi '^Access-Control-Allow-Origin'; then
    fail "CORS headers sent without --cors-origin"; else ok; fi

# Hostile numbers must be clamped, not crash the server.
expect_code 200 "huge numbers are clamped" -X POST "${AUTH[@]}" "${JSON[@]}" -d "{\"url\":\"$SITE/index.html\",\"limit\":1e300,\"depth\":-1e300,\"timeoutMs\":1e300}" "$API/v1/map"

# Framing: chunked bodies, Expect: 100-continue, conflicting headers, size cap.
expect_code 200 "chunked request body" -X POST "${AUTH[@]}" "${JSON[@]}" -H 'Transfer-Encoding: chunked' -d "{\"url\":\"$SITE/index.html\",\"engine\":\"http\"}" "$API/v1/scrape"
BIG=$(python3 -c "import json;print(json.dumps({'url':'$SITE/index.html','engine':'http','pad':'x'*4000}))")
start=$(date +%s%N)
expect_code 200 "Expect: 100-continue" -X POST "${AUTH[@]}" "${JSON[@]}" -H 'Expect: 100-continue' -d "$BIG" "$API/v1/scrape"
elapsed=$(( ($(date +%s%N) - start) / 1000000 ))
if [ "$elapsed" -lt 900 ]; then ok; else fail "100-continue was not answered promptly (${elapsed}ms)"; fi
RAW=$(printf 'POST /v1/scrape HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer s3cret\r\nContent-Type: application/json\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n')
got=$(printf '%s' "$RAW" | python3 -c "
import socket,sys
s=socket.create_connection(('127.0.0.1',$PORT));s.sendall(sys.stdin.buffer.read());print(s.recv(64).split(b' ')[1].decode())")
if [ "$got" = "400" ]; then ok; else fail "Content-Length + Transfer-Encoding should be 400, got $got"; fi
python3 -c "import json;print(json.dumps({'url':'x','pad':'x'*3000000}))" > "$WORK/huge.json"
expect_code 413 "body too large" -X POST "${AUTH[@]}" "${JSON[@]}" --data-binary @"$WORK/huge.json" "$API/v1/scrape"

# map / crawl
expect_code 200 "map" -X POST "${AUTH[@]}" "${JSON[@]}" -d "{\"url\":\"$SITE/index.html\"}" "$API/v1/map"
body_has 'page2.html' "map finds page2"
expect_code 200 "crawl" -X POST "${AUTH[@]}" "${JSON[@]}" -d "{\"url\":\"$SITE/index.html\",\"limit\":5,\"depth\":1,\"scrapeOptions\":{\"engine\":\"http\"}}" "$API/v1/crawl"
count=$(python3 -c "import json;print(json.load(open('$WORK/body'))['count'])" 2>/dev/null || echo 0)
if [ "$count" -ge 2 ]; then ok; else fail "crawl count $count < 2"; fi
expect_code 400 "crawl with invalid regex" -X POST "${AUTH[@]}" "${JSON[@]}" -d "{\"url\":\"$SITE/index.html\",\"include\":\"([\"}" "$API/v1/crawl"

# Concurrency: parallel requests all succeed. (Wait on these PIDs only: a
# bare `wait` would also wait for the long-running servers.)
CURL_PIDS=()
for i in $(seq 1 12); do
    curl -sS --noproxy '*' -o /dev/null -w '%{http_code}\n' --max-time 60 -X POST "${AUTH[@]}" "${JSON[@]}" \
        -d "{\"url\":\"$SITE/page2.html\",\"engine\":\"http\",\"respectRobots\":false}" "$API/v1/scrape" > "$WORK/par.$i" &
    CURL_PIDS+=($!)
done
for p in "${CURL_PIDS[@]}"; do wait "$p"; done
wait_ok=0
for i in $(seq 1 12); do [ "$(cat "$WORK/par.$i" 2>/dev/null)" = "200" ] && wait_ok=$((wait_ok+1)); done
if [ "$wait_ok" -eq 12 ]; then ok; else fail "parallel requests: only $wait_ok/12 returned 200"; fi

# Browser actions (only if the server found a Chromium).
if grep -q '"browser":true' <(curl -s --noproxy '*' "$API/health"); then
    expect_code 200 "scrape with actions" -X POST "${AUTH[@]}" "${JSON[@]}" -d "{\"url\":\"$SITE/form.html\",\"actions\":[{\"type\":\"fill\",\"selector\":\"#q\",\"value\":\"laptop\"},{\"type\":\"click\",\"selector\":\"#go\"},{\"type\":\"wait\",\"ms\":200}]}" "$API/v1/scrape"
    body_has 'TREFFER:laptop' "actions changed the page"
    body_has '"engine":"browser"' "actions use the browser"
    expect_code 422 "failing action" -X POST "${AUTH[@]}" "${JSON[@]}" -d "{\"url\":\"$SITE/form.html\",\"actions\":[{\"type\":\"click\",\"selector\":\"#missing\"}]}" "$API/v1/scrape"
    body_has '"code":"action_failed"' "action failure code"
    expect_code 200 "optional failing action" -X POST "${AUTH[@]}" "${JSON[@]}" -d "{\"url\":\"$SITE/form.html\",\"actions\":[{\"type\":\"click\",\"selector\":\"#missing\",\"optional\":true}]}" "$API/v1/scrape"
else
    echo "  (browser checks skipped: no Chromium)"
fi

# ---- a second server without --allow-private: SSRF guard -----------------------
PORT2=$(free_port)
"$SERVER_BIN" --port "$PORT2" --cors-origin https://app.example --quiet >"$WORK/server2.log" 2>&1 &
SRV2_PID=$!
PIDS+=($SRV2_PID)
wait_http "http://127.0.0.1:$PORT2/health" || { echo "second server did not start" >&2; exit 1; }
expect_code 403 "SSRF: loopback blocked" -X POST "${JSON[@]}" -d "{\"url\":\"$SITE/index.html\"}" "http://127.0.0.1:$PORT2/v1/scrape"
body_has '"code":"blocked"' "SSRF code"
expect_code 403 "SSRF: metadata IP blocked" -X POST "${JSON[@]}" -d '{"url":"http://169.254.169.254/latest/meta-data/"}' "http://127.0.0.1:$PORT2/v1/scrape"
expect_code 403 "SSRF: map blocked" -X POST "${JSON[@]}" -d "{\"url\":\"$SITE/\"}" "http://127.0.0.1:$PORT2/v1/map"
expect_code 403 "SSRF: crawl blocked" -X POST "${JSON[@]}" -d "{\"url\":\"$SITE/\"}" "http://127.0.0.1:$PORT2/v1/crawl"

# DNS rebinding: a loopback server without an API key only answers requests
# addressed to a loopback name.
expect_code 421 "foreign Host refused" -X POST "${JSON[@]}" -H "Host: evil.example:$PORT2" -d '{"url":"https://example.com/"}' "http://127.0.0.1:$PORT2/v1/scrape"
body_has '"code":"host_not_allowed"' "421 code"
expect_code 403 "localhost Host accepted" -X POST "${JSON[@]}" -H "Host: localhost:$PORT2" -d "{\"url\":\"$SITE/\"}" "http://127.0.0.1:$PORT2/v1/scrape"

# CORS when enabled
if curl -sS --noproxy '*' -D - -o /dev/null "http://127.0.0.1:$PORT2/health" | grep -qi '^Access-Control-Allow-Origin: https://app.example'; then
    ok; else fail "--cors-origin not applied"; fi
expect_code 204 "CORS preflight" -X OPTIONS -H 'Origin: https://app.example' -H 'Access-Control-Request-Method: POST' "http://127.0.0.1:$PORT2/v1/scrape"

# ---- graceful shutdown -------------------------------------------------------
for pid in "$SRV_PID" "$SRV2_PID"; do
    kill -TERM "$pid"
    for _ in $(seq 1 100); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
    if kill -0 "$pid" 2>/dev/null; then fail "server did not exit on SIGTERM"; else
        wait "$pid"; rc=$?
        if [ "$rc" -eq 0 ]; then ok; else fail "server exit code $rc on SIGTERM"; fi
    fi
done

# Sanitizer builds report to the server's stderr; any report is a failure.
for log in "$WORK/server.log" "$WORK/server2.log"; do
    if grep -qE "Sanitizer|runtime error:" "$log"; then
        fail "sanitizer report in $(basename "$log")"; cat "$log" >&2
    else ok; fi
done

echo "server smoke: $CHECKS checks, $FAILS failed"
[ "$FAILS" -eq 0 ]
