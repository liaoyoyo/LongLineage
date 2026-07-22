// SPDX-License-Identifier: GPL-3.0-only

#include "longlineage/compat/regional_topology.hpp"

#include <algorithm>
#include <bitset>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace longlineage::compat {
namespace {

using Node = std::uint16_t;
using NodeSet = std::set<Node>;

[[nodiscard]] std::size_t popcount(Node node) noexcept { return std::bitset<16>(node).count(); }

// Python orders a frozenset key by (len(bits), sorted(bit_indices)). Numeric
// bitmask order is different (for example {0,3} vs {1,2}) and changes the
// legacy capped fallback even though it cannot change non-capped feasibility.
[[nodiscard]] bool python_node_less(Node lhs, Node rhs) noexcept {
    if (popcount(lhs) != popcount(rhs)) {
        return popcount(lhs) < popcount(rhs);
    }
    std::size_t lhs_bit = 0;
    std::size_t rhs_bit = 0;
    while (lhs_bit < kRegionalMaximumSites && rhs_bit < kRegionalMaximumSites) {
        while (lhs_bit < kRegionalMaximumSites && (lhs & static_cast<Node>(Node{1} << lhs_bit)) == 0) {
            ++lhs_bit;
        }
        while (rhs_bit < kRegionalMaximumSites && (rhs & static_cast<Node>(Node{1} << rhs_bit)) == 0) {
            ++rhs_bit;
        }
        if (lhs_bit != rhs_bit) {
            return lhs_bit < rhs_bit;
        }
        ++lhs_bit;
        ++rhs_bit;
    }
    return false;
}

[[nodiscard]] std::uint64_t python_frozenset_hash(Node node) noexcept {
    const auto shuffle_bits = [](std::uint64_t hash) noexcept {
        return ((hash ^ UINT64_C(89869747)) ^ (hash << 16U)) * UINT64_C(3644798167);
    };
    std::uint64_t hash = 0;
    std::uint64_t used = 0;
    for (std::size_t bit = 0; bit < kRegionalMaximumSites; ++bit) {
        if ((node & static_cast<Node>(Node{1} << bit)) != 0) {
            hash ^= shuffle_bits(bit);
            ++used;
        }
    }
    hash ^= (used + 1U) * UINT64_C(1927868237);
    hash ^= (hash >> 11U) ^ (hash >> 25U);
    hash = hash * UINT64_C(69069) + UINT64_C(907133923);
    return hash == std::numeric_limits<std::uint64_t>::max() ? UINT64_C(590923713) : hash;
}

// The frozen Python runner used CPython 3.9.12 with PYTHONHASHSEED=0. Its
// greedy fallback iterates list(set), so table-slot order changes the reported
// fallback node count. This small no-delete table reproduces CPython 3.9's set
// insertion, resize, perturb and nine-slot linear-probe policy for bitsets.
class PythonNodeSetOrder final {
   public:
    void insert(Node node) {
        if (!members_.insert(node).second) {
            return;
        }
        insert_clean(node, python_frozenset_hash(node));
        ++used_;
        if (used_ * 5U >= (table_.size() - 1U) * 3U) {
            resize(used_ * 4U);
        }
    }

    [[nodiscard]] std::vector<Node> iteration_order() const {
        std::vector<Node> output;
        output.reserve(used_);
        for (const std::optional<Node>& entry : table_) {
            if (entry.has_value()) {
                output.push_back(*entry);
            }
        }
        return output;
    }

    // CPython's exact-set merge reserves before inserting the right-hand
    // table.  This is observably different from repeated PySet_Add calls: for
    // `{ROOT} | full` the five-entry result is built in a 16-slot table, while
    // five ordinary insertions resize to 32 slots.  The frozen fallback later
    // iterates this table, so preserving the merge path is part of the legacy
    // compatibility contract.
    void merge_from(const PythonNodeSetOrder& other) {
        if ((used_ + other.used_) * 5U >= (table_.size() - 1U) * 3U) {
            resize((used_ + other.used_) * 2U);
        }
        for (const Node node : other.iteration_order()) {
            insert(node);
        }
    }

