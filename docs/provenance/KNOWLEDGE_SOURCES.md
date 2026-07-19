# Reviewed Knowledge Sources

Input-format and external-tool behavior is not inferred from historical result
reports. The reviewed knowledge sources are hash-bound in
`LongLineage/oracle/authority_manifest.json`.

| Contract | Reviewed fact |
|---|---|
| LongPhase-S | HP vocabulary is exactly `. / 1 / 2 / 3 / 4 / 1-1 / 2-1 / 1-2 / 2-2`; missing is normalized explicitly and never guessed |
| SAM/BAM MM/ML | MM positions use the as-sequenced orientation; ML byte N denotes `[N/256,(N+1)/256)`; `?` and `.` have different skip semantics; an MN/SEQ-length mismatch is invalid |

The repository stores only source role, reviewed date, digest and derived contract
facts. It does not copy the external knowledge documents or use them as runtime
inputs.
