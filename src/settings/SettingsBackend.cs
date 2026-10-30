using System.IO.Pipes;
using System.Security.Principal;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using Google.Protobuf;
using Zhixu.Protocol.V1;

namespace ZhixuIME.Settings;

public sealed record SettingsDocument(uint Version, bool PrivacyMode, bool LearningEnabled,
    bool PredictionEnabled, bool TabAcceptEnabled, uint CandidateCount, string Theme,
    bool FuzzyPinyin, IReadOnlyList<string> ExcludedAppHashes);
public sealed record SettingsPatch(bool? PrivacyMode = null, bool? LearningEnabled = null,
    bool? PredictionEnabled = null, bool? TabAcceptEnabled = null, uint? CandidateCount = null,
    string? Theme = null, bool? FuzzyPinyin = null, IReadOnlyList<string>? ExcludedAppHashes = null);
public sealed record InstalledPackage(string Id, string Name, string Version, bool Enabled)
{
    public string State => Enabled ? "启用" : "停用";
}
public sealed record RuntimeStatus(IReadOnlyList<InstalledPackage>? Packages = null);

public interface ISettingsBackend
{
    Task<SettingsDocument> GetSettingsAsync(CancellationToken cancellationToken);
    Task<SettingsDocument> PatchSettingsAsync(SettingsPatch patch, CancellationToken cancellationToken);
    Task ClearLearningAsync(CancellationToken cancellationToken);
    Task ForgetTextAsync(string textId, CancellationToken cancellationToken);
    Task InstallPackageAsync(string packagePath, CancellationToken cancellationToken);
    Task EnablePackageAsync(string packageId, CancellationToken cancellationToken);
    Task DisablePackageAsync(string packageId, CancellationToken cancellationToken);
    Task RemovePackageAsync(string packageId, CancellationToken cancellationToken);
    Task<RuntimeStatus> GetRuntimeStatusAsync(CancellationToken cancellationToken);
    Task RestartChildAsync(string childRole, CancellationToken cancellationToken);
    Task ExportDiagnosticsAsync(string destinationPath, CancellationToken cancellationToken);
}

