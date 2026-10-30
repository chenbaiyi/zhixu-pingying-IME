using System.IO;
using ZhixuIME.Settings;
using Xunit;

namespace ZhixuIME.Settings.Tests;

public sealed class SettingsViewModelTests
{
    [Fact]
    public async Task ClearLearningDoesNotRemoveInstalledPackages()
    {
        var backend = new FakeSettingsBackend();
        var viewModel = new SettingsViewModel(backend);

        await viewModel.ClearLearningAsync(CancellationToken.None);

        Assert.Equal(1, backend.ClearLearningCalls);
        Assert.Equal(0, backend.RemovePackageCalls);
        Assert.Contains("语料包未移除", viewModel.Status);
    }

    [Fact]
    public async Task BrokerUnavailableLeavesDefaultsAndReportsDegradedState()
    {
        var viewModel = new SettingsViewModel(new FakeSettingsBackend { GetSettingsException = new IOException() });

        await viewModel.LoadAsync(CancellationToken.None);

        Assert.False(viewModel.Current.PrivacyMode);
        Assert.Contains("Broker 不可用", viewModel.Status);
    }

    [Fact]
    public async Task UnknownSettingsVersionIsReportedWithoutTreatingItAsCurrent()
    {
        var backend = new FakeSettingsBackend { Settings = new SettingsDocument(2, true, false, false, false, 5, "system", false, []) };
        var viewModel = new SettingsViewModel(backend);

        await viewModel.LoadAsync(CancellationToken.None);

        Assert.Contains("未知设置版本", viewModel.Status);
    }

    [Fact]
    public async Task SaveRoutesEditedSettingsThroughBrokerPatch()
    {
        var backend = new FakeSettingsBackend();
        var viewModel = new SettingsViewModel(backend);
        await viewModel.LoadAsync(CancellationToken.None);
        viewModel.PrivacyMode = true;

        await viewModel.SaveAsync(CancellationToken.None);

        Assert.Equal(1, backend.PatchCalls);
        Assert.True(backend.LastPatch!.PrivacyMode);
    }

    [Fact]
    public async Task DiagnosticsCancellationIsReportedAsCancellation()
    {
        using var cancelled = new CancellationTokenSource();
        cancelled.Cancel();
        var viewModel = new SettingsViewModel(new FakeSettingsBackend());

        await viewModel.ExportDiagnosticsAsync(cancelled.Token);

        Assert.Contains("已取消", viewModel.Status);
    }

    [Fact]
    public async Task PackageFolderInstallationAndManagementGoThroughBroker()
    {
        var backend = new FakeSettingsBackend();
        var viewModel = new SettingsViewModel(backend);
        await viewModel.InstallPackagesAsync(new[] { "poetry.imepack", "writing.imepack" }, CancellationToken.None);
        await viewModel.DisablePackageAsync("com.zhixu.test", CancellationToken.None);
        await viewModel.EnablePackageAsync("com.zhixu.test", CancellationToken.None);
        await viewModel.RemovePackageAsync("com.zhixu.test", CancellationToken.None);
        Assert.Equal(2, backend.InstallCalls);
        Assert.Equal(1, backend.DisableCalls);
        Assert.Equal(1, backend.EnableCalls);
        Assert.Equal(1, backend.RemovePackageCalls);
    }

    [Fact]
    public void WireSettingsUseBrokersSnakeCaseNames()
    {
        var type = typeof(NamedPipeSettingsBackend);
        var options = (System.Text.Json.JsonSerializerOptions)type.GetField("JsonOptions", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static)!.GetValue(null)!;
        var patch = System.Text.Json.JsonSerializer.Serialize(new SettingsPatch(PrivacyMode: true, TabAcceptEnabled: true, CandidateCount: 9, FuzzyPinyin: true, ExcludedAppHashes: new[] { "abc" }), options);
        using var json = System.Text.Json.JsonDocument.Parse(patch);
        Assert.True(json.RootElement.TryGetProperty("privacy_mode", out _));
        Assert.True(json.RootElement.TryGetProperty("tab_accept_enabled", out _));
        Assert.True(json.RootElement.TryGetProperty("excluded_app_hashes", out _));
        var wire = "{\"version\":1,\"privacy_mode\":true,\"learning_enabled\":false,\"prediction_enabled\":false,\"tab_accept_enabled\":true,\"candidate_count\":9,\"theme\":\"dark\",\"fuzzy_pinyin\":true,\"excluded_app_hashes\":[\"abc\"]}";
        var value = System.Text.Json.JsonSerializer.Deserialize<SettingsDocument>(wire, options)!;
        Assert.True(value.PrivacyMode); Assert.True(value.TabAcceptEnabled); Assert.True(value.FuzzyPinyin);
        Assert.False(value.LearningEnabled); Assert.Equal(9u, value.CandidateCount); Assert.Equal("abc", Assert.Single(value.ExcludedAppHashes));
    }

