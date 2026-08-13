// SPDX-License-Identifier: GPL-3.0-only
//
// tag_bam — 讀 raw BAM，一次注入 sidecar 的 HP/PS 與 lineage 階層標籤。
// C++ 取代 ll_bam_tag.py，使用 htslib 原生多執行緒（等同 samtools --threads）。
//
// 寫入的 aux tag（SAM 規格：含小寫字母者保留給 local use）
//   HP:Z  九態 HP          ← sidecar
//   PS:i  phase set         ← sidecar
//   lc:Z  unit_id           ← assignments
//   lu:Z  block_id          ← assignments
//   lv:Z  階層路徑 HP1-1-1  ← lineage_paths（以 pattern==vertex_label join）
//   lp:Z  觀察 pattern      ← assignments
//   lo:Z  突變順序          ← lineage_paths
//   ls:A  U/M/P/A           ← 由 is_full_cov + topology 判定
//
// 不變式：ls=='A' 不寫 lv/lo；lv 存在必有 ls；ls!='U' 時 lv 僅為代表值。

#include <htslib/hts.h>
#include <htslib/kstring.h>
#include <htslib/sam.h>
#include <htslib/tbx.h>
#include <htslib/thread_pool.h>
#include <jansson.h>
#include <openssl/evp.h>
#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

std::string sha256_hex(const std::string& in) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, in.data(), in.size());
    EVP_DigestFinal_ex(ctx, md, &len);
    EVP_MD_CTX_free(ctx);
    static const char* hexd = "0123456789abcdef";
    std::string out(len * 2, '0');
    for (unsigned i = 0; i < len; ++i) {
        out[2 * i] = hexd[md[i] >> 4];
        out[2 * i + 1] = hexd[md[i] & 0xF];
    }
    return out;
}

std::vector<std::string> split_tab(const std::string& s) {
    std::vector<std::string> out;
    std::size_t st = 0;
    while (true) {
        std::size_t p = s.find('\t', st);
        if (p == std::string::npos) {
            out.push_back(s.substr(st));
            break;
        }
        out.push_back(s.substr(st, p - st));
        st = p + 1;
    }
    return out;
}

class GzLines {
   public:
    explicit GzLines(const std::string& p) : fp_(gzopen(p.c_str(), "rb")), buf_(1 << 20) {
        if (fp_ == nullptr) throw std::runtime_error("cannot open: " + p);
    }
    ~GzLines() {
        if (fp_) gzclose(fp_);
    }
    bool next(std::string& line) {
        line.clear();
        while (true) {
            if (gzgets(fp_, buf_.data(), static_cast<int>(buf_.size())) == nullptr) return !line.empty();
            std::size_t n = std::strlen(buf_.data());
            bool done = (n > 0 && buf_[n - 1] == '\n');
            line.append(buf_.data(), done ? n - 1 : n);
            if (done) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return true;
            }
        }
    }

   private:
    gzFile fp_;
    std::vector<char> buf_;
};

std::size_t hidx(const std::vector<std::string>& h, const std::string& n, const std::string& p) {
    auto it = std::find(h.begin(), h.end(), n);
    if (it == h.end()) throw std::runtime_error(p + ": missing column " + n);
    return static_cast<std::size_t>(std::distance(h.begin(), it));
}

