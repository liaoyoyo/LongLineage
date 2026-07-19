// SPDX-License-Identifier: GPL-3.0-only

#include <htslib/bgzf.h>
#include <htslib/hts_endian.h>
#include <htslib/sam.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "longlineage/artifact/bgzf_tsv_writer.hpp"
#include "longlineage/io/block_reader.hpp"
#include "longlineage/runtime/byte_bounded_reorder_sink.hpp"
#include "longlineage/runtime/ordered_thread_pool.hpp"

namespace {

#define CHECK(condition)                                           \
    do {                                                           \
        if (!static_cast<bool>(condition)) {                       \
            throw std::runtime_error("CHECK failed: " #condition); \
        }                                                          \
    } while (false)

using longlineage::AlignmentBlock;
using longlineage::BlockPlanOptions;
using longlineage::FocalSiteCost;
using longlineage::IndexedBamBlockReader;
using longlineage::artifact::BgzfTsvWriter;
using longlineage::runtime::ByteBoundedReorderSink;
using longlineage::runtime::OrderedThreadPool;
using longlineage::runtime::PoolStatus;
using longlineage::runtime::ReorderSinkState;
using longlineage::runtime::ReorderSinkStatus;

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define LONGLINEAGE_TEST_HAS_TSAN 1
#endif
#endif

#if defined(__SANITIZE_THREAD__) || defined(LONGLINEAGE_TEST_HAS_TSAN)
constexpr bool kProductionThreadCapObservable = false;
#else
constexpr bool kProductionThreadCapObservable = true;
#endif

struct ExpectedRead {
    std::string qname;
    std::uint64_t start0;
    longlineage::Strand strand;
    char stored_base;
};

struct SyntheticBamFixture {
    std::filesystem::path root;
    std::filesystem::path bam;
    std::filesystem::path bai;
    std::filesystem::path filter_bam;
    std::filesystem::path filter_bai;
    std::filesystem::path malformed_bam;
    std::filesystem::path malformed_bai;
    std::vector<ExpectedRead> expected_reads;
    std::uint64_t contig_length = 20000;
};

[[nodiscard]] longlineage::ContigId contig(std::string value = "chrSynthetic") {
    auto parsed = longlineage::ContigId::from_string(std::move(value));
    CHECK(parsed.ok());
    return std::move(*parsed.value);
}

[[nodiscard]] longlineage::Position1 position(std::uint64_t value) {
    auto parsed = longlineage::Position1::from_value(value);
    CHECK(parsed.ok());
    return *parsed.value;
}

[[nodiscard]] FocalSiteCost site(std::uint32_t dataset_order, std::string dataset_id, std::uint64_t record_order,
                                 std::string contig_name, std::uint64_t position1, std::uint64_t estimated_alignments,
                                 std::uint64_t contig_length) {
    return FocalSiteCost{dataset_order,       std::move(dataset_id), record_order, contig(std::move(contig_name)),
                         position(position1), estimated_alignments,  contig_length};
}

void append_frozen_mm_ml(bam1_t& record, std::uint32_t query_length) {
    const std::string mm = "C+m?,0;";
    CHECK(bam_aux_append(&record, "MM", 'Z', static_cast<int>(mm.size() + 1),
                         reinterpret_cast<const std::uint8_t*>(mm.c_str())) == 0);
    std::array<std::uint8_t, 6> ml{};
    ml[0] = 'C';
    u32_to_le(1, ml.data() + 1);
    ml[5] = 192;
    CHECK(bam_aux_append(&record, "ML", 'B', static_cast<int>(ml.size()), ml.data()) == 0);
    std::array<std::uint8_t, 4> mn{};
    u32_to_le(query_length, mn.data());
    CHECK(bam_aux_append(&record, "MN", 'i', static_cast<int>(mn.size()), mn.data()) == 0);
}

void write_alignment(samFile& output, sam_hdr_t& header, std::string qname, std::uint64_t start0, std::uint16_t flag,
                     std::uint8_t mapq, std::uint32_t query_length, bool with_mm_ml, char stored_base = 'C') {
    std::unique_ptr<bam1_t, decltype(&bam_destroy1)> record(bam_init1(), &bam_destroy1);
    CHECK(record != nullptr);
    const std::string sequence(query_length, stored_base);
    const std::uint32_t cigar = bam_cigar_gen(query_length, BAM_CMATCH);
    CHECK(bam_set1(record.get(), qname.size(), qname.c_str(), flag, 0, static_cast<hts_pos_t>(start0), mapq, 1, &cigar,
                   -1, -1, 0, query_length, sequence.c_str(), nullptr, 64) >= 0);
    if (with_mm_ml) {
        append_frozen_mm_ml(*record, query_length);
    }
    CHECK(sam_write1(&output, &header, record.get()) >= 0);
}

void write_malformed_aux_alignment(samFile& output, sam_hdr_t& header) {
    std::unique_ptr<bam1_t, decltype(&bam_destroy1)> record(bam_init1(), &bam_destroy1);
    CHECK(record != nullptr);
    const std::string qname = "malformed_aux";
    const std::string sequence(1000, 'C');
    const std::uint32_t cigar = bam_cigar_gen(1000, BAM_CMATCH);
    CHECK(bam_set1(record.get(), qname.size(), qname.c_str(), 0, 0, 0, 60, 1, &cigar, -1, -1, 0, sequence.size(),
                   sequence.c_str(), nullptr, 64) >= 0);
    const std::string sentinel = "x";
    CHECK(bam_aux_append(record.get(), "AA", 'Z', static_cast<int>(sentinel.size() + 1),
                         reinterpret_cast<const std::uint8_t*>(sentinel.c_str())) == 0);
    append_frozen_mm_ml(*record, 1000);
    std::uint8_t* malformed = bam_aux_get(record.get(), "AA");
    CHECK(malformed != nullptr);
    *malformed = '?';
    CHECK(sam_write1(&output, &header, record.get()) >= 0);
}

[[nodiscard]] std::unique_ptr<sam_hdr_t, decltype(&sam_hdr_destroy)> synthetic_header() {
    const std::string header_text =
        "@HD\tVN:1.6\tSO:coordinate\n"
        "@SQ\tSN:chrSynthetic\tLN:20000\n";
    std::unique_ptr<sam_hdr_t, decltype(&sam_hdr_destroy)> header(
        sam_hdr_parse(header_text.size(), header_text.c_str()), &sam_hdr_destroy);
    CHECK(header != nullptr);
    return header;
}

[[nodiscard]] SyntheticBamFixture build_synthetic_bam() {
    SyntheticBamFixture fixture;
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    fixture.root =
        std::filesystem::path("/tmp") /
        ("longlineage_p2_fixture_" + std::to_string(static_cast<long long>(::getpid())) + "_" + std::to_string(nonce));
    std::error_code directory_error;
    std::filesystem::create_directories(fixture.root, directory_error);
    CHECK(!directory_error);
    fixture.bam = fixture.root / "reads.bam";
    fixture.bai = fixture.root / "reads.explicit.bai";
    fixture.filter_bam = fixture.root / "filter-cases.bam";
    fixture.filter_bai = fixture.root / "filter-cases.explicit.bai";
    fixture.malformed_bam = fixture.root / "malformed-aux.bam";
    fixture.malformed_bai = fixture.root / "malformed-aux.explicit.bai";

    std::unique_ptr<samFile, decltype(&hts_close)> output(sam_open(fixture.bam.c_str(), "wb"), &hts_close);
    CHECK(output != nullptr);
    auto header = synthetic_header();
    CHECK(sam_hdr_write(output.get(), header.get()) >= 0);

    write_alignment(*output, *header, "keep00", 0, 0, 60, 1000, true);
    write_alignment(*output, *header, "lowmapq", 250, 0, 19, 1000, true);
    write_alignment(*output, *header, "duplicate", 500, BAM_FDUP, 60, 1000, true);
    write_alignment(*output, *header, "too_short", 750, 0, 60, 999, true);
    write_alignment(*output, *header, "missing_tags", 1000, 0, 60, 1000, false);
    fixture.expected_reads.push_back(ExpectedRead{"keep00", 0, longlineage::Strand::kForward, 'C'});
    for (std::uint64_t start = 2000; start <= 18000; start += 2000) {
        const std::string qname = "keep" + std::to_string(start);
        write_alignment(*output, *header, qname, start, 0, 60, 1000, true);
        fixture.expected_reads.push_back(ExpectedRead{qname, start, longlineage::Strand::kForward, 'C'});
    }
    output.reset();
    CHECK(sam_index_build3(fixture.bam.c_str(), fixture.bai.c_str(), 0, 1) == 0);

    output.reset(sam_open(fixture.filter_bam.c_str(), "wb"));
    CHECK(output != nullptr);
    header = synthetic_header();
    CHECK(sam_hdr_write(output.get(), header.get()) >= 0);
    write_alignment(*output, *header, "secondary", 100, BAM_FSECONDARY, 60, 1000, true);
    write_alignment(*output, *header, "supplementary", 200, BAM_FSUPPLEMENTARY, 60, 1000, true);
    write_alignment(*output, *header, "unmapped-flag", 300, BAM_FUNMAP, 60, 1000, true);
    write_alignment(*output, *header, "mapq20", 400, 0, 20, 1000, true);
    write_alignment(*output, *header, "reverse", 500, BAM_FREVERSE, 60, 1000, true, 'G');
    output.reset();
    CHECK(sam_index_build3(fixture.filter_bam.c_str(), fixture.filter_bai.c_str(), 0, 1) == 0);

    output.reset(sam_open(fixture.malformed_bam.c_str(), "wb"));
    CHECK(output != nullptr);
    header = synthetic_header();
    CHECK(sam_hdr_write(output.get(), header.get()) >= 0);
    write_malformed_aux_alignment(*output, *header);
    output.reset();
    CHECK(sam_index_build3(fixture.malformed_bam.c_str(), fixture.malformed_bai.c_str(), 0, 1) == 0);
    return fixture;
}

[[nodiscard]] std::vector<FocalSiteCost> linear_sites(std::size_t count, std::uint64_t step,
                                                      std::uint64_t first_position1 = 1,
                                                      std::uint64_t estimated_alignments = 1,
                                                      std::uint64_t contig_length = 1000000) {
    std::vector<FocalSiteCost> sites;
    sites.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        sites.push_back(site(0, "synthetic", index, "chrSynthetic",
                             first_position1 + static_cast<std::uint64_t>(index) * step, estimated_alignments,
                             contig_length));
    }
    return sites;
}

void assert_exact_site_partition(const std::vector<FocalSiteCost>& source, const std::vector<AlignmentBlock>& blocks) {
    std::size_t observed = 0;
    for (std::size_t block_index = 0; block_index < blocks.size(); ++block_index) {
        CHECK(blocks[block_index].sequence == block_index);
        for (const FocalSiteCost& focal : blocks[block_index].focal_sites) {
            CHECK(observed < source.size());
            CHECK(focal.dataset_order == source[observed].dataset_order);
            CHECK(focal.dataset_id == source[observed].dataset_id);
            CHECK(focal.vcf_record_order == source[observed].vcf_record_order);
            CHECK(focal.contig == source[observed].contig);
            CHECK(focal.position == source[observed].position);
            ++observed;
        }
    }
    CHECK(observed == source.size());
}

void test_block_plan_boundaries() {
    const auto exact_sites = linear_sites(4096, 1);
    const auto exact_plan = longlineage::plan_alignment_blocks(exact_sites, BlockPlanOptions{4096, 250000, 5000});
    CHECK(exact_plan.ok() && exact_plan.value->size() == 1);
    CHECK(exact_plan.value->front().focal_sites.size() == 4096);
    assert_exact_site_partition(exact_sites, *exact_plan.value);

    const auto plus_one_sites = linear_sites(4097, 1);
    const auto plus_one_plan = longlineage::plan_alignment_blocks(plus_one_sites, BlockPlanOptions{4096, 250000, 5000});
    CHECK(plus_one_plan.ok() && plus_one_plan.value->size() == 2);
    CHECK((*plus_one_plan.value)[0].focal_sites.size() == 4096);
    CHECK((*plus_one_plan.value)[1].focal_sites.size() == 1);
    assert_exact_site_partition(plus_one_sites, *plus_one_plan.value);

    std::vector<FocalSiteCost> exact_cost{
        site(0, "synthetic", 0, "chrSynthetic", 1, 125000, 1000000),
        site(0, "synthetic", 1, "chrSynthetic", 2, 125000, 1000000),
    };
    const auto exact_cost_plan = longlineage::plan_alignment_blocks(exact_cost, BlockPlanOptions{4096, 250000, 5000});
    CHECK(exact_cost_plan.ok() && exact_cost_plan.value->size() == 1);
    CHECK(exact_cost_plan.value->front().estimated_alignments == 250000);

    exact_cost[1].estimated_alignments = 125001;
    const auto over_cost_plan = longlineage::plan_alignment_blocks(exact_cost, BlockPlanOptions{4096, 250000, 5000});
    CHECK(over_cost_plan.ok() && over_cost_plan.value->size() == 2);
    assert_exact_site_partition(exact_cost, *over_cost_plan.value);

    const std::vector<FocalSiteCost> group_transitions{
        site(0, "sample-a", 0, "chr1", 1, 1, 10000),
        site(0, "sample-a", 1, "chr1", 2, 1, 10000),
        site(0, "sample-a", 2, "chr2", 1, 1, 20000),
        site(1, "sample-b", 0, "chr1", 1, 1, 10000),
    };
    const auto transition_plan =
        longlineage::plan_alignment_blocks(group_transitions, BlockPlanOptions{4096, 250000, 5000});
    CHECK(transition_plan.ok() && transition_plan.value->size() == 3);
    assert_exact_site_partition(group_transitions, *transition_plan.value);

    const std::vector<FocalSiteCost> clipped_sites{
        site(0, "synthetic", 0, "chrSynthetic", 1, 1, 10000),
        site(0, "synthetic", 1, "chrSynthetic", 10000, 1, 10000),
    };
    const auto clipped = longlineage::plan_alignment_blocks(clipped_sites, BlockPlanOptions{1, 250000, 5000});
    CHECK(clipped.ok() && clipped.value->size() == 2);
    CHECK((*clipped.value)[0].fetch_interval.begin() == 0);
    CHECK((*clipped.value)[0].fetch_interval.end() == 5001);
    CHECK((*clipped.value)[1].fetch_interval.begin() == 4999);
    CHECK((*clipped.value)[1].fetch_interval.end() == 10000);

    const std::vector<FocalSiteCost> singleton_over_cap{
        site(0, "synthetic", 0, "chrSynthetic", 1, 250001, 10000),
    };
    const auto rejected = longlineage::plan_alignment_blocks(singleton_over_cap, BlockPlanOptions{4096, 250000, 5000});
    CHECK(!rejected.ok() && rejected.reason == longlineage::ParseReason::kUnsupportedValue);

    auto unsorted = linear_sites(2, 1);
    unsorted[1].vcf_record_order = 0;
    CHECK(!longlineage::plan_alignment_blocks(unsorted).ok());
    const std::vector<FocalSiteCost> duplicated_dataset_order{
        site(0, "sample-a", 0, "chr1", 1, 1, 10000),
        site(0, "sample-b", 1, "chr1", 2, 1, 10000),
    };
    CHECK(!longlineage::plan_alignment_blocks(duplicated_dataset_order).ok());
    const std::vector<FocalSiteCost> duplicated_dataset_id{
        site(0, "sample-a", 0, "chr1", 1, 1, 10000),
        site(1, "sample-a", 0, "chr1", 2, 1, 10000),
    };
    CHECK(!longlineage::plan_alignment_blocks(duplicated_dataset_id).ok());
    CHECK(longlineage::plan_alignment_blocks({}).empty());
}

[[nodiscard]] AlignmentBlock full_contig_block(const SyntheticBamFixture& fixture) {
    const std::vector<FocalSiteCost> sites{
        site(0, "synthetic", 0, "chrSynthetic", 1, 1, fixture.contig_length),
        site(0, "synthetic", 1, "chrSynthetic", fixture.contig_length, 1, fixture.contig_length),
    };
    auto blocks = longlineage::plan_alignment_blocks(sites);
    CHECK(blocks.ok() && blocks.value->size() == 1);
    return std::move(blocks.value->front());
}

void test_indexed_block_reader(const SyntheticBamFixture& fixture) {
    const AlignmentBlock block = full_contig_block(fixture);
    std::vector<std::unique_ptr<IndexedBamBlockReader>> readers;
    for (std::size_t index = 0; index < 4; ++index) {
        auto opened = IndexedBamBlockReader::open(fixture.bam, fixture.bai);
        CHECK(opened.ok());
        readers.push_back(std::move(*opened.value));
    }

    for (std::size_t reader_index = 0; reader_index < readers.size(); ++reader_index) {
        auto batch = readers[reader_index]->read_block(block);
        CHECK(batch.ok() && !batch.empty());
        CHECK(batch.value->counters.iterator_records == 14);
        CHECK(batch.value->counters.excluded_flag == 1);
        CHECK(batch.value->counters.excluded_mapq == 1);
        CHECK(batch.value->counters.excluded_query_length == 1);
        CHECK(batch.value->counters.excluded_missing_mm_ml == 1);
        CHECK(batch.value->counters.retained_records == 10);
        CHECK(batch.value->reads.size() == 10);
        CHECK(batch.value->logical_retained_bytes() > 10000);
        CHECK(readers[reader_index]->fetch_invocations() == 1);

        CHECK(batch.value->reads.size() == fixture.expected_reads.size());
        for (std::size_t index = 0; index < fixture.expected_reads.size(); ++index) {
            const auto& read = batch.value->reads[index];
            const ExpectedRead& expected = fixture.expected_reads[index];
            CHECK(read.identity.raw_qname == expected.qname);
            CHECK(read.identity.projection.qname == expected.qname);
            CHECK(read.identity.projection.contig == contig());
            CHECK(read.identity.projection.reference_interval.begin() == expected.start0);
            CHECK(read.identity.projection.reference_interval.end() == expected.start0 + 1000);
            CHECK(read.identity.projection.mapq == 60);
            CHECK(read.identity.projection.strand == expected.strand);
            CHECK(read.sequence_reference_orientation.size() == 1000);
            CHECK(read.sequence_reference_orientation == std::string(1000, expected.stored_base));
            CHECK(read.base_qualities.size() == 1000);
            CHECK(read.mm_ml.calls.size() == 1);
            CHECK(read.mm_ml.calls.front().query_pos0 == 0);
            CHECK(read.mm_ml.calls.front().ml_raw == 192);
        }
    }

    auto missing_index = IndexedBamBlockReader::open(fixture.bam, fixture.root / "missing.bai");
    CHECK(!missing_index.ok());
    CHECK(missing_index.reason == longlineage::ParseReason::kIndexError);

    auto policy_reader = IndexedBamBlockReader::open(fixture.bam, fixture.bai);
    CHECK(policy_reader.ok());
    CHECK(!(*policy_reader.value)->read_block(block, longlineage::BlockReadPolicy{21, 1000, true}).ok());
    CHECK(!(*policy_reader.value)->read_block(block, longlineage::BlockReadPolicy{20, 1001, true}).ok());
    CHECK(!(*policy_reader.value)->read_block(block, longlineage::BlockReadPolicy{20, 1000, false}).ok());
    CHECK((*policy_reader.value)->fetch_invocations() == 0);

    auto filter_reader = IndexedBamBlockReader::open(fixture.filter_bam, fixture.filter_bai);
    CHECK(filter_reader.ok());
    auto filtered = (*filter_reader.value)->read_block(block);
    CHECK(filtered.ok() && !filtered.empty());
    CHECK(filtered.value->counters.iterator_records == 5);
    CHECK(filtered.value->counters.excluded_flag == 3);
    CHECK(filtered.value->counters.excluded_mapq == 0);
    CHECK(filtered.value->counters.excluded_query_length == 0);
    CHECK(filtered.value->counters.excluded_missing_mm_ml == 0);
    CHECK(filtered.value->counters.retained_records == 2);
    CHECK(filtered.value->reads.size() == 2);
    CHECK(filtered.value->reads[0].identity.raw_qname == "mapq20");
    CHECK(filtered.value->reads[0].identity.projection.mapq == 20);
    CHECK(filtered.value->reads[1].identity.raw_qname == "reverse");
    CHECK(filtered.value->reads[1].identity.projection.strand == longlineage::Strand::kReverse);
    CHECK(filtered.value->reads[1].sequence_reference_orientation == std::string(1000, 'G'));
    CHECK(filtered.value->reads[1].mm_ml.calls.size() == 1);
    CHECK(filtered.value->reads[1].mm_ml.calls.front().query_pos0 == 0);

    const std::vector<FocalSiteCost> empty_region_site{
        site(0, "synthetic", 0, "chrSynthetic", 10001, 1, fixture.contig_length),
    };
    auto empty_plan = longlineage::plan_alignment_blocks(empty_region_site, BlockPlanOptions{1, 250000, 0});
    CHECK(empty_plan.ok() && empty_plan.value->size() == 1);
    auto empty_batch = (*filter_reader.value)->read_block(empty_plan.value->front());
    CHECK(empty_batch.ok() && empty_batch.empty());
    CHECK(empty_batch.value->counters.iterator_records == 0);
    CHECK(empty_batch.value->reads.empty());

    auto malformed_reader = IndexedBamBlockReader::open(fixture.malformed_bam, fixture.malformed_bai);
    CHECK(malformed_reader.ok());
    auto malformed = (*malformed_reader.value)->read_block(block);
    CHECK(!malformed.ok());
    CHECK(malformed.reason == longlineage::ParseReason::kMalformedValue);

    auto contract_reader = IndexedBamBlockReader::open(fixture.bam, fixture.bai);
    CHECK(contract_reader.ok());
    const auto rejects_contract = [&](AlignmentBlock corrupted) {
        const auto rejected = (*contract_reader.value)->read_block(corrupted);
        CHECK(!rejected.ok());
        CHECK(rejected.reason == longlineage::ParseReason::kMalformedValue);
    };
    AlignmentBlock corrupted = block;
    corrupted.dataset_id = "other";
    rejects_contract(corrupted);
    corrupted = block;
    ++corrupted.first_vcf_record_order;
    rejects_contract(corrupted);
    corrupted = block;
    ++corrupted.estimated_alignments;
    rejects_contract(corrupted);
    corrupted = block;
    auto shortened_fetch =
        longlineage::Interval0::from_bounds(corrupted.fetch_interval.begin() + 1, corrupted.fetch_interval.end());
    CHECK(shortened_fetch.ok());
    corrupted.fetch_interval = *shortened_fetch.value;
    rejects_contract(corrupted);
    corrupted = block;
    corrupted.focal_sites[1].vcf_record_order = corrupted.focal_sites[0].vcf_record_order;
    rejects_contract(corrupted);
    corrupted = block;
    corrupted.focal_sites[0].contig = contig("chrOther");
    rejects_contract(corrupted);
    corrupted = block;
    corrupted.focal_sites.resize(4097, corrupted.focal_sites.front());
    auto over_site_ceiling = (*contract_reader.value)->read_block(corrupted);
    CHECK(!over_site_ceiling.ok());
    CHECK(over_site_ceiling.reason == longlineage::ParseReason::kUnsupportedValue);
    corrupted = block;
    corrupted.estimated_alignments = 250001;
    auto over_cost_ceiling = (*contract_reader.value)->read_block(corrupted);
    CHECK(!over_cost_ceiling.ok());
    CHECK(over_cost_ceiling.reason == longlineage::ParseReason::kUnsupportedValue);
    CHECK((*contract_reader.value)->fetch_invocations() == 0);
}

struct Payload {
    std::string bytes;