public sealed class NamedPipeSettingsBackend : ISettingsBackend
{
    private const uint ProtocolVersion = 1;
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web)
    { DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull, PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower };
    private long _requestId;

    public async Task<SettingsDocument> GetSettingsAsync(CancellationToken token) => DeserializeSettings(await CallAsync(ControlOperation.GetSettings, [], token));
    public async Task<SettingsDocument> PatchSettingsAsync(SettingsPatch patch, CancellationToken token) => DeserializeSettings(await CallAsync(ControlOperation.PatchSettings, JsonSerializer.SerializeToUtf8Bytes(patch, JsonOptions), token));
    public Task ClearLearningAsync(CancellationToken token) => CallAsync(ControlOperation.ClearLearning, [], token);
    public Task ForgetTextAsync(string textId, CancellationToken token) => CallAsync(ControlOperation.ForgetText, Encoding.UTF8.GetBytes(textId), token);
    public Task InstallPackageAsync(string packagePath, CancellationToken token) => CallAsync(ControlOperation.InstallPackage, Encoding.UTF8.GetBytes(packagePath), token);
    public Task EnablePackageAsync(string packageId, CancellationToken token) => CallAsync(ControlOperation.EnablePackage, Encoding.UTF8.GetBytes(packageId), token);
    public Task DisablePackageAsync(string packageId, CancellationToken token) => CallAsync(ControlOperation.DisablePackage, Encoding.UTF8.GetBytes(packageId), token);
    public Task RemovePackageAsync(string packageId, CancellationToken token) => CallAsync(ControlOperation.RemovePackage, Encoding.UTF8.GetBytes(packageId), token);
    public async Task<RuntimeStatus> GetRuntimeStatusAsync(CancellationToken token) => JsonSerializer.Deserialize<RuntimeStatus>(await CallAsync(ControlOperation.GetRuntimeStatus, [], token), JsonOptions) ?? throw new InvalidOperationException("Broker 返回了空的资料包列表。");
    public Task RestartChildAsync(string childRole, CancellationToken token) => CallAsync(ControlOperation.RestartChild, Encoding.UTF8.GetBytes(childRole), token);
    public Task ExportDiagnosticsAsync(string destinationPath, CancellationToken token) => CallAsync(ControlOperation.ExportDiagnostics, Encoding.UTF8.GetBytes(destinationPath), token);

    private async Task<byte[]> CallAsync(ControlOperation operation, byte[] arguments, CancellationToken token)
    {
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(token);
        deadline.CancelAfter(operation == ControlOperation.InstallPackage ? TimeSpan.FromHours(1) : TimeSpan.FromSeconds(15));
        token = deadline.Token;
        var sid = WindowsIdentity.GetCurrent().User?.Value ?? throw new InvalidOperationException("Current user SID is unavailable.");
        await using var pipe = new NamedPipeClientStream(".", $"ZhixuIME.broker-control.{sid}", PipeDirection.InOut, PipeOptions.Asynchronous);
        await pipe.ConnectAsync(token);
        var sessionId = Guid.NewGuid().ToString("N");
        var hello = new Hello { MinVersion = ProtocolVersion, MaxVersion = ProtocolVersion };
        var helloResponse = await CallEnvelopeAsync(pipe, sessionId, MessageType.Hello, hello.ToByteArray(), token);
        if (helloResponse.MessageType != MessageType.HelloAck) throw new InvalidOperationException("Broker rejected the protocol handshake.");
        var command = new ControlCommand { Operation = operation, Arguments = ByteString.CopyFrom(arguments) };
        var response = await CallEnvelopeAsync(pipe, sessionId, MessageType.ControlCommand, command.ToByteArray(), token);
        if (response.MessageType != MessageType.ControlResult) throw new InvalidOperationException("Broker returned an invalid control response.");
        var result = ControlResult.Parser.ParseFrom(response.Payload);
        if (result.ErrorCode != 0) throw new InvalidOperationException($"Broker control request failed with code {result.ErrorCode}.");
        return result.Data.ToByteArray();
    }

    private async Task<Envelope> CallEnvelopeAsync(Stream pipe, string sessionId, MessageType type, byte[] payload, CancellationToken token)
    {
        var envelope = new Envelope { ProtocolVersion = ProtocolVersion, MessageType = type, SessionId = sessionId, RequestId = (ulong)Interlocked.Increment(ref _requestId), Payload = ByteString.CopyFrom(payload) };
        var body = envelope.ToByteArray();
        await pipe.WriteAsync(BitConverter.GetBytes(body.Length), token);
        await pipe.WriteAsync(body, token);
        await pipe.FlushAsync(token);
        var prefix = new byte[sizeof(uint)]; await ReadExactlyAsync(pipe, prefix, token);
        var responseLength = BitConverter.ToUInt32(prefix);
        if (responseLength > 1024 * 1024) throw new InvalidOperationException("Broker response exceeds the protocol limit.");
        var response = new byte[responseLength]; await ReadExactlyAsync(pipe, response, token);
        return Envelope.Parser.ParseFrom(response);
    }

    private static async Task ReadExactlyAsync(Stream stream, byte[] destination, CancellationToken token)
    {
        var offset = 0;
        while (offset < destination.Length) { var read = await stream.ReadAsync(destination.AsMemory(offset), token); if (read == 0) throw new EndOfStreamException("Broker pipe disconnected."); offset += read; }
    }
    private static SettingsDocument DeserializeSettings(byte[] data) => JsonSerializer.Deserialize<SettingsDocument>(data, JsonOptions) ?? throw new InvalidOperationException("Broker returned an empty settings document.");
}