struct Assign {
    std::string unit_id, block_id, region_id, pattern;
    bool full_cov = false, tree_supported = false;
};
struct PathInfo {
    std::string lineage_path, mutation_order, raw_label;
    long long vertex = -1, parent_vertex = -1;
};
// 一個 region 內的所有 vertex，供 partial read 求最近共同祖先(LCA)
//
// 🔴 by_label 的 key 必須是**狀態字串**（純 R/A），不是 lineage_paths 的
//    vertex_label 原值。vertex_label 有三種形態：
//      實測節點  "AAR"      —— 本來就是狀態字串
//      補入節點  "H_RAAR"   —— 多了 H_ 前綴
//      根        "ROOT"     —— 完全不是狀態字串
//    read pattern 永遠是純 R/A/X，拿它去查原值 key，補入節點與 ROOT
//    **在結構上不可能命中**。這正是 2026-08-09 查出的漏標根因：
//    compatible_labels() 產生的候選全是純 R/A，`by_label.count()` 對
//    "H_RAAR" 與 "ROOT" 一律落空 → cands 為空 → lca_of 回 -1 → 不寫 lv。
//    實測 chr21：3,067 個帶 lv 的 alignment 中落在補入節點的是 **0 個**；
//    全樣本 33.0% 的樹「非 ROOT 節點全是補入」，那些 region 一條 lv 都寫不出來，
//    佔有樹 region 的 assignment 列 48.8%。
struct RegionTree {
    std::map<std::string, PathInfo> by_label;   // 正規化後的狀態字串 -> info
    std::map<long long, std::string> label_of;  // vertex -> 正規化後的狀態字串
};

// vertex_label -> 狀態字串。width 為該 region 的狀態字串寬度（ROOT 用）。
inline std::string state_key(const std::string& label, std::size_t width) {
    if (label == "ROOT") return width ? std::string(width, 'R') : label;
    if (label.rfind("H_", 0) == 0) return label.substr(2);
    return label;
}
struct TopoInfo {
    bool unique = false;
    std::string family_status;
};

// ls 保守度：A < P < M < U
int sev(char c) { return c == 'A' ? 0 : c == 'P' ? 1 : c == 'M' ? 2 : 3; }

// pattern 含 X 時，X 位置可為 R 或 A；回傳該 region 內實際存在的相容 vertex_label
std::vector<std::string> compatible_labels(const RegionTree& tree, const std::string& pattern) {
    std::vector<std::size_t> unknown;
    for (std::size_t i = 0; i < pattern.size(); ++i)
        if (pattern[i] == 'X') unknown.push_back(i);
    // 上限保護：2^12 已遠超實務 block 大小
    if (unknown.empty() || unknown.size() > 12) return {};
    std::vector<std::string> out;
    const std::size_t total = static_cast<std::size_t>(1) << unknown.size();
    for (std::size_t mask = 0; mask < total; ++mask) {
        std::string cand = pattern;
        for (std::size_t b = 0; b < unknown.size(); ++b) cand[unknown[b]] = ((mask >> b) & 1U) ? 'A' : 'R';
        if (tree.by_label.count(cand) != 0) out.push_back(cand);
    }
    return out;
}

// lg 取「層數最深」的一段。深度以 '-' 數量衡量（HP2-1-1 比 HP2-1 深）。
// 傳進來的 path 是 lineage_path 本身，不含 '+'（'+' 是寫 lv 時才加的）。
inline void take_group(std::string& lg, int& depth, const std::string& path) {
    const int d = static_cast<int>(std::count(path.begin(), path.end(), '-'));
    if (d > depth) {
        depth = d;
        lg = path;
    }
}

// 由 vertex 往上走到 root 的路徑（含自身），root 在最後
std::vector<long long> path_to_root(const RegionTree& tree, long long v) {
    std::vector<long long> chain;
    for (int guard = 0; guard < 64 && v >= 0; ++guard) {
        chain.push_back(v);
        auto lit = tree.label_of.find(v);
        if (lit == tree.label_of.end()) break;
        auto pit = tree.by_label.find(lit->second);
        if (pit == tree.by_label.end()) break;
        v = pit->second.parent_vertex;
    }
    return chain;
}

// 一組 vertex 的最近共同祖先；找不到回 -1
long long lca_of(const RegionTree& tree, const std::vector<std::string>& labels) {
    if (labels.empty()) return -1;
    std::vector<std::vector<long long>> chains;
    chains.reserve(labels.size());
    for (const std::string& l : labels) {
        auto it = tree.by_label.find(l);
        if (it == tree.by_label.end()) return -1;
        auto c = path_to_root(tree, it->second.vertex);
        std::reverse(c.begin(), c.end());  // root 在前
        chains.push_back(std::move(c));
    }
    std::size_t shortest = chains[0].size();
    for (const auto& c : chains) shortest = std::min(shortest, c.size());
    long long lca = -1;
    for (std::size_t d = 0; d < shortest; ++d) {
        const long long v = chains[0][d];
        bool all = true;
        for (const auto& c : chains)
            if (c[d] != v) {
                all = false;
                break;
            }
        if (!all) break;
        lca = v;
    }
    return lca;
}

