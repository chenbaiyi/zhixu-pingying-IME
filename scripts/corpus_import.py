"""Reproducible offline corpus conversion; no runtime network or language model."""
import argparse, collections, hashlib, json, pathlib, re, sqlite3, subprocess, unicodedata, zipfile

def latest_shards(files):
    groups={}
    for p in files:
        m=re.match(r'(.+?)(?:_v(\d+))?(?:_(\d+))?\.json$',p.name)
        if not m: continue
        name,version,page=m.groups()
        name=name.replace('classical_literature','classicalliterature')
        name=re.sub(r'(quotes|idioms|expressions|proverbs|writings|sentences|classicpoems)$',lambda x:x[0][:-1],name)
        groups.setdefault(name,[]).append((int(version or 0),int(page or 0),p))
    result=[]
    for name,items in sorted(groups.items()):
        v=max(x[0] for x in items); bypage={}
        for version,page,p in sorted(items,key=lambda x:str(x[2])):
            if version==v: bypage[page]=p
        result.extend(bypage[k] for k in sorted(bypage))
    return result

def plain(value):
    if isinstance(value,str): return value.strip()
    if isinstance(value,list): return '\n'.join(filter(None,(plain(x) for x in value)))
    if isinstance(value,dict):
        lower={k.lower():v for k,v in value.items()}
        for k in ('content','text','sentence','clause','paragraphs','value','name'):
            if k in lower: return plain(lower[k])
    return ''

def convert_records(obj,source,filename):
    # Nested anthologies: retain book / volume / article instead of flattening dictionaries.
    if isinstance(obj,dict) and isinstance(obj.get('content'),list) and any(isinstance(x,dict) for x in obj['content']):
        book=obj.get('work') or obj.get('title') or obj.get('name') or pathlib.Path(filename).stem
        parent=obj.get('chapter','')
        for index,child in enumerate(obj['content']):
            if not isinstance(child,dict): continue
            part=dict(child);part['work']=book
            label=part.get('chapter') or part.get('title') or ''
            part['chapter']=' / '.join(str(x) for x in [parent,label] if x)
            if 'author' not in part and obj.get('author'):part['author']=obj['author']
            yield from convert_records(part,source,filename+'#'+str(index))
        return
    if isinstance(obj,dict) and isinstance(obj.get('articles'),list):
        book=obj.get('name',pathlib.Path(filename).stem)
        for chapter in obj['articles']:
            part=dict(chapter); part['work']=book;part['chapter']=chapter.get('title','')
            if 'author' in obj:part['author']=obj['author']
            yield from convert_records([part],source,filename+'#'+str(part['chapter']))
        return
    if isinstance(obj,dict) and isinstance(obj.get('data'),list): obj=obj['data']
    if isinstance(obj,dict): obj=[obj]
    if not isinstance(obj,list): return
    for offset,row in enumerate(obj):
        if not isinstance(row,dict): continue
        low={k.lower():v for k,v in row.items()}
        if any(x in str(low.get('dynasty','')) for x in ['现代','現代','当代','當代','民国','民國']): continue
        body=next((plain(low[k]) for k in ['clauses','paragraphs','content','body','sentence','text','word'] if k in low and plain(low[k])), '')
        if not body or len(body)<2: continue
        segments=[x.strip() for x in re.findall(r'[^，。！？；：\n]+[，。！？；：]?',body) if x.strip()]
        if not segments: continue
        title=plain(low.get('work')) or plain(low.get('title')) or plain(low.get('rhythmic')) or plain(low.get('from')) or str(low.get('chapter','')) or ('对联' if 'couplet' in filename else '谚语' if 'proverb' in filename else '幽梦影' if 'youmengying' in filename else '来源未标注作品')
        author=plain(low.get('author')) or plain(low.get('writer')) or '来源未标注'
        chapter=' / '.join(str(low[k]) for k in ['chapter','section'] if low.get(k)) or title
        if 'word' in low:
            yield {'id':source+'.'+hashlib.sha256((filename+':'+str(low.get('id',offset))).encode()).hexdigest()[:28], 'type':'phrase', 'text':plain(low['word']), 'pinyin':plain(low.get('pinyin')) or '_', 'weight':0.5, 'tags':[source]+[str(x) for x in (low.get('classes') or [])], 'source':source+'/'+filename, 'source_id':str(low.get('id',offset)), 'notes':plain(low.get('explanation'))}
            continue
        yield {'id':source+'.'+hashlib.sha256((filename+':'+str(low.get('id',offset))).encode()).hexdigest()[:28],
               'type':'quotation','segments':segments,'work':title,'author':author,'chapter':chapter,
               'tags':[source]+[str(x) for x in (low.get('classes') or [])], 'source':source+'/'+filename, 'source_id':str(low.get('id',offset)),
               'notes':plain(low.get('note')) or plain(low.get('explanation')),
               'commentary':low.get('comments'),'source_references':low.get('froms'), 'source_version':source,'source_url':low.get('source_url','')}

