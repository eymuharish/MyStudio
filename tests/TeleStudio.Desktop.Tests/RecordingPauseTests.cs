using System.Text;
using TeleStudio.Contracts;
using TeleStudio.Desktop;
namespace TeleStudio.Desktop.Tests;
public sealed class RecordingPauseTests
{
    [Fact]
    public void Recording_library_lists_only_completed_top_level_MP4_and_opens_through_file_boundary()
    {
        var directory = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "MyStudio-library-" + Guid.NewGuid().ToString("N"));
        System.IO.Directory.CreateDirectory(directory);
        try
        {
            System.IO.File.WriteAllBytes(System.IO.Path.Combine(directory, "MyStudio-1.mp4"), [1, 2, 3]);
            System.IO.File.WriteAllBytes(System.IO.Path.Combine(directory, "unfinished.part.mp4"), [1]);
            System.IO.File.WriteAllBytes(System.IO.Path.Combine(directory, "debug.txt"), [1]);
            var boundary = new Boundary(); var actions = new RecordingFileActions(boundary, () => directory);
            var item = Assert.Single(actions.ListCompletedFiles()); Assert.Equal("MyStudio-1.mp4", item.FileName); Assert.Equal(3, item.Bytes);
            Assert.True(actions.TryOpenFile(item.FileName, out _)); Assert.Equal(System.IO.Path.Combine(directory, item.FileName), boundary.Opened);
            Assert.False(actions.TryOpenFile("../outside.mp4", out _));
        }
        finally { System.IO.Directory.Delete(directory, true); }
    }
    private sealed class Boundary : IRecordingFileBoundary
    {
        public string? Opened; public string GetVideosFolder() => throw new InvalidOperationException();
        public bool FileExists(string path) => System.IO.File.Exists(path); public void OpenFile(string path) => Opened = path;
        public void OpenFolder(string path) => Opened = path;
    }

    [Fact]
    public void Native_paused_recording_snapshot_is_accepted()
    {
        var line = Encoding.UTF8.GetBytes("{\"schema\":1,\"type\":\"recording-state\",\"sequence\":1,\"protocol\":\"sip\",\"state\":\"recording\",\"micIncluded\":true,\"roomIncluded\":false,\"elapsedMs\":1500,\"fileName\":\"\",\"paused\":true}\n");
        Assert.True(NativeJsonEvent.TryParse(line, CallProtocol.Sip, out var value, out var reason), reason);
        Assert.True(Assert.IsType<RecordingStateChanged>(value!.Event).Paused);
    }
}
