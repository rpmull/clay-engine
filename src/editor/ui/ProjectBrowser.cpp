#include "ProjectBrowser.h"
#include <windows.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <imgui.h>
#include "backends/imgui_impl_win32.h"
#include "ui/imgui_backend/imgui_impl_bgfx_docking.h"
#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include "core/rendering/RendererBackend.h"
#include "platform/win32/Win32Window.h"
#include "editor/Project.h"
#include "editor/ProjectGenerator.h"
#include <imgui_claymore_style.h>

using json = nlohmann::json;

static std::filesystem::path GetAppDataRegistryPath()
{
	PWSTR appdata = nullptr;
	std::filesystem::path out;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
		out = std::filesystem::path(appdata) / L"Claymore" / L"projects.json";
		CoTaskMemFree(appdata);
	}
	return out;
}

static std::string WideToUtf8(const std::wstring& wide)
{
	if (wide.empty()) return {};
	int requiredBytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
	if (requiredBytes <= 0) return {};
	std::string utf8(static_cast<size_t>(requiredBytes - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, utf8.data(), requiredBytes, nullptr, nullptr);
	return utf8;
}

static std::string ShowOpenFolderDialog_Local() {
	IFileDialog* pFileDialog = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pFileDialog));
	if (FAILED(hr) || !pFileDialog) return "";

	DWORD options = 0;
	pFileDialog->GetOptions(&options);
	pFileDialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);

	std::string result;
	hr = pFileDialog->Show(nullptr);
	if (SUCCEEDED(hr)) {
		IShellItem* pItem = nullptr;
		hr = pFileDialog->GetResult(&pItem);
		if (SUCCEEDED(hr) && pItem) {
			PWSTR path = nullptr;
			pItem->GetDisplayName(SIGDN_FILESYSPATH, &path);
			if (path) {
				result = WideToUtf8(path);
				CoTaskMemFree(path);
			}
			pItem->Release();
		}
	}
	pFileDialog->Release();
	return result;
}

ProjectBrowserWindow::ProjectBrowserWindow()
{
	m_RegistryPath = GetAppDataRegistryPath();
}

ProjectBrowserWindow::~ProjectBrowserWindow()
{
	DestroyBrowserWindow();
}

bool ProjectBrowserWindow::CreateBrowserWindow()
{
	m_Window = std::make_unique<Win32Window>();
	if (!m_Window->Create(L"Claymore Project Browser", m_Width, m_Height, false, true, /*maximize*/false, /*center*/true))
		return false;
	m_Hwnd = m_Window->GetOSHandle();

	// init bgfx minimal for imgui-only frames
	bgfx::Init init;
	init.platformData.nwh = m_Hwnd;
	init.type = cm::rendering::GetDefaultBgfxRendererType();
	init.resolution.width = (uint32_t)m_Width;
	init.resolution.height = (uint32_t)m_Height;
	init.resolution.reset = BGFX_RESET_VSYNC;
	std::cout << "[ProjectBrowser] Requested bgfx renderer: "
	          << cm::rendering::DescribeBgfxRendererType(init.type)
	          << std::endl;
	if (!bgfx::init(init)) {
		std::cerr << "[ProjectBrowser] bgfx initialization failed." << std::endl;
		return false;
	}
	std::cout << "[ProjectBrowser] Active bgfx renderer: "
	          << bgfx::getRendererName(bgfx::getRendererType())
	          << std::endl;

	IMGUI_CHECKVERSION();
	m_ImGuiContext = ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	ImGui::StyleColorsDark();

	float contentScale = 1.0f;
#ifdef _WIN32
	if (m_Hwnd) {
		UINT dpi = 96;
		HMODULE user32 = ::GetModuleHandleA("User32.dll");
		if (user32) {
			using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
			if (auto fn = reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow"))) {
				dpi = fn((HWND)m_Hwnd);
			} else {
				HDC screen = GetDC(nullptr);
				if (screen) {
					dpi = (UINT)GetDeviceCaps(screen, LOGPIXELSX);
					ReleaseDC(nullptr, screen);
				}
			}
		}
		if (dpi == 0) dpi = 96;
		contentScale = (float)dpi / 96.0f;
	}
#endif

	float baseFontSize = 16.0f * contentScale;
	ImGuiIO& bio = ImGui::GetIO();
	bio.Fonts->Clear();
	ImFontConfig cfg{};
	cfg.OversampleH = 3;
	cfg.OversampleV = 2;
	cfg.PixelSnapH = false;
	ImFont* loadedFont = bio.Fonts->AddFontFromFileTTF("assets/fonts/Roboto-Regular.ttf", baseFontSize, &cfg);
	if (!loadedFont) {
		bio.Fonts->AddFontDefault();
	}
	bio.FontGlobalScale = 1.0f;
	Clay_ApplyEditorStyle(baseFontSize);
	ImGui_ImplWin32_Init((HWND)m_Hwnd);
	ImGui_ImplBgfx_Init(255);
	m_BrowserActive = true;
	return true;
}

void ProjectBrowserWindow::DestroyBrowserWindow()
{
	if (m_BrowserActive) {
		// Ensure the browser's ImGui context is current for backend shutdown
		if (ImGui::GetCurrentContext() != m_ImGuiContext) {
			ImGui::SetCurrentContext(m_ImGuiContext);
		}
		ImGui_ImplBgfx_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext(m_ImGuiContext);
		m_ImGuiContext = nullptr;
		bgfx::shutdown();
		m_BrowserActive = false;
	}
	m_Window.reset();
}

