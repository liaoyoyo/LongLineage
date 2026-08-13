// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/io/variant_sites.hpp"

#include <htslib/hts.h>
#include <htslib/vcf.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>

namespace longlineage {
namespace {

using HtsFilePointer = std::unique_ptr<htsFile, decltype(&hts_close)>;
using HeaderPointer = std::unique_ptr<bcf_hdr_t, decltype(&bcf_hdr_destroy)>;
using IndexPointer = std::unique_ptr<hts_idx_t, decltype(&hts_idx_destroy)>;
using RecordPointer = std::unique_ptr<bcf1_t, decltype(&bcf_destroy)>;

[[nodiscard]] int autosome_order(std::string_view contig) noexcept {
    static constexpr std::array<std::string_view, 22> kAutosomes = {
        "chr1",  "chr2",  "chr3",  "chr4",  "chr5",  "chr6",  "chr7",  "chr8",  "chr9",  "chr10", "chr11",
        "chr12", "chr13", "chr14", "chr15", "chr16", "chr17", "chr18", "chr19", "chr20", "chr21", "chr22",
    };
    const auto found = std::find(kAutosomes.begin(), kAutosomes.end(), contig);
    return found == kAutosomes.end() ? -1 : static_cast<int>(found - kAutosomes.begin());
}

[[nodiscard]] char uppercase_base(const char* allele) noexcept {
    if (allele == nullptr || allele[0] == '\0' || allele[1] != '\0') {
        return '\0';
    }
    char base = allele[0];
    if (base >= 'a' && base <= 'z') {
        base = static_cast<char>(base - ('a' - 'A'));
    }
    return std::string_view("ACGT").find(base) == std::string_view::npos ? '\0' : base;
}

}  // namespace

ParseResult<VariantSiteSet> load_variant_sites(const std::filesystem::path& vcf_path,
                                               const std::filesystem::path& explicit_index_path,
                                               IndexedReferenceReader& reference, std::uint32_t dataset_order,
                                               std::string dataset_id, VariantScope scope) {
    if (scope != VariantScope::kAutosomesChr1To22) {
        return ParseResult<VariantSiteSet>::failure(ParseReason::kUnsupportedValue,
                                                    "Unknown frozen variant-universe scope");
    }
    if (dataset_id.empty() || dataset_id.find_first_of("\t\r\n") != std::string::npos) {
        return ParseResult<VariantSiteSet>::failure(ParseReason::kMalformedValue, "Dataset ID is malformed");
    }
    const std::string vcf_name = vcf_path.string();
    const std::string index_name = explicit_index_path.string();
    if (vcf_name.empty() || index_name.empty()) {
        return ParseResult<VariantSiteSet>::failure(ParseReason::kMissingRequiredField,
                                                    "VCF and explicit index paths must both be non-empty");
    }
    HtsFilePointer input(bcf_open(vcf_name.c_str(), "r"), &hts_close);
    if (!input) {
        return ParseResult<VariantSiteSet>::failure(ParseReason::kIoError, "Cannot open frozen sSNV VCF");
    }
    HeaderPointer header(bcf_hdr_read(input.get()), &bcf_hdr_destroy);
    if (!header) {
        return ParseResult<VariantSiteSet>::failure(ParseReason::kMalformedValue, "Cannot read frozen VCF header");
    }
    IndexPointer index(bcf_index_load2(vcf_name.c_str(), index_name.c_str()), &hts_idx_destroy);
    if (!index) {
        return ParseResult<VariantSiteSet>::failure(ParseReason::kIndexError,
                                                    "Cannot load the explicitly named VCF index");
    }
    RecordPointer record(bcf_init(), &bcf_destroy);
    if (!record) {
        return ParseResult<VariantSiteSet>::failure(ParseReason::kIoError, "Cannot allocate a VCF record");
    }

    VariantSiteSet output;
    int previous_autosome = -1;
    std::uint64_t previous_position1 = 0;
    std::set<std::pair<std::string, std::uint64_t>> observed_positions;
    int read_status = 0;
    while ((read_status = bcf_read(input.get(), header.get(), record.get())) == 0) {
        const std::uint64_t vcf_record_order = output.census.vcf_records++;
        bcf_unpack(record.get(), BCF_UN_STR | BCF_UN_FLT);
        if (record->rid < 0) {
            return ParseResult<VariantSiteSet>::failure(ParseReason::kMalformedValue,
                                                        "VCF record lacks a contig identity");
        }
        const char* contig_name = bcf_hdr_id2name(header.get(), record->rid);
        if (contig_name == nullptr || record->pos < 0) {
            return ParseResult<VariantSiteSet>::failure(ParseReason::kMalformedValue,
                                                        "VCF record contig/position is malformed");
        }
        const bool pass = bcf_has_filter(header.get(), record.get(), const_cast<char*>("PASS")) == 1;
        const char reference_base =
            record->n_allele == 2 ? uppercase_base(record->d.allele[0]) : static_cast<char>('\0');
        const char alternate_base =
            record->n_allele == 2 ? uppercase_base(record->d.allele[1]) : static_cast<char>('\0');
        if (!pass || record->n_allele != 2 || reference_base == '\0' || alternate_base == '\0' ||
            reference_base == alternate_base) {
            return ParseResult<VariantSiteSet>::failure(
                ParseReason::kMalformedValue, "Frozen PASS input contains a non-PASS, non-biallelic, or non-sSNV row");
        }
        ++output.census.pass_biallelic_ssnv;

        const int current_autosome = autosome_order(contig_name);
        if (current_autosome < 0) {
            ++output.census.excluded_outside_scope;
            continue;
        }
        if (current_autosome < previous_autosome) {
            return ParseResult<VariantSiteSet>::failure(ParseReason::kMalformedValue,
                                                        "Autosomal VCF contig order regressed");
        }
        const std::uint64_t position1 = static_cast<std::uint64_t>(record->pos) + 1;
        if (current_autosome == previous_autosome && position1 <= previous_position1) {
            return ParseResult<VariantSiteSet>::failure(ParseReason::kMalformedValue,
                                                        "Autosomal VCF positions are not strictly increasing");
        }
        const auto inserted = observed_positions.emplace(contig_name, position1);
        if (!inserted.second) {
            return ParseResult<VariantSiteSet>::failure(ParseReason::kMalformedValue,
                                                        "Autosomal VCF contains a duplicate genomic position");
        }
        auto contig = ContigId::from_string(contig_name);
        auto position = Position1::from_value(position1);
        if (!contig.ok() || !position.ok()) {
            return ParseResult<VariantSiteSet>::failure(ParseReason::kMalformedValue,
                                                        "Autosomal VCF identity cannot be typed");
        }
        auto contig_length = reference.contig_length(*contig.value);
        if (!contig_length.ok() || position1 > *contig_length.value) {
            return ParseResult<VariantSiteSet>::failure(ParseReason::kMalformedValue,
                                                        "Autosomal VCF position lies outside the reference");
        }
        output.sites.push_back(VariantSite{dataset_order, dataset_id, output.census.selected_scope, vcf_record_order,
                                           std::move(*contig.value), *contig_length.value, std::move(*position.value),
                                           reference_base, alternate_base});
        ++output.census.selected_scope;
        previous_autosome = current_autosome;
        previous_position1 = position1;
    }
    if (read_status < -1) {
        return ParseResult<VariantSiteSet>::failure(ParseReason::kIoError, "VCF reader failed before normal EOF");
    }
    if (output.census.vcf_records == 0 || output.sites.empty()) {
        return ParseResult<VariantSiteSet>::failure(ParseReason::kMalformedValue,
                                                    "Frozen VCF or selected autosomal scope is empty");
    }
    return ParseResult<VariantSiteSet>::success(std::move(output));
}

}  // namespace longlineage
