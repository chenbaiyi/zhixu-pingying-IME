import importlib.util
import unittest
import tempfile
from pathlib import Path
spec = importlib.util.spec_from_file_location("wanxiang_import", Path(__file__).parents[2] / "scripts/import-wanxiang.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
class WanxiangImport(unittest.TestCase):
    def test_normalizes_umlaut_and_combining_nasal_tones(self):
        self.assertEqual(m.normalize_reading("nǚ lǜ n\u0304g ê"), "nv lv ng e")
        self.assertEqual(m.normalize_reading("lǚ sè"), "lv se")
    def test_zero_weight_source_entries_remain_available(self):
        self.assertEqual(m.merge_records([("冷僻字", "leng pi zi", 0)])[ ("冷僻字", "leng pi zi") ], 1)
    def test_optional_source_weight_defaults_to_one(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "source.dict.yaml"
            path.write_text("---\nname: test\n...\n攀\tpān\n", encoding="utf-8")
            self.assertEqual(list(m.read_records(path)), [("攀", "pān", 1)])
    def test_source_annotation_is_kept_in_raw_source_not_runtime_code(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "source.dict.yaml"
            path.write_text("---\nname: test\n...\n食话食说\tshí huà shí shuō\t259\t节目\n", encoding="utf-8")
            self.assertEqual(list(m.read_records(path)), [("食话食说", "shí huà shí shuō", 259)])
    def test_negative_source_frequency_sentinel_uses_minimum_weight(self):
        self.assertEqual(m.merge_records([("柏香镇", "bǎi xiāng zhèn", -1)])[("柏香镇", "bai xiang zhen")], 1)
    def test_rejects_unknown_malformed_weight(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "source.dict.yaml"
            path.write_text("...\n测试\tcè shì\t10s\n", encoding="utf-8")
            with self.assertRaises(ValueError): list(m.read_records(path))
    def test_decomposed_umlaut_tone_is_v(self):
        self.assertEqual(m.normalize_reading("nu\u0308\u030c er"), "nv er")
    def test_rejects_invalid_reading(self):
        with self.assertRaises(ValueError): m.normalize_reading("ni3 h@o")
    def test_merges_max_weight_and_preserves_polyphony_and_extension_chars(self):
        rows = [("行", "xíng", 40), ("行", "xìng", 12), ("行", "háng", 30), ("𠼞", "ā", 1)]
        merged = m.merge_records(rows)
        self.assertEqual(merged[("行", "xing")], 40)
        self.assertEqual(merged[("行", "hang")], 30)
        self.assertIn(("𠼞", "a"), merged)
if __name__ == "__main__": unittest.main()
