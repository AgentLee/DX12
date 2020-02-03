#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h> // For CommandLineToArgvW

// The min/max macros conflict with like-named member functions.
// Only use std::min and std::max defined in <algorithm>.
#if defined(min)
#undef min
#endif

#if defined(max)
#undef max
#endif

// In order to define a function called CreateWindow, the Windows macro needs to
// be undefined.
#if defined(CreateWindow)
#undef CreateWindow
#endif

// Windows Runtime Library. Needed for Microsoft::WRL::ComPtr<> template class.
#include <wrl.h>
using namespace Microsoft::WRL;

// DirectX 12 specific headers.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

// D3D12 extension library.
#include "../include/d3dx12.h"

// STL Headers
#include <algorithm>
#include <cassert>
#include <chrono>

#include "../include/helpers.h"

// Number of swap chain back buffers
const uint8_t g_numFrames = 3;

// Flag to use software rasterizer
bool g_useWarp = false;

uint32_t g_clientWidth = 1280;
uint32_t g_clientHeight = 1080;

bool g_isInitialized = false;

// Window handle
HWND g_hWnd;
// Window rectangle used to toggle fullscreen - stores previous dimensions
RECT g_windowRect;

// DX12 objects
ComPtr<ID3D12Device2> g_device;
ComPtr<ID3D12CommandQueue> g_commandQueue;
ComPtr<IDXGISwapChain4> g_swapChain;
ComPtr<ID3D12Resource> g_backBuffers[g_numFrames];
ComPtr<ID3D12GraphicsCommandList> g_commandList;
ComPtr<ID3D12CommandAllocator> g_commandAllocators[g_numFrames];
ComPtr<ID3D12DescriptorHeap> g_RTVDescriptorHeap;

UINT g_RTVDescriptorSize;
UINT g_currBackBufferIndex;

// Synchronization objects
ComPtr<ID3D12Fence> g_fence;
uint64_t g_fenceValue;
uint64_t g_frameFenceValues[g_numFrames] = {};
HANDLE g_fenceEvent;

// VSync will wait for the swap chain to refresh before presenting next frame
bool g_vSync = true;
bool g_tearingSupport = false;
bool g_fullscreen = false;

// Window callback
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

void ParseCommandLineArguments()
{
	int argc;
	wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);

	for (size_t i = 0; i < argc; ++i)
	{
		if (::wcscmp(argv[i], L"-w") == 0 || ::wcscmp(argv[i], L"--width") == 0)
		{
			g_clientWidth = ::wcstol(argv[++i], nullptr, 10);
		}
		if (::wcscmp(argv[i], L"-h") == 0 || ::wcscmp(argv[i], L"--height") == 0)
		{
			g_clientHeight = ::wcstol(argv[++i], nullptr, 10);
		}
		if (::wcscmp(argv[i], L"-warp") == 0 || ::wcscmp(argv[i], L"--warp") == 0)
		{
			g_useWarp = true;
		}
	}

	// Free memory allocated by CommandLineToArgvW
	::LocalFree(argv);
}

void EnableDebugLayer()
{
#if defined(_DEBUG)
	// Always enable the debug layer before doing anything DX12 related
	// so all possible errors generated while creating DX12 objects
	// are caught by the debug layer.
	// Enabling after creating the device will remove the device at runtime.
	ComPtr<ID3D12Debug> debugInterface;
	ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debugInterface)));
	debugInterface->EnableDebugLayer();
#endif
}

void RegisterWindowClass(HINSTANCE hInst, const wchar_t* windowClassName)
{
	// Register a window class for creating our render window with.
	WNDCLASSEXW windowClass = {};

	windowClass.cbSize = sizeof(WNDCLASSEX);
	windowClass.style = CS_HREDRAW | CS_VREDRAW;
	//windowClass.lpfnWndProc = &WndProc;		// this will probably go away when it gets written
	windowClass.cbClsExtra = 0;
	windowClass.cbWndExtra = 0;
	windowClass.hInstance = hInst;
	windowClass.hIcon = ::LoadIcon(hInst, NULL);
	windowClass.hCursor = ::LoadCursor(NULL, IDC_ARROW);
	windowClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	windowClass.lpszMenuName = NULL;
	windowClass.lpszClassName = windowClassName;
	windowClass.hIconSm = ::LoadIcon(hInst, NULL);

	static ATOM atom = ::RegisterClassExW(&windowClass);
	assert(atom > 0);
}

