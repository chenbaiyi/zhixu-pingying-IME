using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace ZhixuIME.Settings;

public sealed class SettingsViewModel : INotifyPropertyChanged
{
    private readonly ISettingsBackend _backend;
    private SettingsDocument _current = new(1, false, true, true, false, 5, "system", false, []);
    private string _status = "正在连接 Broker…";
    public SettingsViewModel(ISettingsBackend backend) => _backend = backend;
    public SettingsDocument Current { get => _current; private set => Set(ref _current, value); }
    public string Status { get => _status; private set => Set(ref _status, value); }
    public uint[] CandidateCounts { get; } = [1,2,3,4,5,6,7,8,9];
    public uint CandidateCount { get => Current.CandidateCount; set { if(value is >= 1 and <= 9) UpdateCurrent(Current with { CandidateCount = value }); } }
    public string Theme { get => Current.Theme; set { if(value is "system" or "light" or "dark") UpdateCurrent(Current with { Theme = value }); } }
    public IReadOnlyList<string> ExcludedAppHashes { get => Current.ExcludedAppHashes; set => UpdateCurrent(Current with { ExcludedAppHashes = value.Distinct(StringComparer.OrdinalIgnoreCase).Select(x => x.ToLowerInvariant()).ToArray() }); }
    public string ExcludedHashesText => string.Join(Environment.NewLine, Current.ExcludedAppHashes);
    public string? SelectedExcludedHash { get; set; }
    public async Task AddExcludedApplicationAsync(string path, CancellationToken token)
    {
        try {
            await using var stream = File.OpenRead(path);
            var hash = Convert.ToHexString(await System.Security.Cryptography.SHA256.HashDataAsync(stream, token)).ToLowerInvariant();
            ExcludedAppHashes = Current.ExcludedAppHashes.Append(hash).ToArray();
            Status = "已加入排除列表；保存设置后生效。程序升级后需要重新选择。";
        } catch (OperationCanceledException) { Status = "操作已取消。"; }
          catch (Exception) { Status = "无法读取程序文件；排除列表未修改。"; }
    }
    public void RemoveExcludedApplication()
    { if (SelectedExcludedHash is { } hash) ExcludedAppHashes = Current.ExcludedAppHashes.Where(x => x != hash).ToArray(); }
    public bool PrivacyMode { get => Current.PrivacyMode; set => UpdateCurrent(Current with { PrivacyMode = value }); }
    public bool LearningEnabled { get => Current.LearningEnabled; set => UpdateCurrent(Current with { LearningEnabled = value }); }
    public bool PredictionEnabled { get => Current.PredictionEnabled; set => UpdateCurrent(Current with { PredictionEnabled = value }); }
    public bool TabAcceptEnabled { get => Current.TabAcceptEnabled; set => UpdateCurrent(Current with { TabAcceptEnabled = value }); }
    public bool FuzzyPinyin { get => Current.FuzzyPinyin; set => UpdateCurrent(Current with { FuzzyPinyin = value }); }
    public async Task LoadAsync(CancellationToken token) { try { UpdateCurrent(await _backend.GetSettingsAsync(token)); Status = Current.Version == 1 ? "已连接 Broker。" : "Broker 返回了未知设置版本。"; } catch (Exception) when (!token.IsCancellationRequested) { Status = "Broker 不可用；设置未写入本地文件。"; } }
    public async Task SaveAsync(CancellationToken token) { try { UpdateCurrent(await _backend.PatchSettingsAsync(new SettingsPatch(Current.PrivacyMode, Current.LearningEnabled, Current.PredictionEnabled, Current.TabAcceptEnabled, Current.CandidateCount, Current.Theme, Current.FuzzyPinyin, Current.ExcludedAppHashes), token)); Status = "设置已保存到 Broker。"; } catch (Exception) when (!token.IsCancellationRequested) { Status = "未能保存设置。"; } }
    public async Task ClearLearningAsync(CancellationToken token) { try { await _backend.ClearLearningAsync(token); Status = "已清除本地学习数据；语料包未移除。"; } catch (Exception) when (!token.IsCancellationRequested) { Status = "未能清除学习数据。"; } }
    public async Task ExportDiagnosticsAsync(CancellationToken token) { try { await _backend.ExportDiagnosticsAsync(string.Empty, token); Status = "诊断导出已交由 Broker 处理。"; } catch (OperationCanceledException) when (token.IsCancellationRequested) { Status = "诊断导出已取消。"; } catch (Exception) { Status = "未能导出诊断。"; } }
    public ObservableCollection<InstalledPackage> Packages { get; } = [];
    private InstalledPackage? _selectedPackage;
    public InstalledPackage? SelectedPackage { get => _selectedPackage; set => Set(ref _selectedPackage, value); }
    private bool _packageBusy;
    public bool CanManagePackages => !_packageBusy;
    private async Task LoadPackagesAsync(CancellationToken token)
    {
        var status = await _backend.GetRuntimeStatusAsync(token);
        Packages.Clear();
        foreach (var package in status.Packages ?? []) Packages.Add(package);
    }
    public async Task RefreshPackagesAsync(CancellationToken token)
    {
        try { await LoadPackagesAsync(token); Status = $"已安装 {Packages.Count} 个资料包分卷。"; }
        catch (Exception) when (!token.IsCancellationRequested) { Status = "无法获取资料包状态，请确认输入法服务正在运行。"; }
    }
    private async Task ManagePackageAsync(Func<Task> operation, string success, CancellationToken token)
    {
        if (_packageBusy) return;
        _packageBusy = true; PropertyChanged?.Invoke(this, new(nameof(CanManagePackages)));
        try { await operation(); await LoadPackagesAsync(token); Status = success; }
        catch (OperationCanceledException) { Status = "操作已取消或等待服务超时；请刷新资料包状态。"; }
        catch (Exception) { Status = "操作未全部完成；请刷新列表查看已安装的分卷。"; }
        finally { _packageBusy = false; PropertyChanged?.Invoke(this, new(nameof(CanManagePackages))); }
    }
    public Task InstallPackagesAsync(IEnumerable<string> paths, CancellationToken token) => ManagePackageAsync(async () =>
    {
        var files = paths.Distinct().OrderBy(x => x, StringComparer.Ordinal).ToArray();
        if (files.Length == 0) throw new InvalidOperationException("没有资料包");
        for (var index = 0; index < files.Length; index++)
        {
            Status = $"正在安装 {index + 1}/{files.Length}，构建索引期间可继续输入。";
            await _backend.InstallPackageAsync(files[index], token);
        }
    }, "所选资料包已安装并启用。", token);
    public Task EnablePackageAsync(string id, CancellationToken token) => ManagePackageAsync(() => _backend.EnablePackageAsync(id, token), "资料包已启用。", token);
    public Task DisablePackageAsync(string id, CancellationToken token) => ManagePackageAsync(() => _backend.DisablePackageAsync(id, token), "资料包已停用。", token);
    public Task RemovePackageAsync(string id, CancellationToken token) => ManagePackageAsync(() => _backend.RemovePackageAsync(id, token), "资料包已卸载。", token);
    public event PropertyChangedEventHandler? PropertyChanged;
    private void UpdateCurrent(SettingsDocument value) { Current = value; PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(PrivacyMode))); PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(LearningEnabled))); PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(PredictionEnabled))); PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(TabAcceptEnabled))); PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(FuzzyPinyin))); foreach(var name in new[] { nameof(CandidateCount), nameof(Theme), nameof(ExcludedAppHashes), nameof(ExcludedHashesText) }) PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name)); }
    private void Set<T>(ref T field, T value, [CallerMemberName] string? name = null) { if (EqualityComparer<T>.Default.Equals(field, value)) return; field = value; PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name)); }
}