   private:
    static constexpr std::size_t kMinimumSize = 8;
    static constexpr std::size_t kLinearProbes = 9;
    static constexpr unsigned kPerturbShift = 5;

    void insert_clean(Node node, std::uint64_t hash) {
        const std::size_t mask = table_.size() - 1U;
        std::uint64_t perturb = hash;
        std::size_t index = static_cast<std::size_t>(hash) & mask;
        while (true) {
            std::size_t probes = index + kLinearProbes <= mask ? kLinearProbes : 0;
            while (true) {
                if (!table_[index].has_value()) {
                    table_[index] = node;
                    return;
                }
                if (probes == 0) {
                    break;
                }
                --probes;
                ++index;
            }
            perturb >>= kPerturbShift;
            index = (index * 5U + 1U + static_cast<std::size_t>(perturb)) & mask;
        }
    }

    void resize(std::size_t minimum_used) {
        std::size_t new_size = kMinimumSize;
        while (new_size <= minimum_used) {
            new_size <<= 1U;
        }
        std::vector<std::optional<Node>> old = std::move(table_);
        table_.assign(new_size, std::nullopt);
        for (const std::optional<Node>& entry : old) {
            if (entry.has_value()) {
                insert_clean(*entry, python_frozenset_hash(*entry));
            }
        }
    }

    std::vector<std::optional<Node>> table_ = std::vector<std::optional<Node>>(kMinimumSize);
    NodeSet members_;
    std::size_t used_ = 0;
};

[[nodiscard]] bool checked_add(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& output) noexcept {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        return false;
    }
    output = lhs + rhs;
    return true;
}

[[nodiscard]] bool checked_multiply(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& output) noexcept {
    if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
        return false;
    }
    output = lhs * rhs;
    return true;
}

[[nodiscard]] constexpr std::uint64_t bounded_binomial(std::size_t n, std::size_t k, std::uint64_t limit) noexcept {
    if (k > n) {
        return 0;
    }
    k = std::min(k, n - k);
    std::uint64_t value = 1;
    for (std::size_t index = 1; index <= k; ++index) {
        std::uint64_t numerator = static_cast<std::uint64_t>(n - k + index);
        std::uint64_t denominator = static_cast<std::uint64_t>(index);
        const std::uint64_t divisor = std::gcd(numerator, denominator);
        numerator /= divisor;
        denominator /= divisor;
        // C(n-k+i-1,i-1) * (n-k+i) / i is integral.  After cancelling
        // numerator/i, the remaining denominator therefore divides the
        // previous binomial exactly.  Divide before applying the limit so an
        // intermediate product cannot falsely cap a level such as
        // C(45,4)=148,995 <= 150,000.
        value /= denominator;
        if (value > limit / numerator) {
            return limit + 1;
        }
        value *= numerator;
        if (value > limit) {
            return limit + 1;
        }
    }
    return value;
}

static_assert(bounded_binomial(45, 4, 150'000) == 148'995, "legacy level budget must admit the H2009 C(45,4) boundary");
static_assert(bounded_binomial(46, 4, 150'000) == 150'001,
              "legacy level budget must cap the first over-budget adjacent boundary");

[[nodiscard]] ParseResult<Node> parse_full_pattern(std::string_view pattern, std::size_t site_count) {
    if (pattern.size() != site_count || site_count == 0 || site_count > kRegionalMaximumSites) {
        return ParseResult<Node>::failure(ParseReason::kMalformedValue, "full pattern length is invalid");
    }
    Node node = 0;
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        if (pattern[index] == 'A') {
            node = static_cast<Node>(node | (Node{1} << index));
        } else if (pattern[index] != 'R') {
            return ParseResult<Node>::failure(ParseReason::kMalformedValue, "full pattern must contain only R/A");
        }
    }
    return ParseResult<Node>::success(node);
}

struct PartialPattern {
    Node ones = 0;
    Node zeros = 0;
};

[[nodiscard]] ParseResult<PartialPattern> parse_partial_pattern(std::string_view pattern, std::size_t site_count) {
    if (pattern.size() != site_count || site_count == 0 || site_count > kRegionalMaximumSites) {
        return ParseResult<PartialPattern>::failure(ParseReason::kMalformedValue, "partial pattern length is invalid");
    }
    PartialPattern output;
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        const Node bit = static_cast<Node>(Node{1} << index);
        if (pattern[index] == 'A') {
            output.ones = static_cast<Node>(output.ones | bit);
        } else if (pattern[index] == 'R') {
            output.zeros = static_cast<Node>(output.zeros | bit);
        } else if (pattern[index] != 'X') {
            return ParseResult<PartialPattern>::failure(ParseReason::kMalformedValue,
                                                        "partial pattern must contain only R/A/X");
        }
    }
    return ParseResult<PartialPattern>::success(output);
}

