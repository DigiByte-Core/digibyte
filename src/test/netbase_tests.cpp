// Copyright (c) 2012-2022 The Bitcoin Core developers
// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <net_permissions.h>
#include <netaddress.h>
#include <netbase.h>
#include <netgroup.h>
#include <logging.h>
#include <protocol.h>
#include <serialize.h>
#include <streams.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <util/translation.h>
#include <version.h>

#include <string>
#include <utility>
#include <vector>

#include <boost/test/unit_test.hpp>

using namespace std::literals;

BOOST_FIXTURE_TEST_SUITE(netbase_tests, BasicTestingSetup)

namespace {

class Socks5InterruptGuard
{
public:
    explicit Socks5InterruptGuard(bool interrupt)
        : m_previous{InterruptSocks5(interrupt)}
    {
    }
    ~Socks5InterruptGuard() { InterruptSocks5(m_previous); }

private:
    const bool m_previous;
};

class RecordingSocks5Sock : public StaticContentsSock
{
public:
    using StaticContentsSock::StaticContentsSock;

    ssize_t Send(const void* data, size_t len, int) const override
    {
        writes.emplace_back(static_cast<const char*>(data), len);
        return len;
    }

    mutable std::vector<std::string> writes;
};

std::string Socks5Response(bool authenticate, uint8_t reply = 0x00)
{
    std::string response;
    response.push_back('\x05');
    response.push_back(authenticate ? '\x02' : '\x00');
    if (authenticate) {
        response.push_back('\x01');
        response.push_back('\x00');
    }
    response.push_back('\x05');
    response.push_back(static_cast<char>(reply));
    response.push_back('\x00');
    response.push_back('\x01');
    if (reply == 0x00) {
        response.append(4, '\0');
        response.append(2, '\0');
    }
    return response;
}

std::pair<bool, std::string> RunSocks5AndCaptureLog(
    const std::string& response, const std::string& destination, uint16_t port,
    const ProxyCredentials* credentials, ProxyLogPolicy log_policy)
{
    Socks5InterruptGuard interrupt_guard{/*interrupt=*/false};
    const bool net_was_enabled{LogInstance().WillLogCategory(BCLog::NET)};
    const bool proxy_was_enabled{LogInstance().WillLogCategory(BCLog::PROXY)};
    LogInstance().EnableCategory(BCLog::NET);
    LogInstance().EnableCategory(BCLog::PROXY);

    std::string log;
    auto callback = LogInstance().PushBackCallback(
        [&log](const std::string& line) { log += line; });
    StaticContentsSock sock{response};
    const bool success{Socks5(destination, port, credentials, sock, log_policy)};
    LogInstance().DeleteCallback(callback);

    if (!net_was_enabled) LogInstance().DisableCategory(BCLog::NET);
    if (!proxy_was_enabled) LogInstance().DisableCategory(BCLog::PROXY);
    return {success, std::move(log)};
}

} // namespace

static CNetAddr ResolveIP(const std::string& ip)
{
    return LookupHost(ip, false).value_or(CNetAddr{});
}

BOOST_AUTO_TEST_CASE(socks5_private_logging_redacts_destination_and_credentials)
{
    // CConnman::Interrupt() can legitimately leave this process-global flag
    // set between test fixtures. Prove the helper is order-independent and
    // restore the pre-test state when this case ends.
    Socks5InterruptGuard interrupted_before_test{/*interrupt=*/true};
    const std::string destination{"paymaster-private-test.onion"};
    const uint16_t port{41234};
    const ProxyCredentials credentials{"isolation-user-secret", "isolation-password-secret"};

    const auto [private_success, private_log] = RunSocks5AndCaptureLog(
        Socks5Response(/*authenticate=*/true), destination, port, &credentials,
        ProxyLogPolicy::REDACT_DESTINATION);
    BOOST_CHECK(private_success);
    BOOST_CHECK(private_log.find("private destination") != std::string::npos);
    BOOST_CHECK(private_log.find(destination) == std::string::npos);
    BOOST_CHECK(private_log.find(std::to_string(port)) == std::string::npos);
    BOOST_CHECK(private_log.find(credentials.username) == std::string::npos);
    BOOST_CHECK(private_log.find(credentials.password) == std::string::npos);

    const auto [private_failure, private_failure_log] = RunSocks5AndCaptureLog(
        Socks5Response(/*authenticate=*/true, /*reply=*/0x05), destination, port,
        &credentials, ProxyLogPolicy::REDACT_DESTINATION);
    BOOST_CHECK(!private_failure);
    BOOST_CHECK(private_failure_log.find("private destination failed") != std::string::npos);
    BOOST_CHECK(private_failure_log.find(destination) == std::string::npos);
    BOOST_CHECK(private_failure_log.find(std::to_string(port)) == std::string::npos);
    BOOST_CHECK(private_failure_log.find(credentials.username) == std::string::npos);
    BOOST_CHECK(private_failure_log.find(credentials.password) == std::string::npos);

    const auto [private_timeout, private_timeout_log] = RunSocks5AndCaptureLog(
        /*response=*/{}, destination, port, &credentials,
        ProxyLogPolicy::REDACT_DESTINATION);
    BOOST_CHECK(!private_timeout);
    BOOST_CHECK(private_timeout_log.find("private destination failed") != std::string::npos);
    BOOST_CHECK(private_timeout_log.find(destination) == std::string::npos);
    BOOST_CHECK(private_timeout_log.find(std::to_string(port)) == std::string::npos);
    BOOST_CHECK(private_timeout_log.find(credentials.username) == std::string::npos);
    BOOST_CHECK(private_timeout_log.find(credentials.password) == std::string::npos);

    const auto [normal_success, normal_log] = RunSocks5AndCaptureLog(
        Socks5Response(/*authenticate=*/true), destination, port, &credentials,
        ProxyLogPolicy::NORMAL);
    BOOST_CHECK(normal_success);
    BOOST_CHECK(normal_log.find(destination) != std::string::npos);
    BOOST_CHECK(normal_log.find(credentials.username) == std::string::npos);
    BOOST_CHECK(normal_log.find(credentials.password) == std::string::npos);
}

