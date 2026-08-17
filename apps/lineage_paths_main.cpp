// SPDX-License-Identifier: GPL-3.0-only
//
// lineage_paths — 從 topology.jsonl 的 representative_best_edges 算出
// 每個 vertex 的階層路徑（HP1-1-1 形式）、突變順序與深度。
//
// C++ 取代 build_lineage_paths.py（該 Python 版已驗證，作為 parity 對照）。
//
// 規則（docs/HIERARCHICAL_TAG_SPEC.md）
//   lineage_path = HP{hp_family}                     若 vertex 是 ROOT
//                = HP{hp_family}-{d1}-...-{dk}       否則，d_i = 同層兄弟序號(1-based)
//   同層兄弟排序：acquired_position 升冪，再 child_vertex 升冪
//   hidden node（H_ 前綴）納入 depth 計數
//
// 用法:
//   lineage_paths --topology topology.jsonl [--chrom chr1] [--unique-only]
//                 --output paths.tsv.gz [--receipt receipt.json]

#include <jansson.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr const char* kSchemaName = "intersubmod.unit_lineage_paths";
constexpr const char* kSchemaVersion = "1.0.0";

constexpr const char* kHeader =
    "region_id\tchrom\thp_family\tphase_set\tunit_id\tblock_id\tvertex\tvertex_label\t"
    "is_hidden\tdepth\tlineage_path\tparent_vertex\tacquired_position\tmutation_order\t"
    "edge_score_fraction\tn_children\tbest_tree_unique\tfamily_status\n";

struct Edge {
    std::int64_t child_vertex = 0;
    std::int64_t parent_vertex = 0;
    std::int64_t acquired_position = 0;
    std::string edge_score_fraction;
};

struct Row {
    std::string region_id, chrom, hp_family, phase_set, unit_id, block_id;
    std::int64_t vertex = 0;
    std::string vertex_label;
    bool is_hidden = false;
    int depth = 0;
    std::string lineage_path;
    std::string parent_vertex;      // "." 表示 root
    std::string acquired_position;  // "." 表示 root
    std::string mutation_order;     // "." 表示 root
    std::string edge_score_fraction;
    std::size_t n_children = 0;
    bool best_tree_unique = false;
    std::string family_status;
};

std::string json_str(json_t* obj, const char* key) {
    json_t* v = json_object_get(obj, key);
    if (v == nullptr) return "";
    if (json_is_string(v)) return json_string_value(v);
    if (json_is_integer(v)) return std::to_string(json_integer_value(v));
    return "";
}

