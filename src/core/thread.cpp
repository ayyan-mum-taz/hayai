// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "core/thread.hpp"
#include "core/log.hpp"

#include <chiaki/thread.h>

namespace hayai::core {

namespace {

void affinity_cb(ChiakiThreadName name, void *user)
{
	(void)user;

	int core;
	int prio;
	const char *label;

	switch(name)
	{
		case CHIAKI_THREAD_NAME_TAKION:
			// The hot path: recv -> decrypt -> reassemble -> decode -> present.
			core = kCoreStream;
			prio = kPrioHot;
			label = "takion";
			break;
		case CHIAKI_THREAD_NAME_TAKION_SEND:
		case CHIAKI_THREAD_NAME_FEEDBACK:
			// Input leaves through these; keep them snappy, but off both the hot
			// core and the audio core. They live with the input sampler that
			// feeds them.
			core = kCoreMain;
			prio = kPrioHot;
			label = "takion-send/feedback";
			break;
		case CHIAKI_THREAD_NAME_GKCRYPT:
			// Keystream precompute: must stay ahead of the stream but never
			// preempt it, and never share with audio.
			core = kCoreMain;
			prio = kPrioAux;
			label = "gkcrypt";
			break;
		default:
			core = kCoreMain;
			prio = kPrioAux;
			label = "other";
			break;
	}

	const Result rc_core = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
	const Result rc_prio = svcSetThreadPriority(CUR_THREAD_HANDLE, static_cast<u32>(prio));
	if(R_FAILED(rc_core) || R_FAILED(rc_prio))
		HAYAI_LOGW("affinity: %s -> core %d prio 0x%x failed (%x/%x)", label, core, prio, rc_core, rc_prio);
	else
		HAYAI_LOGD("affinity: %s -> core %d prio 0x%x", label, core, prio);
}

} // namespace

void install_chiaki_affinity_hook()
{
	chiaki_thread_set_affinity_cb(&affinity_cb, nullptr);
}

} // namespace hayai::core