BOOST_AUTO_TEST_CASE(socks5_required_auth_fails_closed_without_connect)
{
    Socks5InterruptGuard interrupt_guard{/*interrupt=*/false};
    const std::string destination{"paymaster-private-test.onion"};
    const ProxyCredentials credentials{"isolation-user", "isolation-password"};
    const std::string required_greeting{"\x05\x01\x02", 3};

    RecordingSocks5Sock noauth{Socks5Response(/*authenticate=*/false)};
    BOOST_CHECK(!Socks5(destination, 12033, &credentials, noauth,
                       ProxyLogPolicy::REDACT_DESTINATION, ProxyAuthPolicy::REQUIRE_AUTH));
    BOOST_REQUIRE_EQUAL(noauth.writes.size(), 1);
    BOOST_CHECK_EQUAL(noauth.writes.front(), required_greeting);

    RecordingSocks5Sock missing_credentials{Socks5Response(/*authenticate=*/true)};
    BOOST_CHECK(!Socks5(destination, 12033, nullptr, missing_credentials,
                       ProxyLogPolicy::REDACT_DESTINATION, ProxyAuthPolicy::REQUIRE_AUTH));
    BOOST_CHECK(missing_credentials.writes.empty());

    RecordingSocks5Sock authenticated{Socks5Response(/*authenticate=*/true)};
    BOOST_CHECK(Socks5(destination, 12033, &credentials, authenticated,
                      ProxyLogPolicy::REDACT_DESTINATION, ProxyAuthPolicy::REQUIRE_AUTH));
    BOOST_REQUIRE_EQUAL(authenticated.writes.size(), 3);
    BOOST_CHECK_EQUAL(authenticated.writes.front(), required_greeting);
    BOOST_CHECK(authenticated.writes.back().find(destination) != std::string::npos);

    RecordingSocks5Sock rejected{std::string{"\x05\x02\x01\x01", 4}};
    BOOST_CHECK(!Socks5(destination, 12033, &credentials, rejected,
                       ProxyLogPolicy::REDACT_DESTINATION, ProxyAuthPolicy::REQUIRE_AUTH));
    BOOST_REQUIRE_EQUAL(rejected.writes.size(), 2);
    BOOST_CHECK_EQUAL(rejected.writes.front(), required_greeting);

    // Ordinary proxy users retain backwards-compatible NOAUTH negotiation.
    RecordingSocks5Sock compatible{Socks5Response(/*authenticate=*/false)};
    BOOST_CHECK(Socks5(destination, 12033, &credentials, compatible));
    BOOST_REQUIRE_EQUAL(compatible.writes.size(), 2);
    BOOST_CHECK_EQUAL(compatible.writes.front(), (std::string{"\x05\x02\x00\x02", 4}));
}

BOOST_AUTO_TEST_CASE(socks5_required_auth_uses_fresh_isolation_tokens)
{
    Socks5InterruptGuard interrupt_guard{/*interrupt=*/false};
    const Proxy proxy{CService{LookupHost("127.0.0.1", false).value(), 9050},
                      /*randomize_credentials=*/true};
    RecordingSocks5Sock first{Socks5Response(/*authenticate=*/true)};
    RecordingSocks5Sock second{Socks5Response(/*authenticate=*/true)};
    bool failed{false};
    for (const auto* sock : {&first, &second}) {
        BOOST_CHECK(ConnectThroughProxy(proxy, "paymaster-private-test.onion", 12033,
                                       *sock, 100, failed, ProxyLogPolicy::REDACT_DESTINATION,
                                       ProxyAuthPolicy::REQUIRE_AUTH));
        BOOST_CHECK(!failed);
        BOOST_REQUIRE_EQUAL(sock->writes.size(), 3);
        // RFC1929 version, 64-byte username, 64-byte password (independent hashes).
        BOOST_REQUIRE_EQUAL(sock->writes[1].size(), 131);
        BOOST_CHECK_EQUAL(static_cast<unsigned char>(sock->writes[1][1]), 64);
        BOOST_CHECK_EQUAL(static_cast<unsigned char>(sock->writes[1][66]), 64);
    }
    BOOST_CHECK_NE(first.writes[1], second.writes[1]);

    RecordingSocks5Sock disabled{Socks5Response(/*authenticate=*/true)};
    BOOST_CHECK(!ConnectThroughProxy(Proxy{proxy.proxy, /*randomize_credentials=*/false},
                                    "paymaster-private-test.onion", 12033, disabled,
                                    100, failed, ProxyLogPolicy::REDACT_DESTINATION,
                                    ProxyAuthPolicy::REQUIRE_AUTH));
    BOOST_CHECK(disabled.writes.empty());
}

static CSubNet ResolveSubNet(const std::string& subnet)
{
    CSubNet ret;
    LookupSubNet(subnet, ret);
    return ret;
}

static CNetAddr CreateInternal(const std::string& host)
{
    CNetAddr addr;
    addr.SetInternal(host);
    return addr;
}

BOOST_AUTO_TEST_CASE(netbase_networks)
{
    BOOST_CHECK(ResolveIP("127.0.0.1").GetNetwork() == NET_UNROUTABLE);
    BOOST_CHECK(ResolveIP("::1").GetNetwork() == NET_UNROUTABLE);
    BOOST_CHECK(ResolveIP("8.8.8.8").GetNetwork() == NET_IPV4);
    BOOST_CHECK(ResolveIP("2001::8888").GetNetwork() == NET_IPV6);
    BOOST_CHECK(ResolveIP("pg6mmjiyjmcrsslvykfwnntlaru7p5svn6y2ymmju6nubxndf4pscryd.onion").GetNetwork() == NET_ONION);
    BOOST_CHECK(CreateInternal("foo.com").GetNetwork() == NET_INTERNAL);
}

BOOST_AUTO_TEST_CASE(netbase_properties)
{

    BOOST_CHECK(ResolveIP("127.0.0.1").IsIPv4());
    BOOST_CHECK(ResolveIP("::FFFF:192.168.1.1").IsIPv4());
    BOOST_CHECK(ResolveIP("::1").IsIPv6());
    BOOST_CHECK(ResolveIP("10.0.0.1").IsRFC1918());
    BOOST_CHECK(ResolveIP("192.168.1.1").IsRFC1918());
    BOOST_CHECK(ResolveIP("172.31.255.255").IsRFC1918());
    BOOST_CHECK(ResolveIP("198.18.0.0").IsRFC2544());
    BOOST_CHECK(ResolveIP("198.19.255.255").IsRFC2544());
    BOOST_CHECK(ResolveIP("2001:0DB8::").IsRFC3849());
    BOOST_CHECK(ResolveIP("169.254.1.1").IsRFC3927());
    BOOST_CHECK(ResolveIP("2002::1").IsRFC3964());
    BOOST_CHECK(ResolveIP("FC00::").IsRFC4193());
    BOOST_CHECK(ResolveIP("2001::2").IsRFC4380());
    BOOST_CHECK(ResolveIP("2001:10::").IsRFC4843());
    BOOST_CHECK(ResolveIP("2001:20::").IsRFC7343());
    BOOST_CHECK(ResolveIP("FE80::").IsRFC4862());
    BOOST_CHECK(ResolveIP("64:FF9B::").IsRFC6052());
    BOOST_CHECK(ResolveIP("pg6mmjiyjmcrsslvykfwnntlaru7p5svn6y2ymmju6nubxndf4pscryd.onion").IsTor());
    BOOST_CHECK(ResolveIP("127.0.0.1").IsLocal());
    BOOST_CHECK(ResolveIP("::1").IsLocal());
    BOOST_CHECK(ResolveIP("8.8.8.8").IsRoutable());
    BOOST_CHECK(ResolveIP("2001::1").IsRoutable());
    BOOST_CHECK(ResolveIP("127.0.0.1").IsValid());
    BOOST_CHECK(CreateInternal("FD6B:88C0:8724:edb1:8e4:3588:e546:35ca").IsInternal());
    BOOST_CHECK(CreateInternal("bar.com").IsInternal());

}

