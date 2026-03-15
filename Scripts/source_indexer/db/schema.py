"""SQLite schema for source_indexer."""

from __future__ import annotations

import sqlite3

SCHEMA_VERSION = 1

_DDL = """
-- Core tables ----------------------------------------------------------------

CREATE TABLE IF NOT EXISTS modules (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    name        TEXT NOT NULL,
    path        TEXT NOT NULL,
    module_type TEXT NOT NULL,
    build_cs_path TEXT,
    UNIQUE(name, path)
);

CREATE TABLE IF NOT EXISTS files (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    path          TEXT NOT NULL UNIQUE,
    module_id     INTEGER REFERENCES modules(id),
    file_type     TEXT NOT NULL,
    line_count    INTEGER NOT NULL DEFAULT 0,
    last_modified REAL NOT NULL DEFAULT 0.0
);

CREATE TABLE IF NOT EXISTS symbols (
    id               INTEGER PRIMARY KEY AUTOINCREMENT,
    name             TEXT NOT NULL,
    qualified_name   TEXT NOT NULL,
    kind             TEXT NOT NULL,
    file_id          INTEGER REFERENCES files(id),
    line_start       INTEGER,
    line_end         INTEGER,
    parent_symbol_id INTEGER REFERENCES symbols(id),
    access           TEXT,
    signature        TEXT,
    docstring        TEXT,
    is_ue_macro      INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_symbols_name            ON symbols(name);
CREATE INDEX IF NOT EXISTS idx_symbols_qualified_name  ON symbols(qualified_name);
CREATE INDEX IF NOT EXISTS idx_symbols_kind            ON symbols(kind);
CREATE INDEX IF NOT EXISTS idx_symbols_file_id         ON symbols(file_id);
CREATE INDEX IF NOT EXISTS idx_symbols_parent          ON symbols(parent_symbol_id);

CREATE TABLE IF NOT EXISTS inheritance (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    child_id  INTEGER NOT NULL REFERENCES symbols(id),
    parent_id INTEGER NOT NULL REFERENCES symbols(id),
    UNIQUE(child_id, parent_id)
);

CREATE TABLE IF NOT EXISTS "references" (
    id             INTEGER PRIMARY KEY AUTOINCREMENT,
    from_symbol_id INTEGER NOT NULL REFERENCES symbols(id),
    to_symbol_id   INTEGER NOT NULL REFERENCES symbols(id),
    ref_kind       TEXT NOT NULL,
    file_id        INTEGER REFERENCES files(id),
    line           INTEGER
);

CREATE INDEX IF NOT EXISTS idx_refs_from ON "references"(from_symbol_id);
CREATE INDEX IF NOT EXISTS idx_refs_to   ON "references"(to_symbol_id);
CREATE INDEX IF NOT EXISTS idx_refs_kind ON "references"(ref_kind);

CREATE TABLE IF NOT EXISTS includes (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    file_id       INTEGER NOT NULL REFERENCES files(id),
    included_path TEXT NOT NULL,
    line          INTEGER
);

-- FTS5 virtual tables --------------------------------------------------------

CREATE VIRTUAL TABLE IF NOT EXISTS symbols_fts USING fts5(
    name,
    qualified_name,
    docstring,
    content=symbols,
    content_rowid=id
);

CREATE VIRTUAL TABLE IF NOT EXISTS source_fts USING fts5(
    file_id UNINDEXED,
    line_number UNINDEXED,
    text
);

-- Meta table -----------------------------------------------------------------

CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT
);
"""

_TRIGGERS = """
-- Keep symbols_fts in sync with symbols table

CREATE TRIGGER IF NOT EXISTS symbols_ai AFTER INSERT ON symbols BEGIN
    INSERT INTO symbols_fts(rowid, name, qualified_name, docstring)
    VALUES (new.id, new.name, new.qualified_name, new.docstring);
END;

CREATE TRIGGER IF NOT EXISTS symbols_ad AFTER DELETE ON symbols BEGIN
    INSERT INTO symbols_fts(symbols_fts, rowid, name, qualified_name, docstring)
    VALUES ('delete', old.id, old.name, old.qualified_name, old.docstring);
END;
"""


def init_db(conn: sqlite3.Connection) -> None:
    """Create all tables, indexes, FTS virtual tables, and triggers.

    Sets schema_version=1 in meta table. Safe to call on an existing DB
    (all CREATE statements use IF NOT EXISTS).
    """
    try:
        conn.executescript(_DDL)
        conn.executescript(_TRIGGERS)
        conn.execute(
            "INSERT OR REPLACE INTO meta (key, value) VALUES ('schema_version', ?)",
            (str(SCHEMA_VERSION),),
        )
        conn.commit()
    except sqlite3.OperationalError as e:
        if "readonly" in str(e).lower():
            import logging
            logging.warning(f"Database is read-only, schema already exists: {e}")
        else:
            raise
