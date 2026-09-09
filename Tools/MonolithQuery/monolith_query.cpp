// monolith_query.exe — Standalone offline query tool for Monolith databases.
// Replaces monolith_offline.py with zero Python dependency.
// Links sqlite3 amalgamation directly. No Unreal Engine dependency.
//
// Usage:
//   monolith_query.exe source <action> [params...] [--options]
//   monolith_query.exe project <action> [params...] [--options]
//   monolith_query.exe monolith guide [--section=NAME]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <functional>
#include <filesystem>
#include <ctime>
#include <cstdint>
#include <regex>
#include <cctype>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "sqlite3.h"
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ============================================================
// Utility
// ============================================================

static void die(const std::string& msg) {
    std::cerr << "ERROR: " << msg << std::endl;
    std::exit(1);
}

// Parse a signed base-10 integer, saturating before machine-width overflow so a
// huge --limit clamps like Python's arbitrary-width int() rather than throwing
// out_of_range (which opt_int silently swallows into the default).
static bool try_parse_clamped_decimal(const std::string& raw, int minimum, int maximum, int& out_value) {
    if (minimum > maximum) return false;

    size_t begin = 0;
    size_t end = raw.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(raw[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(raw[end - 1]))) --end;
    if (begin == end) return false;

    bool negative = false;
    if (raw[begin] == '+' || raw[begin] == '-') {
        negative = raw[begin] == '-';
        ++begin;
    }
    if (begin == end) return false;

    int value = 0;
    bool saturated = false;
    for (size_t index = begin; index < end; ++index) {
        const unsigned char ch = static_cast<unsigned char>(raw[index]);
        if (!std::isdigit(ch)) return false;
        if (!saturated) {
            const int digit = ch - static_cast<unsigned char>('0');
            if (value > maximum / 10 || (value == maximum / 10 && digit > maximum % 10)) {
                value = maximum;
                saturated = true;
            } else {
                value = value * 10 + digit;
            }
        }
    }

    out_value = negative ? minimum : std::clamp(value, minimum, maximum);
    return true;
}

// ============================================================
// Fuzzy match — ports MonolithFuzzyMatchDetail::ScoreFuzzyMatches
// (Source/MonolithCore/Private/MonolithFuzzyMatch.cpp). The live version uses
// Algo::LevenshteinDistance (a UE dependency this standalone tool cannot link),
// so the distance is reimplemented inline. Score = 1 - dist/maxLen, top-N
// descending, stable sort on ties — behaviourally identical to the live matcher
// and the Python port in monolith_offline.py.
// ============================================================

static int levenshtein(const std::string& a, const std::string& b) {
    if (a == b) return 0;
    if (a.empty()) return (int)b.size();
    if (b.empty()) return (int)a.size();

    std::vector<int> prev(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = (int)j;

    for (size_t i = 1; i <= a.size(); ++i) {
        std::vector<int> cur(b.size() + 1);
        cur[0] = (int)i;
        for (size_t j = 1; j <= b.size(); ++j) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        prev = std::move(cur);
    }
    return prev[b.size()];
}

// Return up to top_n keys ranked by Levenshtein-normalised score descending.
static std::vector<std::string> fuzzy_top(const std::string& needle,
                                          const std::vector<std::string>& keys,
                                          int top_n = 3) {
    if (needle.empty() || keys.empty() || top_n <= 0) return {};

    std::vector<std::pair<float, std::string>> scored;
    scored.reserve(keys.size());
    for (const auto& k : keys) {
        int dist = levenshtein(needle, k);
        int max_len = std::max((int)needle.size(), (int)k.size());
        float worst = max_len > 0 ? (float)max_len : 1.0f;
        float score = 1.0f - ((float)dist / worst);
        scored.emplace_back(score, k);
    }
    // Stable sort descending by score — preserves insertion order on ties.
    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& x, const auto& y) { return x.first > y.first; });

    std::vector<std::string> out;
    int take = std::min(top_n, (int)scored.size());
    out.reserve(take);
    for (int i = 0; i < take; ++i) out.push_back(scored[i].second);
    return out;
}

static std::string did_you_mean_suffix(const std::string& needle,
                                       const std::vector<std::string>& keys) {
    auto sugg = fuzzy_top(needle, keys, 3);
    if (sugg.empty()) return "";
    std::string s = " did_you_mean: ";
    for (size_t i = 0; i < sugg.size(); ++i) {
        if (i > 0) s += ", ";
        s += sugg[i];
    }
    return s;
}

// Namespaces this OFFLINE tool can serve from on-disk SQLite. The live MCP
// server exposes ~29; the rest are LIVE-ONLY (require a running editor).
static const std::vector<std::string>& offline_namespaces() {
    static const std::vector<std::string> ns = {
        "source", "project", "monolith", "cppreflect", "network", "decision", "risk"
    };
    return ns;
}

// FTS5 query escaping — mirrors Python escape_fts() and C++ EscapeFTS()
static std::string escape_fts(const std::string& query) {
    // Replace :: with space (C++ qualified names)
    std::string q = query;
    for (size_t pos = 0; (pos = q.find("::", pos)) != std::string::npos;)
        q.replace(pos, 2, " ");

    // Strip non-alphanumeric, non-whitespace
    std::string cleaned;
    cleaned.reserve(q.size());
    for (char c : q) {
        if (std::isalnum(static_cast<unsigned char>(c)) || std::isspace(static_cast<unsigned char>(c)) || c == '_')
            cleaned += c;
    }

    // Tokenize and wrap
    std::istringstream iss(cleaned);
    std::string token;
    std::vector<std::string> tokens;
    while (iss >> token)
        tokens.push_back(token);

    if (tokens.empty())
        return "\"\"";

    std::string result;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i > 0) result += " ";
        result += "\"" + tokens[i] + "\"*";
    }
    return result;
}

// ============================================================
// SQLite RAII wrapper
// ============================================================

class Database {
public:
    sqlite3* db = nullptr;

    Database() = default;
    ~Database() { if (db) sqlite3_close(db); }

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void open(const std::string& path) {
        if (!fs::exists(path))
            die("Database not found: " + path);

        int rc = sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr);
        if (rc != SQLITE_OK)
            die("Failed to open database: " + path + " — " + sqlite3_errmsg(db));

        exec("PRAGMA journal_mode=DELETE;");
        exec("PRAGMA query_only=ON;");
    }

    void exec(const char* sql) {
        char* err = nullptr;
        sqlite3_exec(db, sql, nullptr, nullptr, &err);
        if (err) {
            std::string msg(err);
            sqlite3_free(err);
            die("SQL error: " + msg);
        }
    }
};

// Simple row type: vector of (column_name, value) pairs.
//
// `cols` holds the TEXT rendering of every column (sqlite3_column_text). For
// REAL/numeric columns we ALSO capture the native double (sqlite3_column_double)
// in `doubles`, because SQLite's text rendering of a double is only ~15
// significant digits — e.g. confidence 0.6499999761581421 renders as the lossy
// "0.649999976158142". get_double() prefers the native double so nlohmann emits
// the 16-digit shortest-round-trip form that the Python sibling / live produce.
struct Row {
    std::map<std::string, std::string> cols;
    std::map<std::string, double> doubles;   // only populated for REAL columns

    std::string get(const std::string& key, const std::string& def = "") const {
        auto it = cols.find(key);
        return (it != cols.end()) ? it->second : def;
    }

    int get_int(const std::string& key, int def = 0) const {
        auto it = cols.find(key);
        if (it == cols.end() || it->second.empty()) return def;
        try { return std::stoi(it->second); }
        catch (...) { return def; }
    }

    int64_t get_int64(const std::string& key, int64_t def = 0) const {
        auto it = cols.find(key);
        if (it == cols.end() || it->second.empty()) return def;
        try { return std::stoll(it->second); }
        catch (...) { return def; }
    }

    double get_double(const std::string& key, double def = 0.0) const {
        // Prefer the native double captured from sqlite3_column_double — full
        // precision, no text round-trip loss.
        auto dit = doubles.find(key);
        if (dit != doubles.end()) return dit->second;
        auto it = cols.find(key);
        if (it == cols.end() || it->second.empty()) return def;
        try { return std::stod(it->second); }
        catch (...) { return def; }
    }
};

using Rows = std::vector<Row>;

// A single bind parameter that carries its SQLite storage class. Most RI
// queries bind everything as TEXT (fine: SQLite applies the COLUMN's numeric
// affinity to coerce the TEXT operand for `col <op> ?` comparisons). BUT when
// the left side is a no-affinity expression — e.g. the scalar subquery
// `MAX(c2.last_touched)` in get_release_window_hotspots — SQLite applies NO
// conversion and compares storage classes directly. An INTEGER column value
// always sorts BEFORE a TEXT value, so `INTEGER >= '173...'` is ALWAYS false →
// the prior exe returned 0 rows while the Python sibling / live (which bind an
// int64) returned the real set (BUG 3). Binding such params as INTEGER fixes it.
struct Bind {
    enum class Kind { Text, Int } kind = Kind::Text;
    std::string text;
    int64_t i = 0;

    Bind(const std::string& s) : kind(Kind::Text), text(s) {}
    Bind(const char* s) : kind(Kind::Text), text(s ? s : "") {}
    static Bind Integer(int64_t v) { Bind b(""); b.kind = Kind::Int; b.i = v; return b; }
};

static Rows query_typed(Database& db, const std::string& sql, const std::vector<Bind>& params) {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db.db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db.db);
        std::cerr << "[query error] " << err << "\n  SQL: " << sql.substr(0, 200) << std::endl;
        return {};
    }
    for (int i = 0; i < (int)params.size(); ++i) {
        if (params[i].kind == Bind::Kind::Int)
            sqlite3_bind_int64(stmt, i + 1, params[i].i);
        else
            sqlite3_bind_text(stmt, i + 1, params[i].text.c_str(), -1, SQLITE_TRANSIENT);
    }
    Rows rows;
    int ncols = sqlite3_column_count(stmt);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Row row;
        for (int c = 0; c < ncols; ++c) {
            const char* name = sqlite3_column_name(stmt, c);
            std::string key = name ? name : "";
            if (sqlite3_column_type(stmt, c) == SQLITE_FLOAT)
                row.doubles[key] = sqlite3_column_double(stmt, c);
            const char* val = (const char*)sqlite3_column_text(stmt, c);
            row.cols[key] = val ? val : "";
        }
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return rows;
}

static Rows query(Database& db, const std::string& sql, const std::vector<std::string>& params = {}) {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db.db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db.db);
        // Log to stderr for diagnostics, return empty on operational error
        std::cerr << "[query error] " << err << "\n  SQL: " << sql.substr(0, 200) << std::endl;
        return {};
    }

    for (int i = 0; i < (int)params.size(); ++i)
        sqlite3_bind_text(stmt, i + 1, params[i].c_str(), -1, SQLITE_TRANSIENT);

    Rows rows;
    int ncols = sqlite3_column_count(stmt);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Row row;
        for (int c = 0; c < ncols; ++c) {
            const char* name = sqlite3_column_name(stmt, c);
            std::string key = name ? name : "";
            // Capture native double for REAL columns BEFORE column_text (which
            // mutates the column's type via SQLite's type-coercion rules). This
            // preserves shortest-round-trip precision for confidence/score/etc.
            if (sqlite3_column_type(stmt, c) == SQLITE_FLOAT)
                row.doubles[key] = sqlite3_column_double(stmt, c);
            const char* val = (const char*)sqlite3_column_text(stmt, c);
            row.cols[key] = val ? val : "";
        }
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return rows;
}

// Like query(), but reports failure instead of logging to stderr and returning
// an empty row set. `query()` cannot tell a caller's bad query from a storage
// failure -- both look like zero results -- which is the bug this fixes for
// project search. Mirrors FMonolithIndexDatabase::FullTextSearch's step check.
static bool query_checked(Database& db,
                          const std::string& sql,
                          const std::vector<Bind>& params,
                          Rows& out_rows,
                          std::string& out_error) {
    out_rows.clear();
    out_error.clear();

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db.db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        out_error = sqlite3_errmsg(db.db);
        if (stmt) sqlite3_finalize(stmt);
        return false;
    }

    for (int i = 0; i < (int)params.size(); ++i) {
        if (params[i].kind == Bind::Kind::Int)
            rc = sqlite3_bind_int64(stmt, i + 1, params[i].i);
        else
            rc = sqlite3_bind_text(stmt, i + 1, params[i].text.c_str(), -1, SQLITE_TRANSIENT);
        if (rc != SQLITE_OK) {
            out_error = sqlite3_errmsg(db.db);
            sqlite3_finalize(stmt);
            return false;
        }
    }

    int ncols = sqlite3_column_count(stmt);
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        Row row;
        for (int c = 0; c < ncols; ++c) {
            const char* name = sqlite3_column_name(stmt, c);
            std::string key = name ? name : "";
            // Capture native double for REAL columns BEFORE column_text, which
            // mutates the column's type via SQLite's type-coercion rules.
            if (sqlite3_column_type(stmt, c) == SQLITE_FLOAT)
                row.doubles[key] = sqlite3_column_double(stmt, c);
            const char* val = (const char*)sqlite3_column_text(stmt, c);
            row.cols[key] = val ? val : "";
        }
        out_rows.push_back(std::move(row));
    }

    if (rc != SQLITE_DONE) {
        out_error = sqlite3_errmsg(db.db);
        sqlite3_finalize(stmt);
        out_rows.clear();
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}

// Diagnostics emitted by the FTS5 MATCH expression parser rather than by
// storage/schema access. Mirrors MonolithProjectSearchDetail::IsFts5QuerySyntaxError.
static bool is_fts5_syntax_error(const std::string& message) {
    std::string lowered = message;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    static const char* patterns[] = {
        "fts5: syntax error",
        "unterminated string",
        "malformed match",
        "unknown special query",
        "expected integer, got",
    };
    for (const char* pattern : patterns)
        if (lowered.find(pattern) != std::string::npos) return true;
    return false;
}

// The two project FTS tables expose different columns, so on its own this means
// "not answerable here", not "bad query". Only a rejection by BOTH tables makes
// it a caller error.
static bool is_fts5_unknown_column_error(const std::string& message) {
    std::string lowered = message;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return lowered.find("no such column") != std::string::npos;
}

// Mirrors the CREATE VIRTUAL TABLE column lists (fts_assets / fts_nodes).
static const char* const FTS_ASSET_COLUMNS =
    "asset_name, asset_class, description, package_path, module_name";
static const char* const FTS_NODE_COLUMNS = "node_name, node_class, node_type";

// Name the offending column and list the valid ones. Mirrors
// MonolithProjectSearchDetail::DescribeUnknownColumn.
static std::string describe_unknown_column(const std::string& message) {
    std::string lowered = message;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    static const std::string marker = "no such column:";
    std::string subject = message;
    const size_t index = lowered.find(marker);
    if (index != std::string::npos) {
        std::string column_name = message.substr(index + marker.size());
        size_t begin = 0;
        size_t end = column_name.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(column_name[begin]))) ++begin;
        while (end > begin && std::isspace(static_cast<unsigned char>(column_name[end - 1]))) --end;
        column_name = column_name.substr(begin, end - begin);
        if (!column_name.empty())
            subject = "no such column '" + column_name + "'";
    }

    return subject + ". Valid columns are " + FTS_ASSET_COLUMNS + " (assets) or "
           + FTS_NODE_COLUMNS + " (nodes); one filter cannot span both tables.";
}

// Upper bound on a project search query, mirroring
// MonolithProjectSearchActionDetail::MaxQueryLength.
static const size_t PROJECT_SEARCH_MAX_QUERY_LENGTH = 4096;
// Each entry becomes one bound placeholder in an IN clause. Mirrors
// MonolithProjectSearchActionDetail::MaxClassFilters.
static const size_t PROJECT_SEARCH_MAX_CLASS_FILTERS = 32;

// ============================================================
// CLI argument parser
// ============================================================

struct Args {
    std::string ns;       // "source" or "project"
    std::string action;   // e.g. "search_source", "read_source"
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;

    std::string opt(const std::string& key, const std::string& def = "") const {
        auto it = options.find(key);
        return (it != options.end()) ? it->second : def;
    }

    int opt_int(const std::string& key, int def) const {
        auto it = options.find(key);
        if (it == options.end() || it->second.empty()) return def;
        try { return std::stoi(it->second); }
        catch (...) { return def; }
    }

    bool opt_bool(const std::string& key, bool def = false) const {
        auto it = options.find(key);
        if (it == options.end()) return def;
        const auto& v = it->second;
        return v.empty() || v == "true" || v == "1" || v == "yes";
    }
};

static Args parse_args(int argc, char* argv[]) {
    Args args;
    if (argc < 3) {
        std::cerr << "Usage: monolith_query <source|project> <action> [params...] [--options]\n\n"
                  << "Source actions:\n"
                  << "  search_source <query> [--scope=all|cpp|shaders] [--limit=N] [--module=M] [--kind=K]\n"
                  << "  read_source <symbol> [--max-lines=N] [--no-header] [--members-only]\n"
                  << "  find_references <symbol> [--ref-kind=K] [--limit=N]\n"
                  << "  find_callers <symbol> [--limit=N]\n"
                  << "  find_callees <symbol> [--limit=N]\n"
                  << "  get_class_hierarchy <symbol> [--direction=up|down|both] [--depth=N]\n"
                  << "  get_module_info <module_name>\n"
                  << "  get_symbol_context <symbol> [--context-lines=N]\n"
                  << "  read_file <file_path> [--start=N] [--end=N]\n"
                  << "  get_include_path <symbol>\n"
                  << "  get_signature <symbol> [--limit=N]\n"
                  << "  check_deprecations <symbol> [<symbol> ...]\n"
                  << "  verify_symbols <symbol> [<symbol> ...]\n"
                  << "  find_example_usage <symbol> [--prefer=engine|project] [--limit=N]\n"
                  << "  lint_header <file_path>\n"
                  << "  generate_class_stub <parent> <class_name> <module>\n"
                  << "\nProject actions:\n"
                  << "  search <query> [--limit=N] [--asset-class=CLASS[,CLASS...]]\n"
                  << "  find_by_type <asset_class> [--limit=N] [--offset=N]\n"
                  << "  find_references <asset_path>\n"
                  << "  get_stats\n"
                  << "  get_asset_details <asset_path>\n"
                  << "\nMonolith actions:\n"
                  << "  guide [--section=NAME]   (NAME: onboarding|recipes|decisions|errors|skills_map|gotchas)\n"
                  << "\nReflection Intelligence (read EngineSource.db reflect_* tables; FULL parity):\n"
                  << "  cppreflect get_uclass <class_name> [--module_name=M] | list_uproperties [--class_name=C]\n"
                  << "             [--blueprint_visible_only] | list_ufunctions [--class_name=C] [--blueprint_callable_only]\n"
                  << "             | find_interface_impls <interface_name> | find_class_specifier <specifier_name>\n"
                  << "             | list_class_specifiers\n"
                  << "  network    list_replicated_classes | list_rpc_functions [--class_name=C] [--rpc_kind=K]\n"
                  << "             | list_onrep_handlers [--class_name=C] | audit_unbalanced_onreps\n"
                  << "  decision   list_decisions [--path_filter=P] [--min_confidence=N] [--status=S]\n"
                  << "             | get_decision <decision_id> | list_stale --max_age_days=N [--path_filter=P]\n"
                  << "             | find_supersession_chain <decision_id> [--depth=N] | find_referent_decisions <decision_id>\n"
                  << "  risk       get_hotspot_score <file_path> | get_cochange_pairs <file_path>\n"
                  << "             | get_file_churn <file_path> [--repo_tag=R] | get_release_window_hotspots [--since_unix=N]\n"
                  << "             | list_conditional_gates [--macro_filter=M] [--path_filter=P]\n"
                  << "  (all paginated actions accept [--limit=N] [--cursor=B64].  --version prints the build stamp.)\n"
                  << "\nNOTE: only namespaces backed by on-disk SQLite are servable offline. The live\n"
                  << "MCP server exposes ~29 namespaces; the rest are LIVE-ONLY (need a running editor).\n";
        std::exit(1);
    }

    args.ns = argv[1];
    args.action = argv[2];

    // Options that take a VALUE. When written in space-separated form
    // (`--limit 5` rather than `--limit=5`) the NEXT token is the value and
    // must NOT be misread as a positional. The Python sibling (argparse) and
    // the live server both accept the space form; the prior exe only handled
    // `--key=value`, so `--limit 5` silently fell back to the hardcoded
    // default and `5` leaked into positional[] (BUG 1). Flag-style options
    // (store_true: --no-header, --blueprint_visible_only, etc.) are NOT in
    // this set and never consume the following token.
    static const std::set<std::string> value_options = {
        // source.*
        "scope", "limit", "module", "kind", "max_lines", "ref_kind",
        "direction", "depth", "context_lines", "start", "end",
        // project.*
        "offset", "asset_class",
        // monolith.guide
        "section",
        // RI shared
        "cursor", "module_name", "class_name", "rpc_kind", "path_filter",
        "min_confidence", "status", "max_age_days", "since_unix", "repo_tag",
        "macro_filter", "specifier_name", "interface_name", "decision_id",
        "file_path",
        // source.find_example_usage
        "prefer",
        // db overrides
        "db", "source_db", "project_db",
    };

    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a.substr(0, 2) == "--") {
            std::string key, val;
            auto eq = a.find('=');
            bool had_inline_value = false;
            if (eq != std::string::npos) {
                key = a.substr(2, eq - 2);
                val = a.substr(eq + 1);
                had_inline_value = true;
            } else {
                key = a.substr(2);
                val = "";  // flag-style (default)
            }
            // Normalize hyphens to underscores for consistency
            std::replace(key.begin(), key.end(), '-', '_');

            // Space-separated value form: `--key VALUE`. Only consume the
            // following token for known value-taking options, and only when
            // it is not itself another `--option`. This preserves flag-style
            // options (which legitimately precede a positional arg).
            if (!had_inline_value && value_options.count(key) &&
                i + 1 < argc) {
                std::string nxt = argv[i + 1];
                if (nxt.substr(0, 2) != "--") {
                    val = nxt;
                    ++i;  // consume the value token
                }
            }
            args.options[key] = val;
        } else {
            args.positional.push_back(a);
        }
    }

    return args;
}

