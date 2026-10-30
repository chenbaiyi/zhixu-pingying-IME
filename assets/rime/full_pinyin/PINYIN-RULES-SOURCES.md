# 拼音规则来源与适配

正式方案版本：zhixu_pinyin 2.2.1。仅适配拼写运算，不导入上游完整方案或词库。

- Rime 官方拼写运算说明：https://github.com/rime/home/wiki/SpellingAlgebra
- 雾凇拼音 iDvel/rime-ice：schema 2026-03-08，锁定提交 8daac9e1bb8069949b64c6f22b44e3573b2237b7，GPL-3.0。参考单/双字母简拼、h 错位、元音/鼻音尾错序和 ong/on。
- 四叶草 fkxxyz/rime-cloverpinyin：schema 1.0.0，锁定提交 3b8618fa9a44c537fc4170bc0aa3b0ef1d159a8d，LGPL-3.0。参考共同简拼、ao/oa、ng/gn、介音交换与 ong/on。

完整原始 schema、LICENSE 和 SHA-256 清单保存于实现工作树的 docs/development/2026-10-10-pinyin-input-rules/upstream。
本文件随运行资料保留来源信息；上游许可证原文分别为 PINYIN-RULES-ICE-LICENSE 和 PINYIN-RULES-CLOVER-LICENSE。

适配内容：
- 默认启用 z/zh、c/ch、s/sh、an/ang、en/eng、in/ing；使用 fuzz 属性，低于准确全拼可信度。
- 简拼仍为 abbrev 属性，支持全拼简拼混输；不添加词典简拼列，不表达语义关系。
- 选定错序/漏字母规则使用 /correction 属性；可信度惩罚为 librime 的 log(0.01)。
- 2.2.0 在上游有限换位规则基础上自行扩展：覆盖合法音节内所有相邻字母位置的一次换位，包括声母与元音（bu/ub）、h、元音及鼻音尾；不是针对某个词添加映射。
- 五条锚定规则仅接受字母码，派生时添加临时后缀 ~，全部换位结束后统一移除。Rime 逐条应用规则时不能对该变体再次换位；编译后的输入码不带 ~。
- 添加鼻音尾漏 n，支持 shaghai。未启用任意删字、音节颠倒或 n/l、f/h。
- 不启用 librime 邻键搜索，不修改引擎源码。换位统一降权，准确全拼、明确分隔及混输排序由真实 librime 回归验证。

现有方案 ID、万象中文词库、pinyin_simp 学习词库与每页 8 项均保留。

2.2.1 增加通用鼻音韵尾轮换：ang/eng/ing/ong 末尾的元音错按到 ng 后面时仍按原音节召回，例如 ong/ngo。派生仍使用 /correction 和临时 ~，与相邻换位规则互不级联；准确全拼排序保留。

2.2.2: Preserve ue/ve spellings for n/l umlaut syllables in either dictionary convention. The active Wanxiang dictionary uses lve/nve; derive/^([nl])ve$/$1ue/ restores hulue, celue and nuedai without word-specific aliases. Existing hulve/celve/nvedai remain exact.
