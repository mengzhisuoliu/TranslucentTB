#include "explorerdetour.hpp"
#include <cstdint>
#include <libloaderapi.h>
#include <wil/win32_helpers.h>
#include <WinUser.h>

#include "constants.hpp"
#include "detourexception.hpp"
#include "detourtransaction.hpp"

PFN_SET_WINDOW_COMPOSITION_ATTRIBUTE ExplorerDetour::s_SetWindowCompositionAttribute;
std::wstring ExplorerDetour::s_WorkerWindow;
UINT ExplorerDetour::s_RequestAttribute;
bool ExplorerDetour::s_DetourInstalled;

BOOL WINAPI ExplorerDetour::SetWindowCompositionAttributeDetour(HWND hWnd, const WINDOWCOMPOSITIONATTRIBDATA *data) noexcept
{
	if (data->Attrib == WCA_ACCENT_POLICY &&
		Window::Find(s_WorkerWindow, s_WorkerWindow).send_message(s_RequestAttribute, 0, reinterpret_cast<LPARAM>(hWnd)))
	{
		return true;
	}
	else
	{
		return s_SetWindowCompositionAttribute(hWnd, data);
	}
}

LRESULT CALLBACK ExplorerDetour::CallWndProc(int nCode, WPARAM wParam, LPARAM lParam) noexcept
{
	// Dummy
	return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

bool ExplorerDetour::IsInExplorer() noexcept
{
	return DetourFindPayloadEx(EXPLORERDETOUR_PAYLOAD, nullptr);
}

bool ExplorerDetour::Install() noexcept
{
	if (!s_SetWindowCompositionAttribute)
	{
		s_SetWindowCompositionAttribute
			= reinterpret_cast<PFN_SET_WINDOW_COMPOSITION_ATTRIBUTE>(GetProcAddress(GetModuleHandle(SWCA_DLL), SWCA_ORDINAL));

		if (!s_SetWindowCompositionAttribute)
		{
			return false;
		}
	}

	if (s_WorkerWindow.empty())
	{
		try
		{
			s_WorkerWindow = WORKER_WINDOW;
		}
		catch (...)
		{
			return false;
		}
	}

	if (!s_RequestAttribute)
	{
		s_RequestAttribute = RegisterWindowMessage(WM_TTBHOOKREQUESTREFRESH);
		if (!s_RequestAttribute)
		{
			return false;
		}
	}

	if (!s_DetourInstalled)
	{
		try
		{
			DetourTransaction transaction;

			transaction.update_all_threads();
			transaction.attach(s_SetWindowCompositionAttribute, SetWindowCompositionAttributeDetour);
			transaction.commit();
		}
		catch (...)
		{
			return false;
		}

		s_DetourInstalled = true;
	}

	return true;
}

void ExplorerDetour::Uninstall()
{
	if (s_DetourInstalled)
	{
		DetourTransaction transaction;

		transaction.update_all_threads();
		transaction.detach(s_SetWindowCompositionAttribute, SetWindowCompositionAttributeDetour);
		transaction.commit();

		s_DetourInstalled = false;
	}
}

wil::unique_hhook ExplorerDetour::Inject(Window window) noexcept
{
	const DWORD pid = window.process_id();
	wil::unique_process_handle proc(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, false, pid));
	if (!proc)
	{
		return nullptr;
	}

	if (!DetourFindRemotePayload(proc.get(), EXPLORERDETOUR_PAYLOAD, nullptr))
	{
		proc.reset(OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_WRITE, false, pid));
		if (!proc)
		{
			return nullptr;
		}

		const uint32_t content = 0xDEADBEEF;
		if (!DetourCopyPayloadToProcess(proc.get(), EXPLORERDETOUR_PAYLOAD, &content, sizeof(content)))
		{
			return nullptr;
		}
	}

	proc.reset();

	return wil::unique_hhook(SetWindowsHookEx(WH_CALLWNDPROC, CallWndProc, wil::GetModuleInstanceHandle(), window.thread_id()));
}
