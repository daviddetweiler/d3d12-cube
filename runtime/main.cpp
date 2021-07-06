#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <tuple>
#include <utility>

#include <gsl/gsl>

#include <Windows.h>

#include <winrt/base.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>

#include "wavefront_object_loader.h"

namespace helium {
	constexpr auto is_d3d12_debugging_enabled = true;
	constexpr auto ready_message = WM_USER;

	GSL_SUPPRESS(type .1)
	LRESULT handle_message(HWND window, UINT message, WPARAM w, LPARAM l) noexcept
	{
		std::atomic_bool* is_size_updated
			= reinterpret_cast<std::atomic_bool*>(GetWindowLongPtr(window, GWLP_USERDATA));

		switch (message) {
		case ready_message:
			ShowWindow(window, SW_SHOW);
			return 0;

		case WM_SIZE:
			if (is_size_updated)
				*is_size_updated = true;

			return 0;

		case WM_CREATE: {
			SetWindowLongPtr(
				window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<LPCREATESTRUCT>(l)->lpCreateParams));

			return 0;
		}

		case WM_CLOSE:
			ShowWindow(window, SW_HIDE);
			PostQuitMessage(0);
			return 0;

		default:
			return DefWindowProc(window, message, w, l);
		}
	}

	auto get_high_performance_device(IDXGIFactory6& factory)
	{
		const auto adapter = winrt::capture<IDXGIAdapter>(
			&factory, &IDXGIFactory6::EnumAdapterByGpuPreference, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE);

		return winrt::capture<ID3D12Device4>(D3D12CreateDevice, adapter.get(), D3D_FEATURE_LEVEL_12_1);
	}

	auto create_direct_queue(ID3D12Device& device)
	{
		D3D12_COMMAND_QUEUE_DESC description {};
		description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		return winrt::capture<ID3D12CommandQueue>(&device, &ID3D12Device::CreateCommandQueue, &description);
	}

	class gpu_fence {
	public:
		gpu_fence(ID3D12Device& device) :
			m_value {},
			m_fence {winrt::capture<ID3D12Fence>(&device, &ID3D12Device::CreateFence, m_value, D3D12_FENCE_FLAG_NONE)},
			m_event {winrt::check_pointer(CreateEvent(nullptr, false, false, nullptr))}
		{
		}

		void bump(ID3D12CommandQueue& queue) { winrt::check_hresult(queue.Signal(m_fence.get(), ++m_value)); }

		void block()
		{
			if (m_fence->GetCompletedValue() < m_value) {
				winrt::check_hresult(m_fence->SetEventOnCompletion(m_value, m_event.get()));
				winrt::check_bool(WaitForSingleObject(m_event.get(), INFINITE) == WAIT_OBJECT_0);
			}
		}

	private:
		std::uint64_t m_value {};
		winrt::com_ptr<ID3D12Fence> m_fence {};
		winrt::handle m_event {};
	};

	auto attach_swap_chain(HWND window, ID3D12CommandQueue& present_queue, IDXGIFactory3& factory)
	{
		DXGI_SWAP_CHAIN_DESC1 description {};
		description.BufferCount = 2;
		description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		description.SampleDesc.Count = 1;
		description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		winrt::com_ptr<IDXGISwapChain1> swap_chain {};
		winrt::check_hresult(
			factory.CreateSwapChainForHwnd(&present_queue, window, &description, nullptr, nullptr, swap_chain.put()));

		return swap_chain.as<IDXGISwapChain3>();
	}

	auto transition(ID3D12Resource& resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept
	{
		D3D12_RESOURCE_BARRIER barrier {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = &resource;
		barrier.Transition.StateBefore = before;
		barrier.Transition.StateAfter = after;
		return barrier;
	}

	auto reverse(D3D12_RESOURCE_BARRIER& barrier) noexcept
	{
		Expects(barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION);
		std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
	}

	void
	record_commands(ID3D12GraphicsCommandList& command_list, D3D12_CPU_DESCRIPTOR_HANDLE rtv, ID3D12Resource& buffer)
	{
		std::array<D3D12_RESOURCE_BARRIER, 1> barriers {
			transition(buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET)};

		command_list.ResourceBarrier(gsl::narrow<UINT>(barriers.size()), barriers.data());

		std::array<float, 4> clear_color {0.0f, 0.0f, 0.0f, 1.0f};
		command_list.ClearRenderTargetView(rtv, clear_color.data(), 0, nullptr);

		reverse(barriers.at(0));
		command_list.ResourceBarrier(gsl::narrow<UINT>(barriers.size()), barriers.data());
	}

	auto create_rtv_heap(ID3D12Device& device)
	{
		D3D12_DESCRIPTOR_HEAP_DESC description {};
		description.NumDescriptors = 2;
		description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		return winrt::capture<ID3D12DescriptorHeap>(&device, &ID3D12Device::CreateDescriptorHeap, &description);
	}

	template <typename handle_type>
	constexpr auto offset(handle_type handle, unsigned int descriptor_size, std::uint64_t offset) noexcept
	{
		return handle_type {handle.ptr + descriptor_size * offset};
	}

	void create_rtvs(
		IDXGISwapChain& swap_chain, D3D12_CPU_DESCRIPTOR_HANDLE rtv_base, unsigned int rtv_size, ID3D12Device& device)
	{
		D3D12_RENDER_TARGET_VIEW_DESC description {};
		description.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		description.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		for (gsl::index i {}; i < 2; ++i) {
			const auto buffer
				= winrt::capture<ID3D12Resource>(&swap_chain, &IDXGISwapChain::GetBuffer, gsl::narrow<UINT>(i));

			device.CreateRenderTargetView(buffer.get(), &description, offset(rtv_base, rtv_size, i));
		}
	}

	template <typename type>
	gsl::span<type> map(ID3D12Resource& resource, unsigned int subresource, std::size_t count)
	{
		D3D12_RANGE range {};
		void* data {};
		winrt::check_hresult(resource.Map(subresource, &range, &data));
		return {static_cast<type*>(data), count * sizeof(type)};
	}

	template <typename... list_types>
	void execute(ID3D12CommandQueue& queue, list_types&... lists)
	{
		const std::initializer_list<ID3D12CommandList* const> list_list {(&lists, ...)};
		queue.ExecuteCommandLists(gsl::narrow<UINT>(list_list.size()), list_list.begin());
	}

	struct vertex_buffer {
		winrt::com_ptr<ID3D12Resource> resource;
		D3D12_VERTEX_BUFFER_VIEW view;
	};

	struct index_buffer {
		winrt::com_ptr<ID3D12Resource> resource;
		D3D12_INDEX_BUFFER_VIEW view;
	};

	struct object_buffers {
		vertex_buffer vertices;
		index_buffer indices;
	};

	// This function does *a lot*. It loads the wavefront, creates appropriately sized vertex and index buffers, creates
	// an upload buffer big enough for both, writes the upload commands, executes them, then returns the buffers with
	// set-up views
	object_buffers schedule_object_upload(
		ID3D12Device& device,
		ID3D12GraphicsCommandList& list,
		ID3D12CommandQueue& queue,
		gpu_fence& fence,
		ID3D12CommandAllocator& allocator)
	{
		D3D12_HEAP_PROPERTIES heap_properties {};
		heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;

		D3D12_HEAP_PROPERTIES upload_heap_properties {};
		upload_heap_properties.Type = D3D12_HEAP_TYPE_UPLOAD;

		const auto cube = load_wavefront_object("cube.obj");

		D3D12_RESOURCE_DESC vertex_buffer_description {};
		vertex_buffer_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		vertex_buffer_description.Width = cube.positions.size() * sizeof(decltype(cube.positions)::value_type);
		vertex_buffer_description.Height = 1;
		vertex_buffer_description.DepthOrArraySize = 1;
		vertex_buffer_description.Format = DXGI_FORMAT_UNKNOWN;
		vertex_buffer_description.SampleDesc.Count = 1;
		vertex_buffer_description.MipLevels = 1;
		vertex_buffer_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		const auto vertex_buffer = winrt::capture<ID3D12Resource>(
			&device,
			&ID3D12Device::CreateCommittedResource,
			&heap_properties,
			D3D12_HEAP_FLAG_NONE,
			&vertex_buffer_description,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr);

		const auto index_count = cube.faces.size() * 3;
		std::vector<unsigned int> indices(index_count);
		auto index_iterator = indices.begin();
		for (const auto& face : cube.faces) {
			for (const auto& vertex : face.vertices)
				*(index_iterator++) = vertex.position;
		}

		D3D12_RESOURCE_DESC index_buffer_description {};
		index_buffer_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		index_buffer_description.Width = index_count * sizeof(decltype(indices)::value_type);
		index_buffer_description.Height = 1;
		index_buffer_description.DepthOrArraySize = 1;
		index_buffer_description.Format = DXGI_FORMAT_UNKNOWN;
		index_buffer_description.SampleDesc.Count = 1;
		index_buffer_description.MipLevels = 1;
		index_buffer_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		const auto index_buffer = winrt::capture<ID3D12Resource>(
			&device,
			&ID3D12Device::CreateCommittedResource,
			&heap_properties,
			D3D12_HEAP_FLAG_NONE,
			&index_buffer_description,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr);

		D3D12_RESOURCE_DESC upload_buffer_description {};
		upload_buffer_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		upload_buffer_description.Width = vertex_buffer_description.Width + index_buffer_description.Width;
		upload_buffer_description.Height = 1;
		upload_buffer_description.DepthOrArraySize = 1;
		upload_buffer_description.Format = DXGI_FORMAT_UNKNOWN;
		upload_buffer_description.SampleDesc.Count = 1;
		upload_buffer_description.MipLevels = 1;
		upload_buffer_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		const auto upload_buffer = winrt::capture<ID3D12Resource>(
			&device,
			&ID3D12Device::CreateCommittedResource,
			&upload_heap_properties,
			D3D12_HEAP_FLAG_NONE,
			&upload_buffer_description,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr);

		const auto upload_span = map<std::byte>(*upload_buffer, 0, upload_buffer_description.Width);
		const auto upload_iterator = upload_span.begin();
		const auto vertex_bytes = gsl::as_bytes(gsl::span {cube.positions});
		const auto index_bytes = gsl::as_bytes(gsl::span {indices});
		std::copy(vertex_bytes.begin(), vertex_bytes.end(), upload_iterator);
		std::copy(index_bytes.begin(), index_bytes.end(), std::next(upload_iterator, vertex_buffer_description.Width));
		upload_buffer->Unmap(0, nullptr);

		winrt::check_hresult(list.Reset(&allocator, nullptr));

		list.CopyBufferRegion(vertex_buffer.get(), 0, upload_buffer.get(), 0, vertex_buffer_description.Width);
		list.CopyBufferRegion(
			index_buffer.get(),
			0,
			upload_buffer.get(),
			vertex_buffer_description.Width,
			index_buffer_description.Width);

		const std::initializer_list barriers {
			transition(*vertex_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
			transition(*index_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER)};

		list.ResourceBarrier(gsl::narrow<UINT>(barriers.size()), barriers.begin());

		winrt::check_hresult(list.Close());
		execute(queue, list);
		fence.bump(queue);

		D3D12_VERTEX_BUFFER_VIEW vertices_view {};
		vertices_view.BufferLocation = vertex_buffer->GetGPUVirtualAddress();
		vertices_view.SizeInBytes = gsl::narrow<UINT>(vertex_buffer_description.Width);
		vertices_view.StrideInBytes = sizeof(decltype(cube.positions)::value_type);

		D3D12_INDEX_BUFFER_VIEW indices_view {};
		indices_view.BufferLocation = index_buffer->GetGPUVirtualAddress();
		indices_view.Format = DXGI_FORMAT_R32_UINT;
		indices_view.SizeInBytes = gsl::narrow<UINT>(index_buffer_description.Width);

		// Can't release the upload buffer while the command list is still in-flight
		fence.block();

		return {{vertex_buffer, vertices_view}, {index_buffer, indices_view}};
	}

	void execute_game_thread(const std::atomic_bool& is_exit_required, std::atomic_bool& is_size_updated, HWND window)
	{
		if constexpr (is_d3d12_debugging_enabled)
			winrt::capture<ID3D12Debug>(D3D12GetDebugInterface)->EnableDebugLayer();

		const auto factory = winrt::capture<IDXGIFactory6>(
			CreateDXGIFactory2, is_d3d12_debugging_enabled ? DXGI_CREATE_FACTORY_DEBUG : 0);

		const auto device = get_high_performance_device(*factory);
		const auto direct_queue = create_direct_queue(*device);
		gpu_fence fence {*device};

		const auto rtv_heap = create_rtv_heap(*device);
		const auto rtv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		const auto rtv_base = rtv_heap->GetCPUDescriptorHandleForHeapStart();
		const auto swap_chain = attach_swap_chain(window, *direct_queue, *factory);
		create_rtvs(*swap_chain, rtv_base, rtv_size, *device);

		const auto command_allocator = winrt::capture<ID3D12CommandAllocator>(
			device, &ID3D12Device::CreateCommandAllocator, D3D12_COMMAND_LIST_TYPE_DIRECT);

		const auto command_list = winrt::capture<ID3D12GraphicsCommandList>(
			device,
			&ID3D12Device4::CreateCommandList1,
			0,
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			D3D12_COMMAND_LIST_FLAG_NONE);

		const auto object = schedule_object_upload(*device, *command_list, *direct_queue, fence, *command_allocator);

		winrt::check_bool(PostMessage(window, ready_message, 0, 0));
		while (!is_exit_required) {
			fence.block();

			if (is_size_updated) {
				OutputDebugString(L"[INFO] Size was updated\n");
				is_size_updated = false;

				winrt::check_hresult(swap_chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0));
				create_rtvs(*swap_chain, rtv_base, rtv_size, *device);
			}

			winrt::check_hresult(command_allocator->Reset());
			winrt::check_hresult(command_list->Reset(command_allocator.get(), nullptr));
			const auto index = swap_chain->GetCurrentBackBufferIndex();
			record_commands(
				*command_list,
				offset(rtv_base, rtv_size, index),
				*winrt::capture<ID3D12Resource>(swap_chain, &IDXGISwapChain::GetBuffer, index));

			winrt::check_hresult(command_list->Close());

			execute(*direct_queue, *command_list);

			winrt::check_hresult(swap_chain->Present(1, 0));
			fence.bump(*direct_queue);
		}

		fence.block();
	}
}

int WinMain(HINSTANCE self, HINSTANCE, char*, int)
{
	using namespace helium;

	WNDCLASS window_class {};
	window_class.hInstance = self;
	window_class.lpszClassName = L"helium::window";
	window_class.hCursor = winrt::check_pointer(LoadCursor(nullptr, IDC_ARROW));
	window_class.lpfnWndProc = handle_message;
	winrt::check_bool(RegisterClass(&window_class));

	std::atomic_bool is_size_updated {};
	const auto window = winrt::check_pointer(CreateWindowEx(
		WS_EX_APPWINDOW | WS_EX_NOREDIRECTIONBITMAP,
		window_class.lpszClassName,
		L"Helium Prototype",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		nullptr,
		nullptr,
		self,
		&is_size_updated));

	std::atomic_bool is_exit_required {};
	std::thread game_thread {[&is_exit_required, &is_size_updated, window] {
		execute_game_thread(is_exit_required, is_size_updated, window);
	}};

	MSG message {};
	while (GetMessage(&message, nullptr, 0, 0)) {
		TranslateMessage(&message);
		DispatchMessage(&message);
	}

	is_exit_required = true;
	game_thread.join();

	return 0;
}
