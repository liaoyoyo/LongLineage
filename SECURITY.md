# Security and Sensitive Data

LongLineage has no supported production release. The current feature candidate is a
private, non-production research preview; `P3/P4/P5/P7/P8` and production `run` remain
blocked.

Report vulnerabilities privately to the repository owner. Do not open a public issue
containing a credential, private path, real genomic coordinate, read identifier or
restricted data location. Use GitHub private vulnerability reporting when it is
enabled; otherwise contact the owner through an already trusted private channel.

Never commit:

- credentials, tokens, private URLs or environment dumps;
- real BAM/CRAM/VCF/sidecar files;
- real genomic coordinates, read names or run outputs;
- source manifests containing secrets or personally identifying metadata.

Only synthetic fixtures are permitted in Git. Production manifests and receipts stay
in protected run roots and are referenced by sanitized role/digest in documentation.

## Publication and visibility gate

Before any visibility change, tag or release, run:

```bash
scripts/ci/check_public_preview_gate.sh HEAD
```

A non-zero result is the expected safe state while source-origin, license, dependency
or history findings remain. The command reads its immutable history-audit base from
the safety receipt; it never trusts a moving branch name. A passing foundation build
does not override this gate.
The current disposition is `KEEP_PRIVATE_NO_TAG_NO_RELEASE`; see
`LongLineage/docs/release/PUBLIC_SAFETY_RECEIPT.json`.

If the repository is unexpectedly public, first restore private visibility, preserve
the observed state and timestamps, then audit tags, releases, branch history and
credentials. Current private visibility cannot prove that no clone or download occurred
during an earlier exposure. If an actual secret is found, revoke or rotate it before
considering any history-cleanup procedure.