// ============================================================
// Path utilities
// ============================================================

static std::string short_path(const std::string& full_path) {
    static const char* markers[] = {
        "Engine\\Source\\", "Engine/Source/",
        "Engine\\Shaders\\", "Engine/Shaders/"
    };
    for (auto m : markers) {
        auto idx = full_path.find(m);
        if (idx != std::string::npos)
            return full_path.substr(idx);
    }
    return full_path;
}

static std::string read_file_lines(const std::string& file_path, int start, int end) {
    std::ifstream f(file_path);
    if (!f.is_open())
        return "[File not found: " + file_path + "]";

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line))
        lines.push_back(line);

    start = std::max(1, start);
    end = std::min((int)lines.size(), end);

    std::ostringstream out;
    for (int i = start - 1; i < end; ++i) {
        // Trim trailing whitespace
        std::string& l = lines[i];
        while (!l.empty() && (l.back() == '\r' || l.back() == '\n' || l.back() == ' '))
            l.pop_back();

        char buf[16];
        std::snprintf(buf, sizeof(buf), "%5d", i + 1);
        out << buf << " | " << l;
        if (i < end - 1) out << "\n";
    }
    return out.str();
}

// ============================================================
// Source actions
// ============================================================

class SourceActions {
    Database db;

public:
    void open(const std::string& path) { db.open(path); }

    std::string get_file_path(int file_id) {
        auto rows = query(db, "SELECT path FROM files WHERE id = ?", {std::to_string(file_id)});
        return rows.empty() ? "<unknown>" : rows[0].get("path");
    }

    // --- search_source ---
    void search_source(const Args& args) {
        if (args.positional.empty()) die("search_source requires a query argument");
        std::string q = args.positional[0];
        int limit = args.opt_int("limit", 20);
        std::string module = args.opt("module");
        std::string kind = args.opt("kind");
        std::string fts_q = escape_fts(q);

        std::ostringstream out;

        // Symbol FTS search
        {
            std::string sql = "SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, "
                              "s.line_start, s.line_end, s.access, s.signature, s.docstring "
                              "FROM symbols_fts f JOIN symbols s ON s.id = f.rowid";
            std::vector<std::string> conditions = {"symbols_fts MATCH ?"};
            std::vector<std::string> params = {fts_q};

            if (!module.empty()) {
                sql += " JOIN files fi ON fi.id = s.file_id JOIN modules m ON m.id = fi.module_id";
                conditions.push_back("m.name = ?");
                params.push_back(module);
            }
            if (!kind.empty()) {
                conditions.push_back("s.kind = ?");
                params.push_back(kind);
            }

            sql += " WHERE ";
            for (size_t i = 0; i < conditions.size(); ++i) {
                if (i > 0) sql += " AND ";
                sql += conditions[i];
            }
            sql += " ORDER BY bm25(symbols_fts) LIMIT " + std::to_string(limit);

            auto rows = query(db, sql, params);
            if (!rows.empty()) {
                out << "=== Symbol Matches ===\n";
                for (auto& r : rows) {
                    std::string fp = short_path(get_file_path(r.get_int("file_id")));
                    out << "  [" << r.get("kind") << "] " << r.get("qualified_name")
                        << " (" << fp << ":" << r.get("line_start") << ")\n";
                    std::string sig = r.get("signature");
                    if (!sig.empty())
                        out << "         " << sig << "\n";
                }
            }
        }

        // Source line FTS search
        {
            std::string sql = "SELECT sf.file_id, sf.line_number, sf.text FROM source_fts sf";
            std::vector<std::string> conditions = {"source_fts MATCH ?"};
            std::vector<std::string> params = {fts_q};

            if (!module.empty()) {
                sql += " JOIN files fi ON fi.id = sf.file_id JOIN modules m ON m.id = fi.module_id";
                conditions.push_back("m.name = ?");
                params.push_back(module);
            }

            sql += " WHERE ";
            for (size_t i = 0; i < conditions.size(); ++i) {
                if (i > 0) sql += " AND ";
                sql += conditions[i];
            }
            sql += " ORDER BY bm25(source_fts) LIMIT " + std::to_string(limit);

            auto rows = query(db, sql, params);
            if (!rows.empty()) {
                out << "\n=== Source Line Matches ===\n";
                std::set<std::pair<int, int>> seen;
                for (auto& r : rows) {
                    int fid = r.get_int("file_id");
                    int ln = r.get_int("line_number");
                    if (seen.count({fid, ln})) continue;
                    seen.insert({fid, ln});

                    std::string fp = short_path(get_file_path(fid));
                    std::string text = r.get("text");
                    // Trim and truncate
                    while (!text.empty() && std::isspace((unsigned char)text.front())) text.erase(text.begin());
                    if (text.size() > 120) text = text.substr(0, 120) + "...";
                    out << "  " << fp << ":" << ln << "\n";
                    out << "    " << text << "\n";
                }
            }
        }

        std::string result = out.str();
        if (result.empty())
            std::cout << "No results found for '" << q << "'." << std::endl;
        else
            std::cout << result;
    }

    // --- read_source ---
    void read_source(const Args& args) {
        if (args.positional.empty()) die("read_source requires a symbol argument");
        std::string symbol = args.positional[0];
        int max_lines = args.opt_int("max_lines", 0);
        bool include_header = !args.options.count("no_header");
        // bool members_only = args.opt_bool("members_only"); // reserved for future use

        // Exact name lookup
        auto rows = query(db, "SELECT * FROM symbols WHERE name = ? ORDER BY (line_end > line_start) DESC", {symbol});
        if (rows.empty()) {
            std::string fts_q = escape_fts(symbol);
            rows = query(db, "SELECT s.* FROM symbols_fts f JOIN symbols s ON s.id = f.rowid "
                             "WHERE symbols_fts MATCH ? ORDER BY bm25(symbols_fts) LIMIT 5", {fts_q});
        }
        if (rows.empty()) die("No symbol found matching '" + symbol + "'.");

        std::vector<std::string> parts;
        std::set<std::tuple<int, int, int>> seen;

        for (auto& r : rows) {
            int fid = r.get_int("file_id");
            int ls = r.get_int("line_start");
            int le = r.get_int("line_end");
            if (seen.count({fid, ls, le})) continue;
            seen.insert({fid, ls, le});

            std::string fp = get_file_path(fid);
            if (!include_header && fp.size() > 2 && fp.substr(fp.size() - 2) == ".h")
                continue;

            std::string header = "--- " + short_path(fp) + " (lines " +
                                 std::to_string(ls) + "-" + std::to_string(le) + ") ---";
            std::string source = read_file_lines(fp, ls, le);
            parts.push_back(header + "\n" + source);
        }

        std::string result;
        if (parts.empty()) {
            result = "Found symbol '" + symbol + "' but could not read source.";
        } else {
            for (size_t i = 0; i < parts.size(); ++i) {
                if (i > 0) result += "\n\n";
                result += parts[i];
            }
        }

        if (max_lines > 0) {
            std::istringstream iss(result);
            std::string line;
            std::vector<std::string> lines;
            while (std::getline(iss, line)) lines.push_back(line);
            if ((int)lines.size() > max_lines) {
                int remaining = (int)lines.size() - max_lines;
                lines.resize(max_lines);
                std::ostringstream trunc;
                for (auto& l : lines) trunc << l << "\n";
                trunc << "[...truncated, " << remaining << " more lines]";
                result = trunc.str();
            }
        }

        std::cout << result << std::endl;
    }

    // --- find_references ---
    void find_references(const Args& args) {
        if (args.positional.empty()) die("find_references requires a symbol argument");
        std::string symbol = args.positional[0];
        std::string ref_kind = args.opt("ref_kind");
        int limit = args.opt_int("limit", 50);

        auto sym_rows = query(db, "SELECT id, name FROM symbols WHERE name = ?", {symbol});
        if (sym_rows.empty()) {
            std::string fts_q = escape_fts(symbol);
            sym_rows = query(db, "SELECT s.id, s.name FROM symbols_fts f JOIN symbols s ON s.id = f.rowid "
                                 "WHERE symbols_fts MATCH ? LIMIT 5", {fts_q});
        }
        if (sym_rows.empty()) die("No symbol found matching '" + symbol + "'.");

        std::vector<std::string> lines;
        for (auto& sym : sym_rows) {
            Rows refs;
            if (!ref_kind.empty()) {
                refs = query(db,
                    "SELECT r.ref_kind, r.line, s.name as from_name, f.path "
                    "FROM \"references\" r JOIN symbols s ON s.id = r.from_symbol_id "
                    "JOIN files f ON f.id = r.file_id WHERE r.to_symbol_id = ? AND r.ref_kind = ? LIMIT ?",
                    {sym.get("id"), ref_kind, std::to_string(limit)});
            } else {
                refs = query(db,
                    "SELECT r.ref_kind, r.line, s.name as from_name, f.path "
                    "FROM \"references\" r JOIN symbols s ON s.id = r.from_symbol_id "
                    "JOIN files f ON f.id = r.file_id WHERE r.to_symbol_id = ? LIMIT ?",
                    {sym.get("id"), std::to_string(limit)});
            }
            for (auto& ref : refs) {
                lines.push_back("[" + ref.get("ref_kind") + "] " + short_path(ref.get("path")) +
                                ":" + ref.get("line") + " (from " + ref.get("from_name") + ")");
            }
        }

        if (lines.empty())
            std::cout << "No references found for '" << symbol << "'." << std::endl;
        else
            for (auto& l : lines) std::cout << l << "\n";
    }

    // --- find_callers ---
    void find_callers(const Args& args) {
        if (args.positional.empty()) die("find_callers requires a symbol argument");
        std::string symbol = args.positional[0];
        int limit = args.opt_int("limit", 50);

        auto sym_rows = query(db, "SELECT id FROM symbols WHERE name = ? AND kind = 'function'", {symbol});
        if (sym_rows.empty()) {
            std::string fts_q = escape_fts(symbol);
            sym_rows = query(db,
                "SELECT s.id FROM symbols_fts f JOIN symbols s ON s.id = f.rowid "
                "WHERE symbols_fts MATCH ? AND s.kind = 'function' LIMIT 5", {fts_q});
        }
        if (sym_rows.empty()) die("No function found matching '" + symbol + "'.");

        std::vector<std::string> lines;
        for (auto& sym : sym_rows) {
            auto refs = query(db,
                "SELECT s.name as from_name, f.path, r.line "
                "FROM \"references\" r JOIN symbols s ON s.id = r.from_symbol_id "
                "JOIN files f ON f.id = r.file_id WHERE r.to_symbol_id = ? AND r.ref_kind = 'call' LIMIT ?",
                {sym.get("id"), std::to_string(limit)});
            for (auto& ref : refs)
                lines.push_back(ref.get("from_name") + " -- " + short_path(ref.get("path")) + ":" + ref.get("line"));
        }

        if (lines.empty())
            std::cout << "No callers found for '" << symbol << "'." << std::endl;
        else
            for (auto& l : lines) std::cout << l << "\n";
    }

    // --- find_callees ---
    void find_callees(const Args& args) {
        if (args.positional.empty()) die("find_callees requires a symbol argument");
        std::string symbol = args.positional[0];
        int limit = args.opt_int("limit", 50);

        auto sym_rows = query(db, "SELECT id FROM symbols WHERE name = ? AND kind = 'function'", {symbol});
        if (sym_rows.empty()) {
            std::string fts_q = escape_fts(symbol);
            sym_rows = query(db,
                "SELECT s.id FROM symbols_fts f JOIN symbols s ON s.id = f.rowid "
                "WHERE symbols_fts MATCH ? AND s.kind = 'function' LIMIT 5", {fts_q});
        }
        if (sym_rows.empty()) die("No function found matching '" + symbol + "'.");

        std::vector<std::string> lines;
        for (auto& sym : sym_rows) {
            auto refs = query(db,
                "SELECT s.name as to_name, f.path, r.line "
                "FROM \"references\" r JOIN symbols s ON s.id = r.to_symbol_id "
                "JOIN files f ON f.id = r.file_id WHERE r.from_symbol_id = ? AND r.ref_kind = 'call' LIMIT ?",
                {sym.get("id"), std::to_string(limit)});
            for (auto& ref : refs)
                lines.push_back(ref.get("to_name") + " -- " + short_path(ref.get("path")) + ":" + ref.get("line"));
        }

        if (lines.empty())
            std::cout << "No callees found for '" << symbol << "'." << std::endl;
        else
            for (auto& l : lines) std::cout << l << "\n";
    }

    // --- get_class_hierarchy ---
    void get_class_hierarchy(const Args& args) {
        if (args.positional.empty()) die("get_class_hierarchy requires a symbol argument");
        std::string symbol = args.positional[0];
        std::string direction = args.opt("direction", "both");
        int depth = args.opt_int("depth", 5);

        auto sym_rows = query(db,
            "SELECT id, name, file_id FROM symbols WHERE name = ? AND kind IN ('class','struct') "
            "ORDER BY (line_end > line_start) DESC", {symbol});
        if (sym_rows.empty()) {
            std::string fts_q = escape_fts(symbol);
            sym_rows = query(db,
                "SELECT s.id, s.name, s.file_id FROM symbols_fts f JOIN symbols s ON s.id = f.rowid "
                "WHERE symbols_fts MATCH ? AND s.kind IN ('class','struct') LIMIT 1", {fts_q});
        }
        if (sym_rows.empty()) die("No class/struct found matching '" + symbol + "'.");

        auto& sym = sym_rows[0];
        std::string fp = short_path(get_file_path(sym.get_int("file_id")));
        std::vector<std::string> lines;
        lines.push_back(sym.get("name") + " (" + fp + ")");

        std::set<std::string> visited;

        std::function<void(const std::string&, int, int)> walk_up;
        walk_up = [&](const std::string& sid, int indent, int max_d) {
            if (indent > max_d || visited.count(sid)) return;
            visited.insert(sid);
            auto parents = query(db,
                "SELECT s.id, s.name FROM inheritance i JOIN symbols s ON s.id = i.parent_id WHERE i.child_id = ?",
                {sid});
            for (auto& p : parents) {
                lines.push_back(std::string(indent * 2, ' ') + "<- " + p.get("name"));
                walk_up(p.get("id"), indent + 1, max_d);
            }
        };

        std::function<void(const std::string&, int, int)> walk_down;
        walk_down = [&](const std::string& sid, int indent, int max_d) {
            if (indent > max_d || visited.count(sid)) return;
            visited.insert(sid);
            auto children = query(db,
                "SELECT s.id, s.name FROM inheritance i JOIN symbols s ON s.id = i.child_id WHERE i.parent_id = ?",
                {sid});
            for (auto& c : children) {
                lines.push_back(std::string(indent * 2, ' ') + "-> " + c.get("name"));
                walk_down(c.get("id"), indent + 1, max_d);
            }
        };

        if (direction == "up" || direction == "both") {
            lines.push_back("\nAncestors:");
            size_t count_before = lines.size();
            visited.clear();
            walk_up(sym.get("id"), 1, depth);
            if (lines.size() == count_before)
                lines.push_back("  (none)");
        }

        if (direction == "down" || direction == "both") {
            lines.push_back("\nDescendants:");
            size_t count_before = lines.size();
            visited.clear();
            walk_down(sym.get("id"), 1, depth);
            if (lines.size() == count_before)
                lines.push_back("  (none)");
        }

        for (auto& l : lines) std::cout << l << "\n";
    }

    // --- get_module_info ---
    void get_module_info(const Args& args) {
        if (args.positional.empty()) die("get_module_info requires a module_name argument");
        std::string module_name = args.positional[0];

        auto mods = query(db, "SELECT id, name, path, module_type FROM modules WHERE name = ?", {module_name});
        if (mods.empty()) die("No module found matching '" + module_name + "'.");

        auto& mod = mods[0];
        auto file_count = query(db, "SELECT COUNT(*) as c FROM files WHERE module_id = ?", {mod.get("id")});
        auto kind_rows = query(db,
            "SELECT s.kind, COUNT(*) as cnt FROM symbols s JOIN files f ON f.id = s.file_id "
            "WHERE f.module_id = ? GROUP BY s.kind", {mod.get("id")});

        std::cout << "Module: " << mod.get("name") << "\n"
                  << "Path: " << short_path(mod.get("path")) << "\n"
                  << "Type: " << mod.get("module_type") << "\n"
                  << "Files: " << (file_count.empty() ? "0" : file_count[0].get("c")) << "\n"
                  << "\nSymbol counts by kind:\n";

        // Sort by kind name
        std::sort(kind_rows.begin(), kind_rows.end(),
                  [](const Row& a, const Row& b) { return a.get("kind") < b.get("kind"); });
        for (auto& kr : kind_rows)
            std::cout << "  " << kr.get("kind") << ": " << kr.get("cnt") << "\n";

        auto key_classes = query(db,
            "SELECT s.name, s.line_start FROM symbols s JOIN files f ON f.id = s.file_id "
            "JOIN modules m ON m.id = f.module_id WHERE m.name = ? AND s.kind = 'class' LIMIT 20",
            {module_name});
        if (!key_classes.empty()) {
            std::cout << "\nKey classes:\n";
            for (auto& c : key_classes)
                std::cout << "  " << c.get("name") << " (line " << c.get("line_start") << ")\n";
        }
    }

    // --- get_symbol_context ---
    void get_symbol_context(const Args& args) {
        if (args.positional.empty()) die("get_symbol_context requires a symbol argument");
        std::string symbol = args.positional[0];
        int ctx_lines = args.opt_int("context_lines", 10);

        auto rows = query(db, "SELECT * FROM symbols WHERE name = ? ORDER BY (line_end > line_start) DESC", {symbol});
        if (rows.empty()) {
            std::string fts_q = escape_fts(symbol);
            rows = query(db,
                "SELECT s.* FROM symbols_fts f JOIN symbols s ON s.id = f.rowid "
                "WHERE symbols_fts MATCH ? LIMIT 5", {fts_q});
        }
        if (rows.empty()) die("No symbol found matching '" + symbol + "'.");

        std::vector<std::string> parts;
        int count = 0;
        for (auto& r : rows) {
            if (count++ >= 3) break;

            std::string fp = get_file_path(r.get_int("file_id"));
            int ls = r.get_int("line_start");
            int le = r.get_int("line_end");
            int ctx_start = std::max(1, ls - ctx_lines);
            int ctx_end = le + ctx_lines;

            std::ostringstream part;
            part << "--- " << r.get("qualified_name") << " ---\n";
            part << "File: " << short_path(fp) << " (lines " << ls << "-" << le << ")\n";
            std::string sig = r.get("signature");
            if (!sig.empty()) part << "Signature: " << sig << "\n";
            std::string doc = r.get("docstring");
            if (!doc.empty()) part << "Docstring: " << doc << "\n";
            part << "\n" << read_file_lines(fp, ctx_start, ctx_end);
            parts.push_back(part.str());
        }

        for (size_t i = 0; i < parts.size(); ++i) {
            if (i > 0) std::cout << "\n\n";
            std::cout << parts[i];
        }
        std::cout << std::endl;
    }

    // --- read_file ---
    void read_file(const Args& args) {
        if (args.positional.empty()) die("read_file requires a file_path argument");
        std::string file_path = args.positional[0];
        int start = args.opt_int("start", 1);
        int end = args.opt_int("end", 0);

        std::string resolved;

        if (fs::exists(file_path)) {
            resolved = file_path;
        } else {
            // Normalize slashes to backslash for DB lookup
            std::string normalized = file_path;
            std::replace(normalized.begin(), normalized.end(), '/', '\\');

            auto rows = query(db, "SELECT path FROM files WHERE path = ?", {normalized});
            if (rows.empty())
                rows = query(db, "SELECT path FROM files WHERE path LIKE ? LIMIT 1", {"%" + normalized});

            if (!rows.empty())
                resolved = rows[0].get("path");
        }

        if (resolved.empty()) die("No file found matching '" + file_path + "'.");

        if (end <= 0) end = start + 199;

        std::cout << "--- " << short_path(resolved) << " (lines " << start << "-" << end << ") ---\n";
        std::cout << read_file_lines(resolved, start, end) << std::endl;
    }

