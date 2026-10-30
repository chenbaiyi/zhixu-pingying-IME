# 第三方代码与数据

本目录不使用单一许可证覆盖全部内容。请保留原作者、来源、许可证、修改说明和校验清单。

| 内容 | 来源记录 | 许可记录 |
| --- | --- | --- |
| librime 引擎及本地补丁 | `third_party.lock.json`、`cmake/patch_librime_*.cmake` | BSD-3-Clause，`assets/rime/full_pinyin/RIME-ENGINE-LICENSE` |
| librime-octagram | `assets/rime/full_pinyin/GRAMMAR-SOURCES.json` | BSD-3-Clause，`third_party/octagram/LICENSE` |
| 上下文模型 | `assets/rime/full_pinyin/GRAMMAR-SOURCES.json` | CC BY 4.0，`GRAMMAR-MODEL-LICENSE` |
| 万象词库及处理后词表 | `third_party.lock.json`、`assets/rime/wanxiang_source/` | CC BY 4.0，`WANXIANG-LICENSE`、`WANXIANG-ATTRIBUTION.txt` |
| 原始简体拼音词表 | `third_party.lock.json` | Apache-2.0，`assets/rime/full_pinyin/LICENSE` 和 `AUTHORS` |
| 雾凇、白霜等公开增补词表 | `PUBLIC-DICTIONARIES.json`、`assets/rime/public_sources/` | 对应 GPL-3.0 文本与各来源文件中的说明 |
| 拼音规则参考 | `PINYIN-RULES-SOURCES.md` | 对应 `PINYIN-RULES-*-LICENSE` |

以上文件名未写目录时均位于 `assets/rime/full_pinyin/`。运行词表包含不同来源的数据，不能统一标为 MIT 或其他单一许可。上游原始词表及其许可随源码保留。

vcpkg 依赖见 `vcpkg.json` 与配置文件；分发编译后的二进制时还需汇集实际依赖所需的声明。本清单记录现有材料，不是对整套二进制分发许可的最终审查。

项目所有者已于 2026-10-30 为有权授权的原创代码选择 MIT，见根目录 `LICENSE` 与 `LICENSE-SCOPE.md`。本声明不将上表任何第三方内容重新许可为 MIT。

`zhixu_public.dict.yaml` 是处理和合并后的混合来源词表，不是单纯未修改的上游数据。原始输入文件、处理脚本、来源清单和各项许可证已随源码保留；适用的 GPL、CC BY 等条件继续存在。方案中的规则适配也不因根目录 MIT 而免除上游条件。

部分词表注释还列出了腾讯词向量、华宇野风、THUOCL、维基词典及纸质词典参考等更早来源。上游仓库声明许可并不能替代对所有原始数据权利链的核实。本轮未完成该核实，具体证据与分发待办见 `docs/LICENSE-REVIEW.md`。

现有 r19 安装包未因本次添加 MIT 而重新打包，其二进制许可材料与完整可复现构建发布链仍需单独核验。
