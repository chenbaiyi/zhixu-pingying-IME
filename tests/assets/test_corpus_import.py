import importlib.util, pathlib, unittest
MODULE = pathlib.Path(__file__).parents[2] / 'scripts' / 'corpus_import.py'
class CorpusImportTests(unittest.TestCase):
 def test_source_adapter_keeps_original_and_separates_notes(self):
  self.assertTrue(MODULE.exists(), 'corpus adapter has not been implemented')
  spec=importlib.util.spec_from_file_location('corpus_import', MODULE); mod=importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
  rows=list(mod.convert_records([{'Id':7,'Dynasty':'唐','Author':'李白','Title':{'Content':'古风'},'Clauses':[{'Content':'天地一逆旅。'},{'Content':'同悲万古尘。'}],'Note':'现代注释'}], 'jingmo', 'sample.json'))
  self.assertEqual(rows[0]['segments'], ['天地一逆旅。','同悲万古尘。'])
  self.assertEqual(rows[0]['author'], '李白'); self.assertEqual(rows[0]['notes'], '现代注释')
 def test_latest_shards_do_not_mix_old_versions(self):
  self.assertTrue(MODULE.exists(), 'corpus adapter has not been implemented')
  spec=importlib.util.spec_from_file_location('corpus_import',MODULE);mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
  files=[pathlib.Path(x) for x in ['chinese_quotes.json','chinese_quotes_v2.json','chinese_quote_v2_1.json','chinese_quote_v2_2.json','chinese_quote_v3_1.json','chinese_quote_v3_2.json']]
  self.assertEqual([x.name for x in mod.latest_shards(files)], ['chinese_quote_v3_1.json','chinese_quote_v3_2.json'])
class CorpusCoverageTests(unittest.TestCase):
 def test_dictionary_is_not_labeled_as_classical_quotation(self):
  spec=importlib.util.spec_from_file_location('corpus_import',MODULE);mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
  row=next(mod.convert_records([{'word':'黯淡无光','pinyin':'an dan wu guang','explanation':'昏暗'}],'jingmo','idioms.json'))
  self.assertEqual(row['type'],'phrase');self.assertEqual(row['text'],'黯淡无光')
 def test_complete_book_articles_keep_book_and_chapter(self):
  spec=importlib.util.spec_from_file_location('corpus_import',MODULE);mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
  rows=list(mod.convert_records({'name':'老子','articles':[{'title':'第一章','content':['道可道，非常道。']} ]},'ancient','老子.json'))
  self.assertEqual(len(rows),1);self.assertEqual(rows[0]['work'],'老子');self.assertEqual(rows[0]['chapter'],'第一章')
class CorpusMetadataTests(unittest.TestCase):
 def test_quote_uses_real_work_and_couplet_body_is_imported(self):
  spec=importlib.util.spec_from_file_location('corpus_import',MODULE);mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
  quote=next(mod.convert_records([{'id':1,'content':'白日依山尽，黄河入海流。','author':'王之涣','from':'登鹳雀楼'}],'jingmo','quote.json'))
  self.assertEqual(quote['work'],'登鹳雀楼')
  couplets=list(mod.convert_records([{'id':2,'body':'青山不墨千秋画\n流水无弦万古琴'}],'jingmo','antitheticalcouplet.json'))
  self.assertEqual(couplets[0]['segments'],['青山不墨千秋画','流水无弦万古琴'])
class NestedBookTests(unittest.TestCase):
 def test_nested_anthology_keeps_book_volume_article_and_body(self):
  spec=importlib.util.spec_from_file_location('corpus_import',MODULE);mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
  rows=list(mod.convert_records({'title':'古文观止','content':[{'title':'卷一','content':[{'chapter':'郑伯克段于鄢','author':'左丘明','paragraphs':['多行不义，必自毙。']}]}]},'poetry','蒙学/guwenguanzhi.json'))
  self.assertEqual(len(rows),1);self.assertEqual(rows[0]['work'],'古文观止');self.assertEqual(rows[0]['chapter'],'卷一 / 郑伯克段于鄢');self.assertEqual(rows[0]['segments'],['多行不义，','必自毙。'])
 def test_untitled_youmengying_uses_known_source_book(self):
  spec=importlib.util.spec_from_file_location('corpus_import',MODULE);mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
  row=next(mod.convert_records([{'content':'花不可以无蝶。'}],'poetry','幽梦影/youmengying.json'))
  self.assertEqual(row['work'],'幽梦影')
class ResumeTests(unittest.TestCase):
 def test_changed_source_invalidates_shared_entries_and_contributor_ledger(self):
  import sqlite3,json
  spec=importlib.util.spec_from_file_location('corpus_import',MODULE);mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
  db=sqlite3.connect(':memory:');db.execute('create table entries(key text primary key,category text,body text,origins text)');db.execute('create table ingested_files(name text primary key,hash text,rows integer)')
  for k,origins in [('one',[{'source':'a'},{'source':'b'}]),('two',[{'source':'b'}]),('keep',[{'source':'c'}])]:db.execute('insert into entries values(?,?,?,?)',(k,'poetry','{}',json.dumps(origins)))
  for n in ['a','b','c']:db.execute('insert into ingested_files values(?,?,?)',(n,'old',1))
  mod.invalidate_sources(db,['a'])
  self.assertEqual(db.execute('select key from entries').fetchall(),[('keep',)])
  self.assertEqual(db.execute('select name from ingested_files').fetchall(),[('c',)])
 def test_phrase_key_uses_text_even_with_empty_segments(self):
  spec=importlib.util.spec_from_file_location('corpus_import',MODULE);mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
  self.assertNotEqual(mod.content_key({'segments':[],'text':'天地'}),mod.content_key({'segments':[],'text':'山川'}))
if __name__ == '__main__': unittest.main()