def writing_pairs():
    # Authored scene vocabulary combined within each scene, never across scenes.
    scenes=[
      ('景物',['大地一片昏暗','昏暗','夜幕降临','暮色笼罩大地','天地陷入暮色','山野一片昏暗'],['黯淡无光','远山隐入暮色','寂静无声','只余几点星火'],['暮色','大地','昏暗']),
      ('景物',['山风','清风','微风','夜风','晚风','寒风','冷风','晨风','朔风','长风'],['拂过山岗','掠过衣角','穿林而来','吹散薄雾','卷起落叶','送来松香','带着寒意','悄然吹过','渐渐止息','绕过石壁'],['山风','寒风']),
      ('景物',['山峰','群山','远山','青山','苍山','孤峰','高峰','山岭','石峰','雪峰'],['隐入云海','直入苍穹','连绵起伏','横亘天际','覆着薄雪','笼在雾中','若隐若现','静立远方','映着霞光','一片苍茫'],['群山','山峰']),
      ('景物',['月光','清辉','银辉','月色','冷月','残月','明月','寒月','满月','新月'],['洒落庭前','照着石阶','映入水中','穿过云层','笼住山林','清冷如霜','落在剑上','照见归路','悄然西沉','映着孤影'],['月光','月色']),
      ('人物',['他的目光','她的目光','那道目光','双眸','眼神','眸光','视线','眼底','眉眼','眼眸'],['微微一凝','骤然转冷','透出寒意','渐渐柔和','隐含笑意','带着疑惑','落在远处','不露声色','一片清明','浮起波澜'],['目光','眼神']),
      ('人物',['他微微','她微微','少年微微','少女微微','老者微微','青年微微','那人微微','剑客微微','道人微微','书生微微'],['皱眉','点头','一笑','侧身','抬眸','摇头','叹息','颔首','停步','转身'],['微微']),
      ('战斗',['剑光','剑芒','刀光','刀芒','枪芒','锋芒','寒芒','青色剑光','银色剑光','白色剑光'],['破空而至','横扫而过','骤然亮起','掠过长空','直逼眉心','一闪而逝','撕开夜色','照亮山谷','映出寒意','划破云层'],['剑光','刀光']),
      ('战斗',['拳风','掌风','劲风','罡风','气浪','余劲','冲击','风压','拳劲','掌劲'],['迎面而来','震碎石壁','席卷四方','猛然炸开','荡开烟尘','掀起碎石','震动山林','扑向胸前','接连爆发','逼退众人'],['拳风','气浪']),
      ('修炼',['灵气','真气','元气','真元','灵力','气息','内息','法力','元力','精气'],['汇入丹田','沿经脉流转','缓缓凝聚','渐渐平稳','奔涌而出','充盈四肢','隐隐沸腾','归于沉寂','凝成一线','循环不息'],['灵气','丹田']),
      ('修炼',['丹田之中','气海之中','识海之中','经脉之中','灵台之中','体内深处','紫府之中','胸腹之间','眉心深处','周身上下'],['灵力翻涌','气息流转','渐起波澜','一片清明','隐现微光','暖意升起','寒意渐消','力量凝聚','渐归平静','生出异象'],['丹田','识海']),
      ('法术',['阵纹','符文','灵纹','道纹','铭文','金色符文','银色符文','青色阵纹','石上阵纹','空中符文'],['逐一亮起','交错流转','渐渐成形','隐入虚空','绽出微光','连成一片','封住退路','缓缓旋转','化作光幕','映亮石台'],['阵纹','符文']),
      ('器物',['长剑','古剑','青锋','宝剑','短剑','铁剑','灵剑','佩剑','断剑','重剑'],['发出轻鸣','泛着寒光','静卧匣中','缓缓出鞘','横在身前','映着月色','微微震颤','锋芒毕露','锈迹斑斑','光华内敛'],['长剑','古剑']),
      ('环境',['古殿','大殿','宫阙','石殿','神殿','仙殿','庙宇','楼阁','殿宇','庭院'],['寂静无声','笼在雾中','灯火微明','空无一人','映着夕照','门户紧闭','幽深难测','落满尘埃','古意盎然','隐在山间'],['古殿','大殿']),
      ('情绪',['心中','心底','胸中','内心','心头','他的心中','她的心中','少年的心中','老者的心中','众人心中'],['生出疑惑','涌起寒意','渐渐安定','暗自警惕','不由一震','泛起波澜','稍稍一松','仍有不安','满是感慨','难以平静'],['心中','心底']),
      ('对白',['他低声','她低声','少年低声','老者低声','道人低声','那人低声','少女低声','青年低声','剑客低声','书生低声'],['说道','问道','叹道','应了一声','念着旧事','唤了一声','提醒众人','道了声谢','解释缘由','劝了一句'],['低声']),
      ('日常',['他缓缓','她缓缓','少年缓缓','老者缓缓','道人缓缓','那人缓缓','少女缓缓','青年缓缓','剑客缓缓','书生缓缓'],['起身','坐下','抬头','转身','走近','停步','闭眼','伸手','退开','呼气'],['缓缓'])]
    # Prefix variants add ordinary context without changing the scene's grammar.
    prefixes=['','此刻，','转眼间，','不知何时，','就在此时，','片刻之后，','忽然，']
    n=0
    for scene,triggers,completions,keywords in scenes:
      for prefix in prefixes:
       for trigger in triggers:
        n+=1
        yield {'id':f'writing.{n}','type':'continuation','trigger':prefix+trigger,
               'completions':[{'text':s,'weight':1.0-i*0.02} for i,s in enumerate(completions)],
               'tags':['writing',scene],'keywords':sorted(set(keywords+[t for t in triggers if 2<=len(t)<=6])),'source':'ZhixuIME original vocabulary templates'}

