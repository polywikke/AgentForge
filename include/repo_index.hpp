#pragma once

#include "types.hpp"
#include "win_utf8.hpp"
#include "symbol_extract.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// ============================================================================
// 仓库代码地图 (L8)
//
// 三要素：
//   1. 符号抽取：class/struct/enum/function/method（词法解析，tree-sitter 的兜底）
//   2. 依赖建图：#include "..." / <...> → 文件有向边
//   3. 重要性排序：个性化 PageRank（活动文件偏置）
//
// 输出：按 rank 排序、按字符预算截断的符号清单
// ============================================================================

struct FileInfo {
    std::string path;
    std::size_t size = 0;
    std::string extension;
    int64_t modified_time = 0;
    double rank = 0.0;
    std::vector<std::string> includes;
};

struct SymbolInfo {
    std::string name;
    std::string kind;       // class / struct / enum / function / method
    std::string signature;  // 签名（供地图渲染）
    std::string file;
    int line = 0;
};

class RepoIndex {
public:
    RepoIndex() = default;

    bool build(const std::string& root_path,
               const std::vector<std::string>& active_files = {}) {
        namespace fs = std::filesystem;
        root_path_ = root_path;
        files_.clear();
        by_name_.clear();
        symbols_by_file_.clear();
        active_files_ = active_files;

        std::error_code ec;
        if (!fs::is_directory(utf8ToPath(root_path), ec)) return false;

        scanDirectory(root_path);
        extractSymbols();
        buildEdges();
        computePageRank(active_files);
        built_ = true;
        return true;
    }

    bool built() const { return built_; }
    const std::string& root() const { return root_path_; }

    std::vector<FileInfo> searchFiles(const std::string& pattern) const {
        std::vector<FileInfo> results;
        for (const auto& file : files_) {
            if (file.path.find(pattern) != std::string::npos) results.push_back(file);
        }
        return results;
    }

    std::vector<SymbolInfo> searchSymbols(const std::string& name) const {
        std::vector<SymbolInfo> results;
        auto it = by_name_.find(name);
        if (it != by_name_.end()) results = it->second;
        return results;
    }

    // 代码地图：按 PageRank 排序，按字符预算截断
    std::string renderMap(std::size_t max_chars = 10000,
                          const std::vector<std::string>& active_files = {}) const {
        if (!built_) return "（仓库尚未建立索引）";

        auto active = active_files.empty() ? active_files_ : active_files;
        std::map<std::string, double> bias;
        for (const auto& f : active) bias[f] = 8.0;

        std::vector<FileInfo> sorted = files_;
        std::sort(sorted.begin(), sorted.end(),
                  [](const FileInfo& a, const FileInfo& b) { return a.rank > b.rank; });

        std::ostringstream oss;
        oss << "仓库结构（按重要性排序，root: " << root_path_ << "）：\n";
        std::set<std::string> emitted;

        for (const auto& file : sorted) {
            if (oss.str().size() >= max_chars) break;

            std::ostringstream block;
            std::string display = file.path;
            if (display.size() > 100) display = "..." + display.substr(display.size() - 97);
            block << "\n" << display;
            if (bias.count(file.path)) block << "  [active]";
            block << "  (rank " << std::fixed << std::setprecision(3) << file.rank << ")\n";

            auto sit = symbols_by_file_.find(file.path);
            if (sit != symbols_by_file_.end()) {
                int shown = 0;
                for (const auto& sym : sit->second) {
                    if (shown >= 12) {
                        block << "  …\n";
                        break;
                    }
                    block << "  " << sym.kind << ": " << sym.signature << "\n";
                    ++shown;
                }
            }

            std::string text = block.str();
            if (oss.str().size() + text.size() > max_chars) {
                std::size_t remain = max_chars > oss.str().size()
                                         ? max_chars - oss.str().size()
                                         : 0;
                oss << utf8Prefix(text, remain) << "\n… (地图已截断)";
                break;
            }
            oss << text;
            emitted.insert(file.path);
        }

        if (sorted.empty()) return "（空仓库）";
        return oss.str();
    }

