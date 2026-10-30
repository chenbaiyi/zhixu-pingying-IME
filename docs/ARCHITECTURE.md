# 当前架构

```text
应用输入框
    ↕ Windows TSF / COM
ImeTsf.dll
    ↕ 本地命名管道 / Protobuf
InputHost ── Rime + octagram + 词库/上下文模型
    ↕ 本地推荐请求
PredictService ── 语料索引 / 排序 / 个人学习

ZhixuImeBroker：启动与管理后台进程、处理设置控制
ZhixuIME.Settings：WPF 设置界面
```

`InputEngine` 将按键处理、候选、上屏和组合状态抽象为接口，`RimeEngine` 提供实际拼音能力。宿主界面使用 Win32/Direct2D/DirectWrite。通用目录仍含 Windows 路径、安全和 IPC 实现，不是已经完成的跨平台 SDK。

Rime 源码按固定提交下载，本地补丁由 `cmake/` 应用。上下文模型、引擎补丁、公开词库都有来源和摘要记录。个人用户词典与公开词库不同，不纳入源码发布。

未来移动端可复用词库、Rime 方案、引擎改动和部分测试，但系统接入、键盘界面、设置和生命周期需要分别适配 Android/iOS。