def digest(p):
    h=hashlib.sha256()
    with p.open('rb') as f:
     for b in iter(lambda:f.read(1024*1024),b''):h.update(b)
    return h.hexdigest()

def content_key(row):
    text=''.join(row.get('segments') or [row.get('text','')])
    return hashlib.sha256(re.sub(r'[\s，。！？；：、,.!?;:]','',unicodedata.normalize('NFC',text)).encode()).hexdigest()

def invalidate_sources(db,names):
    if not names:return
    # Reingest the whole connected component of shared texts, preserving surviving contributors.
    db.execute('CREATE TEMP TABLE IF NOT EXISTS stale_sources(name TEXT PRIMARY KEY)')
    db.execute('CREATE TEMP TABLE IF NOT EXISTS stale_keys(key TEXT PRIMARY KEY)')
    db.executemany('INSERT OR IGNORE INTO stale_sources VALUES(?)',((n,) for n in names))
    while True:
        before=db.execute('SELECT count(*) FROM stale_sources').fetchone()[0]
        db.execute("INSERT OR IGNORE INTO stale_keys SELECT e.key FROM entries e,json_each(e.origins) j JOIN stale_sources s ON json_extract(j.value,'$.source')=s.name")
        db.execute("INSERT OR IGNORE INTO stale_sources SELECT json_extract(j.value,'$.source') FROM entries e JOIN stale_keys k ON k.key=e.key,json_each(e.origins) j")
        if db.execute('SELECT count(*) FROM stale_sources').fetchone()[0]==before:break
    db.execute('DELETE FROM entries WHERE key IN (SELECT key FROM stale_keys)')
    db.execute('DELETE FROM ingested_files WHERE name IN (SELECT name FROM stale_sources)')
    db.execute('DROP TABLE stale_sources');db.execute('DROP TABLE stale_keys');db.commit()