HWND CreateWindow(const wchar_t* windowClassName, HINSTANCE hInst, const wchar_t* windowTitle, uint32_t width, uint32_t height)
{
	// Dimensions of primary display monitor in pixels
	int screenWidth = ::GetSystemMetrics(SM_CXSCREEN);
	int screenHeight = ::GetSystemMetrics(SM_CYSCREEN);

	// Calculate required size of window rectangle
	// Allow for minimizing and maximizing
	RECT windowRect = { 0, 0, static_cast<LONG>(screenWidth), static_cast<LONG>(screenHeight) };
	::AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

	// Compute width and height of window
	// Top left corner is (0, 0)
	int windowWidth = windowRect.right - windowRect.left;
	int windowHeight = windowRect.bottom - windowRect.top;

	// Center the window within the screen
	int windowX = std::max<int>(0, (screenWidth - windowWidth) / 2);
	int windowY = std::max<int>(0, (screenHeight - windowHeight) / 2);

	HWND hWnd = ::CreateWindowExW(
		NULL,					// extended window style
		windowClassName,		// class name
		windowTitle,			// window name
		WS_OVERLAPPEDWINDOW,	// window style
		windowX,				// horizontal position
		windowY,				// vertical position
		windowWidth,			// window width
		windowHeight,			// window height
		NULL,					// handle to parent
		NULL,					// handle to menu
		hInst,					// handle to instance
		nullptr					// pointer to lParam of WM_CREATE message
	);

	assert(hWnd && "Failed to create window");

	return hWnd;
}

// QUERYING FOR A COMPATIBLE ADAPTER FOR DX12 ---------------------------
// Finds a compatible adapter to run DX12.
ComPtr<IDXGIAdapter4> GetAdapter(bool useWarp)
{
	// Create a DXGI Factory
	ComPtr<IDXGIFactory4> dxgiFactory;
	UINT createFactoryFlags = 0;
#if defined(_DEBUG)
	createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif

	ThrowIfFailed(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&dxgiFactory)));

	ComPtr<IDXGIAdapter1> dxgiAdapter1;
	ComPtr<IDXGIAdapter4> dxgiAdapter4;

	if (useWarp)
	{
		// Create warp adapter
		// Create a pointer to IDXGIAdapter1
		// Return pointer to IDXGIAdapter4
		ThrowIfFailed(dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&dxgiAdapter1)));
		// Need to cast adapter1 to adapter4 using ComPtr::As to make sure 
		// COM objects are casted correctly. 
		// NEVER use static_cast for COM objects!
		ThrowIfFailed(dxgiAdapter1.As(&dxgiAdapter4));
	}
	// Query for hardware adapters
	else
	{
		SIZE_T maxDedicatedVideoMemory = 0;
		for (UINT i = 0; dxgiFactory->EnumAdapters1(i, &dxgiAdapter1) != DXGI_ERROR_NOT_FOUND; ++i)
		{
			DXGI_ADAPTER_DESC1 dxgiAdapterDesc1;
			dxgiAdapter1->GetDesc1(&dxgiAdapterDesc1);

			// Find the adapter with the largest dedicated video memory
			// to use to create a DX12 device. (Don't actually create it here)
			// Ignore adapters that have DXGI_ADAPTER_FLAG_SOFTWARE flagged
			// Then check to see if the adapter can be used for DX12 by 
			// creating a null device that returns S_OK.
			if ((dxgiAdapterDesc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
				SUCCEEDED(D3D12CreateDevice(dxgiAdapter1.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)) &&
				dxgiAdapterDesc1.DedicatedVideoMemory > maxDedicatedVideoMemory)
			{
				maxDedicatedVideoMemory = dxgiAdapterDesc1.DedicatedVideoMemory;
				ThrowIfFailed(dxgiAdapter1.As(&dxgiAdapter4));
			}
		}
	}

	return dxgiAdapter4;
}

// CREATING THE DX12 DEVICE ------------------------------------------
// The device is used to create resources.
// Don't use the device to make commands.
// Think of this as a tracker for memory allocations on the GPU.
// Everything should be destroyed before destorying the device.
// The debug layer should tell you when this is occuring.
ComPtr<ID3D12Device2> CreateDevice(ComPtr<IDXGIAdapter4> adapter)
{
	ComPtr<ID3D12Device2> d3d12Device2;
	// Create a device and store to d3d12Device2.
	ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12Device2)));

	// This will act as a breakpoint to debug the problem with the program.
	// SetBreakOnSeverity() will basically stop on the indicated severity.
