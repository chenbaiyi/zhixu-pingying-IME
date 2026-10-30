# 知序全拼 2.1

运行方案 `zhixu_pinyin` 使用万象完整中文词表，用户学习词典仍命名为 `pinyin_simp`。
每页候选数为 8，保持普通全拼、数字选词、空格及现有翻页键。

万象来源：https://github.com/amzxyz/rime-wanxiang
作者：amzxyz 及项目贡献者
锁定提交：b582a1b2a623a0da2d31729226224fd948e1c735
许可：CC-BY-4.0，见 WANXIANG-LICENSE 与 WANXIANG-ATTRIBUTION.txt。

导入 16 张词表，原始记录 2,212,137 条。归一化后 2,200,505 条；知序补充保留 76 条，权重 1。
原始数据保存在 assets/rime/wanxiang_source。scripts/import-wanxiang.py --check 可离线复现并校验全部资源。

适配：先 NFC 归一化，去除拼音声调、ü 转 v；不同读音保留，相同词语和拼音取最大权重。
省略权重、零权重和 -1 频次标记在运行字库中使用最低权重 1。
诗词表唯一已核对的 10s 频次错误也使用 1；其余未知错误一律拒绝。
错音表的第 4 列 Lua 注释保留于原始数据，不引入运行方案。

原 Apache-2.0 pinyin_simp 原始词表、LICENSE、AUTHORS 保留用于兼容及回滚；不再参与基础排序。
本次未引入万象 Lua 扩展或语法模型。

简拼、默认模糊音与有限纠错规则见 PINYIN-RULES-SOURCES.md；用户学习词库与词表内容保持不变。