def source_inventory(cache):
    result={}; excluded=[]
    for name in ['chinese-poetry','jingmo-data','jingmo-data2','jingmo-data3','chinese-ancient-text','supplement']:
        root=cache/name
        if not root.exists():continue
        files=sorted(root.rglob('*.json')) if name in ['chinese-poetry','chinese-ancient-text','supplement'] else latest_shards(list((root/'api').glob('*.json')))
        for p in files:
            rel=p.relative_to(root).as_posix();key=name+'/'+rel
            ignored=any(k in rel.lower() for k in ['author.','rank/','strains/','hanzi','character','lyric','riddle','tongue','world','dataset','people','color','festival','solarterm','knowledge','modernpoetry','chinese_quote']) or rel=='SOURCES.json'
            if ignored:excluded.append(key)
            else:result[key]=(p,digest(p))
    return result,excluded

def build(cache,out,normalizer=None,config=None,version="1.0.0"):
    if not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)",version):raise ValueError("package version must be numeric semantic version")
    out.mkdir(parents=True,exist_ok=True)
    db=sqlite3.connect(out/'corpus-stage.sqlite');db.execute('PRAGMA journal_mode=WAL');db.execute('PRAGMA cache_size=-8192')
    db.execute('CREATE TABLE IF NOT EXISTS entries(key TEXT PRIMARY KEY, category TEXT, body TEXT, origins TEXT)')
    db.execute('CREATE TABLE IF NOT EXISTS ingested_files(name TEXT PRIMARY KEY, hash TEXT, rows INTEGER)')
    # Old interrupted runs committed exactly at file boundaries. Recover their file ledger from provenance.
    if db.execute('SELECT count(*) FROM ingested_files').fetchone()[0]==0:
        recovered={}
        for (origins,) in db.execute('SELECT origins FROM entries'):
            for origin in json.loads(origins):recovered.setdefault(origin['source'],set()).add(origin['source_id'])
        for filename,ids in recovered.items():
            path=cache/filename
            if path.is_file():db.execute('INSERT OR IGNORE INTO ingested_files VALUES(?,?,?)',(filename,digest(path),len(ids)))
        db.commit()
    inventory,excluded=source_inventory(cache)
    stale=[name for name,file_hash in db.execute('SELECT name,hash FROM ingested_files') if name not in inventory or inventory[name][1]!=file_hash]
    # One-time migration for adapter fields corrected after the initial interrupted import.
    db.execute('CREATE TABLE IF NOT EXISTS import_metadata(key TEXT PRIMARY KEY,value TEXT)')
    if not db.execute("SELECT 1 FROM import_metadata WHERE key='nested_books_v1'").fetchone():
        stale.extend(n for n in inventory if '蒙学/' in n or 'youmengying' in n)
    print('RECONCILE',len(set(stale)),flush=True);invalidate_sources(db,stale)
    db.execute("INSERT OR REPLACE INTO import_metadata VALUES('nested_books_v1','1')");db.commit()
    counts=collections.Counter(); sourcefiles=[]; errors=[]; locks=[]
    try:
      import opencc
      converter=opencc.OpenCC('t2s')
    except ImportError: converter=None
    if converter is None: raise RuntimeError('opencc is required to preserve original text alongside simplified matching text')
    for name in ['chinese-poetry','jingmo-data','jingmo-data2','jingmo-data3','chinese-ancient-text','supplement']:
      source=cache/name
      if not source.exists():continue
      if name=='supplement' and (source/'SOURCES.json').exists():
          locks.append({'name':'supplement','revision':'see per-book revisions','license':'Per-edition source notices; see books','books':json.loads((source/'SOURCES.json').read_text(encoding='utf-8'))})
      if (source/'.git').exists():
       rev=subprocess.check_output(['git','rev-parse','HEAD'],cwd=source).decode().strip()
       locks.append({'name':name,'revision':rev,'license':(source/'LICENSE').read_text(encoding='utf-8') if (source/'LICENSE').exists() else ''})
      files=sorted(source.rglob('*.json')) if name in ['chinese-poetry','chinese-ancient-text','supplement'] else latest_shards(list((source/'api').glob('*.json')))
      for p in files:
       rel=str(p.relative_to(source)).replace('\\','/')
       if any(k in rel.lower() for k in ['author.','rank/','strains/','hanzi','character','lyric','riddle','tongue','world','dataset','people','color','festival','solarterm','knowledge','modernpoetry','chinese_quote']):
           sourcefiles.append({'file':name+'/'+rel,'sha256':digest(p),'rows':0,'excluded':'outside classical text and writing vocabulary scope'});continue
       source_name=name+'/'+rel
       if source_name not in inventory:continue
       file_hash=inventory[source_name][1]
       done=db.execute('SELECT hash,rows FROM ingested_files WHERE name=?',(source_name,)).fetchone()
       if done and done[0]==file_hash:
           sourcefiles.append({'file':source_name,'sha256':file_hash,'rows':done[1]});continue
       try: obj=json.loads(p.read_text(encoding='utf-8-sig'))
       except (ValueError,UnicodeError) as e: errors.append({'file':name+'/'+rel,'error':str(e)});continue
       accepted=0
       rows=list(convert_records(obj,name,rel))
       if normalizer and rows:
           result=subprocess.run([str(normalizer),str(config)],input=json.dumps(rows,ensure_ascii=False).encode(),stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=True)
           rows=json.loads(result.stdout)
       for row in rows:
        if not normalizer:
            originals=row.get('segments',[]);row['original_segments']=originals;row['segments']=[converter.convert(x) for x in originals]
            for field in ['work','author','chapter','text']:
                if field in row:row[field]=converter.convert(row[field])
        row['display_version']='简体转换；原文另存';row['source_version']=next((x['revision'] for x in locks if x['name']==name),'snapshot')
        if len(json.dumps(row,ensure_ascii=False).encode())>900000:
            errors.append({'file':name+'/'+rel,'entry':row.get('id'),'error':'entry exceeds JSONL line limit; requires chapter splitting'});continue
        key=content_key(row)
        row['id']='text.'+key;category='poetry' if name=='chinese-poetry' else ('classics' if name in ['chinese-ancient-text','supplement'] else 'jingmo')
        origin={'source':row['source'],'source_id':row['source_id'],'work':row.get('work',''),'author':row.get('author',''),'chapter':row.get('chapter',''),'revision':row['source_version']}
        old=db.execute('SELECT origins,body FROM entries WHERE key=?',(key,)).fetchone()
        if old:
         origins=json.loads(old[0]); old_body=json.loads(old[1])
         if old_body.get('source')==row.get('source') and old_body.get('source_id')==row.get('source_id'):
             db.execute('UPDATE entries SET body=? WHERE key=?',(json.dumps(row,ensure_ascii=False),key))
         origins=[x for x in origins if (x.get('source'),x.get('source_id'))!=(origin['source'],origin['source_id'])]+[origin]
         db.execute('UPDATE entries SET origins=? WHERE key=?',(json.dumps(origins,ensure_ascii=False),key))
         counts['duplicates']+=1
        else:
         db.execute('INSERT INTO entries VALUES(?,?,?,?)',(key,category,json.dumps(row,ensure_ascii=False),json.dumps([origin],ensure_ascii=False)));counts[category]+=1
        accepted+=1
       db.execute('INSERT OR REPLACE INTO ingested_files VALUES(?,?,?)',(source_name,file_hash,accepted));db.commit();sourcefiles.append({'file':source_name,'sha256':file_hash,'rows':accepted})
      print(name,dict(counts),flush=True)
    # Original writing pair count counts unique trigger/completion pairs, not a inflated sentence count.
    for row in writing_pairs():
     db.execute('INSERT OR REPLACE INTO entries VALUES(?,?,?,?)',(row['id'],'writing',json.dumps(row,ensure_ascii=False),'[]'))
     counts['writing_pairs']+=len(row['completions'])
    db.commit();db.execute('CREATE INDEX IF NOT EXISTS category_index ON entries(category)');db.commit();packages=[]
    for category in ['poetry','jingmo','classics','writing']:
     part=0; f=None;size=0;entry_count=0
     def close_part():
      if f is None:return
      f.close(); print('ARCHIVE',category,part,size,flush=True); pid=f'com.zhixu.{category}.v1part{part}'
      manifest={'schema_version':'1.0','id':pid,'name':f"玄幻写作·{dict(poetry='诗词歌赋',jingmo='名句成语词语',classics='诸子志怪叙事',writing='日常写作搭配')[category]}·{part}",'version':version,'locale':'zh-CN','content_types':['quotation','continuation','phrase'],
       'source':'chinese-poetry / jingmo-data / Wikisource / ZhixuIME; see sources.lock.json','license':'Per-source notices included','generated_at':'2026-10-10T00:00:00Z',
       'entry_count':entry_count,'source_versions':{x['name']:x['revision'] for x in locks},'completeness':{'source_files':'核对来源文件及哈希','editorial_status':'未逐字校勘；部分源本含注疏'},'entries':{'path':'entries.jsonl','sha256':digest(path)},'display_version':'简体转换；保留原文与出处'}
      dest=out/(pid+'.imepack')
      with zipfile.ZipFile(dest,'w',zipfile.ZIP_DEFLATED,compresslevel=1) as z:
       z.writestr('manifest.json',json.dumps(manifest,ensure_ascii=False));z.write(path,'entries.jsonl');z.writestr('NOTICES.json',json.dumps(locks,ensure_ascii=False))
      packages.append({'id':pid,'file':dest.name,'sha256':digest(dest),'bytes':dest.stat().st_size})
     for body,origins in db.execute('SELECT body,origins FROM entries WHERE category=? ORDER BY rowid',(category,)):
      row=json.loads(body);row['origins']=json.loads(origins);line=(json.dumps(row,ensure_ascii=False,separators=(',',':'))+'\n').encode()
      if f is None or size+len(line)>300*1024*1024:
       close_part();part+=1;path=out/f'{category}-{part}.jsonl';f=path.open('wb');size=0;entry_count=0
      f.write(line);size+=len(line);entry_count+=1
     close_part()
    for category,number in db.execute('SELECT category,count(*) FROM entries GROUP BY category'):counts[category]=number
    counts['duplicates']=sum(x['rows'] for x in sourcefiles)-sum(counts[x] for x in ['poetry','jingmo','classics'] if x in counts)
    report={'counts':dict(counts),'sources':locks,'files':sourcefiles,'errors':errors,'packages':packages}
    (out/'sources.lock.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    (out/'bundle.json').write_text(json.dumps({'version':1,'packages':packages},ensure_ascii=False,indent=2),encoding='utf-8')
    print('PACKAGED',len(packages),dict(counts),flush=True)
    db.close()

if __name__=='__main__':
    a=argparse.ArgumentParser();a.add_argument('--cache',type=pathlib.Path,required=True);a.add_argument('--out',type=pathlib.Path,required=True);a.add_argument('--normalizer',type=pathlib.Path);a.add_argument('--config',type=pathlib.Path);a.add_argument("--version",default="1.0.0");args=a.parse_args();build(args.cache,args.out,args.normalizer,args.config,args.version)
