# Third-party and Source-origin Inventory

> **Inventory only — public-release license audit is not complete.**

LongLineage source files declare `GPL-3.0-only`, and the repository includes the
canonical license text in `LICENSE`. This notice does not approve redistribution of
every mapped source or dependency. Unknown or unreviewed licenses remain
`NOASSERTION` in the SPDX document.

## Scientific source origin

LongLineage is a clean C++ implementation informed by source contracts and frozen
vectors from the InterSubMod repository. The origin repository contains a GPLv3
license file, but this audit has not completed per-file copyright, attribution,
source-origin and compatibility review.

The authoritative mapping is `provenance/source_to_target_manifest.json`:

- 21 mapping rows and 21/21 current target digest matches;
- 5 source rows replay at the declared root origin commit;
- 12 source rows replay only at other explicitly pinned commits;
- 4 source rows, representing 3 unique expected hashes, remain unresolved;
- all 21 public-release license dispositions remain pending.

No row may be promoted merely because its target compiles or its target SHA-256
matches.

## Build and runtime dependency inventory

The following values are transcribed from `containers/versions.lock.tsv`. Their
license conclusions remain `NOASSERTION` until a separate compatibility review.

| Component | Version policy | Recorded source |
|---|---|---|
| Ubuntu base | 22.04, digest-pinned | `docker.io/library/ubuntu` |
| apt resolution | Jammy current at image build | Ubuntu Jammy updates/security; captured in image |
| Boost | 1.74.x | Ubuntu jammy |
| CMake | 3.22.x | Ubuntu jammy |
| GCC | 11 | Ubuntu jammy |
| Git | 2.34.x | Ubuntu jammy |
| clang-format | 14.x | Ubuntu jammy |
| HTSlib | 1.18, tarball SHA-256 pinned | samtools/htslib GitHub release asset |
| Jansson | 2.13.x | Ubuntu jammy |
| OpenSSL | 3.0.x | Ubuntu jammy |
| python3-jsonschema | 3.2.x | Ubuntu jammy |

Resolved transitive apt packages are captured inside built images, not in the static
lock. A final release SBOM must bind the immutable image digest and its captured
builder/runtime package manifests.

## CI and presentation tooling

The GitHub workflow also references `actions/checkout@v4`, Playwright 1.60.0,
greenlet 3.2.4, pyee 13.0.0 and typing_extensions 4.15.0. These tools are not
production scientific kernels. Their presence here is disclosure, not a license
compatibility verdict.

## Current disposition

`PENDING_PUBLIC_RELEASE_AUDIT`. Keep the repository private and create no public tag
or release until source-origin, dependency, notice and full-history gates all pass.
