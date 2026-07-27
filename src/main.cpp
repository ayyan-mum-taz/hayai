// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// hayai - low-latency PlayStation Remote Play for Nintendo Switch.
//
// main() owns process-wide bring-up in dependency order: sockets (sized for
// burst absorption), the ring logger (so no later thread ever blocks on I/O),
// the chiaki thread-affinity hook (so the receive thread lands on its own
// core), then the UI loop, which alternates between console menus and streams.

#include "app/config.hpp"
#include "app/ui.hpp"
#include "core/log.hpp"
#include "core/thread.hpp"

#include <switch.h>

#include <chiaki/common.h>

extern "C" {

// Larger service-side UDP receive buffering than the 0xA500 default: the
// stream path deliberately stays inside NVDEC for a few ms while packets keep
// arriving, and this is where they wait. (The per-socket SO_RCVBUF is set by
// libchiaki itself.)
void userAppInit(void)
{
	static const SocketInitConfig socket_config = {
		.tcp_tx_buf_size = 0x8000,
		.tcp_rx_buf_size = 0x10000,
		.tcp_tx_buf_max_size = 0x40000,
		.tcp_rx_buf_max_size = 0x40000,
		.udp_tx_buf_size = 0x10000,
		.udp_rx_buf_size = 0x100000,	// 1 MiB
		.sb_efficiency = 4,
		.num_bsd_sessions = 3,
		.bsd_service_type = BsdServiceType_User,
	};
	socketInitialize(&socket_config);
	romfsInit();
}

void userAppExit(void)
{
	romfsExit();
	socketExit();
}

} // extern "C"

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	// Deliberately NOT nxlinkStdio(): redirecting stdout is what makes logging
	// unsafe once the text console has been torn down. We keep the raw socket
	// and the logger writes to it directly, bypassing stdio entirely.
	const int nxlink_fd = nxlinkConnectToHost(false, false);

	hayai::core::log().set_sink_fd(nxlink_fd);
	hayai::core::log().start();
	hayai::core::install_chiaki_affinity_hook();

	chiaki_lib_init();
	HAYAI_LOGI("hayai 0.10.0 starting");

	hayai::app::Config config;
	if(!config.load())
		HAYAI_LOGI("config: no %s yet, starting fresh", hayai::app::Config::kPath);

	{
		hayai::app::Ui ui(config);
		ui.run();
	}

	config.save();
	HAYAI_LOGI("bye");
	hayai::core::log().stop();
	if(nxlink_fd >= 0)
		close(nxlink_fd);
	return 0;
}