[[nodiscard]] NodeSet subcube_members(const PartialPattern& pattern, Node universe) {
    const Node unknown =
        static_cast<Node>(universe & static_cast<Node>(~static_cast<Node>(pattern.ones | pattern.zeros)));
    NodeSet output;
    Node subset = unknown;
    while (true) {
        output.insert(static_cast<Node>(pattern.ones | subset));
        if (subset == 0) {
            break;
        }
        subset = static_cast<Node>((subset - 1) & unknown);
    }
    return output;
}

[[nodiscard]] bool covers(const NodeSet& nodes, const NodeSet& full, const std::vector<NodeSet>& partial_subcubes) {
    if (!std::includes(nodes.begin(), nodes.end(), full.begin(), full.end())) {
        return false;
    }
    for (const NodeSet& subcube : partial_subcubes) {
        bool hit = false;
        for (const Node member : subcube) {
            if (nodes.count(member) != 0) {
                hit = true;
                break;
            }
        }
        if (!hit) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::uint64_t predecessor_count(const NodeSet& nodes, Node node) noexcept {
    std::uint64_t count = 0;
    for (std::size_t bit = 0; bit < kRegionalMaximumSites; ++bit) {
        const Node mask = static_cast<Node>(Node{1} << bit);
        if ((node & mask) != 0 && nodes.count(static_cast<Node>(node & static_cast<Node>(~mask))) != 0) {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] bool is_closed(const NodeSet& nodes) noexcept {
    for (const Node node : nodes) {
        if (node != 0 && predecessor_count(nodes, node) == 0) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_compatible(const NodeSet& nodes, std::size_t site_count) noexcept {
    for (std::size_t first = 0; first < site_count; ++first) {
        for (std::size_t second = first + 1; second < site_count; ++second) {
            bool seen_11 = false;
            bool seen_10 = false;
            bool seen_01 = false;
            const Node first_mask = static_cast<Node>(Node{1} << first);
            const Node second_mask = static_cast<Node>(Node{1} << second);
            for (const Node node : nodes) {
                const bool a = (node & first_mask) != 0;
                const bool b = (node & second_mask) != 0;
                seen_11 = seen_11 || (a && b);
                seen_10 = seen_10 || (a && !b);
                seen_01 = seen_01 || (!a && b);
            }
            if (seen_11 && seen_10 && seen_01) {
                return false;
            }
        }
    }
    return true;
}

template <typename Callback>
bool for_each_combination(const std::vector<Node>& pool, std::size_t choose, Callback callback) {
    std::vector<Node> selected;
    selected.reserve(choose);
    std::function<bool(std::size_t, std::size_t)> visit = [&](std::size_t begin, std::size_t remaining) {
        if (remaining == 0) {
            return callback(selected);
        }
        if (pool.size() - begin < remaining) {
            return true;
        }
        const std::size_t last = pool.size() - remaining;
        for (std::size_t index = begin; index <= last; ++index) {
            selected.push_back(pool[index]);
            if (!visit(index + 1, remaining - 1)) {
                return false;
            }
            selected.pop_back();
        }
        return true;
    };
    return visit(0, choose);
}

[[nodiscard]] NodeSet greedy_closure(const NodeSet& base, const std::vector<NodeSet>& subcubes,
                                     PythonNodeSetOrder python_order) {
    NodeSet nodes = base;
    for (const NodeSet& subcube : subcubes) {
        const bool hit = std::any_of(subcube.begin(), subcube.end(), [&](Node node) { return nodes.count(node) != 0; });
        if (!hit) {
            const auto representative = std::min_element(subcube.begin(), subcube.end(), python_node_less);
            if (representative != subcube.end()) {
                nodes.insert(*representative);
                python_order.insert(*representative);
            }
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        const std::vector<Node> snapshot = python_order.iteration_order();
        for (const Node node : snapshot) {
            if (node == 0 || predecessor_count(nodes, node) != 0) {
                continue;
            }
            for (std::size_t bit = 0; bit < kRegionalMaximumSites; ++bit) {
                const Node mask = static_cast<Node>(Node{1} << bit);
                if ((node & mask) != 0) {
                    const Node predecessor = static_cast<Node>(node & static_cast<Node>(~mask));
                    nodes.insert(predecessor);
                    python_order.insert(predecessor);
                    changed = true;
                    break;
                }
            }
        }
    }
    return nodes;
}

[[nodiscard]] std::string classify(const LegacySolverResult& result) {
    if (result.capped) {
        return "capped";
    }
    if (result.n_trees == 0) {
        return "underdetermined";
    }
    if (result.has_recurrence_free) {
        if (result.n_trees == 1) {
            return "determined";
        }
        return result.n_feasible_node_sets == 1 ? "ambiguous_order" : "ambiguous_structure";
    }
    if (result.has_recurrence) {
        return "recurrence_required";
    }
    return "underdetermined";
}

}  // namespace

ParseResult<RegionalPlan> make_python_v2_regional_plan(const VariantSiteSet& variants) {
    if (variants.sites.empty()) {
        return ParseResult<RegionalPlan>::failure(ParseReason::kMalformedValue, "variant site set is empty");
    }
    RegionalPlan output;
    output.census.scope_sites = variants.sites.size();
    std::size_t begin = 0;
    while (begin < variants.sites.size()) {
        std::size_t end = begin + 1;
        while (end < variants.sites.size() && variants.sites[end].contig == variants.sites[end - 1].contig &&
               variants.sites[end].position.value() - variants.sites[end - 1].position.value() <= kRegionalGapBp) {
            ++end;
        }
        const std::size_t component_size = end - begin;
        if (component_size < 2) {
            ++output.census.positional_singletons;
            begin = end;
            continue;
        }
        ++output.census.multi_region_count;
        output.census.multi_region_pre_cap_sites += component_size;
        std::size_t selected_begin = begin;
        std::size_t selected_count = component_size;
        if (component_size > kRegionalMaximumSites) {
            ++output.census.capped_region_count;
            selected_count = kRegionalMaximumSites;
            std::uint64_t best_span = std::numeric_limits<std::uint64_t>::max();
            for (std::size_t candidate = begin; candidate + kRegionalMaximumSites <= end; ++candidate) {
                const std::uint64_t span = variants.sites[candidate + kRegionalMaximumSites - 1].position.value() -
                                           variants.sites[candidate].position.value();
                if (span < best_span) {
                    best_span = span;
                    selected_begin = candidate;
                }
            }
        }
        RegionalRegionPlan region;
        region.region_order = output.regions.size();
        region.chrom = variants.sites[selected_begin].contig.value();
        region.start1 = variants.sites[selected_begin].position.value();
        region.end1 = variants.sites[selected_begin + selected_count - 1].position.value();
        region.span = region.end1 - region.start1;
        region.region_id = region.chrom + ":" + std::to_string(region.start1) + "-" + std::to_string(region.end1);
        region.pre_cap_site_count = component_size;
        region.cap_excluded_site_count = component_size - selected_count;
        region.selected_sites.insert(
            region.selected_sites.end(), variants.sites.begin() + static_cast<std::ptrdiff_t>(selected_begin),
            variants.sites.begin() + static_cast<std::ptrdiff_t>(selected_begin + selected_count));
        output.census.cap_excluded_sites += region.cap_excluded_site_count;
        output.census.retained_sites += region.selected_sites.size();
        output.regions.push_back(std::move(region));
        begin = end;
    }
    const std::uint64_t accounted =
        output.census.positional_singletons + output.census.cap_excluded_sites + output.census.retained_sites;
    if (accounted != output.census.scope_sites || output.regions.empty()) {
        return ParseResult<RegionalPlan>::failure(ParseReason::kMalformedValue,
                                                  "regional grouping does not conserve the autosomal site universe");
    }
    return ParseResult<RegionalPlan>::success(std::move(output));
}

std::string python_v2_hp_family(std::string_view raw_hp) {
    if (!raw_hp.empty() && raw_hp.front() == '1') {
        return "1";
    }
    if (!raw_hp.empty() && raw_hp.front() == '2') {
        return "2";
    }
    if (raw_hp == "3") {
        return "3";
    }
    if (raw_hp == "4") {
        return "4";
    }
    return "none";
}

ParseResult<LegacySolverResult> solve_python_v2_legacy(const LegacySolverInput& input) {
    if (input.site_count == 0 || input.site_count > kRegionalMaximumSites ||
        (input.supported_full_patterns.empty() && input.supported_subread_patterns.empty())) {
        return ParseResult<LegacySolverResult>::failure(ParseReason::kMalformedValue,
                                                        "legacy solver requires 1..8 sites and at least one pattern");
    }

    NodeSet full;
    PythonNodeSetOrder python_full_order;
    Node universe = 0;
    for (const auto& entry : input.supported_full_patterns) {
        if (entry.second < kRegionalMinimumPatternReads) {
            return ParseResult<LegacySolverResult>::failure(ParseReason::kMalformedValue,
                                                            "full pattern below frozen MINREAD entered solver");
        }
        auto parsed = parse_full_pattern(entry.first, input.site_count);
        if (!parsed.ok()) {
            return ParseResult<LegacySolverResult>::failure(parsed.reason, parsed.detail);
        }
        full.insert(*parsed.value);
        python_full_order.insert(*parsed.value);
        universe = static_cast<Node>(universe | *parsed.value);
    }
    std::vector<PartialPattern> partials;
    for (const auto& entry : input.supported_subread_patterns) {
        if (entry.second < kRegionalMinimumPatternReads) {
            return ParseResult<LegacySolverResult>::failure(ParseReason::kMalformedValue,
                                                            "subread pattern below frozen MINREAD entered solver");
        }
        auto parsed = parse_partial_pattern(entry.first, input.site_count);
        if (!parsed.ok()) {
            return ParseResult<LegacySolverResult>::failure(parsed.reason, parsed.detail);
        }
        partials.push_back(*parsed.value);
        universe = static_cast<Node>(universe | parsed.value->ones);
    }

    std::vector<NodeSet> partial_subcubes;
    partial_subcubes.reserve(partials.size());
    for (const PartialPattern& partial : partials) {
        partial_subcubes.push_back(subcube_members(partial, universe));
    }

    NodeSet base = full;
    base.insert(0);
    // Reproduce `base = {ROOT} | full`, followed later by `N = set(base)`.
    // CPython pre-sizes an exact-set merge and exact-set construction copies
    // the resulting table.  Re-inserting the entries would change the table
    // size/slot order and therefore the legacy capped fallback.
    PythonNodeSetOrder python_base_order;
    python_base_order.insert(0);
    python_base_order.merge_from(python_full_order);
    PythonNodeSetOrder python_base_copy_order;
    python_base_copy_order.merge_from(python_base_order);
    NodeSet maximal = full;
    for (const NodeSet& subcube : partial_subcubes) {
        maximal.insert(subcube.begin(), subcube.end());
    }
    NodeSet pool_set;
    for (const Node node : maximal) {
        Node subset = node;
        while (true) {
            pool_set.insert(subset);
            if (subset == 0) {
                break;
            }
            subset = static_cast<Node>((subset - 1) & node);
        }
    }
    for (const Node node : base) {
        pool_set.erase(node);
    }
    std::vector<Node> pool(pool_set.begin(), pool_set.end());
    std::sort(pool.begin(), pool.end(), python_node_less);

    std::vector<NodeSet> feasible;
    LegacySolverResult result;
    const std::size_t maximum_extra = std::min(kLegacyExtraNodeCap, pool.size());
    for (std::size_t extra_count = 0; extra_count <= maximum_extra; ++extra_count) {
        const std::uint64_t level_size = bounded_binomial(pool.size(), extra_count, kLegacyPerLevelBudget);
        if (level_size > kLegacyPerLevelBudget) {
            result.capped = true;
            std::ostringstream reason;
            reason << "level e=" << extra_count << ": C(" << pool.size() << ',' << extra_count << ") > budget "
                   << kLegacyPerLevelBudget;
            result.cap_reason = reason.str();
            break;
        }
        for_each_combination(pool, extra_count, [&](const std::vector<Node>& selected) {
            NodeSet nodes = base;
            nodes.insert(selected.begin(), selected.end());
            if (covers(nodes, full, partial_subcubes) && is_closed(nodes)) {
                feasible.push_back(std::move(nodes));
            }
            return true;
        });
        if (!feasible.empty()) {
            result.n_hidden = extra_count;
            break;
        }
    }
    if (feasible.empty()) {
        result.capped = true;
        if (result.cap_reason.empty()) {
            result.cap_reason = "no feasible node set within extra_cap=4";
        }
        feasible.push_back(greedy_closure(base, partial_subcubes, python_base_copy_order));
        result.n_hidden = feasible.front().size() - full.size() - 1;
    }

    result.n_feasible_node_sets = feasible.size();
    for (const NodeSet& nodes : feasible) {
        std::uint64_t tree_count = 1;
        for (const Node node : nodes) {
            if (node == 0) {
                continue;
            }
            const std::uint64_t predecessors = predecessor_count(nodes, node);
            std::uint64_t multiplied = 0;
            if (predecessors == 0 || !checked_multiply(tree_count, predecessors, multiplied)) {
                return ParseResult<LegacySolverResult>::failure(
                    ParseReason::kMalformedValue, "legacy analytical tree count overflow/invariant failure");
            }
            tree_count = multiplied;
        }
        std::uint64_t total = 0;
        if (!checked_add(result.n_trees, tree_count, total)) {
            return ParseResult<LegacySolverResult>::failure(ParseReason::kMalformedValue,
                                                            "legacy total tree count overflow");
        }
        result.n_trees = total;
        if (is_compatible(nodes, input.site_count)) {
            ++result.n_recurrence_free_node_sets;
        } else {
            ++result.n_recurrent_node_sets;
        }
    }
    result.has_recurrence_free = result.n_recurrence_free_node_sets > 0;
    result.has_recurrence = result.n_recurrent_node_sets > 0;
    result.classification = classify(result);
    return ParseResult<LegacySolverResult>::success(std::move(result));
}

bool pattern_is_mutation_bearing(const LegacySolverInput& input) noexcept {
    const auto has_alternate = [](const auto& patterns) {
        return std::any_of(patterns.begin(), patterns.end(),
                           [](const auto& entry) { return entry.first.find('A') != std::string::npos; });
    };
    return has_alternate(input.supported_full_patterns) || has_alternate(input.supported_subread_patterns);
}

std::string python_v2_unit_role(std::string_view family, bool mutation_bearing) {
    if ((family == "1" || family == "2" || family == "3" || family == "4") && !mutation_bearing) {
        return "reference_only_control";
    }
    if ((family == "1" || family == "2") && mutation_bearing) {
        return "primary_mutation_lineage";
    }
    if (family == "3" && mutation_bearing) {
        return "unresolved_H3_auxiliary";
    }
    if (family == "4" && mutation_bearing) {
        return "shared_H4_auxiliary";
    }
    return "unphased_auxiliary";
}

}  // namespace longlineage::compat