    // ============================================================
    // Phase 1 — LLM C++ authoring ergonomics (items 1-3). Mirrors the live
    // FMonolithSourceActions handlers' content[].text rendering for parity.
    // ============================================================

    // Mirror of FMonolithSourceActions::DeriveIncludePath.
    static std::string derive_include_path(const std::string& indexed_path,
                                           bool& out_includable, std::string& out_warning,
                                           const std::string& module_name) {
        out_includable = true;
        out_warning.clear();

        std::string path = indexed_path;
        std::replace(path.begin(), path.end(), '\\', '/');

        auto rfind_ci = [](const std::string& hay, const std::string& needle) -> size_t {
            return hay.rfind(needle);
        };

        static const char* roots[] = { "/Public/", "/Classes/", "/Internal/" };
        for (const char* root : roots) {
            size_t idx = rfind_ci(path, root);
            if (idx != std::string::npos) {
                out_includable = true;
                return path.substr(idx + std::string(root).size());
            }
        }

        size_t pidx = rfind_ci(path, "/Private/");
        if (pidx != std::string::npos) {
            out_includable = false;
            out_warning = "Private header -- not includable outside "
                + (module_name.empty() ? std::string("its module") : module_name)
                + "; same-module include shown";
            return path.substr(pidx + std::string("/Private/").size());
        }

        // basename fallback
        size_t slash = path.find_last_of('/');
        out_includable = true;
        return (slash == std::string::npos) ? path : path.substr(slash + 1);
    }

    // Resolve a symbol (or Class::Method) to its owning file row.
    Row resolve_symbol_row(const std::string& symbol, bool& found) {
        found = false;
        std::string lookup = symbol;
        size_t scope = symbol.rfind("::");
        if (scope != std::string::npos) lookup = symbol.substr(0, scope);

        auto rows = query(db,
            "SELECT id, name, file_id FROM symbols WHERE name = ? ORDER BY (line_end > line_start) DESC",
            {lookup});
        if (rows.empty()) {
            std::string fts_q = escape_fts(lookup);
            rows = query(db,
                "SELECT s.id, s.name, s.file_id FROM symbols_fts f JOIN symbols s ON s.id = f.rowid "
                "WHERE symbols_fts MATCH ? LIMIT 5", {fts_q});
        }
        if (rows.empty()) return Row{};
        found = true;
        return rows[0];
    }

    // --- get_include_path ---
    void get_include_path(const Args& args) {
        if (args.positional.empty()) die("get_include_path requires a symbol argument");
        std::string symbol = args.positional[0];

        bool found = false;
        Row sym = resolve_symbol_row(symbol, found);
        if (!found) die("No symbol found matching '" + symbol + "'.");

        int file_id = sym.get_int("file_id");

        // Prefer a header file among same-name rows.
        std::string lookup = symbol;
        size_t scope = symbol.rfind("::");
        if (scope != std::string::npos) lookup = symbol.substr(0, scope);
        auto allrows = query(db,
            "SELECT s.file_id, f.path FROM symbols s JOIN files f ON f.id = s.file_id WHERE s.name = ?",
            {lookup});
        std::string file_path = get_file_path(file_id);
        for (auto& r : allrows) {
            std::string p = r.get("path");
            if (p.size() >= 2 && p.substr(p.size() - 2) == ".h") {
                file_id = r.get_int("file_id");
                file_path = p;
                break;
            }
        }

        // Module + build_cs_path
        auto mrows = query(db,
            "SELECT m.name, m.build_cs_path FROM files f JOIN modules m ON m.id = f.module_id WHERE f.id = ?",
            {std::to_string(file_id)});
        std::string module_name = mrows.empty() ? "" : mrows[0].get("name");
        std::string build_cs = mrows.empty() ? "" : mrows[0].get("build_cs_path");

        bool includable = true;
        std::string warning;
        std::string include = derive_include_path(file_path, includable, warning, module_name);

        std::string build_cs_note;
        if (!module_name.empty()) {
            if (!build_cs.empty()) {
                size_t s = build_cs.find_last_of("/\\");
                std::string base = (s == std::string::npos) ? build_cs : build_cs.substr(s + 1);
                build_cs_note = "Module '" + module_name + "' -- add to your Build.cs deps (" + base + ")";
            } else {
                build_cs_note = "Module '" + module_name + "' -- add to your Build.cs deps";
            }
        }

        std::cout << "#include \"" << include << "\"";
        if (!module_name.empty()) std::cout << "\nModule: " << module_name;
        if (!build_cs_note.empty()) std::cout << "\n" << build_cs_note;
        if (!warning.empty()) std::cout << "\nWARNING: " << warning;
        std::cout << std::endl;
    }

    // Mirror of FMonolithSourceActions::CompactDeclaration.
    static std::string compact_declaration(const std::vector<std::string>& lines, int start_idx) {
        std::string accum;
        int paren_depth = 0;
        bool saw_open = false;
        for (int i = start_idx; i < (int)lines.size() && i < start_idx + 12; ++i) {
            std::string line = lines[i];
            // trim trailing ws
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            if (!line.empty() && line.back() == '\\') {
                line.pop_back();
                while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
            }
            bool done = false;
            for (size_t c = 0; c < line.size(); ++c) {
                char ch = line[c];
                if (ch == '(') { paren_depth++; saw_open = true; }
                else if (ch == ')') { paren_depth = std::max(0, paren_depth - 1); }
                else if (paren_depth == 0 && saw_open && (ch == '{' || ch == ';')) {
                    // Prefix already accumulated char-by-char above; just stop
                    // (re-appending line.substr(0,c) duplicated the tail).
                    done = true;
                    break;
                }
                accum += ch;
            }
            if (done) break;
            accum += " ";
        }
        // collapse whitespace
        std::string out;
        bool prev_space = false;
        for (char ch : accum) {
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                if (!prev_space) out += ' ';
                prev_space = true;
            } else { out += ch; prev_space = false; }
        }
        // trim
        size_t b = out.find_first_not_of(' ');
        size_t e = out.find_last_not_of(' ');
        return (b == std::string::npos) ? "" : out.substr(b, e - b + 1);
    }

    // --- get_signature ---
    void get_signature(const Args& args) {
        if (args.positional.empty()) die("get_signature requires a symbol argument");
        std::string symbol = args.positional[0];
        int limit = args.opt_int("limit", 10);

        std::string method = symbol;
        size_t scope = symbol.rfind("::");
        if (scope != std::string::npos) method = symbol.substr(scope + 2);

        struct Overload { std::string sig, source, file; int line = 0; };
        std::vector<Overload> overloads;

        // Fast path: body-free signature column.
        auto fnrows = query(db,
            "SELECT signature, file_id, line_start FROM symbols WHERE name = ? AND kind = 'function'",
            {method});
        for (auto& r : fnrows) {
            if ((int)overloads.size() >= limit) break;
            std::string sig = r.get("signature");
            if (sig.empty()) continue;
            if (sig.find('{') != std::string::npos || sig.find('\\') != std::string::npos) continue;
            // trim
            size_t b = sig.find_first_not_of(" \t\r\n");
            size_t e = sig.find_last_not_of(" \t\r\n");
            sig = (b == std::string::npos) ? "" : sig.substr(b, e - b + 1);
            overloads.push_back({sig, "column", short_path(get_file_path(r.get_int("file_id"))), r.get_int("line_start")});
        }

        // Primary: declaration-read via source_fts.
        if (overloads.empty()) {
            std::string fts_q = escape_fts(symbol);
            auto chunks = query(db,
                "SELECT file_id, line_number, text FROM source_fts WHERE source_fts MATCH ? "
                "ORDER BY bm25(source_fts) LIMIT 50", {fts_q});
            std::set<std::string> seen;
            std::string needle = method + "(";
            for (auto& ch : chunks) {
                if ((int)overloads.size() >= limit) break;
                std::string fp = get_file_path(ch.get_int("file_id"));
                std::ifstream f(fp);
                if (!f.is_open()) continue;
                std::vector<std::string> file_lines;
                std::string l;
                while (std::getline(f, l)) file_lines.push_back(l);
                int win_start = std::max(0, ch.get_int("line_number") - 1);
                int win_end = std::min((int)file_lines.size(), win_start + 10);
                for (int i = win_start; i < win_end; ++i) {
                    if ((int)overloads.size() >= limit) break;
                    const std::string& line = file_lines[i];
                    size_t didx = line.find(needle);
                    if (didx == std::string::npos) continue;
                    if (didx > 0) {
                        char prev = line[didx - 1];
                        if (std::isalnum((unsigned char)prev) || prev == '_') continue;
                    }
                    std::string sig = compact_declaration(file_lines, i);
                    if (sig.empty() || sig.find(needle) == std::string::npos) continue;
                    if (seen.count(sig)) continue;
                    seen.insert(sig);
                    overloads.push_back({sig, "declaration_read", short_path(fp), i + 1});
                }
            }
        }

        if (overloads.empty()) die("No signature found for '" + symbol + "'.");

        for (size_t i = 0; i < overloads.size(); ++i) {
            if (i > 0) std::cout << "\n";
            std::cout << overloads[i].sig << "\n  // " << overloads[i].source
                      << " @ " << overloads[i].file << ":" << overloads[i].line;
        }
        std::cout << std::endl;
    }

    // --- check_deprecations ---
    void check_deprecations(const Args& args) {
        if (args.positional.empty()) die("check_deprecations requires one or more symbol arguments");

        // Empty index -> clean "empty" state (Decision 3).
        auto cnt = query(db, "SELECT COUNT(*) as c FROM symbol_deprecations", {});
        int total = cnt.empty() ? 0 : cnt[0].get_int("c");
        if (total == 0) {
            std::cout << "Deprecation index is empty (schema v2 landed but not yet populated). "
                         "Run source.trigger_reindex to populate it." << std::endl;
            return;
        }

        std::vector<std::string> lines;
        for (const auto& name : args.positional) {
            auto rows = query(db,
                "SELECT version, message, kind FROM symbol_deprecations WHERE symbol_name = ? LIMIT 1",
                {name});
            if (!rows.empty()) {
                lines.push_back(name + ": DEPRECATED (" + rows[0].get("version") + ") ["
                    + rows[0].get("kind") + "] " + rows[0].get("message"));
            } else {
                lines.push_back(name + ": not deprecated");
            }
        }
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i > 0) std::cout << "\n";
            std::cout << lines[i];
        }
        std::cout << std::endl;
    }

    // Shared composition: does Class::Method (or plain symbol) EXIST? Class-row +
    // source_fts declaration hit, NEVER the method's own symbols row (Step-0).
    bool symbol_exists(const std::string& symbol) {
        std::string lookup = symbol;
        size_t scope = symbol.rfind("::");
        if (scope != std::string::npos) lookup = symbol.substr(0, scope);

        if (!query(db, "SELECT id FROM symbols WHERE name = ? LIMIT 1", {lookup}).empty())
            return true;
        // FTS class-row fallback.
        {
            std::string fts_q = escape_fts(lookup);
            if (!query(db, "SELECT s.id FROM symbols_fts f JOIN symbols s ON s.id = f.rowid "
                           "WHERE symbols_fts MATCH ? LIMIT 1", {fts_q}).empty())
                return true;
        }
        // Source-line declaration hit for `Name(`.
        std::string method = symbol;
        if (scope != std::string::npos) method = symbol.substr(scope + 2);
        std::string needle = method + "(";
        std::string fts_q = escape_fts(symbol);
        auto chunks = query(db,
            "SELECT file_id, line_number FROM source_fts WHERE source_fts MATCH ? "
            "ORDER BY bm25(source_fts) LIMIT 25", {fts_q});
        for (auto& ch : chunks) {
            std::ifstream f(get_file_path(ch.get_int("file_id")));
            if (!f.is_open()) continue;
            std::vector<std::string> fl; std::string l;
            while (std::getline(f, l)) { if (!l.empty() && l.back() == '\r') l.pop_back(); fl.push_back(l); }
            int ws = std::max(0, ch.get_int("line_number") - 1);
            int we = std::min((int)fl.size(), ws + 10);
            for (int i = ws; i < we; ++i) {
                size_t di = fl[i].find(needle);
                if (di == std::string::npos) continue;
                if (di > 0) { char p = fl[i][di - 1]; if (std::isalnum((unsigned char)p) || p == '_') continue; }
                return true;
            }
        }
        return false;
    }

    // First declaration signature: body-free column fast path, else declaration_read.
    bool first_signature(const std::string& symbol, std::string& out_sig, std::string& out_source) {
        std::string method = symbol;
        size_t scope = symbol.rfind("::");
        if (scope != std::string::npos) method = symbol.substr(scope + 2);
        auto fnrows = query(db,
            "SELECT signature, file_id, line_start FROM symbols WHERE name = ? AND kind = 'function'",
            {method});
        for (auto& r : fnrows) {
            std::string sig = r.get("signature");
            if (sig.empty()) continue;
            if (sig.find('{') != std::string::npos || sig.find('\\') != std::string::npos) continue;
            size_t b = sig.find_first_not_of(" \t\r\n");
            size_t e = sig.find_last_not_of(" \t\r\n");
            out_sig = (b == std::string::npos) ? "" : sig.substr(b, e - b + 1);
            out_source = "column";
            return true;
        }
        std::string fts_q = escape_fts(symbol);
        auto chunks = query(db,
            "SELECT file_id, line_number FROM source_fts WHERE source_fts MATCH ? "
            "ORDER BY bm25(source_fts) LIMIT 50", {fts_q});
        std::string needle = method + "(";
        for (auto& ch : chunks) {
            std::ifstream f(get_file_path(ch.get_int("file_id")));
            if (!f.is_open()) continue;
            std::vector<std::string> fl; std::string l;
            while (std::getline(f, l)) fl.push_back(l);
            int ws = std::max(0, ch.get_int("line_number") - 1);
            int we = std::min((int)fl.size(), ws + 10);
            for (int i = ws; i < we; ++i) {
                size_t di = fl[i].find(needle);
                if (di == std::string::npos) continue;
                if (di > 0) { char p = fl[i][di - 1]; if (std::isalnum((unsigned char)p) || p == '_') continue; }
                std::string sig = compact_declaration(fl, i);
                if (sig.empty() || sig.find(needle) == std::string::npos) continue;
                out_sig = sig; out_source = "declaration_read";
                return true;
            }
        }
        return false;
    }

    // --- verify_symbols (item 4) ---
    void verify_symbols(const Args& args) {
        if (args.positional.empty()) die("verify_symbols requires one or more symbol arguments");

        auto cnt = query(db, "SELECT COUNT(*) as c FROM symbol_deprecations", {});
        bool dep_empty = cnt.empty() || cnt[0].get_int("c") == 0;

        std::vector<std::string> lines;
        for (const auto& symbol : args.positional) {
            bool exists = symbol_exists(symbol);
            if (!exists) { lines.push_back(symbol + ": NOT FOUND"); continue; }

            std::string line = symbol + ": exists";

            // include
            bool found = false;
            Row sym = resolve_symbol_row(symbol, found);
            std::string include, module_name, warning; bool includable = true;
            if (found) {
                std::string lookup = symbol;
                size_t scope = symbol.rfind("::");
                if (scope != std::string::npos) lookup = symbol.substr(0, scope);
                auto allrows = query(db,
                    "SELECT s.file_id, f.path FROM symbols s JOIN files f ON f.id = s.file_id WHERE s.name = ?",
                    {lookup});
                int file_id = sym.get_int("file_id");
                std::string file_path = get_file_path(file_id);
                for (auto& r : allrows) {
                    std::string p = r.get("path");
                    if (p.size() >= 2 && p.substr(p.size() - 2) == ".h") { file_id = r.get_int("file_id"); file_path = p; break; }
                }
                auto mrows = query(db,
                    "SELECT m.name FROM files f JOIN modules m ON m.id = f.module_id WHERE f.id = ?",
                    {std::to_string(file_id)});
                module_name = mrows.empty() ? "" : mrows[0].get("name");
                include = derive_include_path(file_path, includable, warning, module_name);
            }
            if (!include.empty())
                line += " | #include \"" + include + "\"" + (includable ? "" : " (NOT includable)");

            // signature
            std::string sig, sig_src;
            if (first_signature(symbol, sig, sig_src) && !sig.empty())
                line += " | " + sig;

            // deprecation
            if (!dep_empty) {
                std::string method = symbol;
                size_t scope = symbol.rfind("::");
                if (scope != std::string::npos) method = symbol.substr(scope + 2);
                auto drows = query(db,
                    "SELECT version FROM symbol_deprecations WHERE symbol_name = ? LIMIT 1", {method});
                if (!drows.empty()) line += " | DEPRECATED";
            }
            lines.push_back(line);
        }
        for (size_t i = 0; i < lines.size(); ++i) { if (i > 0) std::cout << "\n"; std::cout << lines[i]; }
        std::cout << std::endl;
    }

    // --- find_example_usage (item 5) ---
    void find_example_usage(const Args& args) {
        if (args.positional.empty()) die("find_example_usage requires a symbol argument");
        std::string symbol = args.positional[0];
        std::string prefer = args.opt("prefer", "engine");
        std::transform(prefer.begin(), prefer.end(), prefer.begin(), ::tolower);
        bool prefer_project = (prefer == "project");
        int limit = std::max(1, args.opt_int("limit", 10));
        const int HARD_CAP = 500, FTS_FETCH = 400;

        std::string method = symbol;
        size_t scope = symbol.rfind("::");
        if (scope != std::string::npos) method = symbol.substr(scope + 2);
        std::string needle = method + "(";

        struct Usage { std::string file; int line; std::string context; int rank; std::string sort_path; };
        std::vector<Usage> usages;
        std::set<std::string> seen;

        std::string fts_q = escape_fts(symbol);
        auto chunks = query(db,
            "SELECT file_id, line_number FROM source_fts WHERE source_fts MATCH ? "
            "ORDER BY bm25(source_fts) LIMIT " + std::to_string(FTS_FETCH), {fts_q});
        for (auto& ch : chunks) {
            std::string fp = get_file_path(ch.get_int("file_id"));
            std::ifstream f(fp);
            if (!f.is_open()) continue;
            std::vector<std::string> fl; std::string l;
            while (std::getline(f, l)) { if (!l.empty() && l.back() == '\r') l.pop_back(); fl.push_back(l); }
            int ws = std::max(0, ch.get_int("line_number") - 1);
            int we = std::min((int)fl.size(), ws + 10);
            for (int i = ws; i < we; ++i) {
                size_t hi = fl[i].find(needle);
                if (hi == std::string::npos) continue;
                if (hi > 0) { char p = fl[i][hi - 1]; if (std::isalnum((unsigned char)p) || p == '_') continue; }
                std::string key = std::to_string(ch.get_int("file_id")) + "_" + std::to_string(i + 1);
                if (seen.count(key)) continue;
                seen.insert(key);

                int cs = std::max(0, i - 3), ce = std::min((int)fl.size() - 1, i + 3);
                std::ostringstream ctx;
                for (int c = cs; c <= ce; ++c) {
                    char buf[16]; std::snprintf(buf, sizeof(buf), "%5d", c + 1);
                    ctx << buf << " | " << fl[c];
                    if (c < ce) ctx << "\n";
                }
                // engine-vs-project + Runtime classification. PARITY: identical
                // rule across live / exe / py — classify on the forward-slashed raw
                // stored path via the `Engine/Source/` (engine) + `/Source/Runtime/`
                // (runtime) substrings. An engine plugin path (Engine/Plugins/.../
                // Source/) lacks `Engine/Source/` and so ranks as project in all three.
                std::string norm = fp;
                for (auto& ch2 : norm) if (ch2 == '\\') ch2 = '/';
                bool is_engine = norm.find("Engine/Source/") != std::string::npos;
                bool is_runtime = norm.find("/Source/Runtime/") != std::string::npos;
                int rank = is_engine ? (is_runtime ? 0 : 1) : 2;
                usages.push_back({short_path(fp), i + 1, ctx.str(), rank, fp});
            }
        }

        auto rank_key = [&](const Usage& x) -> int {
            if (!prefer_project) return x.rank;
            switch (x.rank) { case 2: return 0; case 0: return 1; default: return 2; }
        };
        std::stable_sort(usages.begin(), usages.end(), [&](const Usage& a, const Usage& b) {
            int ka = rank_key(a), kb = rank_key(b);
            if (ka != kb) return ka < kb;
            if (a.sort_path != b.sort_path) return a.sort_path < b.sort_path;
            return a.line < b.line;
        });

        int total = (int)usages.size();
        int slice_end = std::min(std::min(limit, total), HARD_CAP);
        if (total == 0) { std::cout << "No call-site examples found for '" << symbol << "'." << std::endl; return; }
        // PARITY: emit ONLY the rendered snippets (no total_estimate / next_cursor).
        // Those are structured-JSON fields the live handler sets on its result
        // object, not on content[].text. The text byte-compare runs against the
        // live content[].text, which likewise omits both. Do NOT add them here.
        std::ostringstream out;
        for (int i = 0; i < slice_end; ++i) {
            if (i > 0) out << "\n\n";
            out << "--- " << usages[i].file << ":" << usages[i].line << " ---\n" << usages[i].context;
        }
        std::cout << out.str() << std::endl;
    }

    // --- lint_header (item 7) ---
    // Mirror of FMonolithSourceActions::LintHeaderLines with an ALWAYS-EMPTY
    // specifier vocabulary (the offline tool cannot reach the RI registry, so the
    // invalid-specifier rule is always skipped — matching the live handler's
    // "degrade gracefully when RI unavailable" path). Text output is byte-identical
    // to the live content[].text: one "[sev] Lnn (rule): msg" per finding, or the
    // "Clean -- no lint findings." line. NOTE: live uses an em-dash ("Clean —");
    // ASCII files keep this ASCII -- the parity case uses a file that yields >=1
    // finding so the clean-line spelling is never compared.
    struct LintFinding { std::string rule_id; int line; std::string message; std::string severity; };

    static std::string derive_module_from_path(const std::string& file_path) {
        std::string p = file_path;
        for (auto& c : p) if (c == '\\') c = '/';
        size_t src = p.rfind("/Source/");
        if (src == std::string::npos) return "";
        std::string after = p.substr(src + 8);
        size_t slash = after.find('/');
        if (slash == std::string::npos) return "";
        return after.substr(0, slash);
    }
    static std::string to_upper(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }
    static std::string trim(const std::string& s) {
        size_t b = s.find_first_not_of(" \t\r\n");
        size_t e = s.find_last_not_of(" \t\r\n");
        return (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
    }
    static std::string base_filename(const std::string& path) {
        std::string p = path; for (auto& c : p) if (c == '\\') c = '/';
        size_t slash = p.find_last_of('/');
        std::string name = (slash == std::string::npos) ? p : p.substr(slash + 1);
        size_t dot = name.rfind('.');
        return (dot == std::string::npos) ? name : name.substr(0, dot);
    }
    static std::string clean_filename(const std::string& path) {
        std::string p = path; for (auto& c : p) if (c == '\\') c = '/';
        size_t slash = p.find_last_of('/');
        return (slash == std::string::npos) ? p : p.substr(slash + 1);
    }
    // Strip block comments threading state through in_block.
    static std::string strip_block_comments(const std::string& line, bool& in_block) {
        std::string result, work = line;
        while (true) {
            if (in_block) {
                size_t end = work.find("*/");
                if (end == std::string::npos) return result;
                work = work.substr(end + 2);
                in_block = false;
            }
            size_t open = work.find("/*");
            if (open == std::string::npos) { result += work; return result; }
            result += work.substr(0, open);
            work = work.substr(open + 2);
            in_block = true;
        }
    }
    static std::string strip_line_comment(const std::string& line) {
        size_t lc = line.find("//");
        return (lc == std::string::npos) ? line : line.substr(0, lc);
    }
    static bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
    static bool ends_with(const std::string& s, const std::string& suf) {
        return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
    }

    void lint_header(const Args& args) {
        if (args.positional.empty()) die("lint_header requires a file_path argument");
        std::string file_path = args.positional[0];
        std::ifstream f(file_path);
        if (!f.is_open()) die("Could not read header file: " + file_path);
        std::vector<std::string> lines; std::string l;
        while (std::getline(f, l)) { if (!l.empty() && l.back() == '\r') l.pop_back(); lines.push_back(l); }

        std::string module = derive_module_from_path(file_path);
        std::string api_macro = module.empty() ? "" : to_upper(module) + "_API";

        bool in_block = false, has_gen_body = false, any_reflected = false;
        int gen_body_line = 0, last_inc_line = 0, gen_h_line = 0;
        std::string last_inc_path;
        std::string gen_h_path;   // the ACTUAL *.generated.h include (NOT the last include — fixes rule-c false positive)
        struct RT { int decl_line; std::string name; bool has_api; };
        std::vector<RT> reflected;
        bool pending = false; int pending_line = 0;

        std::vector<LintFinding> findings;

        std::regex inc_re("^\\s*#\\s*include\\s+[\"<]([^\">]+)[\">]");
        std::regex decl_re("^\\s*(?:class|struct)\\s+(?:[A-Z][A-Z0-9_]*_API\\s+)?([A-Za-z_][A-Za-z0-9_]*)");
        std::regex api_re("\\b([A-Z][A-Z0-9_]*_API)\\b");

        for (size_t i = 0; i < lines.size(); ++i) {
            std::string scan = strip_block_comments(lines[i], in_block);
            std::string code = strip_line_comment(scan);
            std::string trimmed = trim(code);
            if (trimmed.empty()) continue;

            std::smatch m;
            if (std::regex_search(code, m, inc_re)) {
                std::string inc = m[1].str();
                last_inc_line = (int)i + 1; last_inc_path = inc;
                if (ends_with(inc, ".generated.h")) { gen_h_line = (int)i + 1; gen_h_path = inc; }
                continue;
            }
            if (trimmed.find("GENERATED_BODY") != std::string::npos ||
                trimmed.find("GENERATED_UCLASS_BODY") != std::string::npos) {
                has_gen_body = true;
                if (gen_body_line == 0) gen_body_line = (int)i + 1;
            }
            if (starts_with(trimmed, "UCLASS") || starts_with(trimmed, "USTRUCT") ||
                starts_with(trimmed, "UENUM") || starts_with(trimmed, "UINTERFACE")) {
                any_reflected = true;
                if (starts_with(trimmed, "UCLASS")) { pending = true; pending_line = (int)i + 1; }
            }
            if (pending) {
                std::smatch dm;
                if (std::regex_search(code, dm, decl_re)) {
                    RT rt; rt.decl_line = (int)i + 1; rt.name = dm[1].str();
                    std::smatch am; rt.has_api = std::regex_search(code, am, api_re);
                    reflected.push_back(rt);
                    pending = false;
                    (void)pending_line;
                }
            }
            // Invalid-specifier rule SKIPPED offline (empty vocabulary).
        }

        // (a) missing GENERATED_BODY.
        if (any_reflected && !has_gen_body) {
            findings.push_back({"missing_generated_body",
                reflected.empty() ? 0 : reflected[0].decl_line,
                "Reflected type (UCLASS/USTRUCT) is missing a GENERATED_BODY() macro.", "error"});
        }
        // (b) generated.h not last.
        if (gen_h_line != 0 && last_inc_line != 0 && gen_h_line != last_inc_line) {
            std::ostringstream msg;
            msg << "'*.generated.h' must be the LAST #include (an include at line "
                << last_inc_line << " follows it: \"" << last_inc_path << "\").";
            findings.push_back({"generated_h_not_last", gen_h_line, msg.str(), "error"});
        } else if (any_reflected && has_gen_body && gen_h_line == 0) {
            findings.push_back({"missing_generated_h_include", gen_body_line,
                "Reflected type uses GENERATED_BODY() but no '*.generated.h' include is present (must be last).", "error"});
        }
        // (d) missing <MODULE>_API.
        std::string header_base = base_filename(file_path);
        for (auto& rt : reflected) {
            if (!api_macro.empty() && !rt.has_api) {
                std::ostringstream msg;
                msg << "UCLASS-declared type '" << rt.name << "' is missing the '" << api_macro
                    << "' export macro (class " << rt.name << " ...).";
                findings.push_back({"missing_api_macro", rt.decl_line, msg.str(), "warning"});
            }
        }
        // (c) generated.h name mismatch. Use the ACTUAL generated.h include
        // (gen_h_path), NOT the last include -- when generated.h is not last
        // (rule-b case) the last include is some other header and would fire a
        // bogus mismatch. Gate on the captured include ending in `.generated.h`.
        if (gen_h_line != 0 && !header_base.empty() && ends_with(gen_h_path, ".generated.h")) {
            std::string gen_base = clean_filename(gen_h_path);
            gen_base = gen_base.substr(0, gen_base.size() - std::string(".generated.h").size());
            if (!gen_base.empty() && gen_base != header_base) {
                std::ostringstream msg;
                msg << "'" << gen_base << ".generated.h' does not match the header file name '"
                    << header_base << ".h' -- the GENERATED_BODY pairing requires \""
                    << header_base << ".generated.h\".";
                findings.push_back({"generated_h_name_mismatch", gen_h_line, msg.str(), "error"});
            }
        }
        // (e) UPROPERTY/UFUNCTION in non-reflected file.
        if (!any_reflected) {
            bool ib = false;
            for (size_t i = 0; i < lines.size(); ++i) {
                std::string scan = strip_block_comments(lines[i], ib);
                std::string trimmed = trim(strip_line_comment(scan));
                if (starts_with(trimmed, "UPROPERTY") || starts_with(trimmed, "UFUNCTION")) {
                    findings.push_back({"reflected_member_in_non_reflected_type", (int)i + 1,
                        "UPROPERTY/UFUNCTION found but the file declares no reflected type (UCLASS/USTRUCT) -- the macro will not be processed by UHT.", "error"});
                }
            }
        }

        std::stable_sort(findings.begin(), findings.end(), [](const LintFinding& a, const LintFinding& b) {
            if (a.line != b.line) return a.line < b.line;
            return a.rule_id < b.rule_id;
        });

        if (findings.empty()) {
            // ASCII tools cannot emit the live em-dash; the parity case never lints
            // a clean file, so this spelling is never byte-compared.
            std::cout << "Clean -- no lint findings." << std::endl;
            return;
        }
        std::ostringstream out;
        for (size_t i = 0; i < findings.size(); ++i) {
            if (i > 0) out << "\n";
            out << "[" << findings[i].severity << "] L" << findings[i].line << " ("
                << findings[i].rule_id << "): " << findings[i].message;
        }
        std::cout << out.str() << std::endl;
    }

    // --- generate_class_stub (item 9, TEXT-RETURN-ONLY, offline-served) ---
    void generate_class_stub(const Args& args) {
        std::string parent = args.opt("parent", "");
        std::string class_name = args.opt("class_name", "");
        std::string module = args.opt("module", "");
        // Positional fallback: parent class_name module.
        if (parent.empty() && args.positional.size() >= 1) parent = args.positional[0];
        if (class_name.empty() && args.positional.size() >= 2) class_name = args.positional[1];
        if (module.empty() && args.positional.size() >= 3) module = args.positional[2];
        if (parent.empty() || class_name.empty() || module.empty())
            die("generate_class_stub requires parent, class_name, and module");

        bool found = false;
        Row sym = resolve_symbol_row(parent, found);
        if (!found) die("Parent class '" + parent + "' not found in the source index. Run source.trigger_reindex if it is a project type.");

        // UCLASS-derived gate: U/A prefix is the engine convention.
        bool looks_uclass = (parent[0] == 'U' || parent[0] == 'A');
        if (!looks_uclass)
            die("Parent '" + parent + "' is not a UCLASS-derived type. generate_class_stub v1 supports UCLASS-derived parents only (no USTRUCT/UENUM/UINTERFACE).");

        // Resolve parent header include (prefer a .h row).
        auto allrows = query(db,
            "SELECT s.file_id, f.path FROM symbols s JOIN files f ON f.id = s.file_id WHERE s.name = ?",
            {parent});
        std::string parent_path = get_file_path(sym.get_int("file_id"));
        for (auto& r : allrows) {
            std::string p = r.get("path");
            if (p.size() >= 2 && p.substr(p.size() - 2) == ".h") { parent_path = p; break; }
        }
        bool includable = true; std::string warning;
        std::string parent_module = derive_module_from_path(parent_path);
        std::string parent_include = derive_include_path(parent_path, includable, warning, parent_module);

        // Constructor convention: plain default unless the parent ctor requires
        // FObjectInitializer& (and has no plain alternative).
        bool needs_obj_init = false;
        {
            auto ctors = query(db,
                "SELECT signature FROM symbols WHERE name = ? AND kind = 'function'", {parent});
            bool saw_any = false, saw_plain = false, saw_obj = false;
            std::string ctor_open = parent + "(";
            for (auto& r : ctors) {
                std::string s = r.get("signature");
                if (s.empty() || s.find(ctor_open) == std::string::npos) continue;
                saw_any = true;
                if (s.find("FObjectInitializer") != std::string::npos) saw_obj = true;
                else saw_plain = true;
            }
            needs_obj_init = saw_any && saw_obj && !saw_plain;
        }

        std::string api_macro = to_upper(module) + "_API";
        // UE "Add C++ Class" file-naming: drop the U/A UCLASS-derived prefix from the
        // FILE names (class UMyComp -> MyComp.h/.cpp/.generated.h). class_name is
        // validated UCLASS-derived, so strip a leading U/A only when followed by an
        // uppercase letter; else use the raw name. The class IDENTIFIER is unchanged.
        std::string file_base = class_name;
        if (class_name.size() >= 2 && (class_name[0] == 'U' || class_name[0] == 'A')
            && std::isupper((unsigned char)class_name[1])) {
            file_base = class_name.substr(1);
        }
        std::string gen_inc = file_base + ".generated.h";

        std::ostringstream h;
        h << "#pragma once\n\n";
        h << "#include \"CoreMinimal.h\"\n";
        if (!parent_include.empty()) h << "#include \"" << parent_include << "\"\n";
        h << "#include \"" << gen_inc << "\"\n\n";
        h << "UCLASS()\n";
        h << "class " << api_macro << " " << class_name << " : public " << parent << "\n";
        h << "{\n\tGENERATED_BODY()\n\npublic:\n";
        if (needs_obj_init)
            h << "\t" << class_name << "(const FObjectInitializer& ObjectInitializer);\n";
        else
            h << "\t" << class_name << "();\n";
        h << "};\n";

        std::ostringstream c;
        c << "#include \"" << file_base << ".h\"\n\n";
        if (needs_obj_init) {
            c << class_name << "::" << class_name << "(const FObjectInitializer& ObjectInitializer)\n";
            c << "\t: Super(ObjectInitializer)\n{\n}\n";
        } else {
            c << class_name << "::" << class_name << "()\n{\n}\n";
        }

        std::cout << "// === " << file_base << ".h ===\n" << h.str()
                  << "\n// === " << file_base << ".cpp ===\n" << c.str() << std::endl;
    }
};