bool static TestSplitHost(const std::string& test, const std::string& host, uint16_t port, bool validPort=true)
{
    std::string hostOut;
    uint16_t portOut{0};
    bool validPortOut = SplitHostPort(test, portOut, hostOut);
    return hostOut == host && portOut == port && validPortOut == validPort;
}

BOOST_AUTO_TEST_CASE(netbase_splithost)
{
    BOOST_CHECK(TestSplitHost("www.digibyte.org", "www.digibyte.org", 0));
    BOOST_CHECK(TestSplitHost("[www.digibyte.org]", "www.digibyte.org", 0));
    BOOST_CHECK(TestSplitHost("www.digibyte.org:80", "www.digibyte.org", 80));
    BOOST_CHECK(TestSplitHost("[www.digibyte.org]:80", "www.digibyte.org", 80));
    BOOST_CHECK(TestSplitHost("127.0.0.1", "127.0.0.1", 0));
    BOOST_CHECK(TestSplitHost("127.0.0.1:12024", "127.0.0.1", 12024));
    BOOST_CHECK(TestSplitHost("[127.0.0.1]", "127.0.0.1", 0));
    BOOST_CHECK(TestSplitHost("[127.0.0.1]:12024", "127.0.0.1", 12024));
    BOOST_CHECK(TestSplitHost("::ffff:127.0.0.1", "::ffff:127.0.0.1", 0));
    BOOST_CHECK(TestSplitHost("[::ffff:127.0.0.1]:12024", "::ffff:127.0.0.1", 12024));
    BOOST_CHECK(TestSplitHost("[::]:12024", "::", 12024));
    BOOST_CHECK(TestSplitHost("::12024", "::12024", 0));
    BOOST_CHECK(TestSplitHost(":12024", "", 12024));
    BOOST_CHECK(TestSplitHost("[]:12024", "", 12024));
    BOOST_CHECK(TestSplitHost("", "", 0));
    BOOST_CHECK(TestSplitHost(":65535", "", 65535));
    BOOST_CHECK(TestSplitHost(":65536", ":65536", 0, false));
    BOOST_CHECK(TestSplitHost(":-1", ":-1", 0, false));
    BOOST_CHECK(TestSplitHost("[]:70001", "[]:70001", 0, false));
    BOOST_CHECK(TestSplitHost("[]:-1", "[]:-1", 0, false));
    BOOST_CHECK(TestSplitHost("[]:-0", "[]:-0", 0, false));
    BOOST_CHECK(TestSplitHost("[]:0", "", 0, false));
    BOOST_CHECK(TestSplitHost("[]:1/2", "[]:1/2", 0, false));
    BOOST_CHECK(TestSplitHost("[]:1E2", "[]:1E2", 0, false));
    BOOST_CHECK(TestSplitHost("127.0.0.1:65536", "127.0.0.1:65536", 0, false));
    BOOST_CHECK(TestSplitHost("127.0.0.1:0", "127.0.0.1", 0, false));
    BOOST_CHECK(TestSplitHost("127.0.0.1:", "127.0.0.1:", 0, false));
    BOOST_CHECK(TestSplitHost("127.0.0.1:1/2", "127.0.0.1:1/2", 0, false));
    BOOST_CHECK(TestSplitHost("127.0.0.1:1E2", "127.0.0.1:1E2", 0, false));
    BOOST_CHECK(TestSplitHost("www.digibyte.org:65536", "www.digibyte.org:65536", 0, false));
    BOOST_CHECK(TestSplitHost("www.digibyte.org:0", "www.digibyte.org", 0, false));
    BOOST_CHECK(TestSplitHost("www.digibyte.org:", "www.digibyte.org:", 0, false));
}

bool static TestParse(std::string src, std::string canon)
{
    CService addr(LookupNumeric(src, 65535));
    return canon == addr.ToStringAddrPort();
}

BOOST_AUTO_TEST_CASE(netbase_lookupnumeric)
{
    BOOST_CHECK(TestParse("127.0.0.1", "127.0.0.1:65535"));
    BOOST_CHECK(TestParse("127.0.0.1:12024", "127.0.0.1:12024"));
    BOOST_CHECK(TestParse("::ffff:127.0.0.1", "127.0.0.1:65535"));
    BOOST_CHECK(TestParse("::", "[::]:65535"));
    BOOST_CHECK(TestParse("[::]:12024", "[::]:12024"));
    BOOST_CHECK(TestParse("[127.0.0.1]", "127.0.0.1:65535"));
    BOOST_CHECK(TestParse(":::", "[::]:0"));

    // verify that an internal address fails to resolve
    BOOST_CHECK(TestParse("[fd6b:88c0:8724:1:2:3:4:5]", "[::]:0"));
    // and that a one-off resolves correctly
    BOOST_CHECK(TestParse("[fd6c:88c0:8724:1:2:3:4:5]", "[fd6c:88c0:8724:1:2:3:4:5]:65535"));
}

BOOST_AUTO_TEST_CASE(embedded_test)
{
    CNetAddr addr1(ResolveIP("1.2.3.4"));
    CNetAddr addr2(ResolveIP("::FFFF:0102:0304"));
    BOOST_CHECK(addr2.IsIPv4());
    BOOST_CHECK_EQUAL(addr1.ToStringAddr(), addr2.ToStringAddr());
}

