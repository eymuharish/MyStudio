using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Net;
using System.Runtime.CompilerServices;
using TeleStudio.Contracts;

namespace TeleStudio.Desktop;

public sealed record RoomDirectoryItem(string Id, string Name, string Destination, EndpointProfile EndpointProfile);

public sealed class RoomDirectoryViewModel : INotifyPropertyChanged
{
    private readonly Func<IReadOnlyList<RoomDirectoryItem>, Task> _save;
    private readonly Action<IReadOnlyList<RoomDirectoryItem>>? _publish;
    private readonly IReadOnlyList<RoomDirectoryItem> _original;
    private List<RoomDirectoryItem> _working;
    private string _searchText = string.Empty;
    private string? _selectedEntryId;
    private string? _editingId;
    private string _editorName = string.Empty;
    private string _editorDestination = string.Empty;
    private EndpointProfile _editorEndpoint = EndpointProfile.Auto;
    private string _validationMessage = string.Empty;

    public RoomDirectoryViewModel(
        IReadOnlyList<RoomDirectoryItem> entries,
        Func<IReadOnlyList<RoomDirectoryItem>, Task> save,
        Action<IReadOnlyList<RoomDirectoryItem>>? publish = null)
    {
        ArgumentNullException.ThrowIfNull(entries);
        _save = save ?? throw new ArgumentNullException(nameof(save));
        _publish = publish;
        _original = Array.AsReadOnly(entries.ToArray());
        _working = entries.ToList();
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    public IReadOnlyList<RoomDirectoryItem> WorkingEntries => new ReadOnlyCollection<RoomDirectoryItem>(_working.ToArray());
    public IReadOnlyList<RoomDirectoryItem> VisibleEntries => new ReadOnlyCollection<RoomDirectoryItem>(
        _working.Where(IsMatch).OrderBy(RoomSortKey).ThenBy(entry => entry.Name, StringComparer.OrdinalIgnoreCase).ToArray());
    public RoomDirectoryItem? SelectedEntry => _selectedEntryId is null ? null : _working.FirstOrDefault(entry => entry.Id == _selectedEntryId);

    public string SearchText
    {
        get => _searchText;
        set { if (SetField(ref _searchText, value ?? string.Empty)) OnPropertyChanged(nameof(VisibleEntries)); }
    }
    public string? SelectedEntryId => _selectedEntryId;
    public string EditorName { get => _editorName; set => SetField(ref _editorName, value ?? string.Empty); }
    public string EditorDestination { get => _editorDestination; set => SetField(ref _editorDestination, value ?? string.Empty); }
    public EndpointProfile EditorEndpoint { get => _editorEndpoint; set => SetField(ref _editorEndpoint, value); }
    public string ValidationMessage { get => _validationMessage; private set => SetField(ref _validationMessage, value); }

    public void Select(string id)
    {
        var entry = _working.FirstOrDefault(candidate => candidate.Id == id);
        if (entry is null) return;
        _selectedEntryId = entry.Id;
        _editingId = entry.Id;
        EditorName = entry.Name;
        EditorDestination = entry.Destination;
        EditorEndpoint = entry.EndpointProfile;
        ValidationMessage = string.Empty;
        OnPropertyChanged(nameof(SelectedEntryId));
        OnPropertyChanged(nameof(SelectedEntry));
    }

    public void BeginAdd()
    {
        _editingId = null;
        _selectedEntryId = null;
        EditorName = string.Empty;
        EditorDestination = string.Empty;
        EditorEndpoint = EndpointProfile.Auto;
        ValidationMessage = string.Empty;
        OnPropertyChanged(nameof(SelectedEntryId));
        OnPropertyChanged(nameof(SelectedEntry));
    }

    public bool TryApplyEdit()
    {
        var name = EditorName.Trim();
        var destination = EditorDestination.Trim();
        if (name.Length == 0 || destination.Length == 0 || !Enum.IsDefined(EditorEndpoint))
            return Fail("أدخل اسماً وعنواناً ونوع جهاز صالحاً / Enter a valid name, destination, and endpoint.");
        if (!IsDestination(destination))
            return Fail("العنوان غير صالح / The destination is not valid.");
        if (_working.Any(entry => entry.Id != _editingId && entry.Name.Equals(name, StringComparison.OrdinalIgnoreCase)))
            return Fail("اسم الاستديو يجب أن يكون unique / Studio name must be unique.");
        if (_working.Any(entry => entry.Id != _editingId && entry.Destination.Equals(destination, StringComparison.OrdinalIgnoreCase)))
            return Fail("عنوان الاستديو يجب أن يكون unique / Destination must be unique.");

        var id = _editingId ?? Guid.NewGuid().ToString("N");
        var item = new RoomDirectoryItem(id, name, destination, EditorEndpoint);
        var index = _working.FindIndex(entry => entry.Id == id);
        if (index >= 0) _working[index] = item; else _working.Add(item);
        _editingId = id;
        _selectedEntryId = id;
        EditorName = name;
        EditorDestination = destination;
        ValidationMessage = string.Empty;
        NotifyDirectory();
        return true;
    }

    public bool DeleteSelected(bool confirmed)
    {
        if (!confirmed || _selectedEntryId is null) return false;
        var removed = _working.RemoveAll(entry => entry.Id == _selectedEntryId) == 1;
        if (!removed) return false;
        BeginAdd();
        NotifyDirectory();
        return true;
    }

    public async Task<bool> SaveAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var snapshot = Array.AsReadOnly(_working.ToArray());
        try
        {
            await _save(snapshot);
            cancellationToken.ThrowIfCancellationRequested();
            _publish?.Invoke(snapshot);
            ValidationMessage = string.Empty;
            return true;
        }
        catch (OperationCanceledException) { throw; }
        catch
        {
            ValidationMessage = "لم يتم الحفظ وبقيت تعديلاتك / Changes were not saved and remain editable.";
            return false;
        }
    }

    public void Cancel()
    {
        _working = _original.ToList();
        BeginAdd();
        NotifyDirectory();
    }

    private bool IsMatch(RoomDirectoryItem entry)
    {
        var query = SearchText.Trim();
        return query.Length == 0 || entry.Name.Contains(query, StringComparison.OrdinalIgnoreCase) || entry.Destination.Contains(query, StringComparison.OrdinalIgnoreCase);
    }

    private static int RoomSortKey(RoomDirectoryItem entry) => int.TryParse(entry.Name, out var value) ? value : int.MaxValue;
    private static bool IsDestination(string value) => !value.Any(char.IsWhiteSpace) &&
        (IPAddress.TryParse(value, out _) || Uri.CheckHostName(value) == UriHostNameType.Dns);
    private bool Fail(string message) { ValidationMessage = message; return false; }
    private void NotifyDirectory()
    {
        OnPropertyChanged(nameof(WorkingEntries)); OnPropertyChanged(nameof(VisibleEntries)); OnPropertyChanged(nameof(SelectedEntry)); OnPropertyChanged(nameof(SelectedEntryId));
    }
    private bool SetField<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return false;
        field = value; OnPropertyChanged(name); return true;
    }
    private void OnPropertyChanged([CallerMemberName] string? name = null) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}