// ============================================================
// Project actions
// ============================================================

class ProjectActions {
    Database db;

public:
    void open(const std::string& path) { db.open(path); }

    // --- search ---
    void search(const Args& args) {
        auto emit_failure = [](const std::string& message) {
            json out = {{"success", false}, {"error", message}};
            std::cout << out.dump(2) << std::endl;
        };

        if (args.positional.empty()) die("search requires a query argument");

        const std::string& raw_query = args.positional[0];
        size_t query_begin = 0;
        size_t query_end = raw_query.size();
        while (query_begin < query_end && std::isspace(static_cast<unsigned char>(raw_query[query_begin])))
            ++query_begin;
        while (query_end > query_begin && std::isspace(static_cast<unsigned char>(raw_query[query_end - 1])))
            --query_end;
        if (query_begin == query_end) {
            emit_failure("'query' must not be empty");
            return;
        }
        const std::string q = raw_query.substr(query_begin, query_end - query_begin);
        if (q.size() > PROJECT_SEARCH_MAX_QUERY_LENGTH) {
            emit_failure("'query' must be " + std::to_string(PROJECT_SEARCH_MAX_QUERY_LENGTH)
                         + " characters or fewer (got " + std::to_string(q.size()) + ")");
            return;
        }

        int limit = 50;
        auto limit_it = args.options.find("limit");
        if (limit_it != args.options.end()
            && !try_parse_clamped_decimal(limit_it->second, 1, 1000, limit)) {
            emit_failure("'limit' must be an integer");
            return;
        }

        // Mirrors the live `asset_class` filter (SPEC_MonolithIndex "Asset-Class
        // Filtering"). Nothing gates this parity, so it is kept in step by hand.
        // Args::options is single-valued, so the list is comma-separated rather than
        // a repeated flag; the Python sibling accepts the same syntax.
        std::vector<std::string> class_filter;
        // parse_args normalises hyphens to underscores, so `--asset-class` and
        // `--asset_class` both land under the "asset_class" key -- looking it up under
        // the hyphenated spelling would never match and the filter would silently do
        // nothing while compiling clean.
        auto class_it = args.options.find("asset_class");
        if (class_it != args.options.end()) {
            const std::string& raw = class_it->second;
            // Lowercased keys, mirroring FString's case-insensitive AddUnique.
            std::set<std::string> seen;
            size_t start = 0;
            for (;;) {
                const size_t comma = raw.find(',', start);
                const size_t stop = (comma == std::string::npos) ? raw.size() : comma;
                size_t b = start, e = stop;
                while (b < e && std::isspace(static_cast<unsigned char>(raw[b]))) ++b;
                while (e > b && std::isspace(static_cast<unsigned char>(raw[e - 1]))) --e;
                if (e > b) {
                    const std::string name = raw.substr(b, e - b);
                    std::string key = name;
                    std::transform(key.begin(), key.end(), key.begin(),
                                   [](unsigned char ch) { return (char)std::tolower(ch); });
                    if (seen.insert(key).second) class_filter.push_back(name);
                }
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            // Present but empty after trimming is a caller mistake, not a request for
            // unfiltered results -- silently widening the search would be the wrong guess.
            if (class_filter.empty()) {
                emit_failure("'asset_class' must name at least one non-empty class");
                return;
            }
            if (class_filter.size() > PROJECT_SEARCH_MAX_CLASS_FILTERS) {
                emit_failure("'asset_class' must list "
                             + std::to_string(PROJECT_SEARCH_MAX_CLASS_FILTERS)
                             + " classes or fewer (got " + std::to_string(class_filter.size()) + ")");
                return;
            }
        }

        // Only the placeholder count is interpolated; the names stay bound.
        std::string class_predicate;
        if (!class_filter.empty()) {
            class_predicate = " AND a.asset_class COLLATE NOCASE IN (";
            for (size_t i = 0; i < class_filter.size(); ++i)
                class_predicate += (i == 0) ? "?" : ",?";
            class_predicate += ")";
        }

        json results = json::array();
        // Per-table verdicts: a table that reports an unknown column is simply not
        // the one that can answer this query. Only both refusing is a caller error.
        int not_applicable = 0;
        std::string first_not_applicable;

        // Returns false once a failure has been emitted.
        auto run_search = [&](const std::string& sql, const char* table_name) -> bool {
            Rows rows;
            std::string error;
            // The class filters occupy 2..N+1, so the limit stays last.
            std::vector<Bind> binds;
            binds.reserve(class_filter.size() + 2);
            binds.push_back(Bind(q));
            for (const std::string& class_name : class_filter) binds.push_back(Bind(class_name));
            binds.push_back(Bind::Integer(limit));
            if (!query_checked(db, sql, binds, rows, error)) {
                if (is_fts5_unknown_column_error(error)) {
                    ++not_applicable;
                    if (first_not_applicable.empty()) first_not_applicable = error;
                    return true;
                }
                if (is_fts5_syntax_error(error))
                    emit_failure("Invalid FTS5 query: " + error);
                else
                    emit_failure(std::string("Project search failed: ") + table_name
                                 + " FTS query failed: " + error);
                return false;
            }

            for (auto& r : rows) {
                results.push_back({
                    {"asset_path", r.get("package_path")},
                    {"asset_name", r.get("asset_name")},
                    {"asset_class", r.get("asset_class")},
                    {"module_name", r.get("module_name")},
                    {"match_context", r.get("ctx")},
                    {"rank", r.get_double("rank")},
                });
            }
            return true;
        };

        // Search assets FTS
        if (!run_search(
                "SELECT a.package_path, a.asset_name, a.asset_class, a.module_name, "
                "snippet(fts_assets, 2, '>>>', '<<<', '...', 32) as ctx, rank "
                "FROM fts_assets f JOIN assets a ON a.id = f.rowid "
                "WHERE fts_assets MATCH ?" + class_predicate + " ORDER BY rank LIMIT ?",
                "assets"))
            return;

        // Search nodes FTS
        if (!run_search(
                "SELECT a.package_path, a.asset_name, a.asset_class, a.module_name, "
                "snippet(fts_nodes, 0, '>>>', '<<<', '...', 32) as ctx, f.rank "
                "FROM fts_nodes f JOIN nodes n ON n.id = f.rowid "
                "JOIN assets a ON a.id = n.asset_id "
                "WHERE fts_nodes MATCH ?" + class_predicate + " ORDER BY f.rank LIMIT ?",
                "nodes"))
            return;

        if (not_applicable == 2) {
            // No FTS table exposes the requested column.
            emit_failure("Invalid FTS5 query: " + describe_unknown_column(first_not_applicable));
            return;
        }

        // Sort by rank, truncate
        std::sort(results.begin(), results.end(),
                  [](const json& a, const json& b) { return a["rank"].get<double>() < b["rank"].get<double>(); });
        if ((int)results.size() > limit)
            results = json(std::vector<json>(results.begin(), results.begin() + limit));

        json out = {{"success", true}, {"count", results.size()}, {"results", results}};
        std::cout << out.dump(2) << std::endl;
    }

    // --- find_by_type ---
    void find_by_type(const Args& args) {
        if (args.positional.empty()) die("find_by_type requires an asset_class argument");
        std::string asset_class = args.positional[0];
        int limit = args.opt_int("limit", 50);
        int offset = args.opt_int("offset", 0);

        auto rows = query(db,
            "SELECT package_path, asset_name, asset_class, module_name, description FROM assets "
            "WHERE asset_class = ? LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset),
            {asset_class});

        json results = json::array();
        for (auto& r : rows) {
            results.push_back({
                {"package_path", r.get("package_path")},
                {"asset_name", r.get("asset_name")},
                {"asset_class", r.get("asset_class")},
                {"module_name", r.get("module_name")},
                {"description", r.get("description")},
            });
        }

        json out = {{"success", true}, {"count", results.size()}, {"results", results}};
        std::cout << out.dump(2) << std::endl;
    }

    // --- find_references ---
    void find_references(const Args& args) {
        if (args.positional.empty()) die("find_references requires an asset_path argument");
        std::string asset_path = args.positional[0];

        auto assets = query(db, "SELECT id FROM assets WHERE package_path = ?", {asset_path});
        if (assets.empty()) {
            json out = {{"success", false}, {"error", "Asset not found: " + asset_path}};
            std::cout << out.dump(2) << std::endl;
            return;
        }

        std::string aid = assets[0].get("id");

        // Depends on
        auto deps = query(db,
            "SELECT a.package_path, a.asset_class, d.dependency_type "
            "FROM dependencies d JOIN assets a ON a.id = d.target_asset_id WHERE d.source_asset_id = ?",
            {aid});

        // Referenced by
        auto refs = query(db,
            "SELECT a.package_path, a.asset_class, d.dependency_type "
            "FROM dependencies d JOIN assets a ON a.id = d.source_asset_id WHERE d.target_asset_id = ?",
            {aid});

        json depends_on = json::array();
        for (auto& r : deps) {
            depends_on.push_back({
                {"path", r.get("package_path")},
                {"class", r.get("asset_class")},
                {"type", r.get("dependency_type")},
            });
        }

        json referenced_by = json::array();
        for (auto& r : refs) {
            referenced_by.push_back({
                {"path", r.get("package_path")},
                {"class", r.get("asset_class")},
                {"type", r.get("dependency_type")},
            });
        }

        json out = {{"success", true}, {"depends_on", depends_on}, {"referenced_by", referenced_by}};
        std::cout << out.dump(2) << std::endl;
    }

    // --- get_stats ---
    void get_stats(const Args&) {
        json stats;
        static const char* tables[] = {
            "assets", "nodes", "connections", "variables", "parameters",
            "dependencies", "actors", "tags", "configs", "datatable_rows"
        };
        for (auto t : tables) {
            auto rows = query(db, std::string("SELECT COUNT(*) as c FROM ") + t);
            stats[t] = rows.empty() ? 0 : rows[0].get_int("c");
        }

        // Class breakdown
        json breakdown;
        auto class_rows = query(db,
            "SELECT asset_class, COUNT(*) as cnt FROM assets GROUP BY asset_class ORDER BY cnt DESC LIMIT 20");
        for (auto& r : class_rows)
            breakdown[r.get("asset_class")] = r.get_int("cnt");
        stats["asset_class_breakdown"] = breakdown;

        // Module breakdown
        json mod_breakdown;
        auto mod_rows = query(db,
            "SELECT CASE WHEN module_name = '' THEN 'Project' ELSE module_name END as mod, "
            "COUNT(*) as cnt FROM assets GROUP BY module_name ORDER BY cnt DESC");
        for (auto& r : mod_rows)
            mod_breakdown[r.get("mod")] = r.get_int("cnt");
        stats["module_breakdown"] = mod_breakdown;

        std::cout << stats.dump(2) << std::endl;
    }

    // --- get_asset_details ---
    void get_asset_details(const Args& args) {
        if (args.positional.empty()) die("get_asset_details requires an asset_path argument");
        std::string asset_path = args.positional[0];

        auto assets = query(db, "SELECT * FROM assets WHERE package_path = ?", {asset_path});
        if (assets.empty()) {
            json out = {{"error", "Asset not found: " + asset_path}};
            std::cout << out.dump(2) << std::endl;
            return;
        }

        auto& asset = assets[0];
        json details;
        for (auto& [k, v] : asset.cols) details[k] = v;

        std::string aid = asset.get("id");

        // Nodes
        auto nodes = query(db, "SELECT node_type, node_name, node_class FROM nodes WHERE asset_id = ?", {aid});
        json jnodes = json::array();
        for (auto& n : nodes)
            jnodes.push_back({{"node_type", n.get("node_type")}, {"node_name", n.get("node_name")}, {"node_class", n.get("node_class")}});
        details["nodes"] = jnodes;

        // Variables
        auto vars = query(db,
            "SELECT var_name, var_type, category, default_value, is_exposed, is_replicated "
            "FROM variables WHERE asset_id = ?", {aid});
        json jvars = json::array();
        for (auto& v : vars) {
            jvars.push_back({
                {"var_name", v.get("var_name")}, {"var_type", v.get("var_type")},
                {"category", v.get("category")}, {"default_value", v.get("default_value")},
                {"is_exposed", v.get("is_exposed")}, {"is_replicated", v.get("is_replicated")},
            });
        }
        details["variables"] = jvars;

        // Parameters
        auto params = query(db,
            "SELECT param_name, param_type, param_group, default_value FROM parameters WHERE asset_id = ?", {aid});
        json jparams = json::array();
        for (auto& p : params) {
            jparams.push_back({
                {"param_name", p.get("param_name")}, {"param_type", p.get("param_type")},
                {"param_group", p.get("param_group")}, {"default_value", p.get("default_value")},
            });
        }
        details["parameters"] = jparams;

        std::cout << details.dump(2) << std::endl;
    }
};

// ============================================================
// Reflection Intelligence shared infra — base64, cursor codec, filter hash
//
// FULL OFFLINE PARITY (20 RI actions across cppreflect/network/decision/risk).
// Authoritative reference: Docs/plans/offline-parity-spec.md. Every field NAME,
// TYPE (bool vs int!), ORDER, wrapper shape, ORDER BY, and default filter is
// pinned by that spec; a downstream HARD-GATE parity test byte-diffs this exe's
// output against the Python sibling (Scripts/monolith_offline.py) and the live
// server. KEY ORDER MATTERS — UE FJsonObject serializes in insertion order, so
// all RI JSON below uses nlohmann::ordered_json (insertion-order preserving),
// NOT the default json (which sorts keys alphabetically). The source.* and
// project.* actions above are intentionally left on default json — out of scope.
// ============================================================

using ojson = nlohmann::ordered_json;

// Forward declarations — these string helpers are defined later (with the guide
// section logic) but are used by the RI adapters above that point.
static std::string trim_copy(const std::string& s);
static std::string to_lower_copy(std::string s);

// PARITY_SPEC_REV — the agreed parity-rev string. Both this exe and the Python
// sibling mirror this literal so an external parity guard can assert exe and py
// report the same rev (and thus implement the same spec snapshot). Bump in BOTH
// implementers whenever offline-parity-spec.md changes shape.
static const char* PARITY_SPEC_REV = "2026-05-29.1";

// SOURCE_HASH — optionally injected by the build (e.g. /DSOURCE_HASH="...") so
// the orchestrator's staleness guard can detect a stale exe vs source. Defaults
// to "dev" when not injected.
#ifndef SOURCE_HASH
#define SOURCE_HASH "dev"
#endif

// ---- Standard base64 (RFC 4648, '+' '/' '=' padding) — matches UE FBase64. ----
static const char kB64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const std::string& in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        unsigned n = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8 | (unsigned char)in[i + 2];
        out += kB64Chars[(n >> 18) & 63];
        out += kB64Chars[(n >> 12) & 63];
        out += kB64Chars[(n >> 6) & 63];
        out += kB64Chars[n & 63];
        i += 3;
    }
    size_t rem = in.size() - i;
    if (rem == 1) {
        unsigned n = (unsigned char)in[i] << 16;
        out += kB64Chars[(n >> 18) & 63];
        out += kB64Chars[(n >> 12) & 63];
        out += "==";
    } else if (rem == 2) {
        unsigned n = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8;
        out += kB64Chars[(n >> 18) & 63];
        out += kB64Chars[(n >> 12) & 63];
        out += kB64Chars[(n >> 6) & 63];
        out += '=';
    }
    return out;
}

