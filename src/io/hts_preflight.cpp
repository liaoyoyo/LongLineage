// SPDX-License-Identifier: GPL-3.0-only
#include "longlineage/io/hts_preflight.hpp"

#include <htslib/faidx.h>
#include <htslib/hts.h>
#include <htslib/kstring.h>
#include <htslib/sam.h>
#include <htslib/tbx.h>
#include <htslib/vcf.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "longlineage/io/sidecar.hpp"

namespace longlineage {
namespace {

using HtsFilePointer = std::unique_ptr<htsFile, decltype(&hts_close)>;
using SamHeaderPointer = std::unique_ptr<sam_hdr_t, decltype(&sam_hdr_destroy)>;
using BamIndexPointer = std::unique_ptr<hts_idx_t, decltype(&hts_idx_destroy)>;
using BamRecordPointer = std::unique_ptr<bam1_t, decltype(&bam_destroy1)>;
using IteratorPointer = std::unique_ptr<hts_itr_t, decltype(&hts_itr_destroy)>;
using VcfHeaderPointer = std::unique_ptr<bcf_hdr_t, decltype(&bcf_hdr_destroy)>;
using VcfRecordPointer = std::unique_ptr<bcf1_t, decltype(&bcf_destroy)>;
using TabixPointer = std::unique_ptr<tbx_t, decltype(&tbx_destroy)>;
using FaidxPointer = std::unique_ptr<faidx_t, decltype(&fai_destroy)>;

[[nodiscard]] bool is_acgt_base(const char* allele) noexcept {
    return allele != nullptr && allele[0] != '\0' && allele[1] == '\0' &&
           (allele[0] == 'A' || allele[0] == 'C' || allele[0] == 'G' || allele[0] == 'T');
}

}  // namespace

ParseResult<std::string> require_htslib_version(const std::string& required_version) {
    const char* observed = hts_version();
    if (observed == nullptr) {
        return ParseResult<std::string>::failure(ParseReason::kIoError, "HTSlib did not report a runtime version");
    }
    if (required_version != observed) {
        return ParseResult<std::string>::failure(
            ParseReason::kUnsupportedValue,
            "HTSlib runtime version mismatch: required " + required_version + ", observed " + observed);
    }
    return ParseResult<std::string>::success(observed);
}

ParseResult<BamPreflight> preflight_bam(const std::string& bam_path, const std::string& bam_index_path) {
    HtsFilePointer file(sam_open(bam_path.c_str(), "rb"), &hts_close);
    if (!file) {
        return ParseResult<BamPreflight>::failure(ParseReason::kIoError, "Cannot open raw BAM input");
    }
    const htsFormat* format = hts_get_format(file.get());
    if (format == nullptr || format->format != bam) {
        return ParseResult<BamPreflight>::failure(ParseReason::kUnsupportedValue,
                                                  "raw_bam role must physically contain BAM, not SAM/CRAM");
    }
    SamHeaderPointer header(sam_hdr_read(file.get()), &sam_hdr_destroy);
    if (!header) {
        return ParseResult<BamPreflight>::failure(ParseReason::kMalformedValue, "Cannot read BAM header");
    }
    const int reference_count = sam_hdr_nref(header.get());
    if (reference_count <= 0) {
        return ParseResult<BamPreflight>::failure(ParseReason::kMalformedValue,
                                                  "BAM header must contain at least one reference sequence");
    }
    for (int tid = 0; tid < reference_count; ++tid) {
        if (sam_hdr_tid2name(header.get(), tid) == nullptr || sam_hdr_tid2len(header.get(), tid) <= 0) {
            return ParseResult<BamPreflight>::failure(ParseReason::kMalformedValue,
                                                      "BAM header contains an invalid reference sequence");
        }
    }
    BamIndexPointer index(sam_index_load3(file.get(), bam_path.c_str(), bam_index_path.c_str(), 0), &hts_idx_destroy);
    if (!index) {
        return ParseResult<BamPreflight>::failure(ParseReason::kIndexError,
                                                  "Cannot open explicit BAM index or bind it to the BAM");
    }

    bool observed_record = false;
    BamRecordPointer record(bam_init1(), &bam_destroy1);
    if (!record) {
        return ParseResult<BamPreflight>::failure(ParseReason::kIoError, "Cannot allocate BAM probe record");
    }
    for (int tid = 0; tid < reference_count && !observed_record; ++tid) {
        IteratorPointer iterator(
            sam_itr_queryi(index.get(), tid, 0, static_cast<hts_pos_t>(sam_hdr_tid2len(header.get(), tid))),
            &hts_itr_destroy);
        if (!iterator) {
            return ParseResult<BamPreflight>::failure(ParseReason::kIndexError,
                                                      "BAM index cannot construct a contig iterator");
        }
        const int result = sam_itr_next(file.get(), iterator.get(), record.get());
        if (result >= 0) {
            observed_record = true;
        } else if (result < -1) {
            return ParseResult<BamPreflight>::failure(ParseReason::kIndexError,
                                                      "BAM index iterator reported a read error");
        }
    }
    BamPreflight preflight{static_cast<std::uint32_t>(reference_count), observed_record};
    return observed_record ? ParseResult<BamPreflight>::success(preflight)
                           : ParseResult<BamPreflight>::success_empty(preflight);
}

ParseResult<VcfPreflight> preflight_pass_biallelic_ssnv_vcf(const std::string& vcf_path,
                                                            const std::string& vcf_index_path) {
    HtsFilePointer file(bcf_open(vcf_path.c_str(), "r"), &hts_close);
    if (!file) {
        return ParseResult<VcfPreflight>::failure(ParseReason::kIoError, "Cannot open frozen PASS sSNV VCF");
    }
    const htsFormat* format = hts_get_format(file.get());
    if (format == nullptr || format->format != vcf || format->compression != bgzf) {
        return ParseResult<VcfPreflight>::failure(ParseReason::kUnsupportedValue,
                                                  "PASS sSNV input must be a BGZF-compressed text VCF");
    }
    VcfHeaderPointer header(bcf_hdr_read(file.get()), &bcf_hdr_destroy);
    if (!header) {
        return ParseResult<VcfPreflight>::failure(ParseReason::kMalformedValue, "Cannot read VCF header");
    }
    const int contig_count = header->n[BCF_DT_CTG];
    if (contig_count <= 0) {
        return ParseResult<VcfPreflight>::failure(ParseReason::kMalformedValue,
                                                  "VCF header must declare at least one contig");
    }
    TabixPointer index(tbx_index_load3(vcf_path.c_str(), vcf_index_path.c_str(), 0), &tbx_destroy);
    if (!index) {
        return ParseResult<VcfPreflight>::failure(ParseReason::kIndexError, "Cannot open explicit VCF Tabix/CSI index");
    }

    VcfRecordPointer record(bcf_init1(), &bcf_destroy);
    if (!record) {
        return ParseResult<VcfPreflight>::failure(ParseReason::kIoError, "Cannot allocate VCF probe record");
    }
    std::uint64_t record_count = 0;
    int previous_rid = -1;
    hts_pos_t previous_pos = -1;
    std::string previous_ref;
    std::string previous_alt;
    int result = 0;
    char pass_filter[] = "PASS";
    while ((result = bcf_read(file.get(), header.get(), record.get())) >= 0) {
        if (bcf_unpack(record.get(), BCF_UN_STR | BCF_UN_FLT) != 0 || record->rid < 0 || record->pos < 0) {
            return ParseResult<VcfPreflight>::failure(ParseReason::kMalformedValue,
                                                      "VCF contains a malformed record or coordinate");
        }
        if (record->n_allele != 2 || !is_acgt_base(record->d.allele[0]) || !is_acgt_base(record->d.allele[1]) ||
            record->d.allele[0][0] == record->d.allele[1][0]) {
            return ParseResult<VcfPreflight>::failure(ParseReason::kUnsupportedValue,
                                                      "VCF record is not a biallelic single-base A/C/G/T substitution");
        }
        if (record->d.n_flt != 1 || bcf_has_filter(header.get(), record.get(), pass_filter) != 1) {
            return ParseResult<VcfPreflight>::failure(ParseReason::kUnsupportedValue,
                                                      "VCF record is not explicitly FILTER=PASS");
        }
        if (record->rid < previous_rid || (record->rid == previous_rid && record->pos < previous_pos)) {
            return ParseResult<VcfPreflight>::failure(ParseReason::kMalformedValue,
                                                      "VCF records are not sorted in frozen header/position order");
        }
        if (record->rid == previous_rid && record->pos == previous_pos && previous_ref == record->d.allele[0] &&
            previous_alt == record->d.allele[1]) {
            return ParseResult<VcfPreflight>::failure(ParseReason::kMalformedValue,
                                                      "VCF contains a duplicate site key");
        }
        previous_rid = record->rid;
        previous_pos = record->pos;
        previous_ref = record->d.allele[0];
        previous_alt = record->d.allele[1];
        ++record_count;
    }
    if (result < -1) {
        return ParseResult<VcfPreflight>::failure(ParseReason::kIoError,
                                                  "VCF reader reported malformed or truncated input");
    }
    int indexed_contig_count = 0;
    const char** indexed_contigs = tbx_seqnames(index.get(), &indexed_contig_count);
    bool index_observed_record = false;
    for (int index_position = 0; index_position < indexed_contig_count && !index_observed_record; ++index_position) {
        const int tid = tbx_name2id(index.get(), indexed_contigs[index_position]);
        IteratorPointer iterator(tbx_itr_queryi(index.get(), tid, 0, HTS_POS_MAX), &hts_itr_destroy);
        if (!iterator) {
            std::free(indexed_contigs);
            return ParseResult<VcfPreflight>::failure(ParseReason::kIndexError,
                                                      "VCF index cannot construct a contig iterator");
        }
        kstring_t indexed_line{0, 0, nullptr};
        const int query_result = tbx_itr_next(file.get(), index.get(), iterator.get(), &indexed_line);
        std::free(indexed_line.s);
        if (query_result >= 0) {
            index_observed_record = true;
        } else if (query_result < -1) {
            std::free(indexed_contigs);
            return ParseResult<VcfPreflight>::failure(ParseReason::kIndexError,
                                                      "VCF index iterator reported a read error");
        }
    }
    std::free(indexed_contigs);
    if (record_count > 0 && !index_observed_record) {
        return ParseResult<VcfPreflight>::failure(ParseReason::kIndexError,
                                                  "VCF records exist but explicit index returns no records");
    }
    VcfPreflight preflight{static_cast<std::uint32_t>(contig_count), record_count, index_observed_record};
    return record_count == 0 ? ParseResult<VcfPreflight>::success_empty(preflight)
                             : ParseResult<VcfPreflight>::success(preflight);
}

ParseResult<SidecarPreflight> preflight_latest_hp_ps_sidecar(const std::string& sidecar_path,
                                                             const std::string& sidecar_index_path) {
    HtsFilePointer file(hts_open(sidecar_path.c_str(), "r"), &hts_close);
    if (!file) {
        return ParseResult<SidecarPreflight>::failure(ParseReason::kIoError,
                                                      "Cannot open authoritative latest HP/PS sidecar");
    }
    const htsFormat* format = hts_get_format(file.get());
    if (format == nullptr || format->compression != bgzf) {
        return ParseResult<SidecarPreflight>::failure(ParseReason::kUnsupportedValue,
                                                      "Latest HP/PS sidecar must be BGZF-compressed");
    }
    kstring_t line{0, 0, nullptr};
    const int header_result = hts_getline(file.get(), '\n', &line);
    if (header_result < 0 || std::string_view(line.s, line.l) != kLatestTagSidecarHeader) {
        std::free(line.s);
        return ParseResult<SidecarPreflight>::failure(ParseReason::kMalformedValue,
                                                      "Latest HP/PS sidecar header is missing or not exact");
    }
    std::free(line.s);

    TabixPointer index(tbx_index_load3(sidecar_path.c_str(), sidecar_index_path.c_str(), 0), &tbx_destroy);
    if (!index) {
        return ParseResult<SidecarPreflight>::failure(ParseReason::kIndexError,
                                                      "Cannot open explicit sidecar Tabix index");
    }
    int contig_count = 0;
    const char** names = tbx_seqnames(index.get(), &contig_count);
    if (names == nullptr || contig_count <= 0) {
        std::free(names);
        return ParseResult<SidecarPreflight>::failure(ParseReason::kIndexError,
                                                      "Sidecar index contains no contig dictionary");
    }

    bool observed_record = false;
    for (int index_position = 0; index_position < contig_count && !observed_record; ++index_position) {
        const int tid = tbx_name2id(index.get(), names[index_position]);
        IteratorPointer iterator(tbx_itr_queryi(index.get(), tid, 0, HTS_POS_MAX), &hts_itr_destroy);
        if (!iterator) {
            std::free(names);
            return ParseResult<SidecarPreflight>::failure(ParseReason::kIndexError,
                                                          "Sidecar index cannot construct a contig iterator");
        }
        kstring_t record_line{0, 0, nullptr};
        const int result = tbx_itr_next(file.get(), index.get(), iterator.get(), &record_line);
        if (result >= 0) {
            const auto parsed = parse_sidecar_row(std::string_view(record_line.s, record_line.l));
            std::free(record_line.s);
            if (!parsed.ok()) {
                std::free(names);
                return ParseResult<SidecarPreflight>::failure(parsed.reason, parsed.detail);
            }
            observed_record = true;
        } else {
            std::free(record_line.s);
            if (result < -1) {
                std::free(names);
                return ParseResult<SidecarPreflight>::failure(ParseReason::kIndexError,
                                                              "Sidecar index iterator reported a read error");
            }
        }
    }
    std::free(names);
    SidecarPreflight preflight{static_cast<std::uint32_t>(contig_count), observed_record};
    return observed_record ? ParseResult<SidecarPreflight>::success(preflight)
                           : ParseResult<SidecarPreflight>::success_empty(preflight);
}

ParseResult<ReferencePreflight> preflight_reference_fasta(const std::string& fasta_path, const std::string& fai_path) {
    FaidxPointer index(fai_load3_format(fasta_path.c_str(), fai_path.c_str(), nullptr, FAI_NONE, FAI_FASTA),
                       &fai_destroy);
    if (!index) {
        return ParseResult<ReferencePreflight>::failure(ParseReason::kIndexError,
                                                        "Cannot load explicit FASTA/FAI pair without index creation");
    }
    const int contig_count = faidx_nseq(index.get());
    if (contig_count <= 0) {
        return ParseResult<ReferencePreflight>::failure(ParseReason::kMalformedValue,
                                                        "Reference FAI contains no contigs");
    }
    std::set<std::string> names;
    bool observed_base = false;
    for (int contig_index = 0; contig_index < contig_count; ++contig_index) {
        const char* name = faidx_iseq(index.get(), contig_index);
        if (name == nullptr || *name == '\0' || !names.insert(name).second) {
            return ParseResult<ReferencePreflight>::failure(ParseReason::kMalformedValue,
                                                            "Reference FAI contains an empty or duplicate contig name");
        }
        const hts_pos_t length = faidx_seq_len64(index.get(), name);
        if (length <= 0) {
            return ParseResult<ReferencePreflight>::failure(ParseReason::kMalformedValue,
                                                            "Reference FAI contains a non-positive contig length");
        }
        if (!observed_base) {
            hts_pos_t fetched_length = 0;
            std::unique_ptr<char, decltype(&std::free)> base(
                faidx_fetch_seq64(index.get(), name, 0, 0, &fetched_length), &std::free);
            if (!base || fetched_length != 1) {
                return ParseResult<ReferencePreflight>::failure(
                    ParseReason::kIndexError, "Reference FAI cannot fetch the first indexed base from FASTA");
            }
            observed_base = true;
        }
    }
    return ParseResult<ReferencePreflight>::success(
        ReferencePreflight{static_cast<std::uint32_t>(contig_count), observed_base});
}

}  // namespace longlineage
