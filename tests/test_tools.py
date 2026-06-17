#!/usr/bin/env python3
"""Self-tests for the c2sql-gen toolchain (tools/).

Covers spec validation (valid + each rejection rule), guard generation, and a
golden-file drift check: the committed generated/ artifacts must match a fresh
generation from specs/. Run directly or via ctest (target: tools_pytests).
"""
import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import c2sql_gen as gen      # noqa: E402
import c2sql_spec as spec    # noqa: E402


def base_spec(**override):
    s = {
        "schema_version": "1.0",
        "struct_name": "persons",
        "c_struct": "Person",
        "backend": "sqlite3",
        "max_records": 100,
        "fields": [
            {"name": "id", "type": "int32", "primary_key": True},
            {"name": "name", "type": "text", "size": 32},
        ],
    }
    s.update(override)
    return s


class ValidateTests(unittest.TestCase):
    def test_valid(self):
        spec.validate(base_spec())  # must not raise

    def test_reserved_name(self):
        with self.assertRaises(spec.SpecError) as ctx:
            spec.validate(base_spec(struct_name="select"))
        self.assertTrue(any("予約語" in m for m in ctx.exception.errors))

    def test_bad_identifier(self):
        with self.assertRaises(spec.SpecError):
            spec.validate(base_spec(struct_name="1bad"))

    def test_duplicate_columns(self):
        with self.assertRaises(spec.SpecError):
            spec.validate(base_spec(fields=[
                {"name": "id", "type": "int32"},
                {"name": "id", "type": "int32"}]))

    def test_text_requires_size(self):
        with self.assertRaises(spec.SpecError):
            spec.validate(base_spec(fields=[{"name": "x", "type": "text"}]))

    def test_scalar_rejects_size(self):
        with self.assertRaises(spec.SpecError):
            spec.validate(base_spec(fields=[{"name": "x", "type": "int32", "size": 4}]))

    def test_two_primary_keys(self):
        with self.assertRaises(spec.SpecError):
            spec.validate(base_spec(fields=[
                {"name": "a", "type": "int32", "primary_key": True},
                {"name": "b", "type": "int32", "primary_key": True}]))

    def test_pk_not_nullable(self):
        with self.assertRaises(spec.SpecError):
            spec.validate(base_spec(fields=[
                {"name": "a", "type": "int32", "primary_key": True, "nullable": True}]))

    def test_unknown_top_key(self):
        s = base_spec()
        s["maxrecords"] = 1  # typo of max_records
        with self.assertRaises(spec.SpecError):
            spec.validate(s)

    def test_unknown_field_key(self):
        with self.assertRaises(spec.SpecError):
            spec.validate(base_spec(fields=[{"name": "a", "type": "int32", "pk": True}]))

    def test_max_records_zero(self):
        with self.assertRaises(spec.SpecError):
            spec.validate(base_spec(max_records=0))

    def test_bad_backend(self):
        with self.assertRaises(spec.SpecError):
            spec.validate(base_spec(backend="postgres"))

    def test_enforce_blob_pk_rejected(self):
        with self.assertRaises(spec.SpecError):
            spec.validate(base_spec(enforce_max_records=True, fields=[
                {"name": "a", "type": "blob", "size": 8, "primary_key": True}]))

    def test_enforce_must_be_bool(self):
        with self.assertRaises(spec.SpecError):
            spec.validate(base_spec(enforce_max_records="yes"))

    def test_too_many_columns(self):
        cols = [{"name": f"c{i}", "type": "int32"} for i in range(spec.MAX_COLUMNS + 1)]
        with self.assertRaises(spec.SpecError):
            spec.validate(base_spec(fields=cols))


class GenTests(unittest.TestCase):
    def test_no_guard_when_disabled(self):
        d = spec.derive(base_spec())
        self.assertNotIn("Guarded", gen.render_source(d, "persons.json"))
        self.assertNotIn("Guarded", gen.render_header(d, "persons.json"))

    def test_guard_emitted_with_pk(self):
        s = base_spec(struct_name="sessions", c_struct="Session",
                      enforce_max_records=True, max_records=3, fields=[
                          {"name": "id", "type": "int32", "primary_key": True},
                          {"name": "token", "type": "text", "size": 64}])
        spec.validate(s)
        d = spec.derive(s)
        src = gen.render_source(d, "sessions.json")
        hdr = gen.render_header(d, "sessions.json")
        self.assertIn("WriteSessionsGuarded", hdr)
        self.assertIn("SQL_RDB_ERR_CAPACITY_EXCEEDED", src)
        self.assertIn('SqlRDBCondInt("id", SQL_OP_EQ, row->id)', src)

    def test_guard_without_pk_counts_total_only(self):
        s = base_spec(struct_name="logs", c_struct="LogRow",
                      enforce_max_records=True, max_records=10,
                      fields=[{"name": "msg", "type": "text", "size": 80}])
        spec.validate(s)
        d = spec.derive(s)
        src = gen.render_source(d, "logs.json")
        self.assertIn("SQL_RDB_ERR_CAPACITY_EXCEEDED", src)
        self.assertNotIn("existing", src)  # no PK existence check


class GoldenTests(unittest.TestCase):
    """Committed generated/ files must equal a fresh generation (drift guard)."""

    def _check(self, spec_name):
        with tempfile.TemporaryDirectory() as tmp:
            gen.generate(os.path.join(ROOT, "specs", spec_name), tmp)
            for fn in sorted(os.listdir(tmp)):
                fresh = os.path.join(tmp, fn)
                committed = os.path.join(ROOT, "generated", fn)
                self.assertTrue(os.path.exists(committed),
                                f"generated/{fn} がコミットされていません")
                with open(fresh, encoding="utf-8") as a, \
                        open(committed, encoding="utf-8") as b:
                    self.assertEqual(
                        a.read(), b.read(),
                        f"generated/{fn} が specs/{spec_name} と不一致。"
                        f" tools/build_batch.sh で再生成してください。")

    def test_persons(self):
        self._check("persons.json")

    def test_sessions(self):
        self._check("sessions.json")


if __name__ == "__main__":
    unittest.main(verbosity=2)
