#include <array>
#include <atomic>
#include <cstdint>
#include <utility>

#include <gsl/gsl>

#include <Windows.h>

#include <winrt/base.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>

namespace helium {
	constexpr auto is_d3d12_debugging_enabled = true;
	constexpr auto ready_message = WM_USER;

	LRESULT handle_message(HWND window, UINT message, WPARAM w, LPARAM l) noexcept
	{
		switch (message) {
		case ready_message:
			ShowWindow(window, SW_SHOW);
			return 0;

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

	void create_rtvs(IDXGISwapChain& swap_chain, ID3D12DescriptorHeap& heap, ID3D12Device& device)
	{
		D3D12_RENDER_TARGET_VIEW_DESC description {};
		description.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		description.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		const auto size = device.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		auto handle = heap.GetCPUDescriptorHandleForHeapStart();
		for (gsl::index i {}; i < 2; ++i) {
			const auto buffer
				= winrt::capture<ID3D12Resource>(&swap_chain, &IDXGISwapChain::GetBuffer, gsl::narrow<UINT>(i));

			device.CreateRenderTargetView(buffer.get(), &description, handle);
			handle.ptr += size;
		}
	}

	void execute_game_thread(const std::atomic_bool& is_exit_required, HWND window)
	{
		if constexpr (is_d3d12_debugging_enabled)
			winrt::capture<ID3D12Debug>(D3D12GetDebugInterface)->EnableDebugLayer();

		const auto factory = winrt::capture<IDXGIFactory6>(
			CreateDXGIFactory2, is_d3d12_debugging_enabled ? DXGI_CREATE_FACTORY_DEBUG : 0);

		const auto device = get_high_performance_device(*factory);
		const auto direct_queue = create_direct_queue(*device);
		gpu_fence fence {*device};

		const auto rtv_heap = create_rtv_heap(*device);
		const auto swap_chain = attach_swap_chain(window, *direct_queue, *factory);
		create_rtvs(*swap_chain, *rtv_heap, *device);

		const auto command_allocator = winrt::capture<ID3D12CommandAllocator>(
			device, &ID3D12Device::CreateCommandAllocator, D3D12_COMMAND_LIST_TYPE_DIRECT);

		const auto command_list = winrt::capture<ID3D12GraphicsCommandList>(
			device,
			&ID3D12Device4::CreateCommandList1,
			0,
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			D3D12_COMMAND_LIST_FLAG_NONE);

		winrt::check_bool(PostMessage(window, ready_message, 0, 0));
		while (!is_exit_required) {
			fence.block();

			winrt::check_hresult(command_allocator->Reset());
			winrt::check_hresult(command_list->Reset(command_allocator.get(), nullptr));
			const std::uint64_t size {device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)};
			const auto index = swap_chain->GetCurrentBackBufferIndex();
			const D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle {
				rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr + index * size};

			record_commands(
				*command_list,
				rtv_handle,
				*winrt::capture<ID3D12Resource>(swap_chain, &IDXGISwapChain::GetBuffer, index));

			winrt::check_hresult(command_list->Close());

			ID3D12CommandList* const list_pointer {command_list.get()};
			direct_queue->ExecuteCommandLists(1, &list_pointer);

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
	std::thread game_thread {
		[&is_exit_required, &is_size_updated, window] { execute_game_thread(is_exit_required, window); }};

	MSG message {};
	while (GetMessage(&message, nullptr, 0, 0)) {
		TranslateMessage(&message);
		DispatchMessage(&message);
	}

	is_exit_required = true;
	game_thread.join();

	return 0;
}
