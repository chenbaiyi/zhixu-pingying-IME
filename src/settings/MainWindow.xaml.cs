namespace ZhixuIME.Settings;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
        Loaded += async (_, _) =>
        {
            if (DataContext is SettingsViewModel viewModel)
            {
                await viewModel.LoadAsync(CancellationToken.None);
                await viewModel.RefreshPackagesAsync(CancellationToken.None);
            }
        };
    }

    private async void AddExcluded_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new Microsoft.Win32.OpenFileDialog { Title = "选择要排除的程序", Filter = "程序文件|*.exe" };
        if(dialog.ShowDialog(this) == true && DataContext is SettingsViewModel vm)
            await vm.AddExcludedApplicationAsync(dialog.FileName, CancellationToken.None);
    }
    private void RemoveExcluded_Click(object sender, RoutedEventArgs e)
    { if(DataContext is SettingsViewModel vm) vm.RemoveExcludedApplication(); }
    private async void ClearLearning_Click(object sender, RoutedEventArgs e)
    {
        if (DataContext is SettingsViewModel viewModel)
            await viewModel.ClearLearningAsync(CancellationToken.None);
    }

    private async void SaveSettings_Click(object sender, RoutedEventArgs e)
    {
        if (DataContext is SettingsViewModel viewModel)
            await viewModel.SaveAsync(CancellationToken.None);
    }

    private async void ExportDiagnostics_Click(object sender, RoutedEventArgs e)
    {
        if (DataContext is SettingsViewModel viewModel)
            await viewModel.ExportDiagnosticsAsync(CancellationToken.None);
    }
    private async void RefreshPackages_Click(object sender, RoutedEventArgs e)
    { if (DataContext is SettingsViewModel vm) await vm.RefreshPackagesAsync(CancellationToken.None); }
    private async void InstallFolder_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new Microsoft.Win32.OpenFolderDialog { Title = "选择分类资料包所在目录" };
        if (dialog.ShowDialog(this) == true && DataContext is SettingsViewModel vm)
            await vm.InstallPackagesAsync(Directory.GetFiles(dialog.FolderName, "*.imepack"), CancellationToken.None);
    }
    private async void InstallPackages_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new Microsoft.Win32.OpenFileDialog { Title = "选择资料包", Filter = "输入法资料包|*.imepack", Multiselect = true };
        if (dialog.ShowDialog(this) == true && DataContext is SettingsViewModel vm)
            await vm.InstallPackagesAsync(dialog.FileNames, CancellationToken.None);
    }
    private async void EnablePackage_Click(object sender, RoutedEventArgs e)
    { if (DataContext is SettingsViewModel { SelectedPackage: { } p } vm) await vm.EnablePackageAsync(p.Id, CancellationToken.None); }
    private async void DisablePackage_Click(object sender, RoutedEventArgs e)
    { if (DataContext is SettingsViewModel { SelectedPackage: { } p } vm) await vm.DisablePackageAsync(p.Id, CancellationToken.None); }
    private async void RemovePackage_Click(object sender, RoutedEventArgs e)
    { if (DataContext is SettingsViewModel { SelectedPackage: { } p } vm) await vm.RemovePackageAsync(p.Id, CancellationToken.None); }
}
