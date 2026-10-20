# Contributing

Thanks for your interest in rebellion.

## Development setup

```sh
sudo apt-get install -y build-essential cmake libcurl4-openssl-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DREBELLION_WERROR=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Run the sanitizer build before sending a change (this is what CI gates on):

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -g -O1" -DREBELLION_HARDEN=OFF
cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure
```

## Ground rules

- **C11**, no compiler warnings under `-Wall -Wextra -Wshadow` (CI builds
  with `-Werror` on gcc and clang).
- Public API lives in `include/rebellion/rebellion.h`, prefixed `rbl_`.
  Keep it C ABI-friendly and documented.
- **Ownership is explicit.** If a function returns heap memory, say so in a
  comment; the caller frees. Use the `rbl_buf` / `rbl_strset` helpers.
- **Everything is UTF-8 internally.** New input paths must go through
  `rbl_to_utf8` and the entity decoder so legacy encodings keep working.
- **No shell-outs** from the library, and **no disabling TLS verification**.
- Treat all fetched content as untrusted.

## Tests

- Add a unit test under `tests/` for any new pure function (see the existing
  `test_*.c`; register the entry point in `tests/test.h` and `tests/test_main.c`).
- End-to-end behavior (HTTP, charset, robots, redirects, links, SSRF) is
  covered by the hermetic server in `tests/test_integration.c` — extend it
  rather than reaching out to the live network in tests.
- If you touch the parser or Markdown renderer, run the fuzzer for a while
  (see `fuzz/`).

## Style

- Formatting follows `.clang-format` (`clang-format -i src/*.c src/*.h`).
- Keep comments about *why*, not *what*; match the surrounding density.

## Commits & PRs

- Write focused commits with a clear message.
- Describe the change and how you tested it in the PR.
- By contributing you agree your work is licensed under the project's MIT
  license.