    // L3/L8 检索：按符号名、文件路径、文件内容三种信号打分
    std::vector<RetrievedSnippet> search(const std::string& query,
                                         int max_results = 8) const {
        std::vector<RetrievedSnippet> results;
        if (query.empty() || !built_) return results;

        std::vector<std::string> tokens = tokenize(query);
        if (tokens.empty()) return results;

        for (const auto& file : files_) {
            double score = 0.0;
            std::string matched_text;
            int line = 0;

            std::string lower_path = lowerCopy(file.path);
            for (const auto& tok : tokens) {
                if (lower_path.find(tok) != std::string::npos) score += 1.0;
            }

            auto sit = symbols_by_file_.find(file.path);
            if (sit != symbols_by_file_.end()) {
                for (const auto& sym : sit->second) {
                    std::string lower_name = lowerCopy(sym.name);
                    for (const auto& tok : tokens) {
                        if (lower_name.find(tok) != std::string::npos) {
                            score += 4.0;
                            matched_text = sym.kind + ": " + sym.signature;
                            line = sym.line;
                        }
                    }
                }
            }

            if (score == 0.0 && file.size < 512 * 1024) {
                // 内容检索（grep 兜底）
                std::ifstream in(utf8ToPath(file.path), std::ios::binary);
                std::string text((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
                std::string lower = lowerCopy(text);
                for (const auto& tok : tokens) {
                    auto pos = lower.find(tok);
                    if (pos != std::string::npos) {
                        score += 1.5;
                        int line_no = 1;
                        for (std::size_t i = 0; i < pos && i < text.size(); ++i) {
                            if (text[i] == '\n') ++line_no;
                        }
                        std::size_t start = pos > 100 ? pos - 100 : 0;
                        matched_text = utf8Prefix(text.substr(start), 300);
                        line = line_no;
                        break;
                    }
                }
            }

            if (score > 0.0) {
                RetrievedSnippet snippet;
                snippet.path = file.path;
                snippet.line = line;
                snippet.score = score * (1.0 + file.rank * 10.0);
                snippet.text = matched_text.empty() ? file.path : matched_text;
                results.push_back(std::move(snippet));
            }
        }

        std::sort(results.begin(), results.end(),
                  [](const RetrievedSnippet& a, const RetrievedSnippet& b) {
                      return a.score > b.score;
                  });
        if ((int)results.size() > max_results) results.resize(max_results);
        return results;
    }

    struct Stats {
        int total_files = 0;
        int total_symbols = 0;
        std::map<std::string, int> files_by_ext;
    };

    Stats getStats() const {
        Stats stats;
        stats.total_files = static_cast<int>(files_.size());
        for (const auto& file : files_) stats.files_by_ext[file.extension]++;
        for (const auto& [name, infos] : by_name_) {
            stats.total_symbols += static_cast<int>(infos.size());
        }
        return stats;
    }

    const std::vector<SymbolInfo>& symbolsInFile(const std::string& file) const {
        static const std::vector<SymbolInfo> empty;
        auto it = symbols_by_file_.find(file);
        return it == symbols_by_file_.end() ? empty : it->second;
    }

private:
    std::string root_path_;
    std::vector<FileInfo> files_;
    std::map<std::string, std::vector<SymbolInfo>> by_name_;
    std::map<std::string, std::vector<SymbolInfo>> symbols_by_file_;
    std::vector<std::string> active_files_;
    bool built_ = false;

    static std::string lowerCopy(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    static std::vector<std::string> tokenize(const std::string& query) {
        std::vector<std::string> tokens;
        std::string cur;
        for (unsigned char c : query) {
            if (std::isalnum(c) || c == '_' || c >= 0x80) {
                cur += (char)c;
            } else if (!cur.empty()) {
                if (cur.size() >= 2) tokens.push_back(lowerCopy(cur));
                cur.clear();
            }
        }
        if (cur.size() >= 2) tokens.push_back(lowerCopy(cur));
        return tokens;
    }

    static bool skipPath(const std::string& path) {
        std::string lower = lowerCopy(path);
        static const std::vector<std::string> blocked = {
            "/.git/", "\\.git\\", "/build/", "\\build\\", "/bin/", "\\bin\\",
            "/node_modules/", "\\node_modules\\", "/.af-dump/", "\\.af-dump\\",
            "/logs/", "\\logs\\", "/sessions/", "\\sessions\\",
            "/conversations/", "\\conversations\\",
            "/thirdparty/", "\\thirdparty\\", "/.vs/", "\\.vs\\",
            "/include/nlohmann/", "\\include\\nlohmann\\",
            "/include/httplib.h", "\\include\\httplib.h"
        };
        for (const auto& b : blocked) {
            if (lower.find(b) != std::string::npos) return true;
        }
        return false;
    }

    static bool isTextFile(const std::string& ext) {
        static const std::set<std::string> exts = {
            ".cpp", ".hpp", ".h", ".c", ".cc", ".cxx",
            ".py", ".js", ".ts", ".java", ".cs", ".go",
            ".rs", ".rb", ".php", ".swift", ".kt",
            ".txt", ".md", ".json", ".xml", ".yaml", ".yml",
            ".cmake", ".ps1", ".bat", ".html", ".css"
        };
        return exts.count(ext) > 0;
    }

    void scanDirectory(const std::string& path) {
        namespace fs = std::filesystem;
        try {
            for (const auto& entry : fs::recursive_directory_iterator(utf8ToPath(path))) {
                if (!entry.is_regular_file()) continue;
                std::string p = pathToUtf8(entry.path());
                if (skipPath(p)) continue;
                FileInfo info;
                info.path = p;
                info.size = (std::size_t)entry.file_size();
                info.extension = pathToUtf8(entry.path().extension());
                if (info.extension.empty() || !isTextFile(info.extension)) continue;
                if (info.size > 1024 * 1024) continue;  // 跳过超大文件
                files_.push_back(std::move(info));
            }
        } catch (...) {
        }
    }

    void extractSymbols() {
        std::regex include_re(R"(^\s*#\s*include\s*[<"]([^>"]+)[>"]")");

        for (auto& file : files_) {
            try {
                std::ifstream in(utf8ToPath(file.path), std::ios::binary);
                if (!in) continue;
                std::string content((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
                content = sanitizeUtf8(content);

                {
                    std::istringstream lines(content);
                    std::string line;
                    int line_no = 0;
                    while (std::getline(lines, line) && line_no < 4000) {
                        ++line_no;
                        std::smatch match;
                        if (std::regex_search(line, match, include_re)) {
                            file.includes.push_back(match[1].str());
                        }
                    }
                }

                // L8: tree-sitter 真解析 + 词法兜底
                af::ExtractOptions opt;
                opt.backend = af::SymbolBackend::Auto;
                opt.maxBytes = 1u << 20;
                auto syms = af::extractSymbols(utf8ToPath(file.path), content, opt);

                for (const auto& sym : syms) {
                    SymbolInfo info;
                    info.kind = sym.kind;
                    info.name = sym.name;
                    info.signature = sym.signature.empty() ? sym.qualName : sym.signature;
                    info.file = file.path;
                    info.line = sym.line;
                    by_name_[info.name].push_back(info);
                    symbols_by_file_[file.path].push_back(std::move(info));
                }
            } catch (...) {
            }
        }
    }
    void buildEdges() {
        // basename / 相对路径 → 文件
        std::map<std::string, std::string> by_base;
        for (const auto& file : files_) {
            std::filesystem::path p = utf8ToPath(file.path);
            by_base[pathToUtf8(p.filename())] = file.path;
            by_base[file.path] = file.path;
        }

        for (auto& file : files_) {
            std::vector<std::string> resolved;
            for (const auto& inc : file.includes) {
                auto it = by_base.find(inc);
                if (it != by_base.end()) {
                    resolved.push_back(it->second);
                    continue;
                }
                auto bit = by_base.find(inc.substr(inc.find_last_of("/\\") + 1));
                if (bit != by_base.end()) resolved.push_back(bit->second);
            }
            file.includes = std::move(resolved);
        }
    }

    void computePageRank(const std::vector<std::string>& active_files) {
        std::size_t n = files_.size();
        if (n == 0) return;

        std::map<std::string, std::size_t> index;
        for (std::size_t i = 0; i < n; ++i) index[files_[i].path] = i;

        std::vector<std::vector<std::size_t>> out(n);
        for (std::size_t i = 0; i < n; ++i) {
            for (const auto& target : files_[i].includes) {
                auto it = index.find(target);
                if (it == index.end()) continue;
                std::size_t j = it->second;
                if (i == j) continue;
                out[i].push_back(j);
            }
        }

        const double d = 0.85;
        std::vector<double> rank(n, 1.0 / (double)n);
        std::set<std::string> active(active_files.begin(), active_files.end());

        for (int iter = 0; iter < 20; ++iter) {
            std::vector<double> next(n, (1.0 - d) / (double)n);
            double dangling = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                if (out[i].empty()) {
                    dangling += rank[i];
                } else {
                    double share = d * rank[i] / (double)out[i].size();
                    for (std::size_t j : out[i]) next[j] += share;
                }
            }
            for (std::size_t i = 0; i < n; ++i) {
                next[i] += d * dangling / (double)n;
                if (active.count(files_[i].path)) next[i] *= 8.0;
            }
            double sum = 0.0;
            for (double v : next) sum += v;
            if (sum > 0) {
                for (auto& v : next) v /= sum;
            }
            rank.swap(next);
        }

        for (std::size_t i = 0; i < n; ++i) files_[i].rank = rank[i];
    }

    static std::string trimCopy(const std::string& s) {
        std::size_t begin = 0;
        std::size_t end = s.size();
        while (begin < end && std::isspace((unsigned char)s[begin])) ++begin;
        while (end > begin && std::isspace((unsigned char)s[end - 1])) --end;
        std::string out = s.substr(begin, end - begin);
        if (out.size() > 160) out = out.substr(0, 157) + "...";
        return out;
    }
};