    [Fact]
    public async Task GeneralSettingsAndExclusionCanBeEditedWithoutDirectFileWrites()
    {
        var backend = new FakeSettingsBackend();
        var vm = new SettingsViewModel(backend);
        foreach (var name in new[] { "CandidateCount", "Theme", "ExcludedAppHashes" })
            Assert.True(typeof(SettingsViewModel).GetProperty(name)?.CanWrite == true, name);
        typeof(SettingsViewModel).GetProperty("CandidateCount")!.SetValue(vm, 9u);
        typeof(SettingsViewModel).GetProperty("Theme")!.SetValue(vm, "dark");
        typeof(SettingsViewModel).GetProperty("ExcludedAppHashes")!.SetValue(vm, new[] { "abc" });
        await vm.SaveAsync(CancellationToken.None);
        Assert.Equal(9u, backend.LastPatch!.CandidateCount);
        Assert.Equal("dark", backend.LastPatch.Theme);
        Assert.Equal("abc", Assert.Single(backend.LastPatch.ExcludedAppHashes!));
    }

    [Fact]
    public async Task LoadedSettingsNotifyAllVisibleBindingsAndExclusionsUseFileHash()
    {
        var backend = new FakeSettingsBackend { Settings = new SettingsDocument(1, false, true, true, false, 9, "dark", true, new[] { "existing" }) };
        var vm = new SettingsViewModel(backend); var events = new List<string?>();
        vm.PropertyChanged += (_, e) => events.Add(e.PropertyName);
        await vm.LoadAsync(CancellationToken.None);
        Assert.Contains("CandidateCount", events); Assert.Contains("Theme", events); Assert.Contains("ExcludedAppHashes", events);
        var file = Path.Combine(Path.GetTempPath(), Guid.NewGuid()+".exe");
        try {
            await File.WriteAllTextAsync(file, "abc");
            await vm.AddExcludedApplicationAsync(file, CancellationToken.None);
            await vm.AddExcludedApplicationAsync(file, CancellationToken.None);
            Assert.Contains("existing", vm.ExcludedAppHashes);
            Assert.Equal(2, vm.ExcludedAppHashes.Count);
            Assert.Contains("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", vm.ExcludedAppHashes);
        } finally { File.Delete(file); }
    }

    private sealed class FakeSettingsBackend : ISettingsBackend
    {
        public SettingsDocument Settings { get; set; } = new(1, false, true, true, false, 5, "system", false, []);
        public Exception? GetSettingsException { get; set; }
        public int ClearLearningCalls { get; private set; }
        public int RemovePackageCalls { get; private set; }
        public int PatchCalls { get; private set; }
        public SettingsPatch? LastPatch { get; private set; }
        public Task<SettingsDocument> GetSettingsAsync(CancellationToken token) => GetSettingsException is null ? Task.FromResult(Settings) : Task.FromException<SettingsDocument>(GetSettingsException);
        public Task<SettingsDocument> PatchSettingsAsync(SettingsPatch patch, CancellationToken token) { PatchCalls++; LastPatch = patch; return Task.FromResult(Settings); }
        public Task ClearLearningAsync(CancellationToken token) { token.ThrowIfCancellationRequested(); ClearLearningCalls++; return Task.CompletedTask; }
        public Task ForgetTextAsync(string textId, CancellationToken token) => Task.CompletedTask;
        public int InstallCalls, EnableCalls, DisableCalls;
        public Task InstallPackageAsync(string packagePath, CancellationToken token) { InstallCalls++; return Task.CompletedTask; }
        public Task EnablePackageAsync(string packageId, CancellationToken token) { EnableCalls++; return Task.CompletedTask; }
        public Task DisablePackageAsync(string packageId, CancellationToken token) { DisableCalls++; return Task.CompletedTask; }
        public Task RemovePackageAsync(string packageId, CancellationToken token) { RemovePackageCalls++; return Task.CompletedTask; }
        public Task<RuntimeStatus> GetRuntimeStatusAsync(CancellationToken token) => Task.FromResult(new RuntimeStatus());
        public Task RestartChildAsync(string childRole, CancellationToken token) => Task.CompletedTask;
        public Task ExportDiagnosticsAsync(string destinationPath, CancellationToken token) { token.ThrowIfCancellationRequested(); return Task.CompletedTask; }
    }
}
