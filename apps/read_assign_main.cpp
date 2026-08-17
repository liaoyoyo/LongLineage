// SPDX-License-Identifier: GPL-3.0-only
//
// read_assign — 產生 read_lineage_assignments：每條 molecule 屬於哪個 block。
// LongLineage 端與 InterSubMod 端的唯一交界檔。
//
// C++ 取代 build_read_lineage_assignments.py（該 Python 版已驗證，作為 parity 對照）。
// 路由與投影邏輯與 exact_ps_partition_to_mlhp.py:317-472 同源。
//
// 用法:
//   read_assign --chrom-dir DIR --sample S --chrom C --output OUT.tsv.gz
//               [--min-read 3] [--partition-subdir python_partition] [--receipt R.json]

#include <jansson.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kSchemaName = "intersubmod.read_lineage_assignments";
constexpr const char* kSchemaVersion = "1.0.0";

constexpr const char* kHeader =
    "dataset\tchrom\tmolecule_id\tqname_sha256\thp_family\tphase_set\tunit_id\tblock_id\t"
    "block_index\tregion_id\tpattern_vector\tk\tn_fixed_ra_in_block\tis_full_cov\ttree_supported\n";

std::vector<std::string> split_tab(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        std::size_t p = s.find('\t', start);
        if (p == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

std::vector<std::int64_t> parse_positions(const std::string& v, const std::string& label) {
    std::vector<std::int64_t> out;
    if (v.empty()) throw std::runtime_error("empty positions1 for " + label);
    std::size_t start = 0;
    while (true) {
        std::size_t p = v.find(',', start);
        std::string tok = (p == std::string::npos) ? v.substr(start) : v.substr(start, p - start);
        out.push_back(std::stoll(tok));
        if (p == std::string::npos) break;
        start = p + 1;
    }
    for (std::size_t i = 1; i < out.size(); ++i) {
        if (out[i] <= out[i - 1]) throw std::runtime_error("positions1 not strictly increasing for " + label);
    }
    return out;
}

// 逐行讀 .gz
class GzLineReader {
   public:
    explicit GzLineReader(const std::string& path) : fp_(gzopen(path.c_str(), "rb")) {
        if (fp_ == nullptr) throw std::runtime_error("cannot open: " + path);
        buf_.resize(1 << 20);
    }
    ~GzLineReader() {
        if (fp_ != nullptr) gzclose(fp_);
    }
    bool next(std::string& line) {
        line.clear();
        while (true) {
            if (gzgets(fp_, buf_.data(), static_cast<int>(buf_.size())) == nullptr) return !line.empty();
            std::size_t n = std::strlen(buf_.data());
            bool complete = (n > 0 && buf_[n - 1] == '\n');
            line.append(buf_.data(), complete ? n - 1 : n);
            if (complete) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return true;
            }
        }
    }

   private:
    gzFile fp_;
    std::vector<char> buf_;
};

struct Block {
    std::string unit_id, block_id, block_index, hp_family, phase_set;
    std::vector<std::int64_t> positions;
};

struct Assignment {
    std::string molecule_id, qname_sha256, hp_family, phase_set;
    std::size_t block_ref = 0;
    std::string pattern_vector;
    int n_fixed_ra = 0;
    bool is_full_cov = false;
    bool tree_supported = false;
};

std::size_t header_index(const std::vector<std::string>& hdr, const std::string& name, const std::string& path) {
    auto it = std::find(hdr.begin(), hdr.end(), name);
    if (it == hdr.end()) throw std::runtime_error(path + ": missing column " + name);
    return static_cast<std::size_t>(std::distance(hdr.begin(), it));
}

void usage() {
    std::cerr << "Usage: read_assign --chrom-dir DIR --sample S --chrom C --output OUT.tsv.gz\n"
              << "                   [--min-read 3] [--partition-subdir python_partition]\n"
              << "                   [--receipt RECEIPT.json]\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string chrom_dir, sample, chrom, out_path, receipt_path, part_sub = "python_partition";
    int min_read = 3;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << a << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "--chrom-dir")
            chrom_dir = next();
        else if (a == "--sample")
            sample = next();
        else if (a == "--chrom")
            chrom = next();
        else if (a == "--output")
            out_path = next();
        else if (a == "--receipt")
            receipt_path = next();
        else if (a == "--partition-subdir")
            part_sub = next();
        else if (a == "--min-read")
            min_read = std::stoi(next());
        else {
            std::cerr << "unknown option: " << a << "\n";
            return 2;
        }
    }
    if (chrom_dir.empty() || sample.empty() || chrom.empty() || out_path.empty()) {
        usage();
        return 2;
    }

    std::map<std::string, std::uint64_t> stats;
    std::vector<Block> blocks;
    // route: (hp, ps, position) -> block index
    std::map<std::tuple<std::string, std::string, std::int64_t>, std::size_t> route;

    try {
        // ── blocks ────────────────────────────────────────────────
        const std::string block_path = (fs::path(chrom_dir) / part_sub / "blocks.tsv.gz").string();
        GzLineReader br(block_path);
        std::string line;
        if (!br.next(line)) throw std::runtime_error(block_path + ": empty");
        auto bh = split_tab(line);
        const std::size_t bi_ds = header_index(bh, "dataset", block_path);
        const std::size_t bi_ch = header_index(bh, "chrom", block_path);
        const std::size_t bi_ui = header_index(bh, "unit_id", block_path);
        const std::size_t bi_hp = header_index(bh, "hp_family", block_path);
        const std::size_t bi_ps = header_index(bh, "phase_set", block_path);
        const std::size_t bi_bid = header_index(bh, "block_id", block_path);
        const std::size_t bi_bix = header_index(bh, "block_index", block_path);
        const std::size_t bi_k = header_index(bh, "k", block_path);
        const std::size_t bi_pos = header_index(bh, "positions1", block_path);

        std::set<std::pair<std::string, std::string>> seen_keys;
        while (br.next(line)) {
            if (line.empty()) continue;
            auto f = split_tab(line);
            if (f[bi_ds] != sample || f[bi_ch] != chrom)
                throw std::runtime_error(block_path + ": dataset/chrom mismatch");
            if ((f[bi_hp] != "1" && f[bi_hp] != "2") || f[bi_ps].empty())
                throw std::runtime_error(block_path + ": non-primary HP or missing PS");
            Block b;
            b.unit_id = f[bi_ui];
            b.block_id = f[bi_bid];
            b.block_index = f[bi_bix];
            b.hp_family = f[bi_hp];
            b.phase_set = f[bi_ps];
            b.positions = parse_positions(f[bi_pos], b.block_id);
            if (std::stoll(f[bi_k]) != static_cast<std::int64_t>(b.positions.size()))
                throw std::runtime_error(block_path + ": k/positions mismatch for " + b.block_id);
            if (!seen_keys.insert({b.unit_id, b.block_index}).second)
                throw std::runtime_error(block_path + ": duplicate block key");
            const std::size_t idx = blocks.size();
            for (std::int64_t p : b.positions) {
                auto rk = std::make_tuple(b.hp_family, b.phase_set, p);
                if (!route.emplace(rk, idx).second)
                    throw std::runtime_error(block_path + ": route-site belongs to multiple blocks");
            }
            blocks.push_back(std::move(b));
            ++stats["blocks_total"];
        }

        // ── molecule calls ────────────────────────────────────────
        std::string mol_path;
        for (const auto& e : fs::directory_iterator(fs::path(chrom_dir) / "extraction")) {
            const std::string n = e.path().filename().string();
            if (n.size() > 27 && n.rfind(".molecule_sparse_calls.tsv.gz") == n.size() - 29) {
                if (!mol_path.empty()) throw std::runtime_error("multiple molecule_sparse_calls.tsv.gz");
                mol_path = e.path().string();
            }
        }
        if (mol_path.empty()) throw std::runtime_error(chrom_dir + ": molecule_sparse_calls.tsv.gz not found");

        GzLineReader mr(mol_path);
        if (!mr.next(line)) throw std::runtime_error(mol_path + ": empty");
        auto mh = split_tab(line);
        const std::size_t mi_ds = header_index(mh, "dataset", mol_path);
        const std::size_t mi_ch = header_index(mh, "chrom", mol_path);
        const std::size_t mi_mid = header_index(mh, "molecule_id", mol_path);
        const std::size_t mi_qs = header_index(mh, "qname_sha256", mol_path);
        const std::size_t mi_hp = header_index(mh, "hp_family", mol_path);
        const std::size_t mi_ps = header_index(mh, "phase_set", mol_path);
        const std::size_t mi_pos = header_index(mh, "positions1", mol_path);
        const std::size_t mi_cc = header_index(mh, "call_codes", mol_path);

        const std::string kValid = "RAODSLX";
        std::vector<Assignment> raw;
        // patterns[block][vector] -> count
        std::vector<std::unordered_map<std::string, std::uint32_t>> patterns(blocks.size());
        std::set<std::string> seen_mol;

        while (mr.next(line)) {
            if (line.empty()) continue;
            ++stats["molecule_rows_total"];
            auto f = split_tab(line);
            if (f[mi_ds] != sample || f[mi_ch] != chrom)
                throw std::runtime_error(mol_path + ": dataset/chrom mismatch");
            const std::string& mid = f[mi_mid];
            if (mid.empty() || !seen_mol.insert(mid).second)
                throw std::runtime_error(mol_path + ": empty or duplicate molecule_id");
            const std::string& hp = f[mi_hp];
            const std::string& ps = f[mi_ps];
            if ((hp != "1" && hp != "2") || ps.empty()) {
                ++stats["molecule_rows_nonprimary_or_missing_ps"];
                continue;
            }
            auto observed = parse_positions(f[mi_pos], mid);
            const std::string& codes = f[mi_cc];
            if (observed.size() != codes.size()) throw std::runtime_error(mol_path + ": invalid molecule call vector");
            for (char c : codes)
                if (kValid.find(c) == std::string::npos) throw std::runtime_error(mol_path + ": invalid call code");

            // projected[block] -> {position: code}
            std::map<std::size_t, std::map<std::int64_t, char>> projected;
            for (std::size_t i = 0; i < observed.size(); ++i) {
                auto it = route.find(std::make_tuple(hp, ps, observed[i]));
                if (it == route.end()) continue;
                const char c = codes[i];
                projected[it->second][observed[i]] = (c == 'R' || c == 'A') ? c : 'X';
            }
            for (const auto& [bidx, calls] : projected) {
                const Block& b = blocks[bidx];
                std::string vec;
                vec.reserve(b.positions.size());
                for (std::int64_t p : b.positions) {
                    auto cit = calls.find(p);
                    vec.push_back(cit == calls.end() ? 'X' : cit->second);
                }
                if (vec.find_first_not_of('X') == std::string::npos) {
                    ++stats["projected_molecule_blocks_without_fixed_ra"];
                    continue;
                }
                ++patterns[bidx][vec];
                ++stats["projected_molecule_block_incidences"];
                Assignment a;
                a.molecule_id = mid;
                a.qname_sha256 = f[mi_qs];
                a.hp_family = hp;
                a.phase_set = ps;
                a.block_ref = bidx;
                a.n_fixed_ra = static_cast<int>(
                    std::count_if(vec.begin(), vec.end(), [](char c) { return c == 'R' || c == 'A'; }));
                a.is_full_cov = vec.find('X') == std::string::npos;
                a.pattern_vector = std::move(vec);
                raw.push_back(std::move(a));
            }
        }

        // ── tree_supported（與 mlhp adapter :481-490 同判準）──────
        for (Assignment& a : raw) {
            const Block& b = blocks[a.block_ref];
            const bool k_ok = b.positions.size() >= 2;
            const std::uint32_t w = patterns[a.block_ref][a.pattern_vector];
            a.tree_supported = k_ok && w >= static_cast<std::uint32_t>(min_read);
            if (a.tree_supported) ++stats["tree_supported_incidences"];
            if (a.is_full_cov) ++stats["full_cov_incidences"];
        }

        // ── 輸出 ──────────────────────────────────────────────────
        fs::create_directories(fs::path(out_path).parent_path());
        gzFile fout = gzopen(out_path.c_str(), "wb");
        if (fout == nullptr) throw std::runtime_error("cannot write: " + out_path);
        gzwrite(fout, kHeader, static_cast<unsigned>(std::strlen(kHeader)));
        std::uint64_t rows = 0;
        for (const Assignment& a : raw) {
            const Block& b = blocks[a.block_ref];
            std::ostringstream o;
            o << sample << '\t' << chrom << '\t' << a.molecule_id << '\t' << a.qname_sha256 << '\t' << a.hp_family
              << '\t' << a.phase_set << '\t' << b.unit_id << '\t' << b.block_id << '\t' << b.block_index << '\t'
              << chrom << "|PS=" << a.phase_set << "|HP=" << a.hp_family << '|' << b.block_id << '\t'
              << a.pattern_vector << '\t' << b.positions.size() << '\t' << a.n_fixed_ra << '\t'
              << (a.is_full_cov ? "true" : "false") << '\t' << (a.tree_supported ? "true" : "false") << '\n';
            const std::string s = o.str();
            gzwrite(fout, s.data(), static_cast<unsigned>(s.size()));
            ++rows;
        }
        gzclose(fout);

        if (!receipt_path.empty()) {
            json_t* rj = json_object();
            json_object_set_new(rj, "schema_name", json_string(kSchemaName));
            json_object_set_new(rj, "schema_version", json_string(kSchemaVersion));
            json_object_set_new(rj, "sample", json_string(sample.c_str()));
            json_object_set_new(rj, "chrom", json_string(chrom.c_str()));
            json_object_set_new(rj, "min_read", json_integer(min_read));
            json_object_set_new(rj, "blocks_input", json_string(block_path.c_str()));
            json_object_set_new(rj, "molecule_calls_input", json_string(mol_path.c_str()));
            json_object_set_new(rj, "output", json_string(out_path.c_str()));
            json_object_set_new(rj, "rows", json_integer(static_cast<json_int_t>(rows)));
            json_t* sj = json_object();
            for (const auto& [k, v] : stats)
                json_object_set_new(sj, k.c_str(), json_integer(static_cast<json_int_t>(v)));
            json_object_set_new(rj, "metrics", sj);
            json_dump_file(rj, receipt_path.c_str(), JSON_INDENT(1) | JSON_SORT_KEYS);
            json_decref(rj);
        }

        std::cout << "rows written              : " << rows << "\n"
                  << "molecule_rows_total       : " << stats["molecule_rows_total"] << "\n"
                  << "blocks_total              : " << stats["blocks_total"] << "\n"
                  << "projected_incidences      : " << stats["projected_molecule_block_incidences"] << "\n"
                  << "  full_cov                : " << stats["full_cov_incidences"] << "\n"
                  << "  tree_supported          : " << stats["tree_supported_incidences"] << "\n"
                  << "blocks_without_fixed_ra   : " << stats["projected_molecule_blocks_without_fixed_ra"] << "\n"
                  << "nonprimary_or_missing_ps  : " << stats["molecule_rows_nonprimary_or_missing_ps"] << "\n"
                  << "output                    : " << out_path << "\n";
    } catch (const std::exception& ex) {
        std::cerr << "BUILD FAILED: " << ex.what() << "\n";
        return 2;
    }
    return 0;
}
