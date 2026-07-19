// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <string>

#include "longlineage/common/parse_result.hpp"

namespace longlineage {

struct BamPreflight {
    std::uint32_t contig_count;
    bool index_query_observed_record;
};

struct VcfPreflight {
    std::uint32_t contig_count;
    std::uint64_t record_count;
    bool index_query_observed_record;
};

struct SidecarPreflight {
    std::uint32_t indexed_contig_count;
    bool index_query_observed_record;
};

struct ReferencePreflight {
    std::uint32_t contig_count;
    bool index_fetch_observed_base;
};

[[nodiscard]] ParseResult<std::string> require_htslib_version(const std::string& required_version);

// These primitives never infer an index path. The manifest-bound explicit index
// is always loaded, and the raw alignment input must physically be BAM.
[[nodiscard]] ParseResult<BamPreflight> preflight_bam(const std::string& bam_path, const std::string& bam_index_path);

// Reads every VCF record to verify sorted, PASS, biallelic single-base A/C/G/T
// semantics. A header-only valid VCF returns OK_EMPTY.
[[nodiscard]] ParseResult<VcfPreflight> preflight_pass_biallelic_ssnv_vcf(const std::string& vcf_path,
                                                                          const std::string& vcf_index_path);

[[nodiscard]] ParseResult<SidecarPreflight> preflight_latest_hp_ps_sidecar(const std::string& sidecar_path,
                                                                           const std::string& sidecar_index_path);

[[nodiscard]] ParseResult<ReferencePreflight> preflight_reference_fasta(const std::string& fasta_path,
                                                                        const std::string& fai_path);

}  // namespace longlineage
