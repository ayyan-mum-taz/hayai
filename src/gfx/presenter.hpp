// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include "gfx/pool.hpp"

#include <deko3d.hpp>

#include <cstdint>

namespace hayai::gfx {

// Owns the deko3d device, the render queue and the swapchain over the console's
// default window.
//
// Swapchain depth is a real latency/smoothness trade, so it is a choice rather
// than a constant. Two images is the minimum the window system accepts and the
// least the compositor can hold between us and the panel; three lets the
// display pipeline stay busy across a hiccup, at the cost of one more frame of
// potential queueing.
class Presenter
{
public:
	enum class Mode
	{
		// Hand the frame to the compositor the moment it is drawn. Nothing
		// waits for a vblank; the panel may show part of two frames at once.
		Immediate,
		// Hold the frame until the next vblank. Tear-free, evenly paced, and
		// costs up to one refresh interval.
		Vsync,
	};

	static constexpr unsigned kMaxImages = 3;

	Presenter() = default;
	~Presenter() { destroy(); }
	Presenter(const Presenter &) = delete;
	Presenter &operator=(const Presenter &) = delete;

	bool create(unsigned width, unsigned height, Mode mode = Mode::Immediate, unsigned images = 2);
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
	unsigned image_count() const { return image_count_; }

private:
	dk::Device device_ = nullptr;
	dk::Queue queue_ = nullptr;
	dk::Swapchain swapchain_ = nullptr;
	Pool fb_pool_;
	dk::Image images_[kMaxImages];
	unsigned image_count_ = 2;
	unsigned width_ = 0;
	unsigned height_ = 0;
	Mode mode_ = Mode::Immediate;
};

} // namespace hayai::gfx
