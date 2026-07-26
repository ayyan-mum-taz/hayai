// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "app/discovery.hpp"
#include "core/log.hpp"

#include <chiaki/session.h>	// CHIAKI_SESSION_AUTH_SIZE

#include <arpa/inet.h>
#include <netinet/in.h>

#include <cstdlib>
#include <cstring>

namespace hayai::app {

void Discovery::cb(ChiakiDiscoveryHost *hosts, size_t hosts_count, void *user)
{
	auto *self = static_cast<Discovery *>(user);
	std::vector<DiscoveredHost> out;
	out.reserve(hosts_count);
	for(size_t i = 0; i < hosts_count; i++)
	{
		ChiakiDiscoveryHost *h = &hosts[i];
		DiscoveredHost d;
		d.name = h->host_name ? h->host_name : "?";
		d.addr = h->host_addr ? h->host_addr : "";
		d.id = h->host_id ? h->host_id : "";
		d.ps5 = chiaki_discovery_host_is_ps5(h);
		d.ready = h->state == CHIAKI_DISCOVERY_HOST_STATE_READY;
		out.push_back(std::move(d));
	}

	std::lock_guard<std::mutex> lock(self->mutex_);
	self->hosts_ = std::move(out);
}

bool Discovery::start()
{
	if(running_)
		return true;

	// PS5 discovery port is 9302 (PS4: 987). Broadcast to the local subnet.
	auto *addr = static_cast<sockaddr_storage *>(calloc(1, sizeof(sockaddr_storage)));
	auto *in = reinterpret_cast<sockaddr_in *>(addr);
	in->sin_family = AF_INET;
	in->sin_addr.s_addr = INADDR_BROADCAST;

	ChiakiDiscoveryServiceOptions options{};
	options.hosts_max = 8;
	options.host_drop_pings = 3;
	options.ping_ms = 500;
	options.ping_initial_ms = 100;
	options.send_addr = addr;
	options.send_addr_size = sizeof(sockaddr_in);
	options.send_host = nullptr;
	options.cb = &Discovery::cb;
	options.cb_user = this;

	ChiakiErrorCode err = chiaki_discovery_service_init(&service_, &options, core::log().chiaki_log());
	free(addr);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		HAYAI_LOGE("discovery: init failed: %s", chiaki_error_string(err));
		return false;
	}
	running_ = true;
	return true;
}

void Discovery::stop()
{
	if(!running_)
		return;
	chiaki_discovery_service_fini(&service_);
	running_ = false;
	std::lock_guard<std::mutex> lock(mutex_);
	hosts_.clear();
}

std::vector<DiscoveredHost> Discovery::hosts()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return hosts_;
}

bool Discovery::wakeup(const std::string &addr, const char *regist_key)
{
	// The wakeup credential is the regist key parsed as a hex number.
	char key_str[CHIAKI_SESSION_AUTH_SIZE + 1]{};
	memcpy(key_str, regist_key, CHIAKI_SESSION_AUTH_SIZE);
	const uint64_t credential = strtoull(key_str, nullptr, 16);

	ChiakiErrorCode err = chiaki_discovery_wakeup(core::log().chiaki_log(), nullptr,
		addr.c_str(), credential, true);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		HAYAI_LOGE("wakeup: failed: %s", chiaki_error_string(err));
		return false;
	}
	HAYAI_LOGI("wakeup: sent to %s", addr.c_str());
	return true;
}

} // namespace hayai::app