std::int64_t json_int(json_t* obj, const char* key, std::int64_t fallback = 0) {
    json_t* v = json_object_get(obj, key);
    if (json_is_integer(v)) return json_integer_value(v);
    if (json_is_string(v)) {
        try {
            return std::stoll(json_string_value(v));
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

// 回傳 false 表示樹結構不合法（多 root / 重複 parent）
bool build_unit(json_t* rec, std::vector<Row>& out) {
    json_t* vertices = json_object_get(rec, "representative_best_vertices");
    if (!json_is_array(vertices) || json_array_size(vertices) == 0) return false;
    json_t* edges = json_object_get(rec, "representative_best_edges");

    std::map<std::int64_t, std::string> label_by_vertex;
    for (std::size_t i = 0; i < json_array_size(vertices); ++i) {
        json_t* v = json_array_get(vertices, i);
        label_by_vertex[json_int(v, "vertex")] = json_str(v, "label");
    }

    std::map<std::int64_t, std::vector<Edge>> children;
    std::map<std::int64_t, Edge> edge_of;
    std::set<std::int64_t> has_parent;

    if (json_is_array(edges)) {
        for (std::size_t i = 0; i < json_array_size(edges); ++i) {
            json_t* e = json_array_get(edges, i);
            Edge ed;
            ed.child_vertex = json_int(e, "child_vertex");
            ed.parent_vertex = json_int(e, "parent_vertex");
            ed.acquired_position = json_int(e, "acquired_position");
            ed.edge_score_fraction = json_str(e, "edge_score_fraction");
            if (!has_parent.insert(ed.child_vertex).second) return false;  // 重複 parent
            edge_of[ed.child_vertex] = ed;
            children[ed.parent_vertex].push_back(ed);
        }
    }

    std::vector<std::int64_t> roots;
    for (const auto& [v, _] : label_by_vertex) {
        if (has_parent.find(v) == has_parent.end()) roots.push_back(v);
    }
    if (roots.size() != 1) return false;

    for (auto& [pv, kids] : children) {
        (void)pv;
        std::sort(kids.begin(), kids.end(), [](const Edge& a, const Edge& b) {
            if (a.acquired_position != b.acquired_position) return a.acquired_position < b.acquired_position;
            return a.child_vertex < b.child_vertex;
        });
    }

    const std::string region_id = json_str(rec, "region_id");
    const std::string chrom = json_str(rec, "chrom");
    const std::string hp = json_str(rec, "hp_family");
    const std::string ps = json_str(rec, "phase_set");
    const std::string unit_id = json_str(rec, "unit_id");
    const std::string block_id = json_str(rec, "block_id");
    json_t* btu = json_object_get(rec, "best_tree_unique");
    const bool unique = json_is_true(btu);
    const std::string fam = json_str(rec, "family_status");

    // 迭代式 DFS，避免深樹爆棧
    struct Frame {
        std::int64_t vertex;
        std::vector<int> path;
        std::vector<std::int64_t> mut;
    };
    std::vector<Frame> stack;
    stack.push_back({roots[0], {}, {}});

    while (!stack.empty()) {
        Frame f = stack.back();
        stack.pop_back();

        Row r;
        r.region_id = region_id;
        r.chrom = chrom;
        r.hp_family = hp;
        r.phase_set = ps;
        r.unit_id = unit_id;
        r.block_id = block_id;
        r.vertex = f.vertex;
        auto it = label_by_vertex.find(f.vertex);
        r.vertex_label = (it == label_by_vertex.end()) ? "" : it->second;
        r.is_hidden = r.vertex_label.rfind("H_", 0) == 0;
        r.depth = static_cast<int>(f.path.size());

        std::ostringstream lp;
        lp << "HP" << hp;
        for (int d : f.path) lp << '-' << d;
        r.lineage_path = lp.str();

        auto eit = edge_of.find(f.vertex);
        if (eit == edge_of.end()) {
            r.parent_vertex = ".";
            r.acquired_position = ".";
            r.edge_score_fraction = ".";
        } else {
            r.parent_vertex = std::to_string(eit->second.parent_vertex);
            r.acquired_position = std::to_string(eit->second.acquired_position);
            r.edge_score_fraction = eit->second.edge_score_fraction.empty() ? "." : eit->second.edge_score_fraction;
        }

        if (f.mut.empty()) {
            r.mutation_order = ".";
        } else {
            std::ostringstream mo;
            for (std::size_t i = 0; i < f.mut.size(); ++i) {
                if (i != 0) mo << '>';
                mo << f.mut[i];
            }
            r.mutation_order = mo.str();
        }

        auto cit = children.find(f.vertex);
        r.n_children = (cit == children.end()) ? 0 : cit->second.size();
        r.best_tree_unique = unique;
        r.family_status = fam;
        out.push_back(std::move(r));

        if (cit != children.end()) {
            // 反向壓入，讓 pop 時維持兄弟順序（與 Python 版遞迴順序一致）
            for (std::size_t i = cit->second.size(); i-- > 0;) {
                Frame nf;
                nf.vertex = cit->second[i].child_vertex;
                nf.path = f.path;
                nf.path.push_back(static_cast<int>(i + 1));
                nf.mut = f.mut;
                nf.mut.push_back(cit->second[i].acquired_position);
                stack.push_back(std::move(nf));
            }
        }
    }
    return true;
}

void usage() {
    std::cerr << "Usage: lineage_paths --topology TOPOLOGY.jsonl --output PATHS.tsv.gz\n"
              << "                     [--chrom CHR] [--unique-only] [--receipt RECEIPT.json]\n\n"
              << "Computes hierarchical lineage paths (HP1-1-1 form) from\n"
              << "representative_best_edges. Hidden nodes (H_ prefix) count toward depth.\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string topo_path, out_path, receipt_path, chrom_filter;
    bool unique_only = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << what << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "--topology") {
            topo_path = next("--topology");
        } else if (a == "--output") {
            out_path = next("--output");
        } else if (a == "--receipt") {
            receipt_path = next("--receipt");
        } else if (a == "--chrom") {
            chrom_filter = next("--chrom");
        } else if (a == "--unique-only") {
            unique_only = true;
        } else {
            std::cerr << "unknown option: " << a << "\n";
            return 2;
        }
    }
    if (topo_path.empty() || out_path.empty()) {
        usage();
        return 2;
    }

    std::ifstream fin(topo_path);
    if (!fin) {
        std::cerr << "cannot open: " << topo_path << "\n";
        return 2;
    }

    gzFile fout = gzopen(out_path.c_str(), "wb");
    if (fout == nullptr) {
        std::cerr << "cannot write: " << out_path << "\n";
        return 2;
    }
    gzwrite(fout, kHeader, static_cast<unsigned>(std::strlen(kHeader)));

    std::map<std::string, std::uint64_t> stats;
    std::uint64_t rows_written = 0;
    std::string line;

    while (std::getline(fin, line)) {
        if (line.empty()) continue;
        json_error_t err;
        json_t* rec = json_loads(line.c_str(), 0, &err);
        if (rec == nullptr) {
            ++stats["malformed_lines"];
            continue;
        }
        const std::string rc = json_str(rec, "chrom");
        if (!chrom_filter.empty() && rc != chrom_filter) {
            json_decref(rec);
            continue;
        }
        ++stats["units_seen"];
        if (unique_only && !json_is_true(json_object_get(rec, "best_tree_unique"))) {
            ++stats["skipped_not_unique"];
            json_decref(rec);
            continue;
        }
        json_t* vs = json_object_get(rec, "representative_best_vertices");
        if (!json_is_array(vs) || json_array_size(vs) == 0) {
            ++stats["skipped_no_vertices"];
            json_decref(rec);
            continue;
        }

        std::vector<Row> rows;
        if (!build_unit(rec, rows)) {
            ++stats["invalid_tree"];
            json_decref(rec);
            continue;
        }
        ++stats["units_with_paths"];

        for (const Row& r : rows) {
            ++stats["vertices_total"];
            if (r.is_hidden) ++stats["vertices_hidden"];
            ++stats["depth_" + std::to_string(std::min(r.depth, 9))];

            std::ostringstream o;
            o << r.region_id << '\t' << r.chrom << '\t' << r.hp_family << '\t' << r.phase_set << '\t' << r.unit_id
              << '\t' << r.block_id << '\t' << r.vertex << '\t' << r.vertex_label << '\t'
              << (r.is_hidden ? "true" : "false") << '\t' << r.depth << '\t' << r.lineage_path << '\t'
              << r.parent_vertex << '\t' << r.acquired_position << '\t' << r.mutation_order << '\t'
              << r.edge_score_fraction << '\t' << r.n_children << '\t' << (r.best_tree_unique ? "true" : "false")
              << '\t' << r.family_status << '\n';
            const std::string s = o.str();
            gzwrite(fout, s.data(), static_cast<unsigned>(s.size()));
            ++rows_written;
        }
        json_decref(rec);
    }
    gzclose(fout);

    if (!receipt_path.empty()) {
        json_t* rj = json_object();
        json_object_set_new(rj, "schema_name", json_string(kSchemaName));
        json_object_set_new(rj, "schema_version", json_string(kSchemaVersion));
        json_object_set_new(rj, "chrom", json_string(chrom_filter.empty() ? "ALL" : chrom_filter.c_str()));
        json_object_set_new(rj, "unique_only", unique_only ? json_true() : json_false());
        json_object_set_new(rj, "input", json_string(topo_path.c_str()));
        json_object_set_new(rj, "output", json_string(out_path.c_str()));
        json_object_set_new(rj, "rows", json_integer(static_cast<json_int_t>(rows_written)));
        json_t* sj = json_object();
        for (const auto& [k, v] : stats) json_object_set_new(sj, k.c_str(), json_integer(static_cast<json_int_t>(v)));
        json_object_set_new(rj, "stats", sj);
        if (json_dump_file(rj, receipt_path.c_str(), JSON_INDENT(1) | JSON_SORT_KEYS) != 0) {
            std::cerr << "failed to write receipt: " << receipt_path << "\n";
            json_decref(rj);
            return 2;
        }
        json_decref(rj);
    }

    std::cout << "units_seen        : " << stats["units_seen"] << "\n"
              << "units_with_paths  : " << stats["units_with_paths"] << "\n"
              << "vertices_total    : " << stats["vertices_total"] << "\n"
              << "  hidden (H_)     : " << stats["vertices_hidden"] << "\n"
              << "invalid_tree      : " << stats["invalid_tree"] << "\n";
    std::cout << "depth 分佈        : ";
    for (int d = 0; d < 6; ++d) {
        auto it = stats.find("depth_" + std::to_string(d));
        if (it != stats.end()) std::cout << d << "=" << it->second << " ";
    }
    std::cout << "\nrows              : " << rows_written << "\noutput            : " << out_path << "\n";
    return 0;
}