BOOST_AUTO_TEST_CASE(subnet_test)
{

    BOOST_CHECK(ResolveSubNet("1.2.3.0/24") == ResolveSubNet("1.2.3.0/255.255.255.0"));
    BOOST_CHECK(ResolveSubNet("1.2.3.0/24") != ResolveSubNet("1.2.4.0/255.255.255.0"));
    BOOST_CHECK(ResolveSubNet("1.2.3.0/24").Match(ResolveIP("1.2.3.4")));
    BOOST_CHECK(!ResolveSubNet("1.2.2.0/24").Match(ResolveIP("1.2.3.4")));
    BOOST_CHECK(ResolveSubNet("1.2.3.4").Match(ResolveIP("1.2.3.4")));
    BOOST_CHECK(ResolveSubNet("1.2.3.4/32").Match(ResolveIP("1.2.3.4")));
    BOOST_CHECK(!ResolveSubNet("1.2.3.4").Match(ResolveIP("5.6.7.8")));
    BOOST_CHECK(!ResolveSubNet("1.2.3.4/32").Match(ResolveIP("5.6.7.8")));
    BOOST_CHECK(ResolveSubNet("::ffff:127.0.0.1").Match(ResolveIP("127.0.0.1")));
    BOOST_CHECK(ResolveSubNet("1:2:3:4:5:6:7:8").Match(ResolveIP("1:2:3:4:5:6:7:8")));
    BOOST_CHECK(!ResolveSubNet("1:2:3:4:5:6:7:8").Match(ResolveIP("1:2:3:4:5:6:7:9")));
    BOOST_CHECK(ResolveSubNet("1:2:3:4:5:6:7:0/112").Match(ResolveIP("1:2:3:4:5:6:7:1234")));
    BOOST_CHECK(ResolveSubNet("192.168.0.1/24").Match(ResolveIP("192.168.0.2")));
    BOOST_CHECK(ResolveSubNet("192.168.0.20/29").Match(ResolveIP("192.168.0.18")));
    BOOST_CHECK(ResolveSubNet("1.2.2.1/24").Match(ResolveIP("1.2.2.4")));
    BOOST_CHECK(ResolveSubNet("1.2.2.110/31").Match(ResolveIP("1.2.2.111")));
    BOOST_CHECK(ResolveSubNet("1.2.2.20/26").Match(ResolveIP("1.2.2.63")));
    // All-Matching IPv6 Matches arbitrary IPv6
    BOOST_CHECK(ResolveSubNet("::/0").Match(ResolveIP("1:2:3:4:5:6:7:1234")));
    // But not `::` or `0.0.0.0` because they are considered invalid addresses
    BOOST_CHECK(!ResolveSubNet("::/0").Match(ResolveIP("::")));
    BOOST_CHECK(!ResolveSubNet("::/0").Match(ResolveIP("0.0.0.0")));
    // Addresses from one network (IPv4) don't belong to subnets of another network (IPv6)
    BOOST_CHECK(!ResolveSubNet("::/0").Match(ResolveIP("1.2.3.4")));
    // All-Matching IPv4 does not Match IPv6
    BOOST_CHECK(!ResolveSubNet("0.0.0.0/0").Match(ResolveIP("1:2:3:4:5:6:7:1234")));
    // Invalid subnets Match nothing (not even invalid addresses)
    BOOST_CHECK(!CSubNet().Match(ResolveIP("1.2.3.4")));
    BOOST_CHECK(!ResolveSubNet("").Match(ResolveIP("4.5.6.7")));
    BOOST_CHECK(!ResolveSubNet("bloop").Match(ResolveIP("0.0.0.0")));
    BOOST_CHECK(!ResolveSubNet("bloop").Match(ResolveIP("hab")));
    // Check valid/invalid
    BOOST_CHECK(ResolveSubNet("1.2.3.0/0").IsValid());
    BOOST_CHECK(!ResolveSubNet("1.2.3.0/-1").IsValid());
    BOOST_CHECK(ResolveSubNet("1.2.3.0/32").IsValid());
    BOOST_CHECK(!ResolveSubNet("1.2.3.0/33").IsValid());
    BOOST_CHECK(!ResolveSubNet("1.2.3.0/300").IsValid());
    BOOST_CHECK(ResolveSubNet("1:2:3:4:5:6:7:8/0").IsValid());
    BOOST_CHECK(ResolveSubNet("1:2:3:4:5:6:7:8/33").IsValid());
    BOOST_CHECK(!ResolveSubNet("1:2:3:4:5:6:7:8/-1").IsValid());
    BOOST_CHECK(ResolveSubNet("1:2:3:4:5:6:7:8/128").IsValid());
    BOOST_CHECK(!ResolveSubNet("1:2:3:4:5:6:7:8/129").IsValid());
    BOOST_CHECK(!ResolveSubNet("fuzzy").IsValid());

    //CNetAddr constructor test
    BOOST_CHECK(CSubNet(ResolveIP("127.0.0.1")).IsValid());
    BOOST_CHECK(CSubNet(ResolveIP("127.0.0.1")).Match(ResolveIP("127.0.0.1")));
    BOOST_CHECK(!CSubNet(ResolveIP("127.0.0.1")).Match(ResolveIP("127.0.0.2")));
    BOOST_CHECK(CSubNet(ResolveIP("127.0.0.1")).ToString() == "127.0.0.1/32");

    CSubNet subnet = CSubNet(ResolveIP("1.2.3.4"), 32);
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.3.4/32");
    subnet = CSubNet(ResolveIP("1.2.3.4"), 8);
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.0.0.0/8");
    subnet = CSubNet(ResolveIP("1.2.3.4"), 0);
    BOOST_CHECK_EQUAL(subnet.ToString(), "0.0.0.0/0");

    subnet = CSubNet(ResolveIP("1.2.3.4"), ResolveIP("255.255.255.255"));
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.3.4/32");
    subnet = CSubNet(ResolveIP("1.2.3.4"), ResolveIP("255.0.0.0"));
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.0.0.0/8");
    subnet = CSubNet(ResolveIP("1.2.3.4"), ResolveIP("0.0.0.0"));
    BOOST_CHECK_EQUAL(subnet.ToString(), "0.0.0.0/0");

    BOOST_CHECK(CSubNet(ResolveIP("1:2:3:4:5:6:7:8")).IsValid());
    BOOST_CHECK(CSubNet(ResolveIP("1:2:3:4:5:6:7:8")).Match(ResolveIP("1:2:3:4:5:6:7:8")));
    BOOST_CHECK(!CSubNet(ResolveIP("1:2:3:4:5:6:7:8")).Match(ResolveIP("1:2:3:4:5:6:7:9")));
    BOOST_CHECK(CSubNet(ResolveIP("1:2:3:4:5:6:7:8")).ToString() == "1:2:3:4:5:6:7:8/128");
    // IPv4 address with IPv6 netmask or the other way around.
    BOOST_CHECK(!CSubNet(ResolveIP("1.1.1.1"), ResolveIP("ffff::")).IsValid());
    BOOST_CHECK(!CSubNet(ResolveIP("::1"), ResolveIP("255.0.0.0")).IsValid());

    // Create Non-IP subnets.

    const CNetAddr tor_addr{
        ResolveIP("pg6mmjiyjmcrsslvykfwnntlaru7p5svn6y2ymmju6nubxndf4pscryd.onion")};

    subnet = CSubNet(tor_addr);
    BOOST_CHECK(subnet.IsValid());
    BOOST_CHECK_EQUAL(subnet.ToString(), tor_addr.ToStringAddr());
    BOOST_CHECK(subnet.Match(tor_addr));
    BOOST_CHECK(
        !subnet.Match(ResolveIP("kpgvmscirrdqpekbqjsvw5teanhatztpp2gl6eee4zkowvwfxwenqaid.onion")));
    BOOST_CHECK(!subnet.Match(ResolveIP("1.2.3.4")));

    BOOST_CHECK(!CSubNet(tor_addr, 200).IsValid());
    BOOST_CHECK(!CSubNet(tor_addr, ResolveIP("255.0.0.0")).IsValid());

    subnet = ResolveSubNet("1.2.3.4/255.255.255.255");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.3.4/32");
    subnet = ResolveSubNet("1.2.3.4/255.255.255.254");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.3.4/31");
    subnet = ResolveSubNet("1.2.3.4/255.255.255.252");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.3.4/30");
    subnet = ResolveSubNet("1.2.3.4/255.255.255.248");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.3.0/29");
    subnet = ResolveSubNet("1.2.3.4/255.255.255.240");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.3.0/28");
    subnet = ResolveSubNet("1.2.3.4/255.255.255.224");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.3.0/27");
    subnet = ResolveSubNet("1.2.3.4/255.255.255.192");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.3.0/26");
    subnet = ResolveSubNet("1.2.3.4/255.255.255.128");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.3.0/25");
    subnet = ResolveSubNet("1.2.3.4/255.255.255.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.3.0/24");
    subnet = ResolveSubNet("1.2.3.4/255.255.254.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.2.0/23");
    subnet = ResolveSubNet("1.2.3.4/255.255.252.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.0.0/22");
    subnet = ResolveSubNet("1.2.3.4/255.255.248.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.0.0/21");
    subnet = ResolveSubNet("1.2.3.4/255.255.240.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.0.0/20");
    subnet = ResolveSubNet("1.2.3.4/255.255.224.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.0.0/19");
    subnet = ResolveSubNet("1.2.3.4/255.255.192.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.0.0/18");
    subnet = ResolveSubNet("1.2.3.4/255.255.128.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.0.0/17");
    subnet = ResolveSubNet("1.2.3.4/255.255.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.0.0/16");
    subnet = ResolveSubNet("1.2.3.4/255.254.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.2.0.0/15");
    subnet = ResolveSubNet("1.2.3.4/255.252.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.0.0.0/14");
    subnet = ResolveSubNet("1.2.3.4/255.248.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.0.0.0/13");
    subnet = ResolveSubNet("1.2.3.4/255.240.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.0.0.0/12");
    subnet = ResolveSubNet("1.2.3.4/255.224.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.0.0.0/11");
    subnet = ResolveSubNet("1.2.3.4/255.192.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.0.0.0/10");
    subnet = ResolveSubNet("1.2.3.4/255.128.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.0.0.0/9");
    subnet = ResolveSubNet("1.2.3.4/255.0.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1.0.0.0/8");
    subnet = ResolveSubNet("1.2.3.4/254.0.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "0.0.0.0/7");
    subnet = ResolveSubNet("1.2.3.4/252.0.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "0.0.0.0/6");
    subnet = ResolveSubNet("1.2.3.4/248.0.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "0.0.0.0/5");
    subnet = ResolveSubNet("1.2.3.4/240.0.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "0.0.0.0/4");
    subnet = ResolveSubNet("1.2.3.4/224.0.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "0.0.0.0/3");
    subnet = ResolveSubNet("1.2.3.4/192.0.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "0.0.0.0/2");
    subnet = ResolveSubNet("1.2.3.4/128.0.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "0.0.0.0/1");
    subnet = ResolveSubNet("1.2.3.4/0.0.0.0");
    BOOST_CHECK_EQUAL(subnet.ToString(), "0.0.0.0/0");

    subnet = ResolveSubNet("1:2:3:4:5:6:7:8/ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1:2:3:4:5:6:7:8/128");
    subnet = ResolveSubNet("1:2:3:4:5:6:7:8/ffff:0000:0000:0000:0000:0000:0000:0000");
    BOOST_CHECK_EQUAL(subnet.ToString(), "1::/16");
    subnet = ResolveSubNet("1:2:3:4:5:6:7:8/0000:0000:0000:0000:0000:0000:0000:0000");
    BOOST_CHECK_EQUAL(subnet.ToString(), "::/0");
    // Invalid netmasks (with 1-bits after 0-bits)
    subnet = ResolveSubNet("1.2.3.4/255.255.232.0");
    BOOST_CHECK(!subnet.IsValid());
    subnet = ResolveSubNet("1.2.3.4/255.0.255.255");
    BOOST_CHECK(!subnet.IsValid());
    subnet = ResolveSubNet("1:2:3:4:5:6:7:8/ffff:ffff:ffff:fffe:ffff:ffff:ffff:ff0f");
    BOOST_CHECK(!subnet.IsValid());
}

BOOST_AUTO_TEST_CASE(netbase_getgroup)
{
    NetGroupManager netgroupman{std::vector<bool>()}; // use /16
    BOOST_CHECK(netgroupman.GetGroup(ResolveIP("127.0.0.1")) == std::vector<unsigned char>({0})); // Local -> !Routable()
    BOOST_CHECK(netgroupman.GetGroup(ResolveIP("257.0.0.1")) == std::vector<unsigned char>({0})); // !Valid -> !Routable()
    BOOST_CHECK(netgroupman.GetGroup(ResolveIP("10.0.0.1")) == std::vector<unsigned char>({0})); // RFC1918 -> !Routable()
    BOOST_CHECK(netgroupman.GetGroup(ResolveIP("169.254.1.1")) == std::vector<unsigned char>({0})); // RFC3927 -> !Routable()
    BOOST_CHECK(netgroupman.GetGroup(ResolveIP("1.2.3.4")) == std::vector<unsigned char>({(unsigned char)NET_IPV4, 1, 2})); // IPv4
    BOOST_CHECK(netgroupman.GetGroup(ResolveIP("::FFFF:0:102:304")) == std::vector<unsigned char>({(unsigned char)NET_IPV4, 1, 2})); // RFC6145
    BOOST_CHECK(netgroupman.GetGroup(ResolveIP("64:FF9B::102:304")) == std::vector<unsigned char>({(unsigned char)NET_IPV4, 1, 2})); // RFC6052
    BOOST_CHECK(netgroupman.GetGroup(ResolveIP("2002:102:304:9999:9999:9999:9999:9999")) == std::vector<unsigned char>({(unsigned char)NET_IPV4, 1, 2})); // RFC3964
    BOOST_CHECK(netgroupman.GetGroup(ResolveIP("2001:0:9999:9999:9999:9999:FEFD:FCFB")) == std::vector<unsigned char>({(unsigned char)NET_IPV4, 1, 2})); // RFC4380
    BOOST_CHECK(netgroupman.GetGroup(ResolveIP("2001:470:abcd:9999:9999:9999:9999:9999")) == std::vector<unsigned char>({(unsigned char)NET_IPV6, 32, 1, 4, 112, 175})); //he.net
    BOOST_CHECK(netgroupman.GetGroup(ResolveIP("2001:2001:9999:9999:9999:9999:9999:9999")) == std::vector<unsigned char>({(unsigned char)NET_IPV6, 32, 1, 32, 1})); //IPv6

    // baz.net sha256 hash: 12929400eb4607c4ac075f087167e75286b179c693eb059a01774b864e8fe505
    std::vector<unsigned char> internal_group = {NET_INTERNAL, 0x12, 0x92, 0x94, 0x00, 0xeb, 0x46, 0x07, 0xc4, 0xac, 0x07};
    BOOST_CHECK(netgroupman.GetGroup(CreateInternal("baz.net")) == internal_group);
}

BOOST_AUTO_TEST_CASE(netbase_parsenetwork)
{
    BOOST_CHECK_EQUAL(ParseNetwork("ipv4"), NET_IPV4);
    BOOST_CHECK_EQUAL(ParseNetwork("ipv6"), NET_IPV6);
    BOOST_CHECK_EQUAL(ParseNetwork("onion"), NET_ONION);
    BOOST_CHECK_EQUAL(ParseNetwork("tor"), NET_ONION);
    BOOST_CHECK_EQUAL(ParseNetwork("cjdns"), NET_CJDNS);

    BOOST_CHECK_EQUAL(ParseNetwork("IPv4"), NET_IPV4);
    BOOST_CHECK_EQUAL(ParseNetwork("IPv6"), NET_IPV6);
    BOOST_CHECK_EQUAL(ParseNetwork("ONION"), NET_ONION);
    BOOST_CHECK_EQUAL(ParseNetwork("TOR"), NET_ONION);
    BOOST_CHECK_EQUAL(ParseNetwork("CJDNS"), NET_CJDNS);

    BOOST_CHECK_EQUAL(ParseNetwork(":)"), NET_UNROUTABLE);
    BOOST_CHECK_EQUAL(ParseNetwork("tÖr"), NET_UNROUTABLE);
    BOOST_CHECK_EQUAL(ParseNetwork("\xfe\xff"), NET_UNROUTABLE);
    BOOST_CHECK_EQUAL(ParseNetwork(""), NET_UNROUTABLE);
}

BOOST_AUTO_TEST_CASE(netpermissions_test)
{
    bilingual_str error;
    NetWhitebindPermissions whitebindPermissions;
    NetWhitelistPermissions whitelistPermissions;

    // Detect invalid white bind
    BOOST_CHECK(!NetWhitebindPermissions::TryParse("", whitebindPermissions, error));
    BOOST_CHECK(error.original.find("Cannot resolve -whitebind address") != std::string::npos);
    BOOST_CHECK(!NetWhitebindPermissions::TryParse("127.0.0.1", whitebindPermissions, error));
    BOOST_CHECK(error.original.find("Need to specify a port with -whitebind") != std::string::npos);
    BOOST_CHECK(!NetWhitebindPermissions::TryParse("", whitebindPermissions, error));

    // If no permission flags, assume backward compatibility
    BOOST_CHECK(NetWhitebindPermissions::TryParse("1.2.3.4:32", whitebindPermissions, error));
    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(whitebindPermissions.m_flags, NetPermissionFlags::Implicit);
    BOOST_CHECK(NetPermissions::HasFlag(whitebindPermissions.m_flags, NetPermissionFlags::Implicit));
    NetPermissions::ClearFlag(whitebindPermissions.m_flags, NetPermissionFlags::Implicit);
    BOOST_CHECK(!NetPermissions::HasFlag(whitebindPermissions.m_flags, NetPermissionFlags::Implicit));
    BOOST_CHECK_EQUAL(whitebindPermissions.m_flags, NetPermissionFlags::None);
    NetPermissions::AddFlag(whitebindPermissions.m_flags, NetPermissionFlags::Implicit);
    BOOST_CHECK(NetPermissions::HasFlag(whitebindPermissions.m_flags, NetPermissionFlags::Implicit));

    // Can set one permission
    BOOST_CHECK(NetWhitebindPermissions::TryParse("bloom@1.2.3.4:32", whitebindPermissions, error));
    BOOST_CHECK_EQUAL(whitebindPermissions.m_flags, NetPermissionFlags::BloomFilter);
    BOOST_CHECK(NetWhitebindPermissions::TryParse("@1.2.3.4:32", whitebindPermissions, error));
    BOOST_CHECK_EQUAL(whitebindPermissions.m_flags, NetPermissionFlags::None);

    NetWhitebindPermissions noban, noban_download, download_noban, download;

    // "noban" implies "download"
    BOOST_REQUIRE(NetWhitebindPermissions::TryParse("noban@1.2.3.4:32", noban, error));
    BOOST_CHECK_EQUAL(noban.m_flags, NetPermissionFlags::NoBan);
    BOOST_CHECK(NetPermissions::HasFlag(noban.m_flags, NetPermissionFlags::Download));
    BOOST_CHECK(NetPermissions::HasFlag(noban.m_flags, NetPermissionFlags::NoBan));

    // "noban,download" is equivalent to "noban"
    BOOST_REQUIRE(NetWhitebindPermissions::TryParse("noban,download@1.2.3.4:32", noban_download, error));
    BOOST_CHECK_EQUAL(noban_download.m_flags, noban.m_flags);

    // "download,noban" is equivalent to "noban"
    BOOST_REQUIRE(NetWhitebindPermissions::TryParse("download,noban@1.2.3.4:32", download_noban, error));
    BOOST_CHECK_EQUAL(download_noban.m_flags, noban.m_flags);

    // "download" excludes (does not imply) "noban"
    BOOST_REQUIRE(NetWhitebindPermissions::TryParse("download@1.2.3.4:32", download, error));
    BOOST_CHECK_EQUAL(download.m_flags, NetPermissionFlags::Download);
    BOOST_CHECK(NetPermissions::HasFlag(download.m_flags, NetPermissionFlags::Download));
    BOOST_CHECK(!NetPermissions::HasFlag(download.m_flags, NetPermissionFlags::NoBan));

    // Happy path, can parse flags
    BOOST_CHECK(NetWhitebindPermissions::TryParse("bloom,forcerelay@1.2.3.4:32", whitebindPermissions, error));
    // forcerelay should also activate the relay permission
    BOOST_CHECK_EQUAL(whitebindPermissions.m_flags, NetPermissionFlags::BloomFilter | NetPermissionFlags::ForceRelay | NetPermissionFlags::Relay);
    BOOST_CHECK(NetWhitebindPermissions::TryParse("bloom,relay,noban@1.2.3.4:32", whitebindPermissions, error));
    BOOST_CHECK_EQUAL(whitebindPermissions.m_flags, NetPermissionFlags::BloomFilter | NetPermissionFlags::Relay | NetPermissionFlags::NoBan);
    BOOST_CHECK(NetWhitebindPermissions::TryParse("bloom,forcerelay,noban@1.2.3.4:32", whitebindPermissions, error));
    BOOST_CHECK(NetWhitebindPermissions::TryParse("all@1.2.3.4:32", whitebindPermissions, error));
    BOOST_CHECK_EQUAL(whitebindPermissions.m_flags, NetPermissionFlags::All);

    // Allow dups
    BOOST_CHECK(NetWhitebindPermissions::TryParse("bloom,relay,noban,noban@1.2.3.4:32", whitebindPermissions, error));
    BOOST_CHECK_EQUAL(whitebindPermissions.m_flags, NetPermissionFlags::BloomFilter | NetPermissionFlags::Relay | NetPermissionFlags::NoBan | NetPermissionFlags::Download); // "noban" implies "download"

    // Allow empty
    BOOST_CHECK(NetWhitebindPermissions::TryParse("bloom,relay,,noban@1.2.3.4:32", whitebindPermissions, error));
    BOOST_CHECK_EQUAL(whitebindPermissions.m_flags, NetPermissionFlags::BloomFilter | NetPermissionFlags::Relay | NetPermissionFlags::NoBan);
    BOOST_CHECK(NetWhitebindPermissions::TryParse(",@1.2.3.4:32", whitebindPermissions, error));
    BOOST_CHECK_EQUAL(whitebindPermissions.m_flags, NetPermissionFlags::None);
    BOOST_CHECK(NetWhitebindPermissions::TryParse(",,@1.2.3.4:32", whitebindPermissions, error));
    BOOST_CHECK_EQUAL(whitebindPermissions.m_flags, NetPermissionFlags::None);

    // Detect invalid flag
    BOOST_CHECK(!NetWhitebindPermissions::TryParse("bloom,forcerelay,oopsie@1.2.3.4:32", whitebindPermissions, error));
    BOOST_CHECK(error.original.find("Invalid P2P permission") != std::string::npos);

    // Check netmask error
    BOOST_CHECK(!NetWhitelistPermissions::TryParse("bloom,forcerelay,noban@1.2.3.4:32", whitelistPermissions, error));
    BOOST_CHECK(error.original.find("Invalid netmask specified in -whitelist") != std::string::npos);

    // Happy path for whitelist parsing
    BOOST_CHECK(NetWhitelistPermissions::TryParse("noban@1.2.3.4", whitelistPermissions, error));
    BOOST_CHECK_EQUAL(whitelistPermissions.m_flags, NetPermissionFlags::NoBan);
    BOOST_CHECK(NetPermissions::HasFlag(whitelistPermissions.m_flags, NetPermissionFlags::NoBan));

    BOOST_CHECK(NetWhitelistPermissions::TryParse("bloom,forcerelay,noban,relay@1.2.3.4/32", whitelistPermissions, error));
    BOOST_CHECK_EQUAL(whitelistPermissions.m_flags, NetPermissionFlags::BloomFilter | NetPermissionFlags::ForceRelay | NetPermissionFlags::NoBan | NetPermissionFlags::Relay);
    BOOST_CHECK(error.empty());
    BOOST_CHECK_EQUAL(whitelistPermissions.m_subnet.ToString(), "1.2.3.4/32");
    BOOST_CHECK(NetWhitelistPermissions::TryParse("bloom,forcerelay,noban,relay,mempool@1.2.3.4/32", whitelistPermissions, error));

    const auto strings = NetPermissions::ToStrings(NetPermissionFlags::All);
    BOOST_CHECK_EQUAL(strings.size(), 7U);
    BOOST_CHECK(std::find(strings.begin(), strings.end(), "bloomfilter") != strings.end());
    BOOST_CHECK(std::find(strings.begin(), strings.end(), "forcerelay") != strings.end());
    BOOST_CHECK(std::find(strings.begin(), strings.end(), "relay") != strings.end());
    BOOST_CHECK(std::find(strings.begin(), strings.end(), "noban") != strings.end());
    BOOST_CHECK(std::find(strings.begin(), strings.end(), "mempool") != strings.end());
    BOOST_CHECK(std::find(strings.begin(), strings.end(), "download") != strings.end());
    BOOST_CHECK(std::find(strings.begin(), strings.end(), "addr") != strings.end());
}

BOOST_AUTO_TEST_CASE(netbase_dont_resolve_strings_with_embedded_nul_characters)
{
    BOOST_CHECK(LookupHost("127.0.0.1"s, false).has_value());
    BOOST_CHECK(!LookupHost("127.0.0.1\0"s, false).has_value());
    BOOST_CHECK(!LookupHost("127.0.0.1\0example.com"s, false).has_value());
    BOOST_CHECK(!LookupHost("127.0.0.1\0example.com\0"s, false).has_value());
    CSubNet ret;
    BOOST_CHECK(LookupSubNet("1.2.3.0/24"s, ret));
    BOOST_CHECK(!LookupSubNet("1.2.3.0/24\0"s, ret));
    BOOST_CHECK(!LookupSubNet("1.2.3.0/24\0example.com"s, ret));
    BOOST_CHECK(!LookupSubNet("1.2.3.0/24\0example.com\0"s, ret));
    BOOST_CHECK(LookupSubNet("pg6mmjiyjmcrsslvykfwnntlaru7p5svn6y2ymmju6nubxndf4pscryd.onion"s, ret));
    BOOST_CHECK(!LookupSubNet("pg6mmjiyjmcrsslvykfwnntlaru7p5svn6y2ymmju6nubxndf4pscryd.onion\0"s, ret));
    BOOST_CHECK(!LookupSubNet("pg6mmjiyjmcrsslvykfwnntlaru7p5svn6y2ymmju6nubxndf4pscryd.onion\0example.com"s, ret));
    BOOST_CHECK(!LookupSubNet("pg6mmjiyjmcrsslvykfwnntlaru7p5svn6y2ymmju6nubxndf4pscryd.onion\0example.com\0"s, ret));
}

// Since CNetAddr (un)ser is tested separately in net_tests.cpp here we only
// try a few edge cases for port, service flags and time.

static const std::vector<CAddress> fixture_addresses({
    CAddress{
        CService(CNetAddr(in6_addr(IN6ADDR_LOOPBACK_INIT)), 0 /* port */),
        NODE_NONE,
        NodeSeconds{0x4966bc61s}, /* Fri Jan  9 02:54:25 UTC 2009 */
    },
    CAddress{
        CService(CNetAddr(in6_addr(IN6ADDR_LOOPBACK_INIT)), 0x00f1 /* port */),
        NODE_NETWORK,
        NodeSeconds{0x83766279s}, /* Tue Nov 22 11:22:33 UTC 2039 */
    },
    CAddress{
        CService(CNetAddr(in6_addr(IN6ADDR_LOOPBACK_INIT)), 0xf1f2 /* port */),
        static_cast<ServiceFlags>(NODE_WITNESS | NODE_COMPACT_FILTERS | NODE_NETWORK_LIMITED),
        NodeSeconds{0xffffffffs}, /* Sun Feb  7 06:28:15 UTC 2106 */
    },
});

// fixture_addresses should equal to this when serialized in V1 format.
// When this is unserialized from V1 format it should equal to fixture_addresses.
static constexpr const char* stream_addrv1_hex =
    "03" // number of entries

    "61bc6649"                         // time, Fri Jan  9 02:54:25 UTC 2009
    "0000000000000000"                 // service flags, NODE_NONE
    "00000000000000000000000000000001" // address, fixed 16 bytes (IPv4 embedded in IPv6)
    "0000"                             // port

    "79627683"                         // time, Tue Nov 22 11:22:33 UTC 2039
    "0100000000000000"                 // service flags, NODE_NETWORK
    "00000000000000000000000000000001" // address, fixed 16 bytes (IPv6)
    "00f1"                             // port

    "ffffffff"                         // time, Sun Feb  7 06:28:15 UTC 2106
    "4804000000000000"                 // service flags, NODE_WITNESS | NODE_COMPACT_FILTERS | NODE_NETWORK_LIMITED
    "00000000000000000000000000000001" // address, fixed 16 bytes (IPv6)
    "f1f2";                            // port

// fixture_addresses should equal to this when serialized in V2 format.
// When this is unserialized from V2 format it should equal to fixture_addresses.
static constexpr const char* stream_addrv2_hex =
    "03" // number of entries

    "61bc6649"                         // time, Fri Jan  9 02:54:25 UTC 2009
    "00"                               // service flags, COMPACTSIZE(NODE_NONE)
    "02"                               // network id, IPv6
    "10"                               // address length, COMPACTSIZE(16)
    "00000000000000000000000000000001" // address
    "0000"                             // port

    "79627683"                         // time, Tue Nov 22 11:22:33 UTC 2039
    "01"                               // service flags, COMPACTSIZE(NODE_NETWORK)
    "02"                               // network id, IPv6
    "10"                               // address length, COMPACTSIZE(16)
    "00000000000000000000000000000001" // address
    "00f1"                             // port

    "ffffffff"                         // time, Sun Feb  7 06:28:15 UTC 2106
    "fd4804"                           // service flags, COMPACTSIZE(NODE_WITNESS | NODE_COMPACT_FILTERS | NODE_NETWORK_LIMITED)
    "02"                               // network id, IPv6
    "10"                               // address length, COMPACTSIZE(16)
    "00000000000000000000000000000001" // address
    "f1f2";                            // port

BOOST_AUTO_TEST_CASE(caddress_serialize_v1)
{
    DataStream s{};

    s << CAddress::V1_NETWORK(fixture_addresses);
    BOOST_CHECK_EQUAL(HexStr(s), stream_addrv1_hex);
}

BOOST_AUTO_TEST_CASE(caddress_unserialize_v1)
{
    DataStream s{ParseHex(stream_addrv1_hex)};
    std::vector<CAddress> addresses_unserialized;

    s >> CAddress::V1_NETWORK(addresses_unserialized);
    BOOST_CHECK(fixture_addresses == addresses_unserialized);
}

BOOST_AUTO_TEST_CASE(caddress_serialize_v2)
{
    DataStream s{};

    s << CAddress::V2_NETWORK(fixture_addresses);
    BOOST_CHECK_EQUAL(HexStr(s), stream_addrv2_hex);
}

BOOST_AUTO_TEST_CASE(caddress_unserialize_v2)
{
    DataStream s{ParseHex(stream_addrv2_hex)};
    std::vector<CAddress> addresses_unserialized;

    s >> CAddress::V2_NETWORK(addresses_unserialized);
    BOOST_CHECK(fixture_addresses == addresses_unserialized);
}

BOOST_AUTO_TEST_CASE(isbadport)
{
    BOOST_CHECK(IsBadPort(1));
    BOOST_CHECK(IsBadPort(22));
    BOOST_CHECK(IsBadPort(6000));

    BOOST_CHECK(!IsBadPort(80));
    BOOST_CHECK(!IsBadPort(443));
    BOOST_CHECK(!IsBadPort(12024)); // DigiByte mainnet port

    // Check all ports, there must be 80 bad ports in total.
    size_t total_bad_ports{0};
    for (uint16_t port = std::numeric_limits<uint16_t>::max(); port > 0; --port) {
        if (IsBadPort(port)) {
            ++total_bad_ports;
        }
    }
    BOOST_CHECK_EQUAL(total_bad_ports, 80);
}

BOOST_AUTO_TEST_SUITE_END()
