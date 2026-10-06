using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;

namespace TeleStudio.Desktop;

internal readonly record struct NetworkAddressCandidate(
    string AdapterName,
    bool IsOperational,
    bool IsLoopbackAdapter,
    IPAddress Address);

internal readonly record struct ActiveIpv4Address(string AdapterName, string Address);

internal static class ActiveIpv4AddressCatalog
{
    internal static IReadOnlyList<ActiveIpv4Address> GetActive()
    {
        var candidates = new List<NetworkAddressCandidate>();
        foreach (var adapter in NetworkInterface.GetAllNetworkInterfaces())
        {
            var isOperational = adapter.OperationalStatus == OperationalStatus.Up;
            var isLoopback = adapter.NetworkInterfaceType == NetworkInterfaceType.Loopback;
            try
            {
                candidates.AddRange(adapter.GetIPProperties().UnicastAddresses.Select(address =>
                    new NetworkAddressCandidate(adapter.Name, isOperational, isLoopback, address.Address)));
            }
            catch (NetworkInformationException)
            {
                // A broken virtual adapter must not hide valid addresses from other adapters.
            }
        }

        return Filter(candidates);
    }

    internal static IReadOnlyList<ActiveIpv4Address> Filter(IEnumerable<NetworkAddressCandidate> candidates) =>
        candidates
            .Where(candidate => candidate.IsOperational && !candidate.IsLoopbackAdapter && IsUsableIpv4(candidate.Address))
            .Select(candidate => new ActiveIpv4Address(candidate.AdapterName, candidate.Address.ToString()))
            .Distinct()
            .OrderBy(address => address.AdapterName, StringComparer.CurrentCultureIgnoreCase)
            .ThenBy(address => IPAddress.Parse(address.Address).GetAddressBytes(), ByteArrayComparer.Instance)
            .ToArray();

    private static bool IsUsableIpv4(IPAddress address)
    {
        if (address.AddressFamily != AddressFamily.InterNetwork ||
            IPAddress.IsLoopback(address) ||
            address.Equals(IPAddress.Any) ||
            address.Equals(IPAddress.None))
        {
            return false;
        }

        var bytes = address.GetAddressBytes();
        return bytes[0] != 169 || bytes[1] != 254;
    }

    private sealed class ByteArrayComparer : IComparer<byte[]>
    {
        internal static readonly ByteArrayComparer Instance = new();

        public int Compare(byte[]? left, byte[]? right)
        {
            if (ReferenceEquals(left, right)) return 0;
            if (left is null) return -1;
            if (right is null) return 1;
            for (var index = 0; index < Math.Min(left.Length, right.Length); index++)
            {
                var comparison = left[index].CompareTo(right[index]);
                if (comparison != 0) return comparison;
            }
            return left.Length.CompareTo(right.Length);
        }
    }
}
