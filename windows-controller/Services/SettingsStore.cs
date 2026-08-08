using System.Text.Json;
using PadBridge.Controller.Models;

namespace PadBridge.Controller.Services;

public sealed class SettingsStore
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = true
    };

    private readonly string _path = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "PadBridge", "settings.json");

    public AppSettings Load()
    {
        try
        {
            if (!File.Exists(_path)) return new AppSettings();
            var json = File.ReadAllText(_path);
            var settings = JsonSerializer.Deserialize<AppSettings>(json, JsonOptions)
                           ?? new AppSettings();
            using var document = JsonDocument.Parse(json);
            var current = document.RootElement.TryGetProperty(
                              nameof(AppSettings.SettingsVersion), out var version) &&
                          version.TryGetInt32(out var number) && number >= 2;
            if (!current)
            {
                // Version 2 introduces seamless tray startup. Migrate existing
                // 1.0 installs once without overriding later user choices.
                settings.SettingsVersion = 2;
                settings.AutoConnect = true;
                settings.StartWithWindows = true;
                Save(settings);
            }
            return settings;
        }
        catch
        {
            return new AppSettings();
        }
    }

    public void Save(AppSettings settings)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(_path)!);
        var temporary = _path + ".tmp";
        File.WriteAllText(temporary, JsonSerializer.Serialize(settings, JsonOptions));
        File.Move(temporary, _path, true);
    }
}
