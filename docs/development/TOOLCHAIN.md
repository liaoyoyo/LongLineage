# Toolchain and Dependency Contract

## Local verified environment

| Component | Local value | Release rule |
|---|---|---|
| OS | Ubuntu 22.04 | pinned OCI is authoritative |
| C++ | GCC 11.4 | GCC and Clang CI required |
| clang-format | extracted Ubuntu package, 14.0.0-1ubuntu1.1 | clang-format 14 CI gate |
| CMake | `/usr/bin/cmake` 3.22.1 | >=3.22 |
| HTSlib | pkg-config 1.18 | exactly 1.18 |
| Jansson | pkg-config 2.13.1 | >=2.13 |
| OpenSSL library | pkg-config 3.0.2 | C++ EVP only |
| Make | 4.3 | supported |
| Docker | 24 | pinned image build required |

The first `cmake` on the current interactive PATH is 3.14.4 and is too old. Local
commands therefore use `/usr/bin/cmake` explicitly. Likewise, the interactive
`openssl` command resolves to a different environment and is not an authority for the
linked OpenSSL library.

The host PATH has no `clang-format`, so the foundation verification extracted the
Ubuntu Jammy package to `/tmp/clang-format-14-root` without installing it system-wide:

```text
package: clang-format-14_1%3a14.0.0-1ubuntu1.1_amd64.deb
sha256: d354b27c63204db6fc29ef0a0048c91562e97b3bf2497f9ee053faeb4ed10744
binary: /tmp/clang-format-14-root/usr/bin/clang-format-14
version: Ubuntu clang-format version 14.0.0-1ubuntu1.1
```

That `/tmp` path is host-local evidence, not a repository or release dependency.
CI installs the exact major and replays `scripts/ci/check_format.sh
clang-format-14`.

`samtools`, `bcftools` and command-line Python are not production scientific
dependencies. Production code uses pinned HTSlib APIs directly. HiGHS is currently
unavailable locally; solver routes that truly require the pinned direct HiGHS build
must abstain and keep P5 blocked.

Release flags prohibit `-ffast-math` and `-march=native`. The production x86-64
baseline cannot assume AVX.
