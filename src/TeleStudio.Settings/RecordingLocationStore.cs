using System.Text.Json;

namespace TeleStudio.Settings;

public sealed class RecordingLocationStore(string settingsDirectory)
{
    private string FilePath => Path.Combine(settingsDirectory,"recording-location.json");
    public static RecordingLocationStore ForCurrentUser() => new(AppDataPaths.Root);
    public string? Load()
    {
        if (!File.Exists(FilePath)) return null;
        var path=JsonSerializer.Deserialize<string>(File.ReadAllText(FilePath));
        if (string.IsNullOrWhiteSpace(path) || !Path.IsPathFullyQualified(path)) throw new InvalidDataException("Saved recording directory is invalid.");
        if (!Directory.Exists(path)) throw new DirectoryNotFoundException("Recording directory unavailable: "+path);
        return Path.GetFullPath(path);
    }
    public void Save(string path)
    {
        if (!Path.IsPathFullyQualified(path)) throw new ArgumentException("Choose an absolute recording directory.");
        path=Path.GetFullPath(path);
        if (!Directory.Exists(path)) throw new DirectoryNotFoundException(path);
        var probe=Path.Combine(path,"EssaStudio-write-test-"+Guid.NewGuid().ToString("N")+".tmp");
        using(new FileStream(probe,FileMode.CreateNew,FileAccess.Write,FileShare.None,1,FileOptions.DeleteOnClose)) { }
        Directory.CreateDirectory(settingsDirectory);
        var temporary=FilePath+"."+Guid.NewGuid().ToString("N")+".tmp";
        try {File.WriteAllText(temporary,JsonSerializer.Serialize(path));File.Move(temporary,FilePath,true);}
        finally {if(File.Exists(temporary)) File.Delete(temporary);}
    }
}
