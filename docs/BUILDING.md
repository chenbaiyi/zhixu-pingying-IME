# 构建说明

## 环境

- Windows x64；当前 CMake 明确要求 MSVC。
- Visual Studio 2026 C++ 桌面开发工具及 Windows 10/11 SDK。现有 `bootstrap.ps1` 按 18.x 查找工具链；使用其他版本需调整该脚本。
- PowerShell 7、CMake 3.28+、Ninja、vcpkg；托管设置界面需要 .NET 8 SDK。
- Python 3.11+，用于模型校验和词库工具。
- 首次依赖还原需要联网；运行输入法不依赖这些构建下载。

在项目根目录打开 PowerShell。已有脚本会为含中文的源码路径创建临时 ASCII 路径映射，TEMP 自身需要是 ASCII 路径。

## 1. 准备上下文模型

整理交付目录已经附带本地模型，先执行：

```powershell
./scripts/restore-local-model.ps1
```

从 GitHub 克隆的源码不包含 `.local-assets`，改用：

```powershell
python scripts/fetch-context-model.py --fetch
```

下载脚本按 `GRAMMAR-SOURCES.json` 检查长度和 SHA-256。模型来自上游 LTS 发布地址，该地址内容可能更新；校验不匹配时应寻找对应的已锁定模型，不能绕过校验或直接更新摘要。模型约 418 MB，目标为 `assets/rime/full_pinyin/wanxiang-lts-zh-hans.gram`，该文件也被 Git 忽略。

```powershell
python scripts/fetch-context-model.py --check
```

## 2. 基础构建

```powershell
./scripts/bootstrap.ps1
./scripts/build.ps1 -Configuration Debug
./scripts/test.ps1 -Configuration Debug
```

注意：`build.ps1` 默认使用不启用 librime 的基础 preset。完整拼音需继续以下步骤。

## 3. 启用完整拼音

在同一个 PowerShell 进程内执行，以沿用基础构建脚本设置的工具链环境及构建目录：

```powershell
./scripts/build.ps1 -Configuration Debug
$tools = ./scripts/bootstrap.ps1 -AsObject
cmake --preset x64-debug-librime "-DCMAKE_MAKE_PROGRAM=$($tools.Ninja)" "-DCMAKE_CXX_COMPILER=$($tools.Compiler)"
if ($LASTEXITCODE -ne 0) { throw 'Configure failed' }
cmake --build --preset x64-debug-librime --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
ctest --preset x64-debug-librime --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
```

首次完整编译可能下载和编译较多依赖。构建产物位于 `$env:ZHIXU_BUILD_ROOT` 对应 preset 子目录。

## 4. 托管设置界面

```powershell
dotnet build src/settings/ZhixuIME.Settings.csproj -c Release
dotnet test tests/settings/ZhixuIME.Settings.Tests.csproj -c Release
```

## 安装与发布

编译不自动注册 TSF，也不替换正在运行的输入法。`scripts/deploy-*.ps1`、`switch-tsf.ps1` 等是历史开发维护工具，涉及运行目录和注册项，需先阅读参数。`installer/Package.wxs` 是安装器骨架，不代表能直接生成现有 r19 EXE。

整理目录保留现有 r19 安装包于 `.local-assets/releases/`，该目录不进入 Git；需要对外提供二进制时另行上传到 Releases。源码整理未补建完整的历史 EXE 打包流水线。