// Returns false on malformed input (used to flag INVALID_CURSOR).
static bool base64_decode(const std::string& in, std::string& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    int buf = 0, bits = 0;
    for (char c : in) {
        if (c == '=') break;
        if (c == '\r' || c == '\n') continue;
        int v = val(c);
        if (v < 0) return false;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((buf >> bits) & 0xFF);
        }
    }
    return true;
}

// ---- UE-style string + combine hashing (for ComputeFilterHash). ----
//
// The HARD-GATE parity guard byte-diffs `next_cursor`, which embeds the filter
// hash `qh`. So these MUST be a step-for-step port of the live UE hash funcs
// (validated against the live editor in offline-byte-parity-addendum.md §3):
//   GetTypeHash(const FString&) -> FCrc::Strihash_DEPRECATED(Len, *S)  (UnrealString.h.inl:2176)
//   GetTypeHash(double)         -> (uint32)v + ((uint32)(v>>32))*23
//   HashCombine(A, C)           -> full Bob-Jenkins mix (TypeHash.h:36-52)
//
// ROOT-CAUSE FIX (addendum §3): GetTypeHash(FString) is NOT FCrc::StrCrc32
// (reflected CRC-32). It is FCrc::Strihash_DEPRECATED — a CASE-INSENSITIVE hash
// over WIDECHAR using a FORWARD (non-reflected) CRC-32 table (CRCTable_DEPRECATED,
// poly 0x04C11DB7), processing each UTF-16 code unit as lo-byte THEN hi-byte,
// with ToUpper applied per char. The prior StrCrc32 port produced wrong `qh`
// values. ue_hash_combine and ue_hash_double were already correct and are kept.

// FCrc::CRCTable_DEPRECATED[256] (Crc.cpp:40) — the FORWARD CRC-32 table:
//   c = i << 24; repeat 8x: c = (c<<1) ^ (poly if top bit set, else 0).
// Sanity (addendum §3): t[0]=0x00000000, t[1]=0x04C11DB7, t[255]=0xB1F740B4.
static const uint32_t (&strihash_table())[256] {
    static uint32_t t[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = (i << 24) & 0xFFFFFFFFu;
            for (int k = 0; k < 8; ++k)
                c = (c & 0x80000000u) ? (((c << 1) ^ 0x04C11DB7u) & 0xFFFFFFFFu)
                                      : ((c << 1) & 0xFFFFFFFFu);
            t[i] = c;
        }
        init = true;
    }
    return t;
}

// ASCII ToUpper over a 16-bit code unit (UE FChar::ToUpper for BMP a-z; the RI
// filter inputs — class names, paths, stringified ints, "0"/"1"/"__stale__" —
// are all ASCII, so case-folding only matters in the a-z range here).
static uint16_t widechar_to_upper(uint16_t cu) {
    if (cu >= (uint16_t)'a' && cu <= (uint16_t)'z') return (uint16_t)(cu - 32);
    return cu;
}

// FCrc::Strihash_DEPRECATED(Len, *S) over WIDECHAR (Crc.h:195) ==
// GetTypeHash(const FString&). Case-insensitive (ToUpper per char), each UTF-16
// code unit fed lo-byte THEN hi-byte through the forward table. Empty -> 0.
// ASCII bytes map 1:1 to UTF-16 code units (value == byte), matching the live
// FString contents for all RI filter inputs. Validated qh values in addendum §3:
// Strihash("ALeviathanCharacterBase") chain -> 2954246778; Strihash("Blueprintable")
// -> 1087131954 (== "blueprintable", proving case-insensitivity).
static uint32_t ue_str_crc32(const std::string& s) {
    const auto& t = strihash_table();
    uint32_t h = 0;
    for (unsigned char b : s) {
        uint16_t cu = widechar_to_upper((uint16_t)b);
        uint16_t lo = cu & 0xFF;
        h = ((h >> 8) & 0x00FFFFFFu) ^ t[(h ^ lo) & 0xFFu];
        uint16_t hi = (cu >> 8) & 0xFF;
        h = ((h >> 8) & 0x00FFFFFFu) ^ t[(h ^ hi) & 0xFFu];
    }
    return h & 0xFFFFFFFFu;
}

// UE HashCombine (TypeHash.h:36-52) — the full Bob-Jenkins mixing variant
// (NOT HashCombineFast). Ported step-for-step from the Python sibling's
// _hash_combine(a, c).
static uint32_t ue_hash_combine(uint32_t a, uint32_t c) {
    uint32_t b = 0x9e3779b9u;
    a += b;

    a -= b; a -= c; a ^= (c >> 13);
    b -= c; b -= a; b ^= (a << 8);
    c -= a; c -= b; c ^= (b >> 13);
    a -= b; a -= c; a ^= (c >> 12);
    b -= c; b -= a; b ^= (a << 16);
    c -= a; c -= b; c ^= (b >> 5);
    a -= b; a -= c; a ^= (c >> 3);
    b -= c; b -= a; b ^= (a << 10);
    c -= a; c -= b; c ^= (b >> 15);
    return c;
}

// GetTypeHash(double) -> GetTypeHash(*(uint64*)&d) -> (uint32)v + ((uint32)(v>>32))*23.
static uint32_t ue_hash_double(double d) {
    uint64_t bits;
    std::memcpy(&bits, &d, sizeof(bits));
    return (uint32_t)bits + (uint32_t)(bits >> 32) * 23u;
}

// ComputeFilterHash over an ordered list of string parts (initializer-list form).
static uint32_t compute_filter_hash(const std::vector<std::string>& parts) {
    uint32_t h = 0;
    for (const auto& p : parts)
        h = ue_hash_combine(h, ue_str_crc32(p));
    return h;
}

// ---- Cursor codec. Encodes {qh,p,tc} as UE pretty-printed JSON, then base64. ----
// CRITICAL (addendum §2): the live cursor is FBase64::Encode of the DEFAULT
// TJsonWriterFactory<> output — pretty-print policy = CRLF newlines + single-TAB
// indent + one space after each colon, key order qh,p,tc. The exact decoded
// bytes are:
//     {\r\n\t"qh": <qh>,\r\n\t"p": <p>,\r\n\t"tc": <tc>\r\n}
// nlohmann's dump() cannot reproduce this exact whitespace (no CRLF, no
// space-after-colon-without-newline control), so the byte sequence is HAND-BUILT
// here, then base64-encoded. Integer values are plain (qh/p/tc are integral and
// in-range, so WriteDouble("%.17g") yields the same digits as plain int — see
// addendum §2 "Numbers inside the cursor are also %.17g"). tc may be -1.
// Verified samples (addendum §2): list_uproperties qh=2954246778,p=1,tc=17 ->
//   ew0KCSJxaCI6IDI5NTQyNDY3NzgsDQoJInAiOiAxLA0KCSJ0YyI6IDE3DQp9
static std::string encode_cursor(uint32_t query_hash, int32_t page, int32_t cached_total) {
    std::string body;
    body += "{\r\n\t\"qh\": ";
    body += std::to_string((uint32_t)query_hash);
    body += ",\r\n\t\"p\": ";
    body += std::to_string((int32_t)page);
    body += ",\r\n\t\"tc\": ";
    body += std::to_string((int32_t)cached_total);
    body += "\r\n}";
    return base64_encode(body);
}

// ---- %.17g float-field sentinel mechanism (addendum §1). ----
// Live RI numeric fields are written by UE via SetNumberField -> serialized by
// TJsonPrintPolicy::WriteDouble = FString::Printf(TEXT("%.17g"), Value). nlohmann
// dump() emits shortest-round-trip and offers NO %.17g hook, so for the genuine-
// fractional REAL fields we store a unique SENTINEL STRING in the ojson, then run
// a string pass over the dumped output replacing the QUOTED sentinel with the RAW
// unquoted number. Integer-valued doubles (total_estimate, counts, line numbers,
// unix timestamps) print identically as plain ints, so they are left as-is per
// the addendum ("integer-valued doubles print clean").
//
// REAL columns are stored float32 in the indexer; when read into a C++ double
// they widen. The live server formats THAT widened value. So we MUST float32-
// round-trip (double -> float -> double) before %.17g, e.g. 0.65 -> the float32
// nearest -> widened -> "0.64999997615814209" (== live confidence), NOT the
// double "0.6499999761581421".
//
// The sentinel uses the 0x01 control byte as a fence; 0x01 cannot appear in any
// emitted RI JSON value (all are class/property/path/snippet text + numbers),
// and the quoted form "\x01FLT:<digits>\x01" cannot collide with real data.
static const char kFltSentinelByte = '\x01';

// Produce the sentinel STRING for a genuine-float REAL field. Caller assigns it
// to an ojson string value; finalize_float_sentinels() later substitutes the raw
// number. Formats %.17g of the RAW double exactly as read from
// sqlite3_column_double (NO float32 round-trip): the stored doubles are already
// correct. confidence is float32-origin so still prints 0.64999997615814209,
// while genuine doubles like normalised_churn print 0.65714285714285714.
static std::string flt_sentinel(double real_value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", real_value);
    std::string s;
    s += kFltSentinelByte;
    s += "FLT:";
    s += buf;
    s += kFltSentinelByte;
    return s;
}

// After ojson::dump(): replace every  "\x01FLT:<digits>\x01"  (a QUOTED sentinel,
// exactly as nlohmann renders a string value) with  <digits>  (raw JSON number,
// no quotes). nlohmann does not escape 0x01 inside strings with the default dump,
// but to be defensive we also handle the  escaped form. Returns the patched
// string ready for stdout.
static std::string finalize_float_sentinels(const std::string& dumped) {
    std::string out = dumped;
    // Two possible renderings of the fence byte by nlohmann: raw 0x01 or .
    const std::string fences[] = { std::string(1, kFltSentinelByte), "\\u0001" };
    for (const std::string& fence : fences) {
        const std::string open = "\"" + fence + "FLT:";   // opening quote + fence + tag
        const std::string close = fence + "\"";           // fence + closing quote
        size_t pos = 0;
        while ((pos = out.find(open, pos)) != std::string::npos) {
            size_t digits_start = pos + open.size();
            size_t close_pos = out.find(close, digits_start);
            if (close_pos == std::string::npos) break;     // malformed; leave as-is
            std::string digits = out.substr(digits_start, close_pos - digits_start);
            out.replace(pos, (close_pos + close.size()) - pos, digits);
            pos += digits.size();
        }
    }
    return out;
}

// Emit an RI ojson with the float-sentinel finalize pass applied. ALL RI actions
// route their final output through this so the %.17g substitution is uniform.
static void emit_ri(const ojson& out) {
    std::cout << finalize_float_sentinels(out.dump(2)) << std::endl;
}

struct CursorState {
    uint32_t query_hash = 0;
    int32_t page = 0;
    int32_t cached_total = -1;
};

// Returns true on a valid cursor; false sets out_reason to the spec message.
static bool decode_cursor(const std::string& cursor, CursorState& out, std::string& out_reason) {
    std::string jsonStr;
    if (!base64_decode(cursor, jsonStr)) {
        out_reason = "Cursor decode failed; restart pagination without `cursor`.";
        return false;
    }
    ojson o = ojson::parse(jsonStr, nullptr, false);
    if (o.is_discarded() || !o.is_object()) {
        out_reason = "Cursor decode failed; restart pagination without `cursor`.";
        return false;
    }
    if (!o.contains("qh") || !o.contains("p") || !o.contains("tc") ||
        !o["qh"].is_number() || !o["p"].is_number() || !o["tc"].is_number()) {
        out_reason = "Cursor decode failed; restart pagination without `cursor`.";
        return false;
    }
    double qh = o["qh"].get<double>();
    double p = o["p"].get<double>();
    double tc = o["tc"].get<double>();
    if (p < 0 || qh < 0 || qh > 4294967295.0) {
        out_reason = "Cursor decode failed; restart pagination without `cursor`.";
        return false;
    }
    out.query_hash = (uint32_t)qh;
    out.page = (int32_t)p;
    out.cached_total = (int32_t)tc;
    return true;
}

// Emit the INVALID_CURSOR error envelope and return.
static void emit_invalid_cursor(const std::string& reason) {
    ojson out;
    out["success"] = false;
    out["error"] = reason;
    out["error_code"] = "INVALID_CURSOR";
    std::cout << out.dump(2) << std::endl;
}

// Emit a generic missing-required / bad-param error.
static void emit_param_error(const std::string& message) {
    ojson out;
    out["success"] = false;
    out["error"] = message;
    std::cout << out.dump(2) << std::endl;
}

// Emit a missing-table error per spec §0.7.
static void emit_missing_table(const std::string& table) {
    ojson out;
    out["success"] = false;
    out["error"] = table + " not in EngineSource.db. Build the project + rebuild_reflection_index in-editor.";
    std::cout << out.dump(2) << std::endl;
}

// Clamp limit per spec §0.3: default 50, [1,200].
static int clamp_limit(const Args& args) {
    int limit = args.opt_int("limit", 50);
    if (limit < 1) limit = 1;
    if (limit > 200) limit = 200;
    return limit;
}

// COUNT(*) helper.
static int64_t scalar_count(Database& db, const std::string& sql, const std::vector<std::string>& params) {
    auto rows = query(db, sql, params);
    return rows.empty() ? 0 : rows[0].get_int64(rows[0].cols.begin()->first);
}

