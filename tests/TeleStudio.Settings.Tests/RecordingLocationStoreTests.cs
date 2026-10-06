namespace TeleStudio.Settings.Tests;

public sealed class RecordingLocationStoreTests
{
    [Fact]
    public void Saves_existing_absolute_folder_and_preserves_selection_after_invalid_request()
    {
        var root=Path.Combine(Path.GetTempPath(),"EssaStudio-recording-location-"+Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            var store=new RecordingLocationStore(Path.Combine(root,"settings"));
            Assert.Null(store.Load());
            store.Save(root);
            Assert.Equal(root,store.Load());
            Assert.Throws<ArgumentException>(()=>store.Save("relative"));
            Assert.Throws<DirectoryNotFoundException>(()=>store.Save(Path.Combine(root,"missing")));
            Assert.Equal(root,store.Load());
        }
        finally {Directory.Delete(root,true);}
    }
}
