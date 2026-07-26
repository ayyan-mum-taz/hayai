// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include "gfx/pool.hpp"

#include <deko3d.hpp>

#include <cstdint>

namespace hayai::gfx {

// Owns the deko3d device, the render queue and the swapchain over the console's
// default window.
//
// The swapchain is deliberately only two images deep. Every extra image is a
// frame the compositor is allowed to hold between us and the panel, and on a
// 60 Hz display that is 16.7 ms each. Two is the minimum the window system
// accepts and it is what we want: render into one, show the other.
class Presenter
{
public:
	enum class Mode
	{
		// Hand the frame to the compositor the moment it is drawn. The panel
		// may show part of two frames at once, and in exchange nothing waits
		// for a vblank. This is the point of the project.
		Immediate,
		// Hold the frame until the next vblank. Tear-free, and costs up to one
		// refresh interval.
		Vsync,
	};

	static constexpr unsigned kImageCount = 2;

	Presenter() = default;
	~Presenter() { destroy(); }
	Presenter(const Presenter &) = delete;
	Presenter &operator=(const Presenter &) = delete;

	bool create(unsigned width, unsigned height, Mode mode = Mode::Immediate);
	void destroy();

	void set_mode(Mode mode);
	Mode mode() const { return mode_; }

	// Blocks until a backbuffer is free, then returns its slot index.
	int acquire();
	// Queues the slot for display and flushes the queue.
	void present(int slot);

	dk::Device device() const { return device_; }
	dk::Queue queue() const { return queue_; }
	const dk::Image &image(int slot) const { return images_[slot]; }

	unsigned width() const { return width_; }
	unsigned height() const { return height_; }

private:
	dk::Device device_ = nullptr;
	dk::Queue queue_ = nullptr;
	dk::Swapchain swapchain_ = nullptr;
	Pool fb_pool_;
	dk::Image images_[kImageCount];
	unsigned width_ = 0;
	unsigned height_ = 0;
	Mode mode_ = Mode::Immediate;
};

} // namespace hayai::gfx
