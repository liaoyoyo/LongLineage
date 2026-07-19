# Security and Sensitive Data

Report vulnerabilities privately to the repository owner.

Never commit:

- credentials, tokens, private URLs or environment dumps;
- real BAM/CRAM/VCF/sidecar files;
- real genomic coordinates, read names or run outputs;
- source manifests containing secrets or personally identifying metadata.

Only synthetic fixtures are permitted in Git. Production manifests and receipts stay
in protected run roots and are referenced by sanitized role/digest in documentation.
