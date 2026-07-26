// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "gfx/presenter.hpp"
#include "core/log.hpp"

#include <switch.h>

#include <array>

namespace hayai::gfx {

bool Presenter::create(unsigned width, unsigned height, Mode mode, unsigned images)
{
	destroy();

	width_ = width;
	height_ = height;
	image_count_ = images < 2 ? 2 : (images > kMaxImages ? kMaxImages : images);

	device_ = dk::DeviceMaker{}.create();
	if(!device_)
	{
		HAYAI_LOGE("presenter: dkDeviceCreate failed");
		return false;
	}

	queue_ = dk::QueueMaker{ device_ }.setFlags(DkQueueFlags_Graphics).create();
	if(!queue_)
	{
		HAYAI_LOGE("presenter: dkQueueCreate failed");
		return false;
	}

	dk::ImageLayout fb_layout;
	dk::ImageLayoutMaker{ device_ }
		.setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression)
		.setFormat(DkImageFormat_RGBA8_Unorm)
		.setDimensions(width_, height_)
		.initialize(fb_layout);

	const uint32_t fb_size = static_cast<uint32_t>(fb_layout.getSize());
	const uint32_t fb_align = fb_layout.getAlignment();

	if(!fb_pool_.create(device_, (fb_size + fb_align) * image_count_ + DK_MEMBLOCK_ALIGNMENT,
			DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image))
	{
		HAYAI_LOGE("presenter: framebuffer pool alloc failed (%u bytes)",
			(fb_size + fb_align) * image_count_ + DK_MEMBLOCK_ALIGNMENT);
		return false;
	}

	std::array<const DkImage *, kMaxImages> image_ptrs{};
	for(unsigned i = 0; i < image_count_; i++)
	{
		Pool::Alloc a = fb_pool_.allocate(fb_size, fb_align);
		if(!a)
		{
			HAYAI_LOGE("presenter: framebuffer %u suballoc failed (%u/%u used)",
				i, fb_pool_.used(), fb_pool_.capacity());
			return false;
		}
		images_[i].initialize(fb_layout, fb_pool_.block(), a.offset);
		image_ptrs[i] = &images_[i];
	}

	NWindow *win = nwindowGetDefault();
	nwindowSetDimensions(win, width_, height_);

	swapchain_ = dk::SwapchainMaker{ device_, win, image_ptrs.data(), image_count_ }.create();
	if(!swapchain_)
	{
		HAYAI_LOGE("presenter: dkSwapchainCreate failed");
		return false;
	}

	set_mode(mode);
	return true;
}

void Presenter::destroy()
{
	if(queue_)
		queue_.waitIdle();

	if(swapchain_)
	{
		swapchain_.destroy();
		swapchain_ = nullptr;
	}
	fb_pool_.destroy();
	if(queue_)
	{
		queue_.destroy();
		queue_ = nullptr;
	}
	if(device_)
	{
		device_.destroy();
		device_ = nullptr;
	}
}

void Presenter::set_mode(Mode mode)
{
	mode_ = mode;
	// The window's swap interval is what actually decides whether presentation
	// waits for a vblank; deko3d hands the buffer straight to it.
	nwindowSetSwapInterval(nwindowGetDefault(), mode == Mode::Vsync ? 1 : 0);
}

int Presenter::acquire()
{
	return queue_.acquireImage(swapchain_);
}

void Presenter::present(int slot)
{
	queue_.presentImage(swapchain_, slot);
	queue_.flush();
}

} // namespace hayai::gfx
