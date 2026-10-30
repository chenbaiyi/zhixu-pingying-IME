# 2026-10-30 源码整理说明

- 以实际开发树当前工作文件为准，包含尚未提交 Git 的源码，不以旧的主分支提交代替最新实现。
- 原开发目录、原 Git 历史、安装状态和用户数据均未修改。此目录为独立源码快照，不携带旧 `.git`。
- 保留源码、测试、协议、构建配置、脚本、安装器骨架、公开资源和第三方许可。
- 排除编译缓存、bin/obj、临时文件、会话规划、开发日志、截图、历史源码备份和本机个人输入数据。
- 模型及已有 r19 安装包保存在被忽略的 `.local-assets/`，不会被普通 `git add .` 纳入。
- 未创建 GitHub 仓库、未上传、未提交；初次整理时未新增自有代码许可；随后项目所有者明确选择 MIT，许可范围见根目录 `LICENSE-SCOPE.md`。
- `SOURCE-MANIFEST.json` 记录从开发树原样复制的文件及 SHA-256；新增整理文档单独维护。
- 旧的词库 README 保留了较早版本叙述；当前上下文功能以 `GRAMMAR-SOURCES.json`、现行 schema 和代码为准。

## 上传方式

在本目录执行 `git init` 后，可通过 GitHub Desktop 或命令行提交并连接你创建的仓库。先查看 `git status`，确认没有模型、安装包和个人数据。不要使用强制添加命令绕过 `.gitignore`。

`wanxiang_plain.dict.yaml` 约 70 MB，保留以支持当前构建和离线资源校验；首次推送体积较大，推荐使用 Git 客户端。超过 100 MiB 的本地资源均由忽略规则排除。

这是整理与资源校验交付，不代表在新的路径重新完成了 C++ 全量编译、安装升级或应用内验收。

## 生成资源摘要修正

发现第三方清单中三个产品生成文件仍记录旧摘要：`zhixu_pinyin.dict.yaml`、`zhixu_pinyin.schema.yaml`、`zhixu_pinyin_exact.schema.yaml`。仅在整理副本中将这三个摘要更新为当前文件值，未改词表、方案、上游原始资源或其固定摘要。旧值和新值保存在 `SOURCE-MANIFEST.json` 的 `export_metadata_changes` 中，原开发树未修改。
