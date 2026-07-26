// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include <chiaki/discoveryservice.h>

#include <mutex>
#include <string>
#include <vector>

namespace hayai::app {

struct DiscoveredHost
{
	std::string name;
	std::string addr;
	std::string id;
	bool ps5 = false;
	bool ready = false;	// READY vs STANDBY
};

// Broadcast discovery on the local network.
class Discovery
{
public:
	bool start();
	void stop();

	std::vector<DiscoveredHost> hosts();

	// One-shot wakeup: credential is the first 8 bytes of the regist key
	// interpreted as hex (chiaki convention).
	static bool wakeup(const std::string &addr, const char *regist_key);

private:
	static void cb(ChiakiDiscoveryHost *hosts, size_t hosts_count, void *user);

	ChiakiDiscoveryService service_{};
	bool running_ = false;
	std::mutex mutex_;
	std::vector<DiscoveredHost> hosts_;
};

} // namespace hayai::app
