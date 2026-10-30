# 知序 MIT 开源：独立第三方来源核查

核查日期：2026-10-30。核查对象为本仓库源码快照。本报告记录独立核查证据与待审事项；许可文件由主审查流程另行更新。项目所有者已确定自有代码采用 MIT。

## 结论

自有代码可以明确标为 MIT；第三方代码、规则、词表和模型继续按各自原许可发布。不能把仓库概括为“所有内容 MIT”。将 `assets/` 排除在 MIT 范围之外、保留原许可，是清楚表达授权边界的必要整理，但不等于已完成数据权利链或二进制分发审查。

也不能仅因为词表有 GPL 就断言整个 Windows 程序必须 GPL。GNU FAQ 允许独立作品聚合，且明确区分聚合与组合；应用到本项目时仍需考察实际复制、适配、编译与集成方式。GPL 可用于软件以外的作品；源形式是最适合修改的形式。因此不能把“只是数据”当作无须保留 GPL/源数据的理由。[GNU FAQ](https://www.gnu.org/licenses/gpl-faq.en.html#MereAggregation)、[GPL 非软件作品说明](https://www.gnu.org/licenses/gpl-faq.en.html#GPLOtherThanSoftware)。

## 已确认的本地事实

| 组件 | 锁定版本/本地证据 | 已见许可与范围 |
| --- | --- | --- |
| 雾凇增补词表 | `PUBLIC-DICTIONARIES.json`：iDvel/rime-ice `da1fbe602e38f26db846fa10120ee64c2b0324c0` | 本地原始 LICENSE 为 GPLv3，SHA-256 与清单一致；同一提交的上游 LICENSE 在线可读取，确认 GPLv3。[锁定 LICENSE](https://raw.githubusercontent.com/iDvel/rime-ice/da1fbe602e38f26db846fa10120ee64c2b0324c0/LICENSE) |
| 白霜增补词表 | gaboolic/rime-frost `19c615189c1173a84e7e9970469fb3d38db45766` | 本地原始 LICENSE 为 GPLv3，SHA-256 与清单一致；本次在线获取该锁定 LICENSE 失败，故不能声称独立在线校验完成。[锁定来源](https://github.com/gaboolic/rime-frost/tree/19c615189c1173a84e7e9970469fb3d38db45766) |
| 万象增补词表 | amzxyz/rime-wanxiang `3d0a715ac4c3632ad8e1aab239ec8a42284c516a` | 本地 LICENSE 为 CC BY 4.0，SHA-256 与清单一致；在线获取该锁定 LICENSE 失败。[锁定来源](https://github.com/amzxyz/rime-wanxiang/tree/3d0a715ac4c3632ad8e1aab239ec8a42284c516a) |
| 万象基础词表 | `third_party.lock.json` 另锁定 `b582a1b2a623a0da2d31729226224fd948e1c735` | 记录为 CC BY 4.0，包含原始表、处理后表及归属说明，不应被根 MIT 覆盖；本轮未独立联网校验这一版本。 |
| 拼音规则 | `PINYIN-RULES-SOURCES.md`：雾凇 `8daac9e1bb8069949b64c6f22b44e3573b2237b7`；四叶草 `3b8618fa9a44c537fc4170bc0aa3b0ef1d159a8d` | 文档分别声明 GPL-3.0、LGPL-3.0；本地配套文本分别为 GPLv3、LGPLv3。本轮四叶草锁定 LICENSE 在线不可得；该目录没有完整原始 schema，文档称其在实现工作树开发资料中。 |
| 原始简体拼音 | `third_party.lock.json`：rime/rime-pinyin-simp `0c6861ef7420ee780270ca6d993d18d4101049d0` | 本地 `full_pinyin/LICENSE` 为 Apache 2.0，记录其来自 Android Pinyin IME，AUTHORS 随附。 |
| 引擎、octagram、模型 | `third_party.lock.json`、对应 LICENSE | 清单分别记录 BSD-3-Clause、BSD-3-Clause、CC BY 4.0。引擎含本地补丁，模型另有下载资产记录。这些不因自有代码 MIT 而改变。 |

已检查：`PUBLIC-DICTIONARIES.json`、`PUBLIC-DICTIONARIES.txt`、`PINYIN-RULES-SOURCES.md`、`third_party.lock.json`、`THIRD_PARTY_NOTICES.md`、上述配套许可证，以及原始词表头部。三份 public_sources 原始 LICENSE 哈希与清单均相符（万象 `9e5f…cd94`；雾凇/白霜 `3972…6986`）。这里只校验了许可证哈希，没有重新计算所有大词表的哈希。

`PUBLIC-DICTIONARIES.json` 说明生成步骤有声调/ü 归一、汉字与音节过滤、去重、权重变换；因而处理后词表不能简单描述为未修改的上游文件。`PUBLIC-DICTIONARIES.txt` 说明原始数据和许可证随 `public-dictionary-sources.zip`；源码副本也有 `assets/rime/public_sources/`。本轮没有审查实际发布包是否包含该 zip 或等效完整材料。

## 需要明确保留的待审事项

1. **腾讯词向量来源**。白霜 `cn_dicts/tencent.dict.yaml` 文件头明确链接腾讯词向量；雾凇和白霜 `base.dict.yaml` 也列出腾讯词向量。腾讯一手英文页的搜索索引正文同时出现 “for research purpose only” 与 CC BY 3.0 声明，存在需要澄清的表述张力。[腾讯 AI Lab 英文语料页](https://ailab.tencent.com/ailab/nlp/en/embedding.html)。中文原始下载页 `https://ai.tencent.com/ailab/nlp/zh/download.html` 本次不可访问，英文页直接打开返回 502；因此本次取得的是搜索工具返回的一手域名全文缓存，未完成当前在线原页读取。不能把此证据简化成“腾讯数据明确禁止所有商用”，也不能因上游标 GPL 就认为腾讯来源条件已经解决。仍需核实被摘取词表对应的数据版本、下载条款、词表摘取部分的权利范围及上游授权依据；不据文件名判断侵权。
2. **其他基础来源的权利链**。雾凇 `base.dict.yaml` 明列华宇野风系统词库、THUOCL、现代汉语常用词表 gist、腾讯词向量，并参考多部纸质词典；`41448.dict.yaml` 标出 Unicode Unihan 与汉典/pinyin-data；白霜也存在相同或相近来源注释。注释证明来源线索，不能证明每条数据侵权，也不能证明每个原始来源都已提供与 GPL 相容的完整授权。本轮没有逐条回溯这些源许可。
3. **规则适配不是普通资源排除即可覆盖的简单问题**。项目自述规则参考 GPL/LGPL 上游，并给出具体运算类型；仅凭“参考”或规则较短无法断言属于受保护表达的复制，也无法断言完全独立实现。应对锁定原始 schema 与现有规则做表达层面对比，保留来源、许可和修改说明，避免把这些适配规则统一声称为纯 MIT 原创。本次没有完成该对比。
4. **生成词表内部许可边界**。聚合仓库的不同许可证可并存，但单个 `zhixu_public.dict.yaml` 混合并加工了不同来源的条目。应保留来源清单、原始源形式与转换脚本，而不是以“目录独立”推导出其内部许可当然兼容。若处理后的数据被视为改编/组合作品，需逐来源判断适用条件。CC BY 4.0 / GPL 的潜在组合问题本轮未作最终法律定性。
5. **二进制发布另审**。本轮没有审查 vcpkg 实际链接依赖、Windows 安装器内的所有二进制与模型、用户数据/隐私、完整 Corresponding Source 提供方式。原许可证都在不等于发布义务已全部满足。

## Apache 与 GPLv3：已核实的一般规则

GNU 官方许可列表与 Apache 官方说明均认为 Apache 2.0 可与 GPLv3 兼容；Apache 强调 GPLv3 接纳 Apache 代码组成 GPLv3 作品，而不是把 GPLv3 代码改成 Apache。这一规则支持“不能仅凭同时存在 Apache 2.0 与 GPLv3 就判冲突”，也不支持“混合后全部可改 MIT”。具体到本项目仍由各文件的来源和使用方式决定。[GNU 官方列表](https://www.gnu.org/licenses/license-list.html#apache2)、[Apache 官方兼容说明](https://www.apache.org/licenses/GPL-compatibility.html)。

## 证据强度与发布文字建议

- 可以写：“项目自有代码采用 MIT；第三方组件、词库、拼音规则及模型按各自原许可提供，详见 THIRD_PARTY_NOTICES.md 和相邻许可文件。”
- 不宜写：“整个项目/所有资源都是 MIT”“数据开源所以没有版权风险”“GPL 词表必然让整个程序 GPL”“加上 MIT 后已消除第三方风险”。
- 当前最准确的状态是：**MIT 自有代码授权边界可整理，第三方许可材料已存在，原始数据权利链和实际二进制分发尚未完整审查**。来源问题需要具体核实，不是通过改换根 LICENSE 就能解决。

本报告基于只读快照与可得官方材料，不将事实线索升格为侵权结论。