#if defined(_DEBUG)
	ComPtr<ID3D12InfoQueue> pInfoQueue;
	if (SUCCEEDED(d3d12Device2.As(&pInfoQueue)))
	{
		pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
		pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
		pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);

		// Ignore these messages based on different categories
		// Need to comment out because it is expecting items inside.
		//D3D12_MESSAGE_CATEGORY categories[] = {};

		// Ignore these messages based on different severities
		D3D12_MESSAGE_SEVERITY severities[] =
		{
			D3D12_MESSAGE_SEVERITY_INFO
		};

		// Ignore these messages based on IDs
		D3D12_MESSAGE_ID ids[] =
		{
			// Occurs when clear color is not the optimized version during creation
			// Use this when you want to use arbitrary clear color
			D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,	// ???
			// VS graphics debugger is broken (based on the tutorial)
			D3D12_MESSAGE_ID_MAP_INVALID_NULLRANGE,
			D3D12_MESSAGE_ID_UNMAP_INVALID_NULLRANGE
		};

		D3D12_INFO_QUEUE_FILTER filter = {};
		//filter.DenyList.NumCategories = _countof(categories);
		//filter.DenyList.pCategoryList = categories;
		filter.DenyList.NumSeverities = _countof(severities);
		filter.DenyList.pSeverityList = severities;
		filter.DenyList.NumIDs = _countof(ids);
		filter.DenyList.pIDList = ids;

		ThrowIfFailed(pInfoQueue->PushStorageFilter(&filter));
	}
#endif

	return d3d12Device2;
}

// CREATING THE COMMAND QUEUE ----------------------------
// D3D12_COMMAND_QUEUE_DESC
//		Type
//				- Direct: Used to execute draw, compute, copy commands
//				- Compute: Used to execute compute and copy commands
//				- Copy: Used to execute copy commands
//		Priority
//				- Normal
//				- High
//				- Global Realtime
//		Flags: additional flags
//		NodeMask: set to 0 for single GPU operation and use set bits to id the physical adapter
ComPtr<ID3D12CommandQueue> CreateCommandQueue(ComPtr<ID3D12Device2> device, D3D12_COMMAND_LIST_TYPE type)
{
	ComPtr<ID3D12CommandQueue> d3d12CommandQueue;
	
	D3D12_COMMAND_QUEUE_DESC desc = {};
	desc.Type = type;
	desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	desc.NodeMask = 0;

	ThrowIfFailed(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&d3d12CommandQueue)));

	return d3d12CommandQueue;
}

// CHECKING FOR TEARING SUPPORT ------------------------------------
// Screen tearing is when moving image is out of sync with veritcal refresh.
// This can happen when you don't support multiple reefresh rates.
bool CheckTearingSupport()
{
	BOOL allowTearing = FALSE;

	// TODO
	// The tutorial doesn't handle DXGI 1.5 factory for debugging
	// It creates a DXGI 1.4 factory that queries a 1.5 interface.
	ComPtr<IDXGIFactory4> factory4;
	if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory4))))
	{
		ComPtr<IDXGIFactory5> factory5;
		if (SUCCEEDED(factory4.As(&factory5)))
		{
			if (FAILED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
				&allowTearing, sizeof(allowTearing))))
			{
				// TODO
				// Why do you need to do this if CheckFeatureSupport() 
				// fills the data in during the check?
				allowTearing = FALSE;
			}
		}
	}

	return allowTearing == TRUE;
}

// CREATING THE SWAP CHAIN ------------------------------------
// Flip effects
//		Sequential: backbuffer contents stay after presentation
//				- Rendering can lag because some buffers may still be 
//				  utilized and the swap chain has nothing to write to.
//		Discard: discard the contents of the backbuffer after presentation
//				- This will help optimize fps with vsync OFF.
ComPtr<IDXGISwapChain4> CreateSwapChain(HWND hWnd, ComPtr<ID3D12CommandQueue> commandQueue,
	uint32_t width, uint32_t height, uint32_t bufferCount)
{
	return nullptr;
}

int main()
{
	
}