void ProjectBrowserWindow::LoadRegistry()
{
	m_Projects.clear();
	std::error_code ec;
	if (!m_RegistryPath.empty()) {
		std::filesystem::create_directories(m_RegistryPath.parent_path(), ec);
		if (std::filesystem::exists(m_RegistryPath)) {
			try {
				std::ifstream in(m_RegistryPath);
				json j; in >> j;
				if (j.is_array()) {
					for (auto& e : j) {
						RegisteredProject rp;
						rp.name = e.value("name", "");
						rp.root = e.value("root", "");
						if (!rp.root.empty() && std::filesystem::exists(rp.root))
							m_Projects.push_back(std::move(rp));
					}
				}
			}
			catch (...) {}
		}
	}
}

void ProjectBrowserWindow::SaveRegistry()
{
	if (m_RegistryPath.empty()) return;
	std::error_code ec;
	std::filesystem::create_directories(m_RegistryPath.parent_path(), ec);
	json j = json::array();
	for (auto& rp : m_Projects) j.push_back({ {"name", rp.name}, {"root", rp.root.string()} });
	std::ofstream out(m_RegistryPath);
	if (out) out << j.dump(2);
}

void ProjectBrowserWindow::BeginFrame()
{
	ImGui_ImplWin32_NewFrame();
	ImGui_ImplBgfx_NewFrame();
	ImGui::NewFrame();
}

void ProjectBrowserWindow::EndFrame()
{
	ImGui::Render();
	ImGui_ImplBgfx_Render(255, ImGui::GetDrawData(), 0x00000000);
	bgfx::frame();
}

bool ProjectBrowserWindow::PumpOnce()
{
	if (m_Window) m_Window->PumpEvents();
	return m_Window && !m_Window->ShouldClose();
}

void ProjectBrowserWindow::ActionImportFromFilesystem()
{
	std::string folder = ShowOpenFolderDialog_Local();
	if (folder.empty()) return;
	std::filesystem::path root = folder;
	// Find .clayproj
	std::filesystem::path clayproj;
	for (auto& e : std::filesystem::directory_iterator(root)) {
		if (e.path().extension() == ".clayproj") { clayproj = e.path(); break; }
	}
	if (clayproj.empty()) {
		// create minimal
		std::string name = root.filename().string();
		json pj = { {"name", name}, {"version", 1}, {"assetDirectory", "assets"} };
		std::ofstream out(root / (name + ".clayproj")); if (out) out << pj.dump(4);
	}
	RegisteredProject rp; rp.root = root; rp.name = root.filename().string();
	m_Projects.push_back(std::move(rp));
	SaveRegistry();
}

void ProjectBrowserWindow::ActionCreateNewProject()
{
	// Prompt for a target folder that will be the project root; write minimal .clayproj there
	ImGui::OpenPopup("Create New Project");
	if (ImGui::BeginPopupModal("Create New Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		if (ImGui::Button("Choose Folder")) {
			std::string folder = ShowOpenFolderDialog_Local();
			if (!folder.empty()) {
				std::filesystem::path root = folder;
				if (ProjectGenerator::CreateBlankProjectInFolder(root)) {
					RegisteredProject rp; rp.root = root; rp.name = root.filename().string();
					m_Projects.push_back(std::move(rp));
					SaveRegistry();
					ImGui::CloseCurrentPopup();
				}
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}

void ProjectBrowserWindow::ActionRemoveSelected()
{
	if (m_SelectedIndex >= 0 && m_SelectedIndex < (int)m_Projects.size()) {
		m_Projects.erase(m_Projects.begin() + m_SelectedIndex);
		m_SelectedIndex = -1;
		SaveRegistry();
	}
}

void ProjectBrowserWindow::DrawUI()
{
	ImGui::SetNextWindowSize(ImVec2((float)m_Width, (float)m_Height));
	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
	if (ImGui::Begin("Project Browser", nullptr, flags)) {
		ImGui::Text("Select a project to open, or import/create a new one.");
		ImGui::Separator();

		// Left: list
		ImGui::BeginChild("left", ImVec2(0, -80), true);
		for (int i = 0; i < (int)m_Projects.size(); ++i) {
			bool selected = (i == m_SelectedIndex);
			if (ImGui::Selectable((m_Projects[i].name + "##" + std::to_string(i)).c_str(), selected)) {
				m_SelectedIndex = i;
			}
			ImGui::TextDisabled("%s", m_Projects[i].root.string().c_str());
		}
		ImGui::EndChild();

		// Bottom action bar
		ImGui::Separator();
		if (ImGui::Button("Import from Filesystem")) ActionImportFromFilesystem();
		ImGui::SameLine();
		if (ImGui::Button("Create New Project")) ActionCreateNewProject();
		ImGui::SameLine();
		if (ImGui::Button("Remove")) ActionRemoveSelected();
		ImGui::SameLine();
		bool canOpen = (m_SelectedIndex >= 0 && m_SelectedIndex < (int)m_Projects.size());
		if (!canOpen) ImGui::BeginDisabled();
		if (ImGui::Button("Open Project")) {
			m_SelectedProjectRoot = m_Projects[m_SelectedIndex].root;
			m_ShouldClose = true;
		}
		if (!canOpen) ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Quit")) { m_SelectedProjectRoot.clear(); m_ShouldClose = true; }
	}
	ImGui::End();
}

std::filesystem::path ProjectBrowserWindow::OpenModal()
{
	LoadRegistry();
	if (!CreateBrowserWindow()) return {};
	while (!m_ShouldClose && PumpOnce()) {
		BeginFrame();
		DrawUI();
		EndFrame();
	}
	DestroyBrowserWindow();
	return m_SelectedProjectRoot;
}

