#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <utility>

#include <gsl/gsl>

#include <Windows.h>

#include <winrt/base.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>

#include <DirectXMath.h>

#include "wavefront_object_loader.h"

namespace helium {
	constexpr auto is_d3d12_debugging_enabled = false;
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

	void reverse(D3D12_RESOURCE_BARRIER& barrier) noexcept
	{
		Expects(barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION);
		std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
	}

	struct root_constants {
		DirectX::XMMATRIX projection;
		DirectX::XMMATRIX view;
	};

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
		const std::array<ID3D12CommandList* const, sizeof...(lists)> list_list {(&lists, ...)};
		queue.ExecuteCommandLists(gsl::narrow<UINT>(list_list.size()), list_list.data());
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
		unsigned int index_count;
	};

	struct vertex_data {
		std::vector<vector3> vertices {};
		std::vector<unsigned int> indices {};
	};

	vertex_data load_wavefront_vertices(gsl::czstring<> name)
	{
		auto object = load_wavefront_object(name);
		const auto index_count = object.faces.size() * 3;
		std::vector<unsigned int> indices(index_count);
		auto index_iterator = indices.begin();
		for (const auto& face : object.faces) {
			for (const auto& vertex : face.vertices)
				*(index_iterator++) = vertex.position - 1;
		}

		return {std::move(object.positions), std::move(indices)};
	}

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

		const auto cube = load_wavefront_vertices("cube.wv");

		D3D12_RESOURCE_DESC vertex_buffer_description {};
		vertex_buffer_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		vertex_buffer_description.Width = cube.vertices.size() * sizeof(decltype(cube.vertices)::value_type);
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

		D3D12_RESOURCE_DESC index_buffer_description {};
		index_buffer_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		index_buffer_description.Width = cube.indices.size() * sizeof(decltype(cube.indices)::value_type);
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
		const auto vertex_bytes = gsl::as_bytes(gsl::span {cube.vertices});
		const auto index_bytes = gsl::as_bytes(gsl::span {cube.indices});
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
		vertices_view.StrideInBytes = sizeof(decltype(cube.vertices)::value_type);

		D3D12_INDEX_BUFFER_VIEW indices_view {};
		indices_view.BufferLocation = index_buffer->GetGPUVirtualAddress();
		indices_view.Format = DXGI_FORMAT_R32_UINT;
		indices_view.SizeInBytes = gsl::narrow<UINT>(index_buffer_description.Width);

		// Can't release the upload buffer while the command list is still in-flight
		fence.block();

		const auto index_count = cube.indices.size();

		return {{vertex_buffer, vertices_view}, {index_buffer, indices_view}, gsl::narrow<unsigned int>(index_count)};
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

	auto extract_root_signature(ID3D12Device& device, gsl::span<const char> bytecode)
	{
		return winrt::capture<ID3D12RootSignature>(
			&device, &ID3D12Device::CreateRootSignature, 0, bytecode.data(), bytecode.size());
	}

	struct gpu_pipeline {
		winrt::com_ptr<ID3D12RootSignature> root_signature;
		winrt::com_ptr<ID3D12PipelineState> pipeline_state;
	};

	gpu_pipeline create_pipeline(ID3D12Device& device)
	{
		const auto vertex_shader = load_compiled_shader(L"vertex.cso");
		const auto pixel_shader = load_compiled_shader(L"pixel.cso");
		auto root_signature = extract_root_signature(device, vertex_shader);

		D3D12_INPUT_ELEMENT_DESC position {};
		position.Format = DXGI_FORMAT_R32G32B32_FLOAT;
		position.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
		position.SemanticName = "SV_POSITION";

		D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_description {};
		pipeline_description.pRootSignature = root_signature.get();
		pipeline_description.VS.BytecodeLength = vertex_shader.size();
		pipeline_description.VS.pShaderBytecode = vertex_shader.data();
		pipeline_description.PS.BytecodeLength = pixel_shader.size();
		pipeline_description.PS.pShaderBytecode = pixel_shader.data();
		pipeline_description.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
		pipeline_description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		pipeline_description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		pipeline_description.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
		pipeline_description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pipeline_description.NumRenderTargets = 1;
		pipeline_description.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		pipeline_description.SampleDesc.Count = 1;
		pipeline_description.InputLayout.NumElements = 1;
		pipeline_description.InputLayout.pInputElementDescs = &position;

		return {
			std::move(root_signature),
			winrt::capture<ID3D12PipelineState>(
				&device, &ID3D12Device::CreateGraphicsPipelineState, &pipeline_description)};
	}

	void maximize_rasterizer(ID3D12GraphicsCommandList& list, unsigned int width, unsigned int height)
	{
		D3D12_RECT scissor {};
		scissor.right = width;
		scissor.bottom = height;
		list.RSSetScissorRects(1, &scissor);

		D3D12_VIEWPORT viewport {};
		viewport.Width = gsl::narrow<float>(width);
		viewport.Height = gsl::narrow<float>(height);
		viewport.MaxDepth = D3D12_DEFAULT_VIEWPORT_MAX_DEPTH;
		list.RSSetViewports(1, &viewport);
	}

	void record_commands(
		ID3D12GraphicsCommandList& command_list,
		D3D12_CPU_DESCRIPTOR_HANDLE rtv,
		ID3D12Resource& buffer,
		unsigned int width,
		unsigned int height,
		ID3D12RootSignature& root_signature,
		const root_constants& constants,
		const object_buffers& object)
	{
		command_list.SetGraphicsRootSignature(&root_signature);
		command_list.SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);

		command_list.IASetIndexBuffer(&object.indices.view);
		command_list.IASetVertexBuffers(0, 1, &object.vertices.view);
		command_list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		maximize_rasterizer(command_list, width, height);

		command_list.OMSetRenderTargets(1, &rtv, false, nullptr);

		std::array barriers {transition(buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET)};

		command_list.ResourceBarrier(gsl::narrow<UINT>(barriers.size()), barriers.data());

		std::array clear_color {0.0f, 0.0f, 0.0f, 1.0f};
		command_list.ClearRenderTargetView(rtv, clear_color.data(), 0, nullptr);
		command_list.DrawIndexedInstanced(object.index_count, 1, 0, 0, 0);

		reverse(barriers.at(0));
		command_list.ResourceBarrier(gsl::narrow<UINT>(barriers.size()), barriers.data());
	}

	auto process_resize(
		IDXGISwapChain& swap_chain,
		D3D12_CPU_DESCRIPTOR_HANDLE rtv_base,
		unsigned int rtv_size,
		ID3D12Device& device,
		root_constants& constants)
	{
		create_rtvs(swap_chain, rtv_base, rtv_size, device);

		DXGI_SWAP_CHAIN_DESC description {};
		winrt::check_hresult(swap_chain.GetDesc(&description));
		const auto aspect = static_cast<float>(description.BufferDesc.Width) / description.BufferDesc.Height;
		constants.projection = DirectX::XMMatrixPerspectiveFovRH(90.0f * 3.14159265f / 180.0f, aspect, 0.01f, 10.0f);

		return std::make_tuple(description.BufferDesc.Width, description.BufferDesc.Height);
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
		const auto pipeline = create_pipeline(*device);

		const auto command_allocator = winrt::capture<ID3D12CommandAllocator>(
			device, &ID3D12Device::CreateCommandAllocator, D3D12_COMMAND_LIST_TYPE_DIRECT);

		const auto command_list = winrt::capture<ID3D12GraphicsCommandList>(
			device,
			&ID3D12Device4::CreateCommandList1,
			0,
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			D3D12_COMMAND_LIST_FLAG_NONE);

		const auto object = schedule_object_upload(*device, *command_list, *direct_queue, fence, *command_allocator);

		root_constants constants {};
		constants.view = DirectX::XMMatrixIdentity();
		auto [width, height] = process_resize(*swap_chain, rtv_base, rtv_size, *device, constants);

		winrt::check_bool(PostMessage(window, ready_message, 0, 0));
		while (!is_exit_required) {
			fence.block();

			if (is_size_updated) {
				OutputDebugString(L"[INFO] Size was updated\n");
				is_size_updated = false;
				winrt::check_hresult(swap_chain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0));
				std::tie(width, height) = process_resize(*swap_chain, rtv_base, rtv_size, *device, constants);
			}

			winrt::check_hresult(command_allocator->Reset());
			winrt::check_hresult(command_list->Reset(command_allocator.get(), pipeline.pipeline_state.get()));
			const auto index = swap_chain->GetCurrentBackBufferIndex();
			record_commands(
				*command_list,
				offset(rtv_base, rtv_size, index),
				*winrt::capture<ID3D12Resource>(swap_chain, &IDXGISwapChain::GetBuffer, index),
				width,
				height,
				*pipeline.root_signature,
				constants,
				object);

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
