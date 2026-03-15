"""Indexing pipeline — discovers, parses, and stores UE source into SQLite."""

from __future__ import annotations

import logging
import os
import sqlite3
import time
from pathlib import Path
from typing import Any, Callable

from ..db.queries import (
    get_file_by_path,
    insert_file,
    insert_include,
    insert_inheritance,
    insert_module,
    insert_symbol,
)
from .cpp_parser import CppParser
from .reference_builder import ReferenceBuilder
from .shader_parser import ShaderParser

logger = logging.getLogger(__name__)

_CPP_EXTENSIONS = {".h", ".cpp", ".inl"}
_SHADER_EXTENSIONS = {".usf", ".ush"}
_EXT_TO_FILETYPE = {
    ".h": "header",
    ".cpp": "source",
    ".inl": "inline",
    ".usf": "shader",
    ".ush": "shader_header",
}


class IndexingPipeline:
    """Walks an Unreal Engine source tree, parses files, and stores results."""

    def __init__(self, conn: sqlite3.Connection) -> None:
        self._conn = conn
        self._cpp_parser = CppParser()
        self._shader_parser = ShaderParser()
        self._symbol_name_to_id: dict[str, Any] = {}
        self._symbol_spans: dict[str, tuple[int, int]] = {}
        self._class_name_to_id: dict[str, int] = {}
        self._class_spans: dict[str, tuple[int, int]] = {}
        self._new_file_ids: set[int] = set()
        self._diag: dict[str, int] = {
            "forward_decls": 0,
            "definitions": 0,
            "with_base_classes": 0,
            "inheritance_resolved": 0,
            "inheritance_failed": 0,
        }
        conn.commit()
        conn.execute("PRAGMA journal_mode=DELETE")
        conn.execute("PRAGMA synchronous=NORMAL")

    @property
    def diagnostics(self) -> dict[str, int]:
        return dict(self._diag)

    def load_existing_symbols(self) -> int:
        """Load all existing symbols from DB into in-memory maps.

        Call this before incremental indexing so that cross-references
        (e.g. project -> engine) can resolve against the full symbol table.
        Returns the number of symbols loaded.
        """
        t0 = time.monotonic()
        rows = self._conn.execute(
            "SELECT id, name, qualified_name, kind, line_start, line_end "
            "FROM symbols"
        ).fetchall()

        count = 0
        for row in rows:
            sym_id, name, qname, kind, ls, le = row
            self._update_symbol_map(name, sym_id, ls, le)
            if qname != name:
                self._update_symbol_map(qname, sym_id, ls, le)
            if kind in ("class", "struct"):
                self._update_class_map(name, sym_id, ls, le)
            count += 1

        elapsed = time.monotonic() - t0
        print(f"  Loaded {count:,} existing symbols into memory ({elapsed:.1f}s)", flush=True)
        return count

    def index_directory(
        self,
        path: Path,
        module_name: str | None = None,
        module_type: str = "Runtime",
        *,
        finalize: bool = True,
    ) -> dict[str, Any]:
        path = Path(path)
        if module_name is None:
            module_name = path.name

        mod_id = insert_module(
            self._conn, name=module_name, path=str(path), module_type=module_type,
        )

        files_processed = 0
        symbols_extracted = 0
        errors = 0

        for dirpath, _dirnames, filenames in os.walk(path):
            for fname in filenames:
                fpath = Path(dirpath) / fname
                ext = fpath.suffix.lower()
                try:
                    if ext in _CPP_EXTENSIONS:
                        n = self._index_cpp_file(fpath, mod_id)
                        symbols_extracted += n
                        files_processed += 1
                    elif ext in _SHADER_EXTENSIONS:
                        n = self._index_shader_file(fpath, mod_id)
                        symbols_extracted += n
                        files_processed += 1
                except Exception:
                    logger.warning("Error indexing %s", fpath, exc_info=True)
                    errors += 1

        self._conn.commit()

        if finalize:
            self._finalize()

        return {
            "files_processed": files_processed,
            "symbols_extracted": symbols_extracted,
            "errors": errors,
        }

    def index_engine(
        self,
        source_path: Path,
        shader_path: Path | None = None,
        on_progress: Callable[[str, int, int, int, int], None] | None = None,
    ) -> dict[str, Any]:
        source_path = Path(source_path)
        total_files = 0
        total_symbols = 0
        total_errors = 0

        modules: list[tuple[Path, str, str]] = []

        categories = ["Runtime", "Editor", "Developer", "Programs"]
        for category in categories:
            cat_dir = source_path / category
            if not cat_dir.is_dir():
                continue
            for sub in sorted(cat_dir.iterdir()):
                if sub.is_dir():
                    modules.append((sub, sub.name, category))

        plugins_dir = source_path.parent / "Plugins"
        if plugins_dir.is_dir():
            for source_dir in sorted(plugins_dir.rglob("Source")):
                if source_dir.is_dir():
                    modules.append((source_dir, source_dir.parent.name, "Plugin"))

        if shader_path and shader_path.is_dir():
            modules.append((shader_path, "Shaders", "Shaders"))

        total_modules = len(modules)

        for i, (mod_path, mod_name, mod_type) in enumerate(modules):
            stats = self.index_directory(
                mod_path, module_name=mod_name, module_type=mod_type, finalize=False,
            )
            total_files += stats["files_processed"]
            total_symbols += stats["symbols_extracted"]
            total_errors += stats["errors"]

            if on_progress:
                on_progress(mod_name, i + 1, total_modules, total_files, total_symbols)

        if on_progress:
            on_progress("Finalizing (inheritance + references)...", total_modules, total_modules, total_files, total_symbols)
        self._finalize()

        return {
            "files_processed": total_files,
            "symbols_extracted": total_symbols,
            "errors": total_errors,
        }

    def _finalize(self, file_ids: set[int] | None = None) -> None:
        """Resolve inheritance and extract references.

        Args:
            file_ids: If provided, only extract references from these files.
                      If None, uses self._new_file_ids (files added this session).
                      Falls back to all C++ files if no tracking data available.
        """
        target_ids = file_ids or self._new_file_ids

        # Phase 1: Inheritance
        t0 = time.monotonic()
        self._resolve_inheritance()
        self._conn.commit()
        print(f"  Inheritance resolved ({time.monotonic() - t0:.1f}s)", flush=True)

        # Phase 2: Reference extraction (only new files when available)
        if target_ids:
            self._conn.execute("CREATE TEMP TABLE IF NOT EXISTS _new_files (id INTEGER PRIMARY KEY)")
            self._conn.execute("DELETE FROM _new_files")
            self._conn.executemany(
                "INSERT INTO _new_files (id) VALUES (?)",
                [(fid,) for fid in target_ids],
            )
            rows = self._conn.execute(
                "SELECT f.id, f.path FROM files f "
                "JOIN _new_files nf ON nf.id = f.id "
                "WHERE f.file_type IN ('header', 'source', 'inline')"
            ).fetchall()
        else:
            rows = self._conn.execute(
                "SELECT id, path FROM files WHERE file_type IN ('header', 'source', 'inline')"
            ).fetchall()

        total = len(rows)
        if total == 0:
            print("  No files to extract references from", flush=True)
            return

        print(f"  Extracting references from {total:,} files...", flush=True)

        ref_builder = ReferenceBuilder(self._conn, self._symbol_name_to_id)
        refs_total = 0
        errors = 0
        t0 = time.monotonic()
        last_report = t0

        for i, row in enumerate(rows):
            fpath = Path(row[1])
            try:
                refs = ref_builder.extract_references(fpath, row[0])
                refs_total += refs
            except Exception:
                logger.warning("Error extracting refs from %s", fpath, exc_info=True)
                errors += 1

            now = time.monotonic()
            if now - last_report >= 5.0 or (i + 1) % 1000 == 0:
                elapsed = now - t0
                rate = (i + 1) / elapsed if elapsed > 0 else 0
                eta = (total - i - 1) / rate if rate > 0 else 0
                print(
                    f"    [{i+1:,}/{total:,}] {refs_total:,} refs, "
                    f"{rate:.0f} files/s, ETA {eta:.0f}s",
                    flush=True,
                )
                last_report = now

        self._conn.commit()
        elapsed = time.monotonic() - t0
        print(
            f"  References done: {refs_total:,} refs from {total:,} files "
            f"({elapsed:.1f}s, {errors} errors)",
            flush=True,
        )

    def _index_cpp_file(self, path: Path, mod_id: int) -> int:
        result = self._cpp_parser.parse_file(path)

        ext = path.suffix.lower()
        file_type = _EXT_TO_FILETYPE.get(ext, "source")

        file_id = insert_file(
            self._conn, path=str(path), module_id=mod_id,
            file_type=file_type, line_count=len(result.source_lines),
            last_modified=path.stat().st_mtime,
        )
        self._new_file_ids.add(file_id)

        for inc_path in result.includes:
            line_num = 0
            for i, line in enumerate(result.source_lines, 1):
                if inc_path in line and "#include" in line:
                    line_num = i
                    break
            insert_include(self._conn, file_id=file_id, included_path=inc_path, line=line_num)

        count = 0
        for sym in result.symbols:
            if sym.kind == "include":
                continue

            qualified_name = sym.name
            if sym.parent_class:
                qualified_name = f"{sym.parent_class}::{sym.name}"

            parent_symbol_id = None
            if sym.parent_class and sym.parent_class in self._symbol_name_to_id:
                parent_symbol_id = self._symbol_name_to_id[sym.parent_class]

            sym_id = insert_symbol(
                self._conn, name=sym.name, qualified_name=qualified_name,
                kind=sym.kind, file_id=file_id,
                line_start=sym.line_start, line_end=sym.line_end,
                parent_symbol_id=parent_symbol_id,
                access=sym.access or None, signature=sym.signature or None,
                docstring=sym.docstring or None,
                is_ue_macro=1 if sym.is_ue_macro else 0,
            )

            self._update_symbol_map(sym.name, sym_id, sym.line_start, sym.line_end)
            if qualified_name != sym.name:
                self._update_symbol_map(qualified_name, sym_id, sym.line_start, sym.line_end)

            if sym.kind in ("class", "struct"):
                if sym.line_end > sym.line_start:
                    self._diag["definitions"] += 1
                else:
                    self._diag["forward_decls"] += 1
                if sym.base_classes:
                    self._diag["with_base_classes"] += 1
                self._update_class_map(sym.name, sym_id, sym.line_start, sym.line_end)
                if sym.base_classes:
                    self._symbol_name_to_id.setdefault(f"_bases_{sym.name}", sym.base_classes)

            count += 1

        self._insert_source_lines(file_id, result.source_lines)
        return count

    def _index_shader_file(self, path: Path, mod_id: int) -> int:
        result = self._shader_parser.parse_file(path)

        ext = path.suffix.lower()
        file_type = _EXT_TO_FILETYPE.get(ext, "shader")

        try:
            source_text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            source_text = ""
        source_lines = source_text.splitlines()

        file_id = insert_file(
            self._conn, path=str(path), module_id=mod_id,
            file_type=file_type, line_count=len(source_lines),
            last_modified=path.stat().st_mtime,
        )
        self._new_file_ids.add(file_id)

        for inc_path in result.includes:
            line_num = 0
            for i, line in enumerate(source_lines, 1):
                if inc_path in line and "#include" in line:
                    line_num = i
                    break
            insert_include(self._conn, file_id=file_id, included_path=inc_path, line=line_num)

        count = 0
        for sym in result.symbols:
            if sym.kind == "include":
                continue

            insert_symbol(
                self._conn, name=sym.name, qualified_name=sym.name,
                kind=sym.kind, file_id=file_id,
                line_start=sym.line_start, line_end=sym.line_end,
                parent_symbol_id=None, access=None,
                signature=sym.signature or None, docstring=sym.docstring or None,
                is_ue_macro=0,
            )
            count += 1

        self._insert_source_lines(file_id, source_lines)
        return count

    def _insert_source_lines(self, file_id: int, lines: list[str]) -> None:
        batch: list[tuple[int, int, str]] = []
        for i in range(0, len(lines), 10):
            chunk = lines[i : i + 10]
            chunk_start = i + 1
            batch.append((file_id, chunk_start, "\n".join(chunk)))

        if batch:
            self._conn.executemany(
                "INSERT INTO source_fts (file_id, line_number, text) VALUES (?, ?, ?)",
                batch,
            )

    @staticmethod
    def _is_definition(line_start: int, line_end: int) -> bool:
        return line_end > line_start

    def _update_symbol_map(
        self, name: str, sym_id: int, line_start: int, line_end: int
    ) -> None:
        if name.startswith("_bases_"):
            return
        existing_span = self._symbol_spans.get(name)
        if existing_span is None:
            self._symbol_name_to_id[name] = sym_id
            self._symbol_spans[name] = (line_start, line_end)
        elif self._is_definition(line_start, line_end) and not self._is_definition(*existing_span):
            self._symbol_name_to_id[name] = sym_id
            self._symbol_spans[name] = (line_start, line_end)

    def _update_class_map(
        self, name: str, sym_id: int, line_start: int, line_end: int
    ) -> None:
        existing_span = self._class_spans.get(name)
        if existing_span is None:
            self._class_name_to_id[name] = sym_id
            self._class_spans[name] = (line_start, line_end)
        elif self._is_definition(line_start, line_end) and not self._is_definition(*existing_span):
            self._class_name_to_id[name] = sym_id
            self._class_spans[name] = (line_start, line_end)

    def _resolve_inheritance(self) -> None:
        keys_to_process = [k for k in self._symbol_name_to_id if k.startswith("_bases_")]
        total = len(keys_to_process)
        if total > 0:
            print(f"  Resolving inheritance for {total:,} classes...", flush=True)

        for i, key in enumerate(keys_to_process):
            child_name = key[len("_bases_"):]
            base_classes = self._symbol_name_to_id[key]
            child_id = self._class_name_to_id.get(child_name)
            if child_id is None:
                self._diag["inheritance_failed"] += len(base_classes) if isinstance(base_classes, list) else 1
                continue
            for parent_name in base_classes:
                parent_id = self._class_name_to_id.get(parent_name)
                if parent_id is not None:
                    try:
                        insert_inheritance(
                            self._conn, child_id=child_id, parent_id=parent_id,
                        )
                        self._diag["inheritance_resolved"] += 1
                    except sqlite3.IntegrityError:
                        pass
                else:
                    self._diag["inheritance_failed"] += 1
                    logger.debug("Inheritance: %s -> %s (parent not in class map)", child_name, parent_name)
