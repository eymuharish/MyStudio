using System.Collections;

namespace TeleStudio.Desktop;

/// <summary>Permanent UI labels remain owned; transient dialog bindings have a scope.</summary>
public sealed class TranslationBindings : IEnumerable<(Action<string> Set, string Ar, string En)>
{
    private sealed record Entry((Action<string> Set, string Ar, string En) Binding, Scope? Owner);
    private readonly List<Entry> _entries = [];
    private Scope? _active;
    public int Count => _entries.Count;
    public void Add((Action<string> Set, string Ar, string En) entry) => _entries.Add(new(entry, _active));
    public IDisposable BeginScope()
    {
        var scope = new Scope(this, _active); _active = scope; return scope;
    }
    public IEnumerator<(Action<string> Set, string Ar, string En)> GetEnumerator() => _entries.Select(x => x.Binding).GetEnumerator();
    IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();
    private sealed class Scope(TranslationBindings owner, Scope? parent) : IDisposable
    {
        private bool _disposed;
        public void Dispose()
        {
            if (_disposed) return;
            if (!ReferenceEquals(owner._active, this)) throw new InvalidOperationException("Translation scopes must close in reverse opening order.");
            _disposed = true;
            owner._entries.RemoveAll(entry => ReferenceEquals(entry.Owner, this));
            owner._active = parent;
        }
    }
}
