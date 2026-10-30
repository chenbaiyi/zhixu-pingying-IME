"""Generate a small, reviewed supplement; never add the reported whole sentences."""
import argparse
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / 'assets/rime/full_pinyin'
# Independently maintained standard expressions and manual standard readings.
# The frequency adjustment is a product preference, not an asserted corpus count.
PRIORITIES = [('有依据', 'you yi ju', 64000), ('乾纲独断', 'qian gang du duan', 100)]
EXPRESSIONS = [
    ('八仙过海', 'ba xian guo hai', '各显神通', 'ge xian shen tong'),
    ('外甥打灯笼', 'wai sheng da deng long', '照旧', 'zhao jiu'),
    ('哑巴吃黄连', 'ya ba chi huang lian', '有苦说不出', 'you ku shuo bu chu'),
    ('周瑜打黄盖', 'zhou yu da huang gai', '一个愿打，一个愿挨', 'yi ge yuan da yi ge yuan ai'),
    ('泥菩萨过河', 'ni pu sa guo he', '自身难保', 'zi shen nan bao'),
    ('黄鼠狼给鸡拜年', 'huang shu lang gei ji bai nian', '没安好心', 'mei an hao xin'),
    ('竹篮打水', 'zhu lan da shui', '一场空', 'yi chang kong'),
    ('小葱拌豆腐', 'xiao cong ban dou fu', '一清二白', 'yi qing er bai'),
    ('狗咬吕洞宾', 'gou yao lv dong bin', '不识好人心', 'bu shi hao ren xin'),
    ('丈二和尚', 'zhang er he shang', '摸不着头脑', 'mo bu zhao tou nao'),
    ('姜太公钓鱼', 'jiang tai gong diao yu', '愿者上钩', 'yuan zhe shang gou'),
    ('铁公鸡', 'tie gong ji', '一毛不拔', 'yi mao bu ba'),
    ('擀面杖吹火', 'gan mian zhang chui huo', '一窍不通', 'yi qiao bu tong'),
    ('孔夫子搬家', 'kong fu zi ban jia', '净是输', 'jing shi shu'),
    ('兔子尾巴', 'tu zi wei ba', '长不了', 'chang bu liao'),
    ('张飞穿针', 'zhang fei chuan zhen', '粗中有细', 'cu zhong you xi'),
    ('千里送鹅毛', 'qian li song e mao', '礼轻情意重', 'li qing qing yi zhong'),
    ('门缝里看人', 'men feng li kan ren', '把人看扁了', 'ba ren kan bian le'),
    ('老鼠钻风箱', 'lao shu zuan feng xiang', '两头受气', 'liang tou shou qi'),
    ('芝麻开花', 'zhi ma kai hua', '节节高', 'jie jie gao'),
]

def header(name):
    return f'# Independently curated; provenance: CONTEXT-VOCABULARY.json.\n---\nname: {name}\nversion: "20261028.1"\nsort: by_weight\nuse_preset_vocabulary: false\n...\n'

def main(check=False):
    existing = set()
    for name in ['wanxiang_plain', 'zhixu_daily', 'zhixu_public']:
        for line in (DEST / f'{name}.dict.yaml').open(encoding='utf-8-sig'):
            fields = line.rstrip('\r\n').split('\t')
            if len(fields) >= 2:
                existing.add((fields[0],fields[1]))
    additions = dict(((text, code), weight) for text, code, weight in PRIORITIES)
    shortcuts = []
    for head, head_code, tail, tail_code in EXPRESSIONS:
        tail_plain = tail.replace('，','')
        for text, code in [(head,head_code),(head+tail_plain,head_code+' '+tail_code)]:
            assert len(text) == len(code.split()) and re.fullmatch('[a-z ]+',code)
            if (text,code) not in existing:
                additions[(text,code)] = 100
        shortcuts.append((head+'——'+tail,head_code.replace(' ',''),100))
    for (text,code),weight in additions.items():
        assert len(text) == len(code.split()) and weight > 0
        assert text not in ['检查并实测后','你的感觉有依据']
    generated = {
        'zhixu_context.dict.yaml':header('zhixu_context')+''.join(f'{text}\t{code}\t{weight}\n' for (text,code),weight in sorted(additions.items())),
        'zhixu_xiehou.dict.yaml':header('zhixu_xiehou')+''.join(f'{text}\t{code}\t{weight}\n' for text,code,weight in shortcuts),
    }
    metadata = dict(revision='20261028.1',provenance='Independently maintained traditional expressions and manual standard pinyin. No external dictionary copied.',
                    frequency_policy='Explicit product-priority weights; not corpus counts. 有依据 is recalibrated from the reported frequency error; reported whole sentences are excluded.',
                    priorities=[dict(text=t,pinyin=p,weight=w) for t,p,w in PRIORITIES],
                    new_phonetic_records=len(additions)-1, recalibrated_records=1, prefix_expansions=len(shortcuts),
                    records=[dict(text=t,pinyin=p,weight=w) for (t,p),w in sorted(additions.items())],
                    expressions=[dict(head=h,head_pinyin=p,tail=t,tail_pinyin=q) for h,p,t,q in EXPRESSIONS])
    metadata['generated_sha256']={name:hashlib.sha256(data.encode()).hexdigest() for name,data in generated.items()}
    generated['CONTEXT-VOCABULARY.json'] = json.dumps(metadata,ensure_ascii=False,indent=2)+'\n'
    for name,data in generated.items():
        if check:
            assert (DEST/name).read_bytes()==data.encode(),name
        else:
            (DEST/name).write_bytes(data.encode())
    print(f'CONTEXT_VOCABULARY_OK phonetic={len(additions)} expansions={len(shortcuts)} check={check}')

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--check',action='store_true')
    main(parser.parse_args().check)
