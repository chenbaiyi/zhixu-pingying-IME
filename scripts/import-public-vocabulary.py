"""Reproducible, offline, additive public dictionary import for ZhixuIME."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import shutil
import unicodedata

ROOT = Path(__file__).resolve().parents[1]
RESEARCH = ROOT / 'docs/development/2026-10-16-dictionary-research'
OUT = ROOT / 'docs/development/2026-10-16-dictionary-expansion'
BASE = ROOT / '.cache/installer-idle-space-20261016/build/setup/program/rime'
if (ROOT/'assets/rime/full_pinyin').exists():BASE=ROOT/'assets/rime/full_pinyin'
if (ROOT/'assets/rime/public_sources').exists():RESEARCH=ROOT/'assets/rime/public_sources'
DESTINATION = None
HAN = re.compile(r'[\u3400-\u9fff\uf900-\ufaff\U00020000-\U000323af]{1,24}\Z')
PINYIN = re.compile(r'[a-z]+(?: [a-z]+)*\Z')
TABLES = {
    'rime-wanxiang': ['dicts/'+x+'.dict.yaml' for x in ['zi','jichu','duoyin','diming','mingren','renming','yiren','daolu','shiti']],
    'rime-ice': ['cn_dicts/'+x+'.dict.yaml' for x in ['8105','41448','base','ext','others']],
    'rime-frost': ['cn_dicts/'+x+'.dict.yaml' for x in ['8105','41448','GB18030-2022','base','ext','others','tencent']],
}
# Independently curated standard readings and product vocabulary priorities.
# These are ordinary words, never bespoke short-code or typo mappings.
CURATED = [
    ('操作记录','cao zuo ji lu',2500), ('批量删除','pi liang shan chu',1500),
    ('回滚机制','hui gun ji zhi',1000), ('候选词库','hou xuan ci ku',1000),
    ('候选排序','hou xuan pai xu',500), ('全拼输入','quan pin shu ru',500),
    ('简拼输入','jian pin shu ru',500), ('混合输入','hun he shu ru',500),
    ('用户词库','yong hu ci ku',500), ('保留数据','bao liu shu ju',500),
    ('异常顺序','yi chang shun xu',100), ('词库扩充','ci ku kuo chong',500),
]

def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def normalize(value):
    value=unicodedata.normalize('NFC',value).lower().translate(str.maketrans({c:'v' for c in 'üǖǘǚǜ'}))
    return ' '.join(''.join(c for c in unicodedata.normalize('NFD',value) if unicodedata.category(c)!='Mn').split())

def records(path):
    started=False
    with path.open(encoding='utf-8-sig') as stream:
        for number,line in enumerate(stream,1):
            line=line.rstrip('\r\n')
            if line=='...': started=True;continue
            if not started or not line or line.startswith('#'):continue
            fields=line.split('\t')
            if len(fields)<2:
                yield None
                continue
            text,reading=fields[:2]
            reading=normalize(reading)
            if not HAN.fullmatch(text) or not PINYIN.fullmatch(reading) or len(text)!=len(reading.split()):
                yield None
                continue
            weight=fields[2] if len(fields)>2 else '1'
            if not re.fullmatch(r'-?[0-9]+',weight):
                yield None
                continue
            yield (text,reading),int(weight)
    if not started:raise ValueError(f'{path.name}: missing dictionary terminator')

def run(check=False):
    target=DESTINATION or OUT/'source/assets/rime/full_pinyin'
    OUT.mkdir(parents=True,exist_ok=True)
    manifest=json.loads((RESEARCH/'sources.json').read_text(encoding='utf-8'))
    sources={entry['repo'].split('/')[1]:entry for entry in manifest}
    source_files=[]
    for directory,tables in TABLES.items():
        locked={f['path']:f for f in sources[directory]['files']}
        for name in [*tables,'LICENSE']:
            path=RESEARCH/directory/name
            if digest(path)!=locked[name]['sha256']:raise ValueError(f'Changed source: {path}')
            source_files.append(dict(repository=sources[directory]['repo'],revision=sources[directory]['commit'],**locked[name]))
    baseline=set()
    for name in ['wanxiang_plain.dict.yaml','zhixu_daily.dict.yaml']:
        # Baseline keys include all records, even those outside the supplement's text filter.
        with (BASE/name).open(encoding='utf-8') as stream:
            for line in stream:
                fields=line.rstrip('\r\n').split('\t')
                if len(fields)>=2:baseline.add((fields[0],normalize(fields[1])))
    curated={(text,reading):weight for text,reading,weight in CURATED if (text,reading) not in baseline}
    additions={}
    counts={}
    for directory,tables in TABLES.items():
        for name in tables:
            accepted=filtered=0
            for row in records(RESEARCH/directory/name):
                if row is None:filtered+=1;continue
                key,weight=row
                accepted+=1
                if key in baseline or key in curated:continue
                # Preserve existing table frequencies. New-source frequencies use
                # a bounded, monotonic scale because upstream scales differ.
                scaled=min(128,max(1,round(math.log1p(max(0,weight))*8)))
                additions[key]=max(scaled,additions.get(key,0))
            counts[directory+'/'+name]=dict(accepted=accepted,filtered=filtered)
    generated={}
    header='# Additive public and independently curated vocabulary; provenance and licenses: PUBLIC-DICTIONARIES.json.\n---\nname: zhixu_public\nversion: "20261016.1"\nsort: by_weight\nuse_preset_vocabulary: false\n...\n'
    combined={**additions,**curated}
    generated['zhixu_public.dict.yaml']=(header+''.join(f'{text}\t{reading}\t{weight}\n' for (text,reading),weight in sorted(combined.items()))).encode('utf-8')
    generated['zhixu_daily.dict.yaml']=(BASE/'zhixu_daily.dict.yaml').read_bytes()
    aggregate=(BASE/'zhixu_pinyin.dict.yaml').read_text(encoding='utf-8').replace('version: "2.0.0"','version: "2.1.0"')
    if '  - zhixu_public' not in aggregate:
        aggregate=aggregate.replace('  - zhixu_daily','  - zhixu_daily\n  - zhixu_public')
    generated['zhixu_pinyin.dict.yaml']=aggregate.encode('utf-8')
    for name in ['zhixu_pinyin.schema.yaml','zhixu_pinyin_exact.schema.yaml']:
        text=(BASE/name).read_text(encoding='utf-8').replace('version: "2.2.2"','version: "2.3.0"').replace('version: "2.2.2-exact"','version: "2.3.0-exact"')
        generated[name]=text.encode('utf-8')
    for directory,label in [('rime-ice','ICE'),('rime-frost','FROST')]:
        generated[f'PUBLIC-{label}-LICENSE']=(RESEARCH/directory/'LICENSE').read_bytes()
    metadata=dict(revision='20261016.1',sources=source_files,licenses={'rime-wanxiang':'CC-BY-4.0','rime-ice':'GPL-3.0','rime-frost':'GPL-3.0'},changes='Tone/umlaut normalization; strict Han text and syllable-count filter; additive text/reading deduplication; positive log-scaled weights capped at 128. Existing base frequencies unchanged. Independently curated entries and priorities are listed separately in this manifest and share the additive table.',counts=counts,public_records=len(additions),curated_records=len(curated),curated_entries=[dict(text=text,reading=reading,weight=weight) for (text,reading),weight in sorted(curated.items())],baseline_hashes={name:digest(BASE/name) for name in ['wanxiang_plain.dict.yaml','zhixu_daily.dict.yaml']})
    generated['PUBLIC-DICTIONARIES.json']=(json.dumps(metadata,ensure_ascii=False,indent=2)+'\n').encode('utf-8')
    generated['PUBLIC-DICTIONARIES.txt']=('Public vocabulary supplement 20261016.1\nSources and pinned revisions, file hashes, licenses and modifications: PUBLIC-DICTIONARIES.json\nWanxiang: https://github.com/amzxyz/rime-wanxiang (CC-BY-4.0)\nRime Ice: https://github.com/iDvel/rime-ice (GPL-3.0)\nRime Frost: https://github.com/gaboolic/rime-frost (GPL-3.0)\nCorresponding unmodified dictionary data and license texts: public-dictionary-sources.zip alongside this notice.\nOriginal attribution comments are preserved in the corresponding source files.\n').encode('utf-8')
    if not check:
        target.mkdir(parents=True,exist_ok=True)
        for file in BASE.iterdir():
            if file.is_file() and file.resolve()!=(target/file.name).resolve():shutil.copy2(file,target/file.name)
    for name,data in generated.items():
        if check:
            if (target/name).read_bytes()!=data:raise ValueError(f'Non-reproducible asset: {name}')
        else:(target/name).write_bytes(data)
    for name in ['wanxiang_plain.dict.yaml','zhixu_daily.dict.yaml','WANXIANG-LICENSE','WANXIANG-ATTRIBUTION.txt']:
        if digest(target/name)!=digest(BASE/name):raise ValueError(f'Baseline asset changed: {name}')
    summary=dict(public_records=len(additions),curated_records=len(curated),new_records=len(additions)+len(curated),base_unchanged=True,check=check,validation='PASS')
    (OUT/('import-check.json' if check else 'import-result.json')).write_text(json.dumps(summary,indent=2),encoding='utf-8')
    print(json.dumps(summary),flush=True)

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--check',action='store_true')
    parser.add_argument('--baseline',type=Path,default=BASE)
    parser.add_argument('--sources',type=Path,default=RESEARCH)
    parser.add_argument('--output',type=Path,default=OUT)
    parser.add_argument('--destination',type=Path)
    args=parser.parse_args()
    BASE=args.baseline;RESEARCH=args.sources;OUT=args.output;DESTINATION=args.destination
    run(args.check)