    [[nodiscard]] std::size_t retained_bytes() const noexcept { return bytes.size(); }
};

using PayloadSink = ByteBoundedReorderSink<Payload>;

[[nodiscard]] PayloadSink make_payload_sink(std::size_t capacity, std::size_t max_item, std::string& emitted) {
    return PayloadSink(capacity, max_item, [&emitted](std::uint64_t sequence, Payload&& value) {
        emitted.append(std::to_string(sequence));
        emitted.append(value.bytes);
    });
}

void test_reorder_sink_adversarial() {
    std::string emitted;
    auto sink = make_payload_sink(12, 4, emitted);
    CHECK(sink.publish(2, Payload{"x"}).status == ReorderSinkStatus::kSuccess);
    CHECK(sink.publish(0, Payload{"x"}).status == ReorderSinkStatus::kSuccess);
    CHECK(sink.publish(1, Payload{"x"}).status == ReorderSinkStatus::kSuccess);
    CHECK(emitted == "0x1x2x");
    CHECK(sink.close(3).status == ReorderSinkStatus::kSuccess);

    emitted.clear();
    auto pressure = make_payload_sink(12, 4, emitted);
    CHECK(pressure.publish(2, Payload{"2222"}).status == ReorderSinkStatus::kSuccess);
    CHECK(pressure.publish(3, Payload{"3333"}).status == ReorderSinkStatus::kSuccess);
    auto blocked = std::async(std::launch::async, [&pressure] { return pressure.publish(4, Payload{"4444"}); });
    CHECK(blocked.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    CHECK(pressure.publish(0, Payload{"0000"}).status == ReorderSinkStatus::kSuccess);
    CHECK(pressure.publish(1, Payload{"1111"}).status == ReorderSinkStatus::kSuccess);
    CHECK(blocked.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(blocked.get().status == ReorderSinkStatus::kSuccess);
    CHECK(pressure.close(5).status == ReorderSinkStatus::kSuccess);
    CHECK(emitted ==
          "0"
          "0000"
          "1"
          "1111"
          "2"
          "2222"
          "3"
          "3333"
          "4"
          "4444");
    CHECK(pressure.snapshot().peak_retained_bytes <= 12);

    emitted.clear();
    auto oversize = make_payload_sink(8, 4, emitted);
    CHECK(oversize.publish(0, Payload{std::string(5, 'z')}).status == ReorderSinkStatus::kItemTooLarge);
    CHECK(oversize.snapshot().state == ReorderSinkState::kFailed);

    emitted.clear();
    auto invalid_oversize = make_payload_sink(8, 4, emitted);
    CHECK(invalid_oversize.publish(1, Payload{std::string(5, 'z')}).status == ReorderSinkStatus::kItemTooLarge);
    CHECK(invalid_oversize.snapshot().state == ReorderSinkState::kFailed);

    emitted.clear();
    auto duplicate = make_payload_sink(8, 4, emitted);
    CHECK(duplicate.publish(1, Payload{"x"}).status == ReorderSinkStatus::kSuccess);
    CHECK(duplicate.publish(1, Payload{"x"}).status == ReorderSinkStatus::kDuplicateSequence);
    CHECK(duplicate.snapshot().state == ReorderSinkState::kFailed);

    emitted.clear();
    auto late = make_payload_sink(8, 4, emitted);
    CHECK(late.publish(0, Payload{"x"}).status == ReorderSinkStatus::kSuccess);
    CHECK(late.publish(0, Payload{"x"}).status == ReorderSinkStatus::kLateSequence);
    CHECK(late.snapshot().state == ReorderSinkState::kFailed);

    emitted.clear();
    auto gap = make_payload_sink(8, 4, emitted);
    CHECK(gap.publish(1, Payload{"x"}).status == ReorderSinkStatus::kSuccess);
    CHECK(gap.close(2).status == ReorderSinkStatus::kGapAtClose);
    CHECK(gap.snapshot().state == ReorderSinkState::kFailed);

    emitted.clear();
    auto cancellable = make_payload_sink(8, 4, emitted);
    CHECK(cancellable.publish(1, Payload{"1111"}).status == ReorderSinkStatus::kSuccess);
    auto waiting = std::async(std::launch::async, [&cancellable] { return cancellable.publish(2, Payload{"2222"}); });
    CHECK(waiting.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    CHECK(cancellable.cancel("fault injection"));
    CHECK(waiting.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(waiting.get().status == ReorderSinkStatus::kCancelled);
    CHECK(cancellable.snapshot().buffered_bytes == 0);

    emitted.clear();
    auto zero = make_payload_sink(8, 4, emitted);
    CHECK(zero.publish(0, Payload{}).status == ReorderSinkStatus::kInvalidByteSize);
    CHECK(zero.snapshot().state == ReorderSinkState::kFailed);

    PayloadSink throwing(8, 4, [](std::uint64_t, Payload&&) { throw std::runtime_error("writer fault"); });
    CHECK(throwing.publish(0, Payload{"x"}).status == ReorderSinkStatus::kEmitError);
    CHECK(throwing.snapshot().state == ReorderSinkState::kFailed);
}

struct BlockRows {
    std::vector<std::array<std::string, 3>> rows;
    std::size_t retained_byte_count = 0;

    [[nodiscard]] std::size_t retained_bytes() const noexcept { return retained_byte_count; }
};

struct PipelineRun {
    std::string semantic_sha256;
    std::uint64_t row_count = 0;
    std::size_t peak_sink_bytes = 0;
    std::size_t observed_workers = 0;
    std::size_t peak_process_threads = 0;
    bool forced_out_of_order_observed = false;
};

[[nodiscard]] std::size_t process_thread_count() {
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "Threads:") {
            std::size_t count = 0;
            status >> count;
            return count;
        }
        std::string ignored;
        std::getline(status, ignored);
    }
    return 0;
}

void update_max(std::atomic<std::size_t>& target, std::size_t value) {
    std::size_t observed = target.load(std::memory_order_relaxed);
    while (observed < value && !target.compare_exchange_weak(observed, value, std::memory_order_relaxed)) {
    }
}

[[nodiscard]] std::string read_bgzf(const std::filesystem::path& path) {
    std::unique_ptr<BGZF, decltype(&bgzf_close)> input(bgzf_open(path.c_str(), "r"), &bgzf_close);
    CHECK(input != nullptr);
    std::string output;
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t count = bgzf_read(input.get(), buffer.data(), buffer.size());
        CHECK(count >= 0);
        if (count == 0) {
            break;
        }
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return output;
}

void verify_decompressed_rows(const std::string& physical, std::size_t expected_rows) {
    std::istringstream lines(physical);
    std::string line;
    std::size_t row = 0;
    while (std::getline(lines, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t first_tab = line.find('\t');
        CHECK(first_tab != std::string::npos);
        CHECK(std::stoull(line.substr(0, first_tab)) == row);
        const std::size_t second_tab = line.find('\t', first_tab + 1);
        CHECK(second_tab != std::string::npos);
        CHECK(std::stoull(line.substr(first_tab + 1, second_tab - first_tab - 1)) == 100 + row * 200);
        const std::uint64_t expected_covering = row % 10 <= 4 ? 1 : 0;
        CHECK(std::stoull(line.substr(second_tab + 1)) == expected_covering);
        ++row;
    }
    CHECK(row == expected_rows);
}

[[nodiscard]] PipelineRun run_pipeline_matrix_case(const SyntheticBamFixture& fixture,
                                                   const std::vector<FocalSiteCost>& sites,
                                                   std::size_t max_sites_per_block, std::size_t worker_count,
                                                   int writer_threads, std::string run_suffix) {
    auto planned = longlineage::plan_alignment_blocks(sites, BlockPlanOptions{max_sites_per_block, 250000, 5000});
    CHECK(planned.ok());
    CHECK(planned.value->size() >= worker_count);

    std::vector<std::unique_ptr<IndexedBamBlockReader>> readers;
    readers.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        auto opened = IndexedBamBlockReader::open(fixture.bam, fixture.bai);
        CHECK(opened.ok());
        readers.push_back(std::move(*opened.value));
    }

    const std::filesystem::path output = fixture.root / ("matrix_" + std::move(run_suffix) + ".tsv.bgz");
    BgzfTsvWriter writer(output, "longlineage.synthetic_block_rows", "1.0.0", "p2-synthetic",
                         {"record_order", "position1", "covering_reads"}, writer_threads);

    ByteBoundedReorderSink<BlockRows> sink(65536, 16384, [&writer](std::uint64_t, BlockRows&& result) {
        for (const auto& row : result.rows) {
            writer.write_row({row[0], row[1], row[2]});
        }
    });

    auto observed = std::make_unique<std::atomic<bool>[]>(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        observed[index].store(false, std::memory_order_relaxed);
    }
    std::atomic<std::size_t> observed_count{0};
    std::atomic<std::size_t> peak_threads{0};
    std::atomic<bool> forced_out_of_order_observed{false};

    OrderedThreadPool<std::uint64_t> pool(
        worker_count, 1024 * 1024, [&sink](const std::string& reason) { sink.cancel("pool terminal: " + reason); });
    for (std::size_t block_index = 0; block_index < planned.value->size(); ++block_index) {
        const auto submitted = pool.submit_indexed(1, [&, block_index](std::size_t worker_index) -> std::uint64_t {
            if (!observed[worker_index].exchange(true, std::memory_order_acq_rel)) {
                observed_count.fetch_add(1, std::memory_order_acq_rel);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (observed_count.load(std::memory_order_acquire) != worker_count) {
                    if (std::chrono::steady_clock::now() >= deadline) {
                        throw std::runtime_error("worker activation barrier timed out");
                    }
                    std::this_thread::yield();
                }
            }
            update_max(peak_threads, process_thread_count());
            if (worker_count > 1 && block_index == 0) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (!forced_out_of_order_observed.load(std::memory_order_acquire)) {
                    if (std::chrono::steady_clock::now() >= deadline) {
                        throw std::runtime_error("forced out-of-order publication timed out");
                    }
                    std::this_thread::yield();
                }
            }

            const AlignmentBlock& block = (*planned.value)[block_index];
            auto batch = readers[worker_index]->read_block(block);
            if (!batch.ok()) {
                throw std::runtime_error("block read failed: " + batch.detail);
            }
            BlockRows result;
            result.rows.reserve(block.focal_sites.size());
            for (const FocalSiteCost& focal : block.focal_sites) {
                std::uint64_t covering = 0;
                for (const auto& read : batch.value->reads) {
                    if (read.identity.projection.reference_interval.contains(focal.position)) {
                        ++covering;
                    }
                }
                std::array<std::string, 3> row{
                    std::to_string(focal.vcf_record_order),
                    std::to_string(focal.position.value()),
                    std::to_string(covering),
                };
                result.retained_byte_count += row[0].size() + row[1].size() + row[2].size() + 3;
                result.rows.push_back(std::move(row));
            }
            const auto published = sink.publish(block.sequence, std::move(result));
            if (published.status != ReorderSinkStatus::kSuccess) {
                throw std::runtime_error("reorder publish failed: " + published.message);
            }
            if (worker_count > 1 && block_index == 1) {
                const auto out_of_order = sink.snapshot();
                if (out_of_order.next_expected_sequence != 0 || out_of_order.buffered_items == 0) {
                    throw std::runtime_error("sequence one was not observed buffered ahead of sequence zero");
                }
                forced_out_of_order_observed.store(true, std::memory_order_release);
            }
            return block.sequence;
        });
        CHECK(submitted.status == PoolStatus::kSuccess);
        CHECK(submitted.sequence == block_index);
    }

    auto batch = pool.finish();
    CHECK(batch.status == PoolStatus::kSuccess);
    CHECK(batch.completed == planned.value->size());
    CHECK(sink.close(planned.value->size()).status == ReorderSinkStatus::kSuccess);
    const auto receipt = writer.close();
    CHECK(receipt.row_count == sites.size());
    verify_decompressed_rows(read_bgzf(output), sites.size());

    const auto snapshot = sink.snapshot();
    return PipelineRun{receipt.semantic_sha256,
                       receipt.row_count,
                       snapshot.peak_retained_bytes,
                       observed_count.load(std::memory_order_acquire),
                       peak_threads.load(std::memory_order_relaxed),
                       forced_out_of_order_observed.load(std::memory_order_acquire)};
}

void test_determinism_matrix(const SyntheticBamFixture& fixture) {
    const std::vector<FocalSiteCost> sites = linear_sites(80, 200, 100, 1, fixture.contig_length);
    static constexpr std::array<std::size_t, 5> kWorkerCounts{1, 2, 4, 24, 40};
    std::string authority_digest;
    for (const std::size_t max_sites : {std::size_t{1}, std::size_t{2}}) {
        for (const std::size_t workers : kWorkerCounts) {
            const PipelineRun run =
                run_pipeline_matrix_case(fixture, sites, max_sites, workers, 1,
                                         "c" + std::to_string(max_sites) + "_w" + std::to_string(workers));
            CHECK(run.row_count == 80);
            CHECK(run.observed_workers == workers);
            CHECK(run.peak_sink_bytes <= 65536);
            if (workers > 1) {
                CHECK(run.peak_sink_bytes > 0);
                CHECK(run.forced_out_of_order_observed);
            } else {
                CHECK(!run.forced_out_of_order_observed);
            }
            CHECK(run.peak_process_threads > 0);
            if (kProductionThreadCapObservable) {
                CHECK(run.peak_process_threads <= 46);
            }
            if (authority_digest.empty()) {
                authority_digest = run.semantic_sha256;
            } else {
                CHECK(run.semantic_sha256 == authority_digest);
            }
        }
    }
    const PipelineRun writer_four = run_pipeline_matrix_case(fixture, sites, 2, 40, 4, "c2_w40_writer4");
    CHECK(writer_four.semantic_sha256 == authority_digest);
    if (kProductionThreadCapObservable) {
        CHECK(writer_four.peak_process_threads <= 46);
    }

    static constexpr std::string_view kFrozenSyntheticGolden =
        "9179c42faf14000c9b1c87386a09cd33cae4bce27078d7d5af2798269c4fead0";
    if (authority_digest != kFrozenSyntheticGolden) {
        std::cerr << "observed P2 synthetic semantic SHA-256: " << authority_digest << '\n';
    }
    CHECK(authority_digest == kFrozenSyntheticGolden);
}

void test_worker_failure_cancellation(const SyntheticBamFixture& fixture) {
    const std::filesystem::path staging = fixture.root / "failure.staging.tsv.bgz";
    {
        BgzfTsvWriter writer(staging, "longlineage.failure_fixture", "1.0.0", "p2-failure", {"sequence"}, 1);
        ByteBoundedReorderSink<Payload> sink(
            8, 4, [&writer](std::uint64_t sequence, Payload&&) { writer.write_row({std::to_string(sequence)}); });

        std::mutex gate_mutex;
        std::condition_variable gate_changed;
        bool release_failure = false;
        OrderedThreadPool<int> pool(4, 1024,
                                    [&sink](const std::string& reason) { sink.cancel("pool terminal: " + reason); });
        CHECK(pool.submit(1,
                          [&] {
                              std::unique_lock<std::mutex> lock(gate_mutex);
                              gate_changed.wait(lock, [&] { return release_failure; });
                              throw std::runtime_error("injected sequence-zero failure");
                              return 0;
                          })
                  .status == PoolStatus::kSuccess);
        for (std::uint64_t sequence = 1; sequence < 8; ++sequence) {
            CHECK(pool.submit(1,
                              [&, sequence] {
                                  const auto published = sink.publish(sequence, Payload{"xxxx"});
                                  if (published.status != ReorderSinkStatus::kSuccess) {
                                      throw std::runtime_error("sink cancelled after injected worker failure");
                                  }
                                  return static_cast<int>(sequence);
                              })
                      .status == PoolStatus::kSuccess);
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (sink.snapshot().buffered_items == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        CHECK(sink.snapshot().buffered_items == 1);
        {
            std::lock_guard<std::mutex> lock(gate_mutex);
            release_failure = true;
        }
        gate_changed.notify_all();

        const auto failed = pool.finish();
        CHECK(failed.status == PoolStatus::kWorkerError);
        CHECK(failed.failed_sequence == 0);
        CHECK(failed.ordered_results.empty());
        CHECK(sink.snapshot().state == ReorderSinkState::kCancelled);
        // Deliberately do not call writer.close(): a partial staging BGZF is
        // diagnostic artifact. P2 does not claim the production
        // validator/freeze publication boundary.
    }
    CHECK(std::filesystem::exists(staging));
    CHECK(std::filesystem::file_size(staging) > 0);
}

void run_case(std::string_view requested, const SyntheticBamFixture& fixture) {
    if (requested == "block-plan") {
        test_block_plan_boundaries();
    } else if (requested == "block-reader") {
        test_indexed_block_reader(fixture);
    } else if (requested == "reorder") {
        test_reorder_sink_adversarial();
    } else if (requested == "determinism") {
        test_determinism_matrix(fixture);
    } else if (requested == "failure") {
        test_worker_failure_cancellation(fixture);
    } else {
        throw std::runtime_error("unknown test case: " + std::string(requested));
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const SyntheticBamFixture fixture = build_synthetic_bam();
        if (argc == 1) {
            for (const std::string_view test_case :
                 {"block-plan", "block-reader", "reorder", "determinism", "failure"}) {
                run_case(test_case, fixture);
            }
        } else if (argc == 2) {
            run_case(argv[1], fixture);
        } else {
            throw std::runtime_error("usage: test_block_pipeline [case]");
        }
        std::cout << "LongLineage P2 block pipeline synthetic contract: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "LongLineage P2 block pipeline synthetic contract: FAIL: " << error.what() << '\n';
        return 1;
    }
}
