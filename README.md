# 知序输入法 · Zhixu IME

面向 Windows 的中文拼音输入法，采用 C++20、Windows TSF、Rime 和 WPF 设置界面。当前目录是 2026-10-30 整理的开发源码快照，包含 r19 的学习与上下文排序修正；尚无 Android 或 iOS 实现。

## 已有能力

- 全拼、简拼、模糊音、候选翻页和个人词典学习。
- 基于公开词库的中文词汇，以及离线 octagram 上下文模型。
- 候选窗口、推荐窗口、智能标点和输入设置。
- TSF 前端、输入引擎宿主、进程管理和本地推荐服务。

## 目录

| 路径 | 内容 |
| --- | --- |
| `src/tsf` | Windows TSF 接入和编辑会话 |
| `src/input_host` | Rime 适配、输入会话和候选界面 |
| `src/broker` | 进程管理和设置控制 |
| `src/predict`、`src/predict_service` | 本地检索、排序和个人学习 |
| `src/settings` | .NET 8 WPF 设置程序 |
| `assets/rime` | 方案、运行词表、上游原始词表和许可证 |
| `third_party` | octagram 组件及 librime 本地补丁 |
| `cmake`、`scripts` | 构建、资源校验和开发辅助脚本 |
| `tests`、`testdata` | 原生及托管测试、测试数据 |
| `installer` | 原有 WiX 安装器骨架，尚非完整 r19 发布流水线 |
| `docs` | 构建、架构、来源和整理说明 |

## 开始开发

详见 [构建说明](docs/BUILDING.md)、[架构说明](docs/ARCHITECTURE.md) 和 [整理说明](docs/EXPORT-NOTES.md)。

完整拼音版本需要单独准备模型。模型和现有安装包放在本地 `.local-assets/` 中，由 `.gitignore` 排除；上传源码时请保留此规则。克隆 GitHub 仓库后可按构建说明下载并校验模型。

## 当前状态

这是开发快照，尚未建立完整的干净机器构建与安装发布流程。历史 r19 原生回归有一项与旧版相同的首键冷加载超时失败；不代表所有测试通过。本次整理只校验文件和资源完整性，没有重新执行整套 C++ 构建或应用内验收。

## 许可与来源

项目作者有权授权的原创程序代码采用 [MIT License](LICENSE)，允许按 MIT 条件商用及闭源复用。具体范围见 [许可范围](LICENSE-SCOPE.md)。

第三方代码、词库、适配规则和模型保留原许可证，详见 [第三方说明](THIRD_PARTY_NOTICES.md)。GPL 来源及原始外部数据授权仍有待核实事项，见 [许可审查](docs/LICENSE-REVIEW.md)；本项目不能整体概括为“全部 MIT、无条件闭源商用”。
