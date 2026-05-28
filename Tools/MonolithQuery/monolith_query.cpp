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

// Simple row type: vector of (column_name, value) pairs
struct Row {
    std::map<std::string, std::string> cols;

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
        auto it = cols.find(key);
        if (it == cols.end() || it->second.empty()) return def;
        try { return std::stod(it->second); }
        catch (...) { return def; }
    }
};

using Rows = std::vector<Row>;

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
            const char* val = (const char*)sqlite3_column_text(stmt, c);
            row.cols[name ? name : ""] = val ? val : "";
        }
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return rows;
}

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
                  << "\nProject actions:\n"
                  << "  search <query> [--limit=N]\n"
                  << "  find_by_type <asset_class> [--limit=N] [--offset=N]\n"
                  << "  find_references <asset_path>\n"
                  << "  get_stats\n"
                  << "  get_asset_details <asset_path>\n"
                  << "\nMonolith actions:\n"
                  << "  guide [--section=NAME]   (NAME: onboarding|recipes|decisions|errors|skills_map|gotchas)\n";
        std::exit(1);
    }

    args.ns = argv[1];
    args.action = argv[2];

    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a.substr(0, 2) == "--") {
            std::string key, val;
            auto eq = a.find('=');
            if (eq != std::string::npos) {
                key = a.substr(2, eq - 2);
                val = a.substr(eq + 1);
            } else {
                key = a.substr(2);
                val = "";  // flag-style
            }
            // Normalize hyphens to underscores for consistency
            std::replace(key.begin(), key.end(), '-', '_');
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
        if (args.positional.empty()) die("search requires a query argument");
        std::string q = args.positional[0];
        int limit = args.opt_int("limit", 50);

        json results = json::array();

        // Search assets FTS
        {
            auto rows = query(db,
                "SELECT a.package_path, a.asset_name, a.asset_class, a.module_name, "
                "snippet(fts_assets, 2, '>>>', '<<<', '...', 32) as ctx, rank "
                "FROM fts_assets f JOIN assets a ON a.id = f.rowid "
                "WHERE fts_assets MATCH ? ORDER BY rank LIMIT " + std::to_string(limit),
                {q});
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
        }

        // Search nodes FTS
        {
            auto rows = query(db,
                "SELECT a.package_path, a.asset_name, a.asset_class, a.module_name, "
                "snippet(fts_nodes, 0, '>>>', '<<<', '...', 32) as ctx, f.rank "
                "FROM fts_nodes f JOIN nodes n ON n.id = f.rowid "
                "JOIN assets a ON a.id = n.asset_id "
                "WHERE fts_nodes MATCH ? ORDER BY f.rank LIMIT " + std::to_string(limit),
                {q});
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

    } else {
        die("Unknown namespace: " + args.ns + " (expected 'source', 'project', or 'monolith')");
    }

    return 0;
}