// Does a table exist?
static bool table_exists(Database& db, const std::string& name) {
    auto rows = query(db,
        "SELECT name FROM sqlite_master WHERE type='table' AND name = ? LIMIT 1", {name});
    return !rows.empty();
}

// ============================================================
// Reflection Intelligence actions (read-only, EngineSource.db reflect_* tables)
//
// Implements all 20 RI actions to Docs/plans/offline-parity-spec.md. The live
// F*QueryAdapter.cpp handlers + *Schema.cpp CREATE TABLE statements are the
// upstream source of truth; the spec distilled them. Offline output is shaped to
// be byte-identical to the Python sibling and the live server.
// ============================================================

class ReflectionActions {
    Database db;

    // Resolve pagination page from an optional cursor. On a cursor present:
    // decode + validate filter hash; on mismatch/decode-fail emit INVALID_CURSOR
    // and signal abort via return false. On no cursor: page 0. CachedTotal is
    // carried out for actions that forward it (unused for total-less actions).
    bool resolve_page(const Args& args, uint32_t expected_hash,
                      int32_t& out_page, int32_t& out_cached_total, bool& has_cursor) {
        std::string cursor = args.opt("cursor");
        has_cursor = !cursor.empty();
        if (!has_cursor) { out_page = 0; out_cached_total = -1; return true; }
        CursorState st;
        std::string reason;
        if (!decode_cursor(cursor, st, reason)) { emit_invalid_cursor(reason); return false; }
        if (st.query_hash != expected_hash) {
            emit_invalid_cursor("Cursor filter mismatch; restart pagination without `cursor`.");
            return false;
        }
        out_page = st.page;
        out_cached_total = st.cached_total;
        return true;
    }

    // Per-function source-line join (list_ufunctions). Returns 0 on miss.
    int lookup_function_source_line(const std::string& function_name, const std::string& owning_class) {
        if (function_name.empty()) return 0;
        if (!owning_class.empty()) {
            auto rows = query(db,
                "SELECT s.line_start FROM symbols s JOIN symbols p ON p.id = s.parent_symbol_id "
                "WHERE s.name = ? AND s.kind IN ('function','method') "
                "AND p.name = ? AND p.kind IN ('class','struct') LIMIT 1",
                {function_name, owning_class});
            return rows.empty() ? 0 : rows[0].get_int("line_start");
        }
        auto rows = query(db,
            "SELECT line_start FROM symbols WHERE name = ? AND kind IN ('function','method') LIMIT 1",
            {function_name});
        return rows.empty() ? 0 : rows[0].get_int("line_start");
    }

    // Class source-line auto-join (get_uclass). Returns 0 on miss.
    int lookup_class_source_line(const std::string& class_name) {
        if (class_name.empty()) return 0;
        auto rows = query(db,
            "SELECT s.line_start FROM symbols s WHERE s.name = ? "
            "AND s.kind IN ('class','struct') LIMIT 1", {class_name});
        return rows.empty() ? 0 : rows[0].get_int("line_start");
    }

    // Class source-path auto-join (get_uclass). Returns "" on miss.
    std::string lookup_class_source_path(const std::string& class_name) {
        if (class_name.empty()) return "";
        auto rows = query(db,
            "SELECT f.path FROM symbols s JOIN files f ON f.id = s.file_id "
            "WHERE s.name = ? AND s.kind IN ('class','struct') LIMIT 1", {class_name});
        return rows.empty() ? "" : rows[0].get("path");
    }

public:
    void open(const std::string& path) { db.open(path); }

    // ========================================================
    // NAMESPACE: cppreflect (6)
    // ========================================================

    // --- cppreflect get_uclass ---  (NOT paginated)
    void get_uclass(const Args& args) {
        std::string class_name = args.opt("class_name");
        if (class_name.empty() && !args.positional.empty()) class_name = args.positional[0];
        if (class_name.empty()) { emit_param_error("`class_name` is required."); return; }
        std::string module = args.opt("module");
        if (module.empty()) module = args.opt("module_name");

        if (!table_exists(db, "reflect_uclasses")) { emit_missing_table("reflect_uclasses"); return; }

        std::string sql = "SELECT class_name, module_name, parent_class, source_path, source_line, flags "
                          "FROM reflect_uclasses WHERE class_name = ?";
        std::vector<std::string> params = {class_name};
        if (!module.empty()) { sql += " AND module_name = ?"; params.push_back(module); }
        sql += " LIMIT 1";

        auto rows = query(db, sql, params);
        if (rows.empty()) {
            ojson out;
            out["success"] = true;
            out["uclass"] = nullptr;   // live miss shape: { uclass: null }
            std::cout << out.dump(2) << std::endl;
            return;
        }

        auto& r = rows[0];
        std::string cn = r.get("class_name");
        std::string mn = r.get("module_name");

        int src_line = r.get_int("source_line");
        std::string src_path = r.get("source_path");
        if (src_line == 0) src_line = lookup_class_source_line(cn);
        if (src_path.empty()) src_path = lookup_class_source_path(cn);

        ojson uclass;
        uclass["class_name"] = cn;
        uclass["module_name"] = mn;
        uclass["parent_class"] = r.get("parent_class");
        uclass["source_path"] = src_path;
        uclass["source_line"] = src_line;
        uclass["flags"] = r.get("flags");

        // Parent chain: iterative, cap 16.
        ojson chain = ojson::array();
        {
            std::string cur = r.get("parent_class");
            std::string last_appended;
            int guard = 0;
            while (!cur.empty() && guard < 16) {
                chain.push_back(cur);
                last_appended = cur;
                auto prow = query(db,
                    "SELECT parent_class FROM reflect_uclasses WHERE class_name = ? LIMIT 1", {cur});
                if (prow.empty()) break;            // engine class outside index → stop
                std::string next = prow[0].get("parent_class");
                if (next == last_appended) break;   // cycle guard
                cur = next;
                ++guard;
                if (cur.empty()) break;
            }
        }
        uclass["parent_chain"] = chain;

        // UPROPERTYs (module filter only when module passed).
        {
            std::string psql = "SELECT property_name, property_type, cpp_module, blueprint_visibility, specifiers "
                               "FROM reflect_uproperties WHERE owning_class = ?";
            std::vector<std::string> pp = {cn};
            if (!module.empty()) { psql += " AND cpp_module = ?"; pp.push_back(module); }
            psql += " ORDER BY property_name";
            auto props = query(db, psql, pp);
            ojson jprops = ojson::array();
            for (auto& p : props) {
                ojson o;
                o["property_name"] = p.get("property_name");
                o["property_type"] = p.get("property_type");
                o["cpp_module"] = p.get("cpp_module");
                o["blueprint_visibility"] = p.get("blueprint_visibility");
                o["specifiers"] = p.get("specifiers");
                jprops.push_back(o);
            }
            uclass["uproperties"] = jprops;
        }

        // UFUNCTIONs (module filter only when module passed). No source join here.
        {
            std::string fsql = "SELECT function_name, return_type, blueprint_callable, cpp_module, specifiers "
                               "FROM reflect_ufunctions WHERE owning_class = ?";
            std::vector<std::string> fp = {cn};
            if (!module.empty()) { fsql += " AND cpp_module = ?"; fp.push_back(module); }
            fsql += " ORDER BY function_name";
            auto funcs = query(db, fsql, fp);
            ojson jfuncs = ojson::array();
            for (auto& f : funcs) {
                ojson o;
                o["function_name"] = f.get("function_name");
                o["return_type"] = f.get("return_type");
                o["blueprint_callable"] = f.get_int("blueprint_callable") != 0;
                o["cpp_module"] = f.get("cpp_module");
                o["specifiers"] = f.get("specifiers");
                jfuncs.push_back(o);
            }
            uclass["ufunctions"] = jfuncs;
        }

        ojson out;
        out["success"] = true;
        out["uclass"] = uclass;
        std::cout << out.dump(2) << std::endl;
    }

    // --- cppreflect list_uproperties ---  (paginated, total_estimate)
    void list_uproperties(const Args& args) {
        if (!table_exists(db, "reflect_uproperties")) { emit_missing_table("reflect_uproperties"); return; }
        // Precedence: --class_name flag wins, else the positional arg (mirrors
        // get_uclass and the Python sibling: Args::opt then positional[0]).
        // The prior exe read ONLY opt("class_name"), so a positional class name
        // (`list_uproperties ALeviathanCharacterBase`) was dropped and the query
        // scanned the ENTIRE table (BUG 2).
        std::string class_name = args.opt("class_name");
        if (class_name.empty() && !args.positional.empty()) class_name = args.positional[0];
        bool bp_only = args.opt_bool("blueprint_visible_only", false);
        int limit = clamp_limit(args);

        uint32_t qh = compute_filter_hash({class_name, bp_only ? "1" : "0"});
        int32_t page = 0, cached_total = -1; bool has_cursor = false;
        if (!resolve_page(args, qh, page, cached_total, has_cursor)) return;

        std::string where = " WHERE 1=1";
        std::vector<std::string> binds;
        if (!class_name.empty()) { where += " AND owning_class = ?"; binds.push_back(class_name); }
        if (bp_only) where += " AND blueprint_visibility IS NOT NULL AND blueprint_visibility <> ''";

        std::string sql = "SELECT owning_class, property_name, property_type, cpp_module, "
                          "blueprint_visibility, specifiers FROM reflect_uproperties" + where +
                          " ORDER BY owning_class, property_name LIMIT ? OFFSET ?";
        std::vector<std::string> params = binds;
        params.push_back(std::to_string(limit));
        params.push_back(std::to_string(page * limit));
        auto rows = query(db, sql, params);

        ojson arr = ojson::array();
        for (auto& r : rows) {
            ojson o;
            o["owning_class"] = r.get("owning_class");
            o["property_name"] = r.get("property_name");
            o["property_type"] = r.get("property_type");
            o["cpp_module"] = r.get("cpp_module");
            o["blueprint_visibility"] = r.get("blueprint_visibility");
            o["specifiers"] = r.get("specifiers");
            arr.push_back(o);
        }

        ojson out;
        out["success"] = true;
        out["uproperties"] = arr;
        int64_t total = -1;
        if (!has_cursor) {
            total = scalar_count(db, "SELECT COUNT(*) FROM reflect_uproperties" + where, binds);
            out["total_estimate"] = total;
        }
        if ((int)rows.size() == limit)
            out["next_cursor"] = encode_cursor(qh, page + 1, has_cursor ? cached_total : (int32_t)total);
        std::cout << out.dump(2) << std::endl;
    }

    // --- cppreflect list_ufunctions ---  (paginated, total_estimate, source join)
    void list_ufunctions(const Args& args) {
        if (!table_exists(db, "reflect_ufunctions")) { emit_missing_table("reflect_ufunctions"); return; }
        // Precedence: --class_name flag wins, else the positional arg (mirrors
        // get_uclass and the Python sibling: Args::opt then positional[0]). See
        // list_uproperties for the BUG 2 root-cause note.
        std::string class_name = args.opt("class_name");
        if (class_name.empty() && !args.positional.empty()) class_name = args.positional[0];
        bool bp_only = args.opt_bool("blueprint_callable_only", false);
        int limit = clamp_limit(args);

        uint32_t qh = compute_filter_hash({class_name, bp_only ? "1" : "0"});
        int32_t page = 0, cached_total = -1; bool has_cursor = false;
        if (!resolve_page(args, qh, page, cached_total, has_cursor)) return;

        std::string where = " WHERE 1=1";
        std::vector<std::string> binds;
        if (!class_name.empty()) { where += " AND owning_class = ?"; binds.push_back(class_name); }
        if (bp_only) where += " AND blueprint_callable = 1";

        std::string sql = "SELECT owning_class, function_name, return_type, blueprint_callable, "
                          "cpp_module, specifiers, source_line FROM reflect_ufunctions" + where +
                          " ORDER BY owning_class, function_name LIMIT ? OFFSET ?";
        std::vector<std::string> params = binds;
        params.push_back(std::to_string(limit));
        params.push_back(std::to_string(page * limit));
        auto rows = query(db, sql, params);

        ojson arr = ojson::array();
        for (auto& r : rows) {
            int sl = r.get_int("source_line");
            if (sl == 0) sl = lookup_function_source_line(r.get("function_name"), r.get("owning_class"));
            ojson o;
            o["owning_class"] = r.get("owning_class");
            o["function_name"] = r.get("function_name");
            o["return_type"] = r.get("return_type");
            o["blueprint_callable"] = r.get_int("blueprint_callable") != 0;
            o["cpp_module"] = r.get("cpp_module");
            o["specifiers"] = r.get("specifiers");
            o["source_line"] = sl;
            arr.push_back(o);
        }

        ojson out;
        out["success"] = true;
        out["ufunctions"] = arr;
        int64_t total = -1;
        if (!has_cursor) {
            total = scalar_count(db, "SELECT COUNT(*) FROM reflect_ufunctions" + where, binds);
            out["total_estimate"] = total;
        }
        if ((int)rows.size() == limit)
            out["next_cursor"] = encode_cursor(qh, page + 1, has_cursor ? cached_total : (int32_t)total);
        std::cout << out.dump(2) << std::endl;
    }

    // --- cppreflect find_interface_impls ---  (NOT paginated)
    void find_interface_impls(const Args& args) {
        std::string iface = args.opt("interface_name");
        if (iface.empty() && !args.positional.empty()) iface = args.positional[0];
        if (iface.empty()) { emit_param_error("`interface_name` is required."); return; }
        if (!table_exists(db, "reflect_uinterface_impls")) { emit_missing_table("reflect_uinterface_impls"); return; }

        auto rows = query(db,
            "SELECT impl.implementing_class, impl.cpp_module, cls.source_path "
            "FROM reflect_uinterface_impls impl "
            "LEFT JOIN reflect_uclasses cls ON cls.class_name = impl.implementing_class "
            "AND cls.module_name = impl.cpp_module "
            "WHERE impl.interface_name = ? "
            "ORDER BY impl.cpp_module, impl.implementing_class", {iface});

        ojson arr = ojson::array();
        for (auto& r : rows) {
            ojson o;
            o["implementing_class"] = r.get("implementing_class");
            o["cpp_module"] = r.get("cpp_module");
            o["source_path"] = r.get("source_path");
            arr.push_back(o);
        }
        ojson out;
        out["success"] = true;
        out["interface_name"] = iface;
        out["implementers"] = arr;
        std::cout << out.dump(2) << std::endl;
    }

