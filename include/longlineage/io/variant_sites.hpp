// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "longlineage/common/parse_result.hpp"
#include "longlineage/common/types.hpp"
#include "longlineage/io/reference_reader.hpp"

namespace longlineage {

enum class VariantScope {
    kAutosomesChr1To22,
};

struct VariantSite {
    std::uint32_t dataset_order;
    std::string dataset_id;
    std::uint64_t site_order;
    std::uint64_t vcf_record_order;
    ContigId contig;
    std::uint64_t contig_length;
    Position1 position;
    char reference;
    char alternate;
};

struct VariantSiteCensus {
    std::uint64_t vcf_records = 0;
    std::uint64_t pass_biallelic_ssnv = 0;
    std::uint64_t selected_scope = 0;
    std::uint64_t excluded_outside_scope = 0;
};

struct VariantSiteSet {
    std::vector<VariantSite> sites;
    VariantSiteCensus census;
};

// Sequentially reads the frozen VCF while requiring the explicitly named
// index to be loadable. For the autosomal scope, records are emitted only for
// chr1..chr22 in canonical order. Duplicate positions and non-increasing VCF
// order fail closed.
[[nodiscard]] ParseResult<VariantSiteSet> load_variant_sites(const std::filesystem::path& vcf_path,
                                                             const std::filesystem::path& explicit_index_path,
                                                             IndexedReferenceReader& reference,
                                                             std::uint32_t dataset_order, std::string dataset_id,
                                                             VariantScope scope = VariantScope::kAutosomesChr1To22);

}  // namespace longlineage
