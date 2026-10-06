using System.Net;
using TeleStudio.Desktop;

namespace TeleStudio.Desktop.Tests;

public sealed class ActiveIpv4AddressCatalogTests
{
    [Fact]
    public void Filter_returns_only_active_routable_ipv4_addresses_with_adapter_names()
    {
        var candidates = new[]
        {
            new NetworkAddressCandidate("Ethernet", true, false, IPAddress.Parse("10.136.51.100")),
            new NetworkAddressCandidate("Wi-Fi", true, false, IPAddress.Parse("192.168.1.8")),
            new NetworkAddressCandidate("Loopback adapter", true, true, IPAddress.Parse("127.0.0.1")),
            new NetworkAddressCandidate("Automatic address", true, false, IPAddress.Parse("169.254.10.20")),
            new NetworkAddressCandidate("Offline adapter", false, false, IPAddress.Parse("172.16.0.9")),
            new NetworkAddressCandidate("IPv6 adapter", true, false, IPAddress.Parse("2001:db8::5")),
            new NetworkAddressCandidate("Unspecified", true, false, IPAddress.Any)
        };

        var addresses = ActiveIpv4AddressCatalog.Filter(candidates);

        Assert.Collection(addresses,
            first => Assert.Equal(new ActiveIpv4Address("Ethernet", "10.136.51.100"), first),
            second => Assert.Equal(new ActiveIpv4Address("Wi-Fi", "192.168.1.8"), second));
    }
}