    // Build the token universe (token,count) for known_tokens / list_class_specifiers.
    std::vector<std::pair<std::string, int>> build_token_universe() {
        std::map<std::string, int> counts;
        auto rows = query(db,
            "SELECT flags FROM reflect_uclasses WHERE flags IS NOT NULL AND flags <> ''");
        for (auto& r : rows) {
            std::string flags = r.get("flags");
            std::set<std::string> seen_this_row;
            std::string token;
            std::istringstream ss(flags);
            // split on ':' culling empty, trimming each
            size_t start = 0;
            while (start <= flags.size()) {
                size_t pos = flags.find(':', start);
                std::string tok = (pos == std::string::npos)
                    ? flags.substr(start) : flags.substr(start, pos - start);
                tok = trim_copy(tok);
                if (!tok.empty()) seen_this_row.insert(tok);
                if (pos == std::string::npos) break;
                start = pos + 1;
            }
            for (const auto& t : seen_this_row) counts[t]++;
        }
        std::vector<std::pair<std::string, int>> out(counts.begin(), counts.end());
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            if (a.second != b.second) return a.second > b.second;  // count desc
            return a.first < b.first;                              // token asc
        });
        return out;
    }

    // --- cppreflect find_class_specifier ---  (paginated, NO total)
    void find_class_specifier(const Args& args) {
        std::string spec = args.opt("specifier_name");
        if (spec.empty() && !args.positional.empty()) spec = args.positional[0];
        if (spec.empty()) { emit_param_error("`specifier_name` is required."); return; }
        if (!table_exists(db, "reflect_uclasses")) { emit_missing_table("reflect_uclasses"); return; }

        int limit = clamp_limit(args);
        uint32_t qh = compute_filter_hash({spec});
        int32_t page = 0, cached_total = -1; bool has_cursor = false;
        if (!resolve_page(args, qh, page, cached_total, has_cursor)) return;

        std::string spec_lower = to_lower_copy(spec);

        // DROPPED tokens: no SQL, immediate note + known_tokens.
        if (spec_lower == "minimalapi" || spec_lower == "notblueprintable") {
            auto universe = build_token_universe();
            ojson known = ojson::array();
            for (auto& kv : universe) {
                ojson o; o["token"] = kv.first; o["count"] = kv.second; known.push_back(o);
            }
            ojson out;
            out["success"] = true;
            out["specifier_name"] = spec;
            out["uclasses"] = ojson::array();
            out["token_stored"] = false;
            out["note"] = "\"" + spec + "\" is a C++ UCLASS specifier that UHT does not store in the "
                          "metadata-key vocabulary (the `flags` column), so it can never match. Call "
                          "list_class_specifiers to see the tokens that are queryable.";
            out["known_tokens"] = known;
            std::cout << out.dump(2) << std::endl;
            return;
        }

        // ALIAS.
        std::string effective = spec;
        if (spec_lower == "blueprintable") effective = "IsBlueprintBase";

        std::string sql =
            "SELECT class_name, module_name, parent_class, source_path, flags FROM reflect_uclasses "
            "WHERE flags = ? COLLATE NOCASE OR flags LIKE ? OR flags LIKE ? OR flags LIKE ? "
            "ORDER BY module_name, class_name LIMIT ? OFFSET ?";
        std::vector<std::string> params = {
            effective,
            effective + ":%",
            "%:" + effective + ":%",
            "%:" + effective,
            std::to_string(limit),
            std::to_string(page * limit)
        };
        auto rows = query(db, sql, params);

        ojson arr = ojson::array();
        for (auto& r : rows) {
            ojson o;
            o["class_name"] = r.get("class_name");
            o["module_name"] = r.get("module_name");
            o["parent_class"] = r.get("parent_class");
            o["source_path"] = r.get("source_path");
            o["flags"] = r.get("flags");
            arr.push_back(o);
        }

        ojson out;
        out["success"] = true;
        out["specifier_name"] = spec;
        if (effective != spec) out["effective_token"] = effective;
        out["uclasses"] = arr;
        if (rows.empty() && !has_cursor) {
            auto universe = build_token_universe();
            ojson known = ojson::array();
            for (auto& kv : universe) {
                ojson o; o["token"] = kv.first; o["count"] = kv.second; known.push_back(o);
            }
            out["known_tokens"] = known;
        }
        if ((int)rows.size() == limit)
            out["next_cursor"] = encode_cursor(qh, page + 1, -1);  // tc always -1
        std::cout << out.dump(2) << std::endl;
    }

    // --- cppreflect list_class_specifiers ---  (NOT paginated)
    void list_class_specifiers(const Args&) {
        if (!table_exists(db, "reflect_uclasses")) { emit_missing_table("reflect_uclasses"); return; }
        auto universe = build_token_universe();
        ojson arr = ojson::array();
        for (auto& kv : universe) {
            ojson o; o["token"] = kv.first; o["count"] = kv.second; arr.push_back(o);
        }
        ojson out;
        out["success"] = true;
        out["specifiers"] = arr;
        out["distinct_count"] = (int)universe.size();
        std::cout << out.dump(2) << std::endl;
    }

    // ========================================================
    // NAMESPACE: network (4)
    // ========================================================

    // --- network list_replicated_classes ---  (paginated, total_estimate)
    void list_replicated_classes(const Args& args) {
        if (!table_exists(db, "reflect_replicated_properties")) {
            emit_missing_table("reflect_replicated_properties"); return;
        }
        int limit = clamp_limit(args);
        uint32_t qh = compute_filter_hash({});   // empty parts → 0
        int32_t page = 0, cached_total = -1; bool has_cursor = false;
        if (!resolve_page(args, qh, page, cached_total, has_cursor)) return;

        auto rows = query(db,
            "SELECT owning_class, cpp_module, COUNT(*) AS prop_count "
            "FROM reflect_replicated_properties GROUP BY owning_class, cpp_module "
            "ORDER BY cpp_module, owning_class LIMIT ? OFFSET ?",
            {std::to_string(limit), std::to_string(page * limit)});

        ojson arr = ojson::array();
        for (auto& r : rows) {
            ojson o;
            o["owning_class"] = r.get("owning_class");
            o["cpp_module"] = r.get("cpp_module");
            o["replicated_property_count"] = r.get_int("prop_count");
            arr.push_back(o);
        }

        ojson out;
        out["success"] = true;
        out["classes"] = arr;
        int64_t total = -1;
        if (!has_cursor) {
            total = scalar_count(db,
                "SELECT COUNT(*) FROM (SELECT 1 FROM reflect_replicated_properties "
                "GROUP BY owning_class, cpp_module)", {});
            out["total_estimate"] = total;
        }
        if ((int)rows.size() == limit)
            out["next_cursor"] = encode_cursor(qh, page + 1, has_cursor ? cached_total : (int32_t)total);
        std::cout << out.dump(2) << std::endl;
    }

    // --- network list_rpc_functions ---  (paginated, NO total, post-fetch filter)
    void list_rpc_functions(const Args& args) {
        if (!table_exists(db, "reflect_ufunctions")) { emit_missing_table("reflect_ufunctions"); return; }
        std::string class_name = args.opt("class_name");
        std::string rpc_kind = args.opt("rpc_kind");
        int limit = clamp_limit(args);

        uint32_t qh = compute_filter_hash({class_name, rpc_kind});
        int32_t page = 0, cached_total = -1; bool has_cursor = false;
        if (!resolve_page(args, qh, page, cached_total, has_cursor)) return;

        std::string where = " WHERE (specifiers LIKE '%Server%' OR specifiers LIKE '%Client%' "
                            "OR specifiers LIKE '%NetMulticast%')";
        std::vector<std::string> binds;
        if (!class_name.empty()) { where += " AND owning_class = ?"; binds.push_back(class_name); }

        std::string sql = "SELECT owning_class, function_name, cpp_module, blueprint_callable, specifiers "
                          "FROM reflect_ufunctions" + where +
                          " ORDER BY cpp_module, owning_class, function_name LIMIT ? OFFSET ?";
        std::vector<std::string> params = binds;
        params.push_back(std::to_string(limit));
        params.push_back(std::to_string(page * limit));
        auto rows = query(db, sql, params);

        std::string rpc_kind_lower = to_lower_copy(rpc_kind);
        ojson arr = ojson::array();
        int emitted = 0;
        for (auto& r : rows) {
            std::string specs = r.get("specifiers");
            std::string kind;
            if (specs.find("Server") != std::string::npos) kind = "Server";
            else if (specs.find("Client") != std::string::npos) kind = "Client";
            else if (specs.find("NetMulticast") != std::string::npos) kind = "Multicast";
            else kind = "";

            if (!rpc_kind.empty() && to_lower_copy(kind) != rpc_kind_lower) continue;

            ojson o;
            o["owning_class"] = r.get("owning_class");
            o["function_name"] = r.get("function_name");
            o["cpp_module"] = r.get("cpp_module");
            o["rpc_kind"] = kind;
            o["specifiers"] = specs;
            o["blueprint_callable"] = r.get_int("blueprint_callable") != 0;
            arr.push_back(o);
            ++emitted;
        }

        ojson out;
        out["success"] = true;
        out["rpcs"] = arr;
        if (emitted == limit)
            out["next_cursor"] = encode_cursor(qh, page + 1, -1);
        std::cout << out.dump(2) << std::endl;
    }

    // --- network list_onrep_handlers ---  (paginated, NO total)
    void list_onrep_handlers(const Args& args) {
        if (!table_exists(db, "reflect_ufunctions")) { emit_missing_table("reflect_ufunctions"); return; }
        std::string class_name = args.opt("class_name");
        int limit = clamp_limit(args);

        uint32_t qh = compute_filter_hash({class_name});
        int32_t page = 0, cached_total = -1; bool has_cursor = false;
        if (!resolve_page(args, qh, page, cached_total, has_cursor)) return;

        std::string where = " WHERE function_name LIKE 'OnRep_%'";
        std::vector<std::string> binds;
        if (!class_name.empty()) { where += " AND owning_class = ?"; binds.push_back(class_name); }

        std::string sql = "SELECT owning_class, function_name, cpp_module FROM reflect_ufunctions" + where +
                          " ORDER BY cpp_module, owning_class, function_name LIMIT ? OFFSET ?";
        std::vector<std::string> params = binds;
        params.push_back(std::to_string(limit));
        params.push_back(std::to_string(page * limit));
        auto rows = query(db, sql, params);

        ojson arr = ojson::array();
        for (auto& r : rows) {
            ojson o;
            o["owning_class"] = r.get("owning_class");
            o["function_name"] = r.get("function_name");
            o["cpp_module"] = r.get("cpp_module");
            arr.push_back(o);
        }

        ojson out;
        out["success"] = true;
        out["handlers"] = arr;
        if ((int)rows.size() == limit)
            out["next_cursor"] = encode_cursor(qh, page + 1, -1);
        std::cout << out.dump(2) << std::endl;
    }

    // --- network audit_unbalanced_onreps ---  (paginated, NO total)
    void audit_unbalanced_onreps(const Args& args) {
        if (!table_exists(db, "reflect_replicated_properties")) {
            emit_missing_table("reflect_replicated_properties"); return;
        }
        int limit = clamp_limit(args);
        uint32_t qh = compute_filter_hash({});
        int32_t page = 0, cached_total = -1; bool has_cursor = false;
        if (!resolve_page(args, qh, page, cached_total, has_cursor)) return;

        auto rows = query(db,
            "SELECT rp.owning_class, rp.property_name, rp.cpp_module, rp.rep_notify_func "
            "FROM reflect_replicated_properties rp "
            "LEFT JOIN reflect_ufunctions uf ON uf.owning_class = rp.owning_class "
            "AND uf.function_name = rp.rep_notify_func AND uf.cpp_module = rp.cpp_module "
            "WHERE rp.rep_kind = 'ReplicatedUsing' "
            "AND rp.rep_notify_func IS NOT NULL AND rp.rep_notify_func <> '' "
            "AND uf.function_name IS NULL "
            "ORDER BY rp.cpp_module, rp.owning_class, rp.property_name LIMIT ? OFFSET ?",
            {std::to_string(limit), std::to_string(page * limit)});

        ojson arr = ojson::array();
        for (auto& r : rows) {
            ojson o;
            o["owning_class"] = r.get("owning_class");
            o["property_name"] = r.get("property_name");
            o["cpp_module"] = r.get("cpp_module");
            o["missing_function"] = r.get("rep_notify_func");
            o["violation"] = "UPROPERTY(ReplicatedUsing) references an OnRep_ UFUNCTION that does not "
                             "exist on this class.";
            arr.push_back(o);
        }

        ojson out;
        out["success"] = true;
        out["violations"] = arr;
        if ((int)rows.size() == limit)
            out["next_cursor"] = encode_cursor(qh, page + 1, -1);
        std::cout << out.dump(2) << std::endl;
    }

    // ========================================================
    // NAMESPACE: decision (5)
    // ========================================================

    // Shared RowToJson for decision_records (8 cols → 8 keys, ordered).
    ojson decision_row_to_json(const Row& r) {
        ojson o;
        o["decision_id"] = r.get("decision_id");
        o["title"] = r.get("title");
        o["status"] = r.get("status");
        o["source_path"] = r.get("source_path");
        o["source_line"] = r.get_int("source_line");
        // confidence is a genuine REAL (float32 upstream) -> %.17g via sentinel.
        o["confidence"] = flt_sentinel(r.get_double("confidence"));
        o["rationale"] = r.get("rationale");
        o["source_mtime"] = r.get_int64("source_mtime");
        return o;
    }

    static const char* decision_columns() {
        return "decision_id, title, status, source_path, source_line, confidence, rationale, source_mtime";
    }

    // 3-arg decision filter hash form (path_filter, min_confidence/double, status-or-marker).
    uint32_t decision_filter_hash(const std::string& path_filter, double conf_or_age, const std::string& status) {
        uint32_t h = ue_str_crc32(path_filter);
        h = ue_hash_combine(h, ue_hash_double(conf_or_age));
        h = ue_hash_combine(h, ue_str_crc32(status));
        return h;
    }

    // --- decision list_decisions ---  (paginated, total_estimate, default conf 0.6)
    void list_decisions(const Args& args) {
        if (!table_exists(db, "decision_records")) { emit_missing_table("decision_records"); return; }
        std::string path_filter = args.opt("path_filter");
        double min_conf = 0.6;
        {
            auto it = args.options.find("min_confidence");
            if (it != args.options.end() && !it->second.empty()) {
                try { min_conf = std::stod(it->second); } catch (...) {}
            }
        }
        std::string status = args.opt("status");
        int limit = clamp_limit(args);

        uint32_t qh = decision_filter_hash(path_filter, min_conf, status);
        int32_t page = 0, cached_total = -1; bool has_cursor = false;
        if (!resolve_page(args, qh, page, cached_total, has_cursor)) return;

        std::string where = " WHERE confidence >= ?";
        std::vector<std::string> binds;
        // bind min_confidence as text; SQLite coerces for the numeric comparison.
        {
            std::ostringstream ss; ss << min_conf; binds.push_back(ss.str());
        }
        if (!path_filter.empty()) { where += " AND source_path LIKE ?"; binds.push_back("%" + path_filter + "%"); }
        if (!status.empty()) { where += " AND status = ?"; binds.push_back(status); }

        std::string sql = std::string("SELECT ") + decision_columns() + " FROM decision_records" + where +
                          " ORDER BY source_path, source_line LIMIT ? OFFSET ?";
        std::vector<std::string> params = binds;
        params.push_back(std::to_string(limit));
        params.push_back(std::to_string(page * limit));
        auto rows = query(db, sql, params);

        ojson arr = ojson::array();
        for (auto& r : rows) arr.push_back(decision_row_to_json(r));

        ojson out;
        out["success"] = true;
        out["decisions"] = arr;
        int64_t total = -1;
        if (!has_cursor) {
            total = scalar_count(db, "SELECT COUNT(*) FROM decision_records" + where, binds);
            out["total_estimate"] = total;
        }
        if ((int)rows.size() == limit)
            out["next_cursor"] = encode_cursor(qh, page + 1, has_cursor ? cached_total : (int32_t)total);
        emit_ri(out);   // %.17g float-sentinel finalize (confidence)
    }

    // --- decision get_decision ---  (NOT paginated)
    void get_decision(const Args& args) {
        std::string did = args.opt("decision_id");
        if (did.empty() && !args.positional.empty()) did = args.positional[0];
        if (did.empty()) { emit_param_error("`decision_id` is required."); return; }
        if (!table_exists(db, "decision_records")) { emit_missing_table("decision_records"); return; }

        auto rows = query(db,
            std::string("SELECT ") + decision_columns() + " FROM decision_records WHERE decision_id = ? LIMIT 1",
            {did});
        ojson out;
        out["success"] = true;
        out["decision"] = rows.empty() ? ojson(nullptr) : decision_row_to_json(rows[0]);
        emit_ri(out);   // %.17g float-sentinel finalize (confidence)
    }

    // --- decision list_stale ---  (paginated, NO total, cutoff_unix)
    void list_stale(const Args& args) {
        if (!table_exists(db, "decision_records")) { emit_missing_table("decision_records"); return; }
        // max_age_days required + positive.
        auto it = args.options.find("max_age_days");
        std::string raw = (it != args.options.end()) ? it->second : "";
        if (raw.empty() && !args.positional.empty()) raw = args.positional[0];
        int max_age_days = 0;
        try { max_age_days = std::stoi(raw); } catch (...) { max_age_days = 0; }
        if (max_age_days <= 0) { emit_param_error("`max_age_days` must be positive."); return; }

        std::string path_filter = args.opt("path_filter");
        int limit = clamp_limit(args);

        int64_t now_unix = (int64_t)std::time(nullptr);
        int64_t cutoff_unix = now_unix - (int64_t)max_age_days * 86400;

        uint32_t qh = decision_filter_hash(path_filter, (double)max_age_days, "__stale__");
        int32_t page = 0, cached_total = -1; bool has_cursor = false;
        if (!resolve_page(args, qh, page, cached_total, has_cursor)) return;

        std::string where = " WHERE source_mtime > 0 AND source_mtime < ?";
        std::vector<std::string> binds = {std::to_string(cutoff_unix)};
        if (!path_filter.empty()) { where += " AND source_path LIKE ?"; binds.push_back("%" + path_filter + "%"); }

        std::string sql = std::string("SELECT ") + decision_columns() + " FROM decision_records" + where +
                          " ORDER BY source_mtime ASC LIMIT ? OFFSET ?";
        std::vector<std::string> params = binds;
        params.push_back(std::to_string(limit));
        params.push_back(std::to_string(page * limit));
        auto rows = query(db, sql, params);

        ojson arr = ojson::array();
        for (auto& r : rows) arr.push_back(decision_row_to_json(r));

        ojson out;
        out["success"] = true;
        out["stale_decisions"] = arr;
        out["cutoff_unix"] = cutoff_unix;
        if ((int)rows.size() == limit)
            out["next_cursor"] = encode_cursor(qh, page + 1, -1);
        emit_ri(out);   // %.17g float-sentinel finalize (confidence)
    }

    // --- decision find_supersession_chain ---  (NOT paginated, BFS)
    void find_supersession_chain(const Args& args) {
        std::string start = args.opt("decision_id");
        if (start.empty() && !args.positional.empty()) start = args.positional[0];
        if (start.empty()) { emit_param_error("`decision_id` is required."); return; }
        if (!table_exists(db, "decision_supersedes")) { emit_missing_table("decision_supersedes"); return; }

        int depth = args.opt_int("depth", 10);
        if (depth < 1) depth = 1;
        if (depth > 50) depth = 50;

        std::set<std::string> visited;
        visited.insert(start);
        std::vector<std::string> frontier = {start};
        ojson chain = ojson::array();
        int cur_depth = 0;
        bool truncated = false;

        while (!frontier.empty()) {
            if (cur_depth == depth) { truncated = true; break; }
            std::vector<std::string> next;
            for (const auto& from : frontier) {
                auto rows = query(db,
                    "SELECT to_decision_id FROM decision_supersedes WHERE from_decision_id = ?", {from});
                for (auto& r : rows) {
                    std::string to = r.get("to_decision_id");
                    if (visited.count(to)) continue;
                    visited.insert(to);
                    ojson edge;
                    edge["from"] = from;
                    edge["to"] = to;
                    edge["depth"] = cur_depth + 1;
                    chain.push_back(edge);
                    next.push_back(to);
                }
            }
            frontier = std::move(next);
            ++cur_depth;
        }

        ojson out;
        out["success"] = true;
        out["start"] = start;
        out["chain"] = chain;
        out["truncated"] = truncated;
        std::cout << out.dump(2) << std::endl;
    }

    // --- decision find_referent_decisions ---  (NOT paginated)
    void find_referent_decisions(const Args& args) {
        std::string did = args.opt("decision_id");
        if (did.empty() && !args.positional.empty()) did = args.positional[0];
        if (did.empty()) { emit_param_error("`decision_id` is required."); return; }
        if (!table_exists(db, "decision_supersedes")) { emit_missing_table("decision_supersedes"); return; }

        auto rows = query(db,
            "SELECT r.decision_id, r.title, r.status, r.source_path, r.source_line, "
            "r.confidence, r.rationale, r.source_mtime "
            "FROM decision_supersedes s JOIN decision_records r ON r.decision_id = s.from_decision_id "
            "WHERE s.to_decision_id = ? ORDER BY r.source_path, r.source_line", {did});

        ojson arr = ojson::array();
        for (auto& r : rows) arr.push_back(decision_row_to_json(r));

        ojson out;
        out["success"] = true;
        out["decision_id"] = did;
        out["referent_decisions"] = arr;
        emit_ri(out);   // %.17g float-sentinel finalize (confidence)
    }

    // ========================================================
    // NAMESPACE: risk (5)
    // ========================================================

    // CanonPath: backslash → forward-slash ONLY (no lowercasing).
    static std::string canon_path(const std::string& p) {
        std::string out = p;
        std::replace(out.begin(), out.end(), '\\', '/');
        return out;
    }

    // --- risk get_hotspot_score ---  (NOT paginated)
    void get_hotspot_score(const Args& args) {
        std::string fp = args.opt("file_path");
        if (fp.empty() && !args.positional.empty()) fp = args.positional[0];
        if (fp.empty()) { emit_param_error("`file_path` is required."); return; }
        if (!table_exists(db, "risk_hotspot_scores")) { emit_missing_table("risk_hotspot_scores"); return; }

        std::string cp = canon_path(fp);
        auto rows = query(db,
            "SELECT file_path, churn, complexity_proxy, normalised_churn, normalised_complexity, score "
            "FROM risk_hotspot_scores WHERE file_path = ? LIMIT 1", {cp});

        ojson out;
        out["success"] = true;
        if (rows.empty()) {
            out["hotspot"] = nullptr;
        } else {
            auto& r = rows[0];
            ojson h;
            h["file_path"] = r.get("file_path");
            h["churn"] = r.get_int("churn");
            h["complexity_proxy"] = r.get_int("complexity_proxy");
            // normalised_churn / normalised_complexity / score are genuine REAL
            // (float32 upstream) -> %.17g via sentinel.
            h["normalised_churn"] = flt_sentinel(r.get_double("normalised_churn"));
            h["normalised_complexity"] = flt_sentinel(r.get_double("normalised_complexity"));
            h["score"] = flt_sentinel(r.get_double("score"));
            out["hotspot"] = h;
        }
        emit_ri(out);   // %.17g float-sentinel finalize (score/normalised_*)
    }

    // --- risk get_cochange_pairs ---  (paginated, total_estimate)
    void get_cochange_pairs(const Args& args) {
        std::string fp = args.opt("file_path");
        if (fp.empty() && !args.positional.empty()) fp = args.positional[0];
        if (fp.empty()) { emit_param_error("`file_path` is required."); return; }
        if (!table_exists(db, "git_cochange_pairs")) { emit_missing_table("git_cochange_pairs"); return; }

        std::string cp = canon_path(fp);
        int limit = clamp_limit(args);
        uint32_t qh = compute_filter_hash({cp});
        int32_t page = 0, cached_total = -1; bool has_cursor = false;
        if (!resolve_page(args, qh, page, cached_total, has_cursor)) return;

        auto rows = query(db,
            "SELECT repo_tag, CASE WHEN file_a = ? THEN file_b ELSE file_a END AS partner, count "
            "FROM git_cochange_pairs WHERE file_a = ? OR file_b = ? "
            "ORDER BY count DESC, partner ASC LIMIT ? OFFSET ?",
            {cp, cp, cp, std::to_string(limit), std::to_string(page * limit)});

        ojson arr = ojson::array();
        for (auto& r : rows) {
            ojson o;
            o["repo_tag"] = r.get("repo_tag");
            o["partner"] = r.get("partner");
            o["count"] = r.get_int("count");
            arr.push_back(o);
        }

        ojson out;
        out["success"] = true;
        out["file_path"] = cp;
        out["partners"] = arr;
        int64_t total = -1;
        if (!has_cursor) {
            total = scalar_count(db,
                "SELECT COUNT(*) FROM git_cochange_pairs WHERE file_a = ? OR file_b = ?", {cp, cp});
            out["total_estimate"] = total;
        }
        if ((int)rows.size() == limit)
            out["next_cursor"] = encode_cursor(qh, page + 1, has_cursor ? cached_total : (int32_t)total);
        std::cout << out.dump(2) << std::endl;
    }

    // --- risk get_file_churn ---  (NOT paginated)
    void get_file_churn(const Args& args) {
        std::string fp = args.opt("file_path");
        if (fp.empty() && !args.positional.empty()) fp = args.positional[0];
        if (fp.empty()) { emit_param_error("`file_path` is required."); return; }
        if (!table_exists(db, "git_file_churn")) { emit_missing_table("git_file_churn"); return; }

        std::string cp = canon_path(fp);
        std::string repo_tag = args.opt("repo_tag");

        std::string sql = "SELECT repo_tag, commit_count, last_touched FROM git_file_churn WHERE file_path = ?";
        std::vector<std::string> binds = {cp};
        if (!repo_tag.empty()) { sql += " AND repo_tag = ?"; binds.push_back(repo_tag); }
        auto rows = query(db, sql, binds);

        ojson arr = ojson::array();
        for (auto& r : rows) {
            ojson o;
            o["repo_tag"] = r.get("repo_tag");
            o["commit_count"] = r.get_int("commit_count");
            o["last_touched_unix"] = r.get_int64("last_touched");
            arr.push_back(o);
        }

        ojson out;
        out["success"] = true;
        out["file_path"] = cp;
        out["churn_by_repo"] = arr;
        std::cout << out.dump(2) << std::endl;
    }

    // --- risk get_release_window_hotspots ---  (paginated, NO total, since_unix)
    void get_release_window_hotspots(const Args& args) {
        if (!table_exists(db, "risk_hotspot_scores")) { emit_missing_table("risk_hotspot_scores"); return; }
        int64_t now_unix = (int64_t)std::time(nullptr);
        // Spec §risk.get_release_window_hotspots default window = now - 30 days.
        // int64 arithmetic throughout to match the Python sibling exactly.
        int64_t since = now_unix - (int64_t)30 * 86400;
        {
            auto it = args.options.find("since_unix");
            if (it != args.options.end() && !it->second.empty()) {
                try { since = std::stoll(it->second); } catch (...) {}
            }
        }
        int limit = clamp_limit(args);

        // FilterHash part: stringified int value used.
        uint32_t qh = compute_filter_hash({std::to_string(since)});
        int32_t page = 0, cached_total = -1; bool has_cursor = false;
        if (!resolve_page(args, qh, page, cached_total, has_cursor)) return;

        // BUG 3: `since` MUST bind as INTEGER. The WHERE-side is a scalar
        // subquery aggregate (MAX) with NO column affinity, so SQLite will not
        // coerce a TEXT-bound param to numeric — it would compare storage
        // classes (INTEGER always < TEXT) and match nothing. The live adapter
        // (FRiskQueryAdapter::HandleGetReleaseWindowHotspots) and the Python
        // sibling both bind an int64 here; query_typed mirrors that.
        auto rows = query_typed(db,
            "SELECT h.file_path, h.score, h.churn, h.complexity_proxy, "
            "(SELECT MAX(c.last_touched) FROM git_file_churn c WHERE c.file_path = h.file_path) AS last_touched "
            "FROM risk_hotspot_scores h "
            "WHERE (SELECT MAX(c2.last_touched) FROM git_file_churn c2 WHERE c2.file_path = h.file_path) >= ? "
            "ORDER BY h.score DESC LIMIT ? OFFSET ?",
            {Bind::Integer(since), Bind::Integer(limit), Bind::Integer((int64_t)page * limit)});

        ojson arr = ojson::array();
        for (auto& r : rows) {
            ojson o;
            o["file_path"] = r.get("file_path");
            // score is a genuine REAL (float32 upstream) -> %.17g via sentinel.
            o["score"] = flt_sentinel(r.get_double("score"));
            o["churn"] = r.get_int("churn");
            o["complexity_proxy"] = r.get_int("complexity_proxy");
            o["last_touched_unix"] = r.get_int64("last_touched");
            arr.push_back(o);
        }

        ojson out;
        out["success"] = true;
        out["since_unix"] = since;
        out["hotspots"] = arr;
        if ((int)rows.size() == limit)
            out["next_cursor"] = encode_cursor(qh, page + 1, -1);
        emit_ri(out);   // %.17g float-sentinel finalize (score)
    }

    // --- risk list_conditional_gates ---  (paginated, NO total)
    void list_conditional_gates(const Args& args) {
        if (!table_exists(db, "reflect_conditional_gates")) {
            emit_missing_table("reflect_conditional_gates"); return;
        }
        std::string macro_filter = args.opt("macro_filter");
        std::string path_filter = args.opt("path_filter");
        int limit = clamp_limit(args);

        uint32_t qh = compute_filter_hash({macro_filter, path_filter});
        int32_t page = 0, cached_total = -1; bool has_cursor = false;
        if (!resolve_page(args, qh, page, cached_total, has_cursor)) return;

        std::string where = " WHERE 1=1";
        std::vector<std::string> binds;
        if (!macro_filter.empty()) { where += " AND macro_name LIKE ?"; binds.push_back("%" + macro_filter + "%"); }
        if (!path_filter.empty()) { where += " AND source_path LIKE ?"; binds.push_back("%" + path_filter + "%"); }

        std::string sql = "SELECT id, source_path, source_line, macro_name, gate_kind, context_snippet "
                          "FROM reflect_conditional_gates" + where +
                          " ORDER BY source_path, source_line LIMIT ? OFFSET ?";
        std::vector<std::string> params = binds;
        params.push_back(std::to_string(limit));
        params.push_back(std::to_string(page * limit));
        auto rows = query(db, sql, params);

        ojson arr = ojson::array();
        for (auto& r : rows) {
            ojson o;
            o["id"] = r.get_int("id");
            o["source_path"] = r.get("source_path");
            o["source_line"] = r.get_int("source_line");
            o["macro_name"] = r.get("macro_name");
            o["gate_kind"] = r.get("gate_kind");
            o["context_snippet"] = r.get("context_snippet");
            arr.push_back(o);
        }

        ojson out;
        out["success"] = true;
        out["gates"] = arr;
        if ((int)rows.size() == limit)
            out["next_cursor"] = encode_cursor(qh, page + 1, -1);
        std::cout << out.dump(2) << std::endl;
    }
};

