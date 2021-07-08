#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <tuple>
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

	struct gpu_pipeline {
		winrt::com_ptr<ID3D12RootSignature> root_signature;
		winrt::com_ptr<ID3D12PipelineState> pipeline_state;
	};

	auto create_descriptor_heap(ID3D12Device& device, D3D12_DESCRIPTOR_HEAP_TYPE type, unsigned int size)
	{
		D3D12_DESCRIPTOR_HEAP_DESC description {};
		description.NumDescriptors = size;
		description.Type = type;
		return winrt::capture<ID3D12DescriptorHeap>(&device, &ID3D12Device::CreateDescriptorHeap, &description);
	}

	auto get_self_path()
	{
		std::vector<wchar_t> path_buffer(MAX_PATH + 1);
		winrt::check_bool(GetModuleFileName(nullptr, path_buffer.data(), MAX_PATH + 1));
		return std::filesystem::path {path_buffer.data()}.parent_path();
	}

	auto load_compiled_shader(gsl::cwzstring<> name)
	{
		static const auto parent_path {get_self_path()};
		const auto path = parent_path / name;
		std::vector<char> buffer(std::filesystem::file_size(path));
		std::ifstream reader {path, reader.binary};
		reader.exceptions(reader.badbit | reader.failbit);
		reader.read(buffer.data(), buffer.size());
		return buffer;
	}

	gpu_pipeline create_pipeline_state(ID3D12Device& device)
	{
		const auto vertex_shader = load_compiled_shader(L"vertex.cso");
		const auto pixel_shader = load_compiled_shader(L"pixel.cso");
		auto root_signature = winrt::capture<ID3D12RootSignature>(
			&device, &ID3D12Device::CreateRootSignature, 0, vertex_shader.data(), vertex_shader.size());

		D3D12_GRAPHICS_PIPELINE_STATE_DESC description {};
		description.pRootSignature = root_signature.get();
		description.VS.BytecodeLength = vertex_shader.size();
		description.VS.pShaderBytecode = vertex_shader.data();
		description.PS.BytecodeLength = pixel_shader.size();
		description.PS.pShaderBytecode = pixel_shader.data();
		description.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
		description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		description.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
		description.RasterizerState.DepthClipEnable = true;
		description.DepthStencilState.DepthEnable = true;
		description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
		description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		description.NumRenderTargets = 1;
		description.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		description.DSVFormat = DXGI_FORMAT_D32_FLOAT;
		description.SampleDesc.Count = 1;

		return {
			std::move(root_signature),
			winrt::capture<ID3D12PipelineState>(&device, &ID3D12Device::CreateGraphicsPipelineState, &description)};
	}

	auto
	create_depth_buffer(ID3D12Device& device, D3D12_CPU_DESCRIPTOR_HANDLE dsv, unsigned int width, unsigned int height)
	{
		D3D12_HEAP_PROPERTIES properties {};
		properties.Type = D3D12_HEAP_TYPE_DEFAULT;

		D3D12_RESOURCE_DESC description {};
		description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		description.DepthOrArraySize = 1;
		description.Width = width;
		description.Height = height;
		description.MipLevels = 1;
		description.SampleDesc.Count = 1;
		description.Format = DXGI_FORMAT_D32_FLOAT;
		description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

		D3D12_CLEAR_VALUE clear_value {};
		clear_value.DepthStencil.Depth = 1.0f;
		clear_value.Format = description.Format;

		const auto buffer = winrt::capture<ID3D12Resource>(
			&device,
			&ID3D12Device::CreateCommittedResource,
			&properties,
			D3D12_HEAP_FLAG_NONE,
			&description,
			D3D12_RESOURCE_STATE_DEPTH_WRITE,
			&clear_value);

		D3D12_DEPTH_STENCIL_VIEW_DESC dsv_description {};
		dsv_description.Format = description.Format;
		dsv_description.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		device.CreateDepthStencilView(buffer.get(), &dsv_description, dsv);

		return buffer;
	}

	struct descriptor_size_table {
		unsigned int rtv_size {};
		unsigned int dsv_size {};
	};

	auto get_sizes(ID3D12Device& device)
	{
		descriptor_size_table table {};
		table.rtv_size = device.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		table.dsv_size = device.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
		return table;
	}

	struct device_resources {
		const winrt::com_ptr<ID3D12Device4> device {};
		const descriptor_size_table size_table {};
		const winrt::com_ptr<ID3D12CommandQueue> queue {};
		const winrt::com_ptr<ID3D12DescriptorHeap> rtv_heap {};
		const winrt::com_ptr<ID3D12DescriptorHeap> dsv_heap {};
		const winrt::com_ptr<ID3D12CommandAllocator> allocator {};
		const winrt::com_ptr<ID3D12GraphicsCommandList> commands {};
		gpu_fence fence;

		device_resources(winrt::com_ptr<ID3D12Device4> device) :
			device {device},
			size_table {get_sizes(*device)},
			queue {create_direct_queue(*device)},
			rtv_heap {create_descriptor_heap(*device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2)},
			dsv_heap {create_descriptor_heap(*device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1)},
			allocator {winrt::capture<ID3D12CommandAllocator>(
				device, &ID3D12Device::CreateCommandAllocator, D3D12_COMMAND_LIST_TYPE_DIRECT)},
			commands {winrt::capture<ID3D12GraphicsCommandList>(
				device,
				&ID3D12Device4::CreateCommandList1,
				0,
				D3D12_COMMAND_LIST_TYPE_DIRECT,
				D3D12_COMMAND_LIST_FLAG_NONE)},
			fence {*device}
		{
		}
	};

	constexpr D3D12_CPU_DESCRIPTOR_HANDLE
	offset(D3D12_CPU_DESCRIPTOR_HANDLE handle, std::size_t size, std::size_t index)
	{
		return {handle.ptr + index * size};
	}

	struct swap_chain {
		winrt::com_ptr<IDXGISwapChain3> chain;
		std::array<winrt::com_ptr<ID3D12Resource>, 2> buffers;
		unsigned int width;
		unsigned int height;
	};

	swap_chain attach_swap_chain(
		HWND window, const device_resources& device, D3D12_CPU_DESCRIPTOR_HANDLE rtv_base, IDXGIFactory3& factory)
	{
		DXGI_SWAP_CHAIN_DESC1 description {};
		description.BufferCount = 2;
		description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		description.SampleDesc.Count = 1;
		description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		winrt::com_ptr<IDXGISwapChain1> swap_chain {};
		winrt::check_hresult(factory.CreateSwapChainForHwnd(
			device.queue.get(), window, &description, nullptr, nullptr, swap_chain.put()));

		std::array buffers {
			winrt::capture<ID3D12Resource>(swap_chain, &IDXGISwapChain::GetBuffer, 0),
			winrt::capture<ID3D12Resource>(swap_chain, &IDXGISwapChain::GetBuffer, 1)};

		for (gsl::index i {}; i < 2; ++i) {
			D3D12_RENDER_TARGET_VIEW_DESC rtv_description {};
			rtv_description.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			rtv_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
			device.device->CreateRenderTargetView(
				buffers.at(i).get(), &rtv_description, offset(rtv_base, device.size_table.rtv_size, i));
		}

		winrt::check_hresult(swap_chain->GetDesc1(&description));

		return {swap_chain.as<IDXGISwapChain3>(), std::move(buffers), description.Width, description.Height};
	}

	// Lots of reorganization opportunities here, around formats, order of initalization, and storing texture size / not
	// fetching them
	// Mismatch between declaration order and how the data is actually initialized (initialization layout vs. packed
	// layout) constness enforces constructor initializers
	// Also, how to handle resizes?
	// TODO: plenty of invariants to be enforced, methinks...
	struct window_resources {
		D3D12_CPU_DESCRIPTOR_HANDLE rtv_base {};
		swap_chain swap_chain {};
		D3D12_CPU_DESCRIPTOR_HANDLE dsv {};
		winrt::com_ptr<ID3D12Resource> depth_buffer {};

		window_resources(const device_resources& device, HWND window, IDXGIFactory3& factory) : window_resources {}
		{
			rtv_base = device.rtv_heap->GetCPUDescriptorHandleForHeapStart();
			swap_chain = attach_swap_chain(window, device, rtv_base, factory);
			dsv = device.dsv_heap->GetCPUDescriptorHandleForHeapStart();
			depth_buffer = create_depth_buffer(*device.device, dsv, swap_chain.width, swap_chain.height);
		}

		window_resources() = default;
	};

	void
	record_commands(const device_resources& device, const gpu_pipeline& pipeline, const window_resources& window_stuff)
	{
		auto& command_list = *device.commands;
		const auto index = window_stuff.swap_chain.chain->GetCurrentBackBufferIndex();
		auto& buffer = *window_stuff.swap_chain.buffers.at(index);
		const auto rtv = offset(window_stuff.rtv_base, device.size_table.rtv_size, index);

		winrt::check_hresult(command_list.Reset(device.allocator.get(), pipeline.pipeline_state.get()));

		command_list.SetGraphicsRootSignature(pipeline.root_signature.get());
		command_list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		command_list.OMSetRenderTargets(1, &rtv, false, &window_stuff.dsv);

		D3D12_RECT scissor {};
		scissor.right = window_stuff.swap_chain.width;
		scissor.bottom = window_stuff.swap_chain.height;

		D3D12_VIEWPORT viewport {};
		viewport.Width = gsl::narrow<float>(window_stuff.swap_chain.width);
		viewport.Height = gsl::narrow<float>(window_stuff.swap_chain.height);
		viewport.MaxDepth = 1.0f;

		command_list.RSSetScissorRects(1, &scissor);
		command_list.RSSetViewports(1, &viewport);

		std::array<D3D12_RESOURCE_BARRIER, 1> barriers {
			transition(buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET)};

		command_list.ResourceBarrier(gsl::narrow<UINT>(barriers.size()), barriers.data());

		std::array<float, 4> clear_color {0.0f, 0.0f, 0.0f, 1.0f};
		command_list.ClearDepthStencilView(window_stuff.dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
		command_list.ClearRenderTargetView(rtv, clear_color.data(), 0, nullptr);
		command_list.DrawInstanced(3, 1, 0, 0);

		reverse(barriers.at(0));
		command_list.ResourceBarrier(gsl::narrow<UINT>(barriers.size()), barriers.data());

		winrt::check_hresult(command_list.Close());
	}

	void execute_game_thread(const std::atomic_bool& is_exit_required, HWND window)
	{
		if constexpr (is_d3d12_debugging_enabled)
			winrt::capture<ID3D12Debug>(D3D12GetDebugInterface)->EnableDebugLayer();

		const auto factory = winrt::capture<IDXGIFactory6>(
			CreateDXGIFactory2, is_d3d12_debugging_enabled ? DXGI_CREATE_FACTORY_DEBUG : 0);

		device_resources device {get_high_performance_device(*factory)};
		window_resources window_stuff {device, window, *factory};
		const auto pipeline = create_pipeline_state(*device.device);

		winrt::check_bool(PostMessage(window, ready_message, 0, 0));
		while (!is_exit_required) {
			device.fence.block();

			winrt::check_hresult(device.allocator->Reset());

			record_commands(device, pipeline, window_stuff);

			ID3D12CommandList* const list_pointer {device.commands.get()};
			device.queue->ExecuteCommandLists(1, &list_pointer);

			winrt::check_hresult(window_stuff.swap_chain.chain->Present(1, 0));
			device.fence.bump(*device.queue);
		}

		device.fence.block();
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
