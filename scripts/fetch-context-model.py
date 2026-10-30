"""Check the pinned build asset; fetching is explicit and never part of typing."""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile
import urllib.request

ROOT=Path(__file__).resolve().parents[1]
DEST=ROOT/'assets/rime/full_pinyin'

def validate(path,record):
    with path.open('rb') as stream:
        digest=hashlib.file_digest(stream,'sha256').hexdigest()
    if path.stat().st_size!=record['bytes'] or digest!=record['sha256'].removeprefix('sha256:'):
        raise ValueError('Pinned context-model size or SHA256 mismatch')
    return digest

def main(fetch=False):
    record=json.loads((DEST/'GRAMMAR-SOURCES.json').read_text(encoding='utf-8-sig'))['model']
    target=DEST/'wanxiang-lts-zh-hans.gram'
    if not target.exists():
        if not fetch:
            raise SystemExit('Model absent. Copy the verified installer asset, or explicitly use --fetch.')
        request=urllib.request.Request(record['url'],headers={'User-Agent':'ZhixuIME-pinned-build-assets'})
        with tempfile.NamedTemporaryFile(prefix='context-model-',suffix='.download',dir=DEST,delete=False) as stream:
            temporary=Path(stream.name)
        try:
            with urllib.request.urlopen(request,timeout=120) as response, temporary.open('wb') as stream:
                while chunk:=response.read(1024*1024):stream.write(chunk)
            validate(temporary,record)
            if target.exists():raise FileExistsError('Model appeared during download; preserve it')
            temporary.replace(target)
        finally:
            if temporary.exists() and temporary.resolve().parent==DEST.resolve():temporary.unlink()
    print('CONTEXT_MODEL_VERIFIED sha256='+validate(target,record))

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--fetch',action='store_true');parser.add_argument('--check',action='store_true')
    options=parser.parse_args();main(options.fetch)