// ============================================================
// DB path resolution
// ============================================================

// Directory containing the running executable, or empty on failure.
static fs::path get_exe_dir() {
    fs::path exe_path;
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH)
        exe_path = buf;
#endif
    return exe_path.empty() ? fs::path() : exe_path.parent_path();
}

static std::string resolve_db_dir() {
    // Default: ../../Saved/ relative to exe location
    // (exe at Plugins/Monolith/Tools/MonolithQuery/ or Plugins/Monolith/Binaries/)
    auto exe_dir = get_exe_dir();
    if (exe_dir.empty()) {
        // Fallback: try current directory
        return ".";
    }

    // Try ../../Saved/ (from Tools/MonolithQuery/)
    auto saved1 = exe_dir / ".." / ".." / "Saved";
    if (fs::exists(saved1)) return fs::canonical(saved1).string();

    // Try ../Saved/ (from Binaries/)
    auto saved2 = exe_dir / ".." / "Saved";
    if (fs::exists(saved2)) return fs::canonical(saved2).string();

    // Try ./Saved/ (from plugin root)
    auto saved3 = exe_dir / "Saved";
    if (fs::exists(saved3)) return fs::canonical(saved3).string();

    return exe_dir.string();
}

// Resolve the Monolith plugin root (the dir containing Monolith.uplugin) by
// probing the same exe-relative layouts resolve_db_dir() uses. Returns empty
// if not found.
//   exe at Plugins/Monolith/Tools/MonolithQuery/ -> root is ../../
//   exe at Plugins/Monolith/Binaries/            -> root is ../
//   exe at Plugins/Monolith/ (plugin root)       -> root is ./
static fs::path resolve_plugin_root() {
    auto exe_dir = get_exe_dir();
    if (exe_dir.empty())
        return fs::path();

    const fs::path candidates[] = {
        exe_dir / ".." / "..",   // Tools/MonolithQuery/
        exe_dir / "..",          // Binaries/
        exe_dir                  // plugin root
    };
    for (const auto& c : candidates) {
        if (fs::exists(c / "Monolith.uplugin"))
            return fs::canonical(c);
    }
    return fs::path();
}

// Read "VersionName" from Monolith.uplugin via a plain string scan (no JSON
// dependency for this single field). Returns empty on any failure — the guide
// is still served; the version line is simply omitted.
static std::string parse_uplugin_version(const fs::path& plugin_root) {
    if (plugin_root.empty())
        return "";

    std::ifstream f((plugin_root / "Monolith.uplugin").string());
    if (!f.is_open())
        return "";

    std::stringstream ss;
    ss << f.rdbuf();
    std::string contents = ss.str();

    // Find "VersionName", then the value inside the next pair of quotes after
    // the following colon. Tolerant of arbitrary whitespace.
    auto key = contents.find("\"VersionName\"");
    if (key == std::string::npos)
        return "";

    auto colon = contents.find(':', key);
    if (colon == std::string::npos)
        return "";

    auto open_quote = contents.find('"', colon);
    if (open_quote == std::string::npos)
        return "";

    auto close_quote = contents.find('"', open_quote + 1);
    if (close_quote == std::string::npos)
        return "";

    return contents.substr(open_quote + 1, close_quote - open_quote - 1);
}

// ============================================================
// Monolith namespace — offline guide server
// ============================================================
//
// Mirrors the in-editor FMonolithGuideTool but WITHOUT the live registry
// overlay (offline has no registry — it just serves the markdown). The H2
// section split mirrors FMonolithGuideTool::SplitSections: a line is a section
// header iff it starts with "## " (after trimming) but NOT "### "; the key is
// the trimmed, lowercased remainder. Blank lines inside bodies are preserved;
// trailing whitespace per body is trimmed. Content before the first H2 (the H1
// title + intro) is intentionally dropped.

// Canonical section order — must match GetCanonicalSectionOrder() in
// MonolithGuideTool.cpp so offline and in-editor agree on index ordering.
static const std::vector<std::string>& get_canonical_section_order() {
    static const std::vector<std::string> order = {
        "onboarding", "recipes", "decisions", "errors", "skills_map", "gotchas"
    };
    return order;
}

static std::string join_canonical_section_names() {
    std::string out;
    const auto& order = get_canonical_section_order();
    for (size_t i = 0; i < order.size(); ++i) {
        if (i > 0) out += ", ";
        out += order[i];
    }
    return out;
}

// Trim leading whitespace.
static std::string ltrim_copy(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    return s.substr(i);
}

// Trim trailing whitespace (mirrors FString::TrimEnd for body cleanup).
static std::string rtrim_copy(const std::string& s) {
    size_t end = s.size();
    while (end > 0 && std::isspace((unsigned char)s[end - 1])) --end;
    return s.substr(0, end);
}

static std::string trim_copy(const std::string& s) {
    return rtrim_copy(ltrim_copy(s));
}

static std::string to_lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// Split markdown into H2-keyed sections, preserving body order in `ordered`.
static void split_sections(const std::string& markdown,
                           std::map<std::string, std::string>& out_sections,
                           std::vector<std::string>& out_ordered) {
    out_sections.clear();
    out_ordered.clear();

    std::string current_name;
    std::string current_body;
    bool in_section = false;

    auto flush_current = [&]() {
        if (!current_name.empty()) {
            out_sections[current_name] = rtrim_copy(current_body);
            out_ordered.push_back(current_name);
        }
    };

    std::istringstream iss(markdown);
    std::string line;
    while (std::getline(iss, line)) {
        // Strip a trailing CR so CRLF files split cleanly.
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        std::string trimmed = trim_copy(line);
        // H2 == "## " but NOT "### " (H3) and NOT "#" (H1 title).
        if (trimmed.rfind("## ", 0) == 0 && trimmed.rfind("### ", 0) != 0) {
            flush_current();
            current_name = to_lower_copy(trim_copy(trimmed.substr(3)));
            current_body.clear();
            in_section = true;
            continue;
        }

        if (in_section) {
            current_body += line;
            current_body += "\n";
        }
        // Content before the first H2 is intentionally dropped.
    }

    flush_current();
}

class MonolithActions {
public:
    // --- guide ---
    void print_guide(const Args& args) {
        fs::path plugin_root = resolve_plugin_root();
        fs::path guide_path = plugin_root.empty()
            ? fs::path("MONOLITH_GUIDE.md")
            : (plugin_root / "Docs" / "MONOLITH_GUIDE.md");

        std::ifstream f(guide_path.string());
        if (!f.is_open()) {
            std::cout << "Guide markdown not found at " << guide_path.string()
                      << "\nRestore Docs/MONOLITH_GUIDE.md (it ships in the release zip) "
                         "or pull the latest Monolith release, then retry." << std::endl;
            return;
        }

        std::stringstream ss;
        ss << f.rdbuf();
        std::string markdown = ss.str();

        std::map<std::string, std::string> sections;
        std::vector<std::string> ordered;
        split_sections(markdown, sections, ordered);

        const std::vector<std::string>& canonical = get_canonical_section_order();
        std::string version = parse_uplugin_version(plugin_root);

        // ----- Section-keyed dispatch -----
        std::string requested = to_lower_copy(trim_copy(args.opt("section")));
        if (!requested.empty()) {
            auto it = sections.find(requested);
            if (it == sections.end()) {
                std::cout << "Unknown section '" << requested << "'. Valid sections: "
                          << join_canonical_section_names() << "." << std::endl;
                return;
            }
            std::cout << "## " << requested << "\n\n" << it->second << std::endl;
            return;
        }

        // ----- No-arg dispatch: index + all section bodies (no live overlay) -----
        std::cout << "=== Monolith Guide";
        if (!version.empty())
            std::cout << " (v" << version << ")";
        std::cout << " ===\n\n";

        std::cout << "Sections (pass --section=NAME to print just one):\n";
        for (const auto& name : canonical) {
            if (sections.count(name))
                std::cout << "  - " << name << "\n";
        }
        std::cout << "\n";

        for (const auto& name : canonical) {
            auto it = sections.find(name);
            if (it != sections.end()) {
                std::cout << "## " << name << "\n\n" << it->second << "\n\n";
            }
        }
        std::cout << std::flush;
    }
};

// ============================================================
// Main
// ============================================================

int main(int argc, char* argv[]) {
    // --version / -v: print the build stamp BEFORE the 3-arg usage gate fires.
    // The orchestrator's staleness guard compares parity_spec_rev across the exe
    // and the Python sibling; both mirror the same PARITY_SPEC_REV literal.
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--version" || a == "-v") {
            fs::path plugin_root = resolve_plugin_root();
            std::string version = parse_uplugin_version(plugin_root);
            ojson out;
            out["tool"] = "monolith_query";
            out["plugin_version"] = version;          // Monolith.uplugin VersionName ("" if unresolved)
            out["parity_spec_rev"] = PARITY_SPEC_REV; // mirrored by the Python sibling
            out["source_hash"] = SOURCE_HASH;         // build-injected (/DSOURCE_HASH=...) or "dev"
            std::cout << out.dump(2) << std::endl;
            return 0;
        }
    }

    Args args = parse_args(argc, argv);

    std::string db_dir = args.opt("db");
    if (db_dir.empty()) db_dir = resolve_db_dir();

    if (args.ns == "source") {
        std::string db_path = args.opt("source_db");
        if (db_path.empty()) db_path = (fs::path(db_dir) / "EngineSource.db").string();

        SourceActions sa;
        sa.open(db_path);

        static const std::map<std::string, std::function<void(SourceActions&, const Args&)>> actions = {
            {"search_source",       [](SourceActions& s, const Args& a) { s.search_source(a); }},
            {"read_source",         [](SourceActions& s, const Args& a) { s.read_source(a); }},
            {"find_references",     [](SourceActions& s, const Args& a) { s.find_references(a); }},
            {"find_callers",        [](SourceActions& s, const Args& a) { s.find_callers(a); }},
            {"find_callees",        [](SourceActions& s, const Args& a) { s.find_callees(a); }},
            {"get_class_hierarchy", [](SourceActions& s, const Args& a) { s.get_class_hierarchy(a); }},
            {"get_module_info",     [](SourceActions& s, const Args& a) { s.get_module_info(a); }},
            {"get_symbol_context",  [](SourceActions& s, const Args& a) { s.get_symbol_context(a); }},
            {"read_file",           [](SourceActions& s, const Args& a) { s.read_file(a); }},
            {"get_include_path",    [](SourceActions& s, const Args& a) { s.get_include_path(a); }},
            {"get_signature",       [](SourceActions& s, const Args& a) { s.get_signature(a); }},
            {"check_deprecations",  [](SourceActions& s, const Args& a) { s.check_deprecations(a); }},
            {"verify_symbols",      [](SourceActions& s, const Args& a) { s.verify_symbols(a); }},
            {"find_example_usage",  [](SourceActions& s, const Args& a) { s.find_example_usage(a); }},
            {"lint_header",         [](SourceActions& s, const Args& a) { s.lint_header(a); }},
            {"generate_class_stub", [](SourceActions& s, const Args& a) { s.generate_class_stub(a); }},
        };

        auto it = actions.find(args.action);
        if (it == actions.end()) die("Unknown source action: " + args.action);
        it->second(sa, args);

    } else if (args.ns == "project") {
        std::string db_path = args.opt("project_db");
        if (db_path.empty()) db_path = (fs::path(db_dir) / "ProjectIndex.db").string();

        ProjectActions pa;
        pa.open(db_path);

        static const std::map<std::string, std::function<void(ProjectActions&, const Args&)>> actions = {
            {"search",            [](ProjectActions& p, const Args& a) { p.search(a); }},
            {"find_by_type",      [](ProjectActions& p, const Args& a) { p.find_by_type(a); }},
            {"find_references",   [](ProjectActions& p, const Args& a) { p.find_references(a); }},
            {"get_stats",         [](ProjectActions& p, const Args& a) { p.get_stats(a); }},
            {"get_asset_details", [](ProjectActions& p, const Args& a) { p.get_asset_details(a); }},
        };

        auto it = actions.find(args.action);
        if (it == actions.end()) die("Unknown project action: " + args.action);
        it->second(pa, args);

    } else if (args.ns == "monolith") {
        // No database — the guide is served straight from Docs/MONOLITH_GUIDE.md.
        MonolithActions ma;

        static const std::map<std::string, std::function<void(MonolithActions&, const Args&)>> actions = {
            {"guide", [](MonolithActions& m, const Args& a) { m.print_guide(a); }},
        };

        auto it = actions.find(args.action);
        if (it == actions.end()) die("Unknown monolith action: " + args.action + " (expected 'guide')");
        it->second(ma, args);

    } else if (args.ns == "cppreflect" || args.ns == "network" ||
               args.ns == "decision" || args.ns == "risk") {
        // All Reflection Intelligence read tables live in EngineSource.db.
        std::string db_path = args.opt("source_db");
        if (db_path.empty()) db_path = (fs::path(db_dir) / "EngineSource.db").string();

        ReflectionActions ra;
        ra.open(db_path);

        using RIFn = std::function<void(ReflectionActions&, const Args&)>;
        // Per-namespace action tables. Phantom `risk list_hotspots` REMOVED — it
        // is not a live action; the closest live equivalent is
        // get_release_window_hotspots (different shape).
        static const std::map<std::string, std::map<std::string, RIFn>> ns_actions = {
            {"cppreflect", {
                {"get_uclass",             [](ReflectionActions& r, const Args& a){ r.get_uclass(a); }},
                {"list_uproperties",       [](ReflectionActions& r, const Args& a){ r.list_uproperties(a); }},
                {"list_ufunctions",        [](ReflectionActions& r, const Args& a){ r.list_ufunctions(a); }},
                {"find_interface_impls",   [](ReflectionActions& r, const Args& a){ r.find_interface_impls(a); }},
                {"find_class_specifier",   [](ReflectionActions& r, const Args& a){ r.find_class_specifier(a); }},
                {"list_class_specifiers",  [](ReflectionActions& r, const Args& a){ r.list_class_specifiers(a); }},
            }},
            {"network", {
                {"list_replicated_classes", [](ReflectionActions& r, const Args& a){ r.list_replicated_classes(a); }},
                {"list_rpc_functions",      [](ReflectionActions& r, const Args& a){ r.list_rpc_functions(a); }},
                {"list_onrep_handlers",     [](ReflectionActions& r, const Args& a){ r.list_onrep_handlers(a); }},
                {"audit_unbalanced_onreps", [](ReflectionActions& r, const Args& a){ r.audit_unbalanced_onreps(a); }},
            }},
            {"decision", {
                {"list_decisions",          [](ReflectionActions& r, const Args& a){ r.list_decisions(a); }},
                {"get_decision",            [](ReflectionActions& r, const Args& a){ r.get_decision(a); }},
                {"list_stale",              [](ReflectionActions& r, const Args& a){ r.list_stale(a); }},
                {"find_supersession_chain", [](ReflectionActions& r, const Args& a){ r.find_supersession_chain(a); }},
                {"find_referent_decisions", [](ReflectionActions& r, const Args& a){ r.find_referent_decisions(a); }},
            }},
            {"risk", {
                {"get_hotspot_score",          [](ReflectionActions& r, const Args& a){ r.get_hotspot_score(a); }},
                {"get_cochange_pairs",         [](ReflectionActions& r, const Args& a){ r.get_cochange_pairs(a); }},
                {"get_file_churn",             [](ReflectionActions& r, const Args& a){ r.get_file_churn(a); }},
                {"get_release_window_hotspots",[](ReflectionActions& r, const Args& a){ r.get_release_window_hotspots(a); }},
                {"list_conditional_gates",     [](ReflectionActions& r, const Args& a){ r.list_conditional_gates(a); }},
            }},
        };

        const auto& table = ns_actions.at(args.ns);
        auto it = table.find(args.action);
        if (it == table.end()) {
            std::vector<std::string> known;
            for (const auto& kv : table) known.push_back(kv.first);
            // Spec ERROR FORMAT: "Unknown action: <ns>.<action> — call
            // monolith_discover(\"<ns>\") to enumerate valid actions in this
            // namespace.<suffix>" where suffix = " Did you mean: a, b, c?".
            auto sugg = fuzzy_top(args.action, known, 3);
            std::string suffix;
            if (!sugg.empty()) {
                suffix = " Did you mean: ";
                for (size_t i = 0; i < sugg.size(); ++i) { if (i) suffix += ", "; suffix += sugg[i]; }
                suffix += "?";
            }
            ojson out;
            out["success"] = false;
            out["error"] = "Unknown action: " + args.ns + "." + args.action +
                           " \xE2\x80\x94 call monolith_discover(\"" + args.ns +
                           "\") to enumerate valid actions in this namespace." + suffix;
            std::cout << out.dump(2) << std::endl;
            std::exit(0);
        }
        it->second(ra, args);

    } else {
        // Unknown namespace — list all offline-servable namespaces (matching the
        // live server's error shape) + did_you_mean suggestions + the boundary note.
        const auto& ns = offline_namespaces();
        std::string ns_list;
        for (size_t i = 0; i < ns.size(); ++i) {
            if (i > 0) ns_list += ", ";
            ns_list += "'" + ns[i] + "'";
        }
        std::cerr << "ERROR: Unknown namespace: " << args.ns
                  << " (offline-servable: " << ns_list << ")"
                  << did_you_mean_suffix(args.ns, ns) << std::endl;
        std::cerr << "NOTE: this offline tool serves only namespaces backed by on-disk SQLite. "
                     "The live MCP server exposes ~29 namespaces; the rest are LIVE-ONLY "
                     "(require a running editor + UObject reflection)." << std::endl;
        std::exit(2);
    }

    return 0;
}
