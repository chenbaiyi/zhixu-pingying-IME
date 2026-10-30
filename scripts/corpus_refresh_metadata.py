"""Finalize unpublished artifacts after a provenance-only adapter migration.
Never run against a live package store. Text changes are rejected, not patched into indices.
"""
import argparse,hashlib,json,pathlib,sqlite3,subprocess,zipfile,shutil
import corpus_import as ci

def prepare(cache,out,normalizer,config,mapfile):
    report=json.loads((out/'sources.lock.json').read_text(encoding='utf-8'))
    revisions={s['name']:s['revision'] for s in report['sources']}
    db=sqlite3.connect(out/'corpus-stage.sqlite');changes={};seen=0
    for file in report['files']:
        if 'sentence' not in file['file'] or file.get('excluded'):continue
        source,rel=file['file'].split('/',1);path=cache/file['file']
        if ci.digest(path)!=file['sha256']:raise RuntimeError('source hash changed')
        rows=list(ci.convert_records(json.loads(path.read_text(encoding='utf-8-sig')),source,rel))
        result=subprocess.run([str(normalizer),str(config)],input=json.dumps(rows,ensure_ascii=False).encode(),stdout=subprocess.PIPE,check=True)
        for row in json.loads(result.stdout):
            key=ci.content_key(row);old=db.execute('SELECT body,origins FROM entries WHERE key=?',(key,)).fetchone()
            if not old:continue
            body=json.loads(old[0]);origins=json.loads(old[1]);seen+=1
            if body.get('source')!=row['source'] or body.get('source_id')!=row['source_id']:continue
            if ci.content_key(body)!=key:raise RuntimeError('text migration requires full rebuild')
            changed=False
            for field in ['work','author','chapter']:
                if body.get(field)!=row.get(field):body[field]=row.get(field,'');changed=True
            for origin in origins:
                if origin.get('source')==row['source'] and origin.get('source_id')==row['source_id']:
                    for field in ['work','author','chapter']:origin[field]=row.get(field,'')
            if changed:
                db.execute('UPDATE entries SET body=?,origins=? WHERE key=?',(json.dumps(body,ensure_ascii=False),json.dumps(origins,ensure_ascii=False),key))
                body['origins']=origins;changes[body['id']]=body
    db.commit();db.close()
    mapfile.write_text(json.dumps(changes,ensure_ascii=False),encoding='utf-8');print('PROVENANCE_REFRESH',seen,len(changes),flush=True)

def finalize(out,prepared,mapfile):
    if prepared.name!='packages' or not prepared.parent.name.startswith('ZhixuIME-corpus-ready-'):
        raise RuntimeError('Only an unpublished prepared store may be finalized')
    changes=json.loads(mapfile.read_text(encoding='utf-8'))
    report=json.loads((out/'sources.lock.json').read_text(encoding='utf-8'));updated=[]
    source_versions={s['name']:s['revision'] for s in report['sources']}
    deferred=['三略','围炉夜话','小窗幽记','佛教心经','智囊完整版']
    for package in report['packages']:
        archive=out/package['file'];root=prepared/package['id']/'1.0.0'
        manifest=json.loads((root/'manifest.json').read_text(encoding='utf-8'))
        category=package['id'].split('.')[2];part=package['id'].split('v1part')[-1]
        source=out/(category+'-'+part+'.jsonl');pending=source.with_suffix('.finalizing.jsonl');count=0;changed=0;h=hashlib.sha256()
        with source.open('rb') as src,pending.open('wb') as dest:
            for line in src:
                count+=1
                entry_id=json.loads(line)['id']
                if entry_id in changes:
                    old=json.loads(line);new=changes[entry_id]
                    if ci.content_key(old)!=ci.content_key(new):raise RuntimeError('text migration rejected')
                    line=(json.dumps(new,ensure_ascii=False,separators=(',',':'))+'\n').encode();changed+=1
                dest.write(line);h.update(line)
        if changed:pending.replace(source)
        else:
            if h.hexdigest()!=manifest['entries']['sha256']:raise RuntimeError('unchanged entries hash mismatch')
            pending.unlink()
        manifest['entries']['sha256']=h.hexdigest();manifest['entry_count']=count;manifest['source_versions']=source_versions
        manifest['completeness']={'source_files':'核对来源文件及哈希','editorial_status':'未逐字校勘；部分源本含注疏','deferred_books':deferred}
        if changed:
            shutil.copyfile(source,root/'entries.jsonl')
            db=sqlite3.connect(root/'indexes'/'lookup.sqlite')
            db.execute('CREATE TEMP TABLE refreshed(entry TEXT PRIMARY KEY,work TEXT,author TEXT,chapter TEXT)')
            db.executemany('INSERT INTO refreshed VALUES(?,?,?,?)',((k,v.get('work',''),v.get('author',''),v.get('chapter','')) for k,v in changes.items()))
            db.execute('UPDATE lookup SET work=(SELECT work FROM refreshed WHERE entry=lookup.entry),author=(SELECT author FROM refreshed WHERE entry=lookup.entry),chapter=(SELECT chapter FROM refreshed WHERE entry=lookup.entry) WHERE entry IN(SELECT entry FROM refreshed)')
            db.commit();db.close()
        (root/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False),encoding='utf-8')
        temporary=archive.with_suffix('.finalizing.imepack')
        with zipfile.ZipFile(temporary,'w',zipfile.ZIP_DEFLATED,compresslevel=1) as z:
            z.writestr('manifest.json',json.dumps(manifest,ensure_ascii=False));z.write(source,'entries.jsonl');z.writestr('NOTICES.json',json.dumps(report['sources'],ensure_ascii=False))
        temporary.replace(archive)
        package.update(sha256=ci.digest(archive),bytes=archive.stat().st_size,entry_count=count)
        updated.append({'package':package['id'],'entries':count,'provenance_refreshed':changed})
        print('FINALIZED',package['id'],count,changed,flush=True)
    report['provenance_refresh']=updated;report['deferred_books']=deferred
    report['counts'].update({category:sum(x['entries'] for x in updated if x['package'].split('.')[2]==category) for category in ['poetry','jingmo','classics','writing']})
    report['counts']['duplicates']=sum(f['rows'] for f in report['files'])-sum(report['counts'][c] for c in ['poetry','jingmo','classics'])
    (out/'sources.lock.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    (out/'bundle.json').write_text(json.dumps({'version':1,'packages':report['packages']},ensure_ascii=False,indent=2),encoding='utf-8')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('mode',choices=['prepare','finalize']);p.add_argument('--cache',type=pathlib.Path);p.add_argument('--out',type=pathlib.Path,required=True);p.add_argument('--prepared',type=pathlib.Path);p.add_argument('--map',type=pathlib.Path,required=True);p.add_argument('--normalizer',type=pathlib.Path);p.add_argument('--config',type=pathlib.Path);a=p.parse_args()
    if a.mode=='prepare':prepare(a.cache,a.out,a.normalizer,a.config,a.map)
    else:finalize(a.out,a.prepared,a.map)