void usage() {
    std::cerr << "Usage: tag_bam --in-bam RAW.bam --sidecar TAGS.tsv.gz --assignments A.tsv.gz\n"
              << "               --region CHR[:START-END] --out-bam OUT.bam\n"
              << "               [--lineage-paths P.tsv.gz] [--topology T.jsonl]\n"
              << "               [--threads 4] [--receipt R.json] [--out-format bam|cram]\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string in_bam, sidecar, assign_p, paths_p, topo_p, region, out_bam, receipt_p, out_fmt = "bam";
    int threads = 4;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto nx = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << a << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a == "--in-bam")
            in_bam = nx();
        else if (a == "--sidecar")
            sidecar = nx();
        else if (a == "--assignments")
            assign_p = nx();
        else if (a == "--lineage-paths")
            paths_p = nx();
        else if (a == "--topology")
            topo_p = nx();
        else if (a == "--region")
            region = nx();
        else if (a == "--out-bam")
            out_bam = nx();
        else if (a == "--receipt")
            receipt_p = nx();
        else if (a == "--out-format")
            out_fmt = nx();
        else if (a == "--threads")
            threads = std::stoi(nx());
        else {
            std::cerr << "unknown option: " << a << "\n";
            return 2;
        }
    }
    if (in_bam.empty() || sidecar.empty() || assign_p.empty() || region.empty() || out_bam.empty()) {
        usage();
        return 2;
    }

    std::string chrom = region;
    if (auto c = region.find(':'); c != std::string::npos) chrom = region.substr(0, c);

    std::map<std::string, std::uint64_t> st;

    try {
        // ── assignments ───────────────────────────────────────────
        std::unordered_map<std::string, std::vector<Assign>> assign;
        {
            GzLines r(assign_p);
            std::string line;
            if (!r.next(line)) throw std::runtime_error(assign_p + ": empty");
            auto h = split_tab(line);
            const std::size_t i_ch = hidx(h, "chrom", assign_p), i_qs = hidx(h, "qname_sha256", assign_p);
            const std::size_t i_ui = hidx(h, "unit_id", assign_p), i_bi = hidx(h, "block_id", assign_p);
            const std::size_t i_ri = hidx(h, "region_id", assign_p), i_pv = hidx(h, "pattern_vector", assign_p);
            const std::size_t i_fc = hidx(h, "is_full_cov", assign_p), i_ts = hidx(h, "tree_supported", assign_p);
            while (r.next(line)) {
                if (line.empty()) continue;
                auto f = split_tab(line);
                if (f[i_ch] != chrom) continue;
                Assign a;
                a.unit_id = f[i_ui];
                a.block_id = f[i_bi];
                a.region_id = f[i_ri];
                a.pattern = f[i_pv];
                a.full_cov = (f[i_fc] == "true");
                a.tree_supported = (f[i_ts] == "true");
                assign[f[i_qs]].push_back(std::move(a));
            }
        }
        std::cerr << "assignments   : " << assign.size() << " qname_sha256\n";

        // ── lineage_paths（region_id + vertex_label -> path）──────
        std::unordered_map<std::string, RegionTree> trees;
        std::unordered_map<std::string, std::vector<PathInfo>> staging;
        std::size_t n_vertices = 0;
        if (!paths_p.empty()) {
            GzLines r(paths_p);
            std::string line;
            if (r.next(line)) {
                auto h = split_tab(line);
                const std::size_t i_ri = hidx(h, "region_id", paths_p), i_vl = hidx(h, "vertex_label", paths_p);
                const std::size_t i_lp = hidx(h, "lineage_path", paths_p), i_mo = hidx(h, "mutation_order", paths_p);
                const std::size_t i_v = hidx(h, "vertex", paths_p), i_pv = hidx(h, "parent_vertex", paths_p);
                while (r.next(line)) {
                    if (line.empty()) continue;
                    auto f = split_tab(line);
                    PathInfo pi;
                    pi.lineage_path = f[i_lp];
                    pi.mutation_order = f[i_mo];
                    try {
                        pi.vertex = std::stoll(f[i_v]);
                    } catch (...) {
                        continue;
                    }
                    pi.parent_vertex = (f[i_pv] == ".") ? -1 : std::stoll(f[i_pv]);
                    pi.raw_label = f[i_vl];
                    staging[f[i_ri]].push_back(std::move(pi));
                    ++n_vertices;
                }
            }
        }
        // ROOT 的狀態字串寬度要看同 region 其他 vertex，所以必須整個 region
        // 讀完才能正規化 —— 這是上面用 staging 暫存而非直接建索引的原因。
        std::size_t n_root = 0, n_hidden = 0;
        for (auto& kv : staging) {
            std::size_t width = 0;
            for (const auto& pi : kv.second) {
                if (pi.raw_label == "ROOT") continue;
                width = std::max(width, state_key(pi.raw_label, 0).size());
            }
            RegionTree& t = trees[kv.first];
            for (auto& pi : kv.second) {
                const std::string key = state_key(pi.raw_label, width);
                if (pi.raw_label == "ROOT")
                    ++n_root;
                else if (pi.raw_label.rfind("H_", 0) == 0)
                    ++n_hidden;
                t.label_of[pi.vertex] = key;
                t.by_label[key] = std::move(pi);
            }
        }
        std::cerr << "lineage_paths : " << n_vertices << " vertices in " << trees.size() << " regions (正規化 ROOT "
                  << n_root << "、補入 " << n_hidden << ")\n";

        // ── topology（ls 判定）────────────────────────────────────
        std::unordered_map<std::string, TopoInfo> topo;
        if (!topo_p.empty()) {
            std::ifstream tf(topo_p);
            std::string line;
            while (std::getline(tf, line)) {
                if (line.empty()) continue;
                json_error_t e;
                json_t* d = json_loads(line.c_str(), 0, &e);
                if (d == nullptr) continue;
                json_t* c = json_object_get(d, "chrom");
                json_t* r = json_object_get(d, "region_id");
                if (json_is_string(c) && json_string_value(c) == chrom && json_is_string(r)) {
                    TopoInfo t;
                    t.unique = json_is_true(json_object_get(d, "best_tree_unique"));
                    json_t* fs = json_object_get(d, "family_status");
                    t.family_status = json_is_string(fs) ? json_string_value(fs) : "";
                    topo[json_string_value(r)] = std::move(t);
                }
                json_decref(d);
            }
        }
        std::cerr << "topology      : " << topo.size() << " region_id\n";

        // ── sidecar（tabix 只取所需區間）──────────────────────────
        std::unordered_map<std::string, std::pair<std::string, std::string>> side;
        {
            htsFile* tf = hts_open(sidecar.c_str(), "r");
            if (tf == nullptr) throw std::runtime_error("cannot open sidecar: " + sidecar);
            tbx_t* tbx = tbx_index_load(sidecar.c_str());
            if (tbx == nullptr) {
                hts_close(tf);
                throw std::runtime_error("cannot load tabix for: " + sidecar);
            }
            hts_itr_t* itr = tbx_itr_querys(tbx, region.c_str());
            kstring_t ks = {0, 0, nullptr};
            if (itr != nullptr) {
                while (tbx_itr_next(tf, tbx, itr, &ks) >= 0) {
                    auto f = split_tab(std::string(ks.s, ks.l));
                    if (f.size() >= 9) side[f[3]] = {f[7], f[8]};
                }
                hts_itr_destroy(itr);
            }
            free(ks.s);
            tbx_destroy(tbx);
            hts_close(tf);
        }
        std::cerr << "sidecar       : " << side.size() << " qname\n\n";

        // ── BAM 串流 ──────────────────────────────────────────────
        samFile* fin = sam_open(in_bam.c_str(), "r");
        if (fin == nullptr) throw std::runtime_error("cannot open BAM: " + in_bam);
        hts_idx_t* idx = sam_index_load(fin, in_bam.c_str());
        if (idx == nullptr) {
            sam_close(fin);
            throw std::runtime_error("cannot load BAM index");
        }
        bam_hdr_t* hdr = sam_hdr_read(fin);

        const std::string mode = (out_fmt == "cram") ? "wc" : "wb";
        samFile* fout = sam_open_format(out_bam.c_str(), mode.c_str(), nullptr);
        if (fout == nullptr) throw std::runtime_error("cannot write: " + out_bam);

        // htslib 原生多執行緒（等同 samtools --threads）
        htsThreadPool pool = {nullptr, 0};
        if (threads > 1) {
            pool.pool = hts_tpool_init(threads);
            if (pool.pool != nullptr) {
                hts_set_opt(fin, HTS_OPT_THREAD_POOL, &pool);
                hts_set_opt(fout, HTS_OPT_THREAD_POOL, &pool);
            }
        }
        if (sam_hdr_write(fout, hdr) < 0) throw std::runtime_error("failed to write header");

        hts_itr_t* iter = sam_itr_querys(idx, hdr, region.c_str());
        if (iter == nullptr) throw std::runtime_error("bad region: " + region);
        bam1_t* aln = bam_init1();

        while (sam_itr_next(fin, iter, aln) >= 0) {
            ++st["reads_total"];
            const std::string qname = bam_get_qname(aln);

            auto sit = side.find(qname);
            if (sit == side.end()) {
                ++st["no_sidecar_row"];
            } else {
                const std::string& hp = sit->second.first;
                const std::string& ps = sit->second.second;
                if (!hp.empty() && hp != ".") {
                    bam_aux_update_str(aln, "HP", static_cast<int>(hp.size()) + 1, hp.c_str());
                    ++st["hp_written"];
                }
                if (!ps.empty() && ps != ".") {
                    try {
                        bam_aux_update_int(aln, "PS", std::stoll(ps));
                        ++st["ps_written"];
                    } catch (...) {
                        ++st["ps_malformed"];
                    }
                }
            }

            auto ait = assign.find(sha256_hex(qname));
            if (ait == assign.end()) {
                ++st["no_lineage"];
            } else {
                const auto& es = ait->second;
                std::string lc, lu, lv, lp, lo;
                char ls = 'U';
                bool any_lv = false;
                // lg = 給 IGV「color by tag」用的合併觀察標籤，ln = 該歸屬有多不確定。
                //   lg：lv 去掉 '+' 後綴；read 跨多 block 時取**層數最深**的那一段。
                //       為什麼不照抄逗號串：多 block 的 lv 形如 "HP2+,HP2-1"，直接拿去
                //       配色會產生一堆只出現一兩次的組合值（實測 chr21 原值 30 種）。
                //       取最深那段 = 「這條分子最確定能到達的位置」，值收斂到 13 種，
                //       IGV 的 color-by-tag 才可讀。
                //   ln：相容頂點數。完整覆蓋直接命中頂點為 1；部分覆蓋走 LCA 時為
                //       compatible_labels() 的候選數。跨多 block 取**最大值**（最保守）。
                //       這個數字原本算完就丟（只進 receipt 的 lca_candidates_sum 總和），
                //       導致相容 2 個節點與相容 16 個節點的 read 在 BAM 裡完全一樣。
                std::string lg;
                int lg_depth = -1;
                int ln = 0;
                for (std::size_t i = 0; i < es.size(); ++i) {
                    const Assign& e = es[i];
                    if (i != 0) {
                        lc += ',';
                        lu += ',';
                        lp += ',';
                    }
                    lc += e.unit_id;
                    lu += e.block_id;
                    lp += e.pattern;

                    // ls 判定順序：**region 層的「定不出來」優先於 read 層的「覆蓋不足」**。
                    // 🔴 原本先看 full_cov 再看 topology，導致無樹 region 裡的部分覆蓋 read
                    //    被標成 'P'（只覆蓋部分），暗示「覆蓋夠就能定位」—— 但那個 region
                    //    根本沒有樹，覆蓋再多也定不出來，正確標記是 'A'。
                    //    實測 chr13 的 ABSTAIN_RESOURCE_LIMIT region：149/153 條被標 'P'，
                    //    下游若用 `ls != A` 篩掉拓撲未定的 read，會留下 97.4%。
                    //    全樣本 19.32% 的 read×region 落在無樹 region。
                    char c;
                    auto t = topo.find(e.region_id);
                    const bool undetermined =
                        (t == topo.end()) ? !e.tree_supported : (t->second.family_status != "FAMILY_COMPLETE");
                    if (undetermined)
                        c = 'A';
                    else if (!e.full_cov)
                        c = 'P';
                    else if (t == topo.end())
                        c = 'M';
                    else
                        c = t->second.unique ? 'U' : 'M';
                    if (sev(c) < sev(ls)) ls = c;

                    // lv/lo 只在非 abstain 時取（不變式 3）
                    if (c != 'A') {
                        auto tit = trees.find(e.region_id);
                        if (tit == trees.end()) ++st["lv_skip_region_has_no_tree"];
                        if (tit != trees.end()) {
                            const RegionTree& tree = tit->second;
                            auto pit = tree.by_label.find(e.pattern);
                            if (pit != tree.by_label.end()) {
                                // 完整覆蓋：pattern 直接命中一個 vertex
                                if (any_lv) {
                                    lv += ',';
                                    lo += ',';
                                }
                                lv += pit->second.lineage_path;
                                lo += pit->second.mutation_order;
                                any_lv = true;
                                ++st["lv_exact_vertex"];
                                take_group(lg, lg_depth, pit->second.lineage_path);
                                ln = std::max(ln, 1);
                                if (pit->second.raw_label == "ROOT")
                                    ++st["lv_at_root"];
                                else if (pit->second.raw_label.rfind("H_", 0) == 0)
                                    ++st["lv_at_hidden"];
                            } else if (!e.full_cov) {
                                // 部分覆蓋：枚舉相容 vertex 求最近共同祖先。
                                // 結論是「屬於該節點或其後代」，以 '+' 後綴標明是子樹範圍，
                                // 不是單一 vertex —— 這是可證的斷言，不是猜測。
                                auto cands = compatible_labels(tree, e.pattern);
                                const long long anc = lca_of(tree, cands);
                                if (anc >= 0) {
                                    auto lit = tree.label_of.find(anc);
                                    if (lit != tree.label_of.end()) {
                                        auto anc_it = tree.by_label.find(lit->second);
                                        if (anc_it != tree.by_label.end()) {
                                            if (any_lv) {
                                                lv += ',';
                                                lo += ',';
                                            }
                                            lv += anc_it->second.lineage_path + "+";
                                            lo += anc_it->second.mutation_order;
                                            any_lv = true;
                                            ++st["lca_resolved"];
                                            st["lca_candidates_sum"] += cands.size();
                                            take_group(lg, lg_depth, anc_it->second.lineage_path);
                                            ln = std::max(ln, static_cast<int>(cands.size()));
                                            if (anc_it->second.raw_label == "ROOT")
                                                ++st["lv_at_root"];
                                            else if (anc_it->second.raw_label.rfind("H_", 0) == 0)
                                                ++st["lv_at_hidden"];
                                        }
                                    }
                                } else if (!cands.empty()) {
                                    ++st["lca_no_common_ancestor"];
                                } else {
                                    // 相容集為空 = 該 pattern 與這棵樹的任何狀態都矛盾
                                    ++st["lv_skip_no_compatible_vertex"];
                                }
                            }
                        }
                    }
                }
                bam_aux_update_str(aln, "lc", static_cast<int>(lc.size()) + 1, lc.c_str());
                bam_aux_update_str(aln, "lu", static_cast<int>(lu.size()) + 1, lu.c_str());
                bam_aux_update_str(aln, "lp", static_cast<int>(lp.size()) + 1, lp.c_str());
                if (any_lv && ls != 'A') {
                    bam_aux_update_str(aln, "lv", static_cast<int>(lv.size()) + 1, lv.c_str());
                    bam_aux_update_str(aln, "lo", static_cast<int>(lo.size()) + 1, lo.c_str());
                    ++st["lv_written"];
                    if (!lg.empty()) {
                        bam_aux_update_str(aln, "lg", static_cast<int>(lg.size()) + 1, lg.c_str());
                        ++st["lg_written"];
                    }
                    if (ln > 0) {
                        bam_aux_update_int(aln, "ln", ln);
                        ++st["ln_written"];
                        if (ln == 1) ++st["ln_pinned"];
                    }
                }
                bam_aux_append(aln, "ls", 'A', 1, reinterpret_cast<const uint8_t*>(&ls));
                ++st["lineage_written"];
                ++st[std::string("ls_") + ls];
                if (es.size() > 1) ++st["multi_block_reads"];
            }

            if (sam_write1(fout, hdr, aln) < 0) throw std::runtime_error("failed to write alignment");
        }

        bam_destroy1(aln);
        hts_itr_destroy(iter);
        hts_idx_destroy(idx);
        bam_hdr_destroy(hdr);
        sam_close(fin);
        sam_close(fout);
        if (pool.pool != nullptr) hts_tpool_destroy(pool.pool);

        // 輸出必須自帶索引 —— 沒有 .bai/.crai 的 BAM 在 IGV 與任何 region query
        // 下都不可用，而且失敗方式是**靜默回空**（samtools view FILE chr21 印不出東西
        // 也不報錯），比直接報錯更難查。輸入是座標排序的、我們照序寫出，所以這裡
        // 一定建得起來；建不起來就 fail-loud，不要留一個半殘的產物。
        // 🔴 先前這一步在 scripts/run_sample.sh 裡用 samtools index 做，但只做在
        //    「合併成單一 BAM」那條路徑上 —— --split-by-chrom 產出的分檔全部無索引。
        //    索引屬於「輸出的一部分」，該由產出它的程式負責，不是 shell。
        {
            const int nthr = threads > 1 ? threads : 1;
            if (sam_index_build3(out_bam.c_str(), nullptr, 0, nthr) < 0)
                throw std::runtime_error("failed to build index for: " + out_bam);
            st["index_built"] = 1;
        }

        if (!receipt_p.empty()) {
            json_t* rj = json_object();
            json_object_set_new(rj, "schema_name", json_string("intersubmod.lineage_tagged_bam_receipt"));
            json_object_set_new(rj, "schema_version", json_string("1.0.0"));
            json_object_set_new(rj, "in_bam", json_string(in_bam.c_str()));
            json_object_set_new(rj, "sidecar", json_string(sidecar.c_str()));
            json_object_set_new(rj, "assignments", json_string(assign_p.c_str()));
            json_object_set_new(rj, "lineage_paths", json_string(paths_p.c_str()));
            json_object_set_new(rj, "topology", json_string(topo_p.c_str()));
            json_object_set_new(rj, "region", json_string(region.c_str()));
            json_object_set_new(rj, "out_bam", json_string(out_bam.c_str()));
            json_object_set_new(rj, "threads", json_integer(threads));
            json_t* sj = json_object();
            for (const auto& [k, v] : st) json_object_set_new(sj, k.c_str(), json_integer(static_cast<json_int_t>(v)));
            json_object_set_new(rj, "stats", sj);
            json_dump_file(rj, receipt_p.c_str(), JSON_INDENT(1) | JSON_SORT_KEYS);
            json_decref(rj);
        }

        for (const auto& [k, v] : st) {
            const std::size_t pad = (k.size() < 24U) ? (24U - k.size()) : 1U;
            std::cout << k << std::string(pad, ' ') << v << "\n";
        }
        std::cout << "\nout-bam: " << out_bam << "\n";
    } catch (const std::exception& ex) {
        std::cerr << "FAILED: " << ex.what() << "\n";
        return 2;
    }
    return 0;
}
