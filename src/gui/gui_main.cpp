// hollywood_gui -- Dear ImGui + Direct3D 11 front-end for the Quest 2 emulator.
//
// Runs the boot pipeline on a background thread and shows it live: a "Guest
// Display" panel (renders the guest boot console now; the real guest framebuffer
// once a virtual display device publishes one), a full scrolling boot log, and a
// status panel (instructions executed, PC, stage). This is the control GUI + the
// render surface for the guest display.
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <windows.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "common/log.h"
#include "core/emulator.h"
#include "core/boot_pipeline.h"
#include "cpu/unicorn_cpu.h"
#include "gui/gui_bridge.h"

// ---- Direct3D 11 ----
static ID3D11Device*           g_dev = nullptr;
static ID3D11DeviceContext*    g_ctx = nullptr;
static IDXGISwapChain*         g_sc  = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static ID3D11Texture2D*        g_fbTex = nullptr;
static ID3D11ShaderResourceView* g_fbSrv = nullptr;
static int g_fbTexW = 0, g_fbTexH = 0;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    g_sc->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) { g_dev->CreateRenderTargetView(back, nullptr, &g_rtv); back->Release(); }
}
static void CleanupRenderTarget() { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } }

static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60; sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    UINT flags = 0;
    D3D_FEATURE_LEVEL fl; const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2,
            D3D11_SDK_VERSION, &sd, &g_sc, &g_dev, &fl, &g_ctx) != S_OK) {
        // Fall back to WARP (software) so the GUI still opens without a GPU.
        if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 2,
                D3D11_SDK_VERSION, &sd, &g_sc, &g_dev, &fl, &g_ctx) != S_OK)
            return false;
    }
    CreateRenderTarget();
    return true;
}
static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_fbSrv) { g_fbSrv->Release(); g_fbSrv = nullptr; }
    if (g_fbTex) { g_fbTex->Release(); g_fbTex = nullptr; }
    if (g_sc) { g_sc->Release(); g_sc = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (g_dev && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_sc->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ---- log sink: emulator thread -> bridge ----
static void gui_log_sink(hw::LogLevel l, std::string_view mod, std::string_view msg) {
    const char* t = (l >= hw::LogLevel::Warn) ? "! " : "  ";
    hw::gui::bridge().push_log(std::string(t) + std::string(mod) + " | " + std::string(msg));
}

// ---- emulator boot thread ----
static void emu_thread() {
    using namespace hw;
    Log::set_sink(gui_log_sink);
    Log::set_level(LogLevel::Info);
    gui::bridge().push_log("== hollywood_gui: starting Quest 2 boot (kmshim, non-faithful secure world) ==");

    core::EmuConfig cfg;
    cfg.ota_zip = R"(C:\Users\drych\Downloads\q2_52242990021400150.zip)";
    cfg.ram_mb = 2048;
    cfg.max_instructions = 40000000000ull;
    cfg.stop_on_mmio = false;    // permissive
    cfg.profile = "minimal";
    // Minimal profile: disable display/camera/GPU nodes (mirrors cmd_boot --profile minimal).
    cfg.dtb_disable = {
        "/soc/qcom,sde_rscc@af20000", "/soc/qcom,mdss_mdp@ae00000",
        "/soc/qcom,mdss_dsi_ctrl0@ae94000", "/soc/qcom,mdss_dsi_ctrl1@ae96000",
        "/soc/qcom,mdss_dsi_phy0@ae94400", "/soc/qcom,mdss_dsi_phy1@ae96400",
        "/soc/qcom,mdss_dsi_pll@ae94900", "/soc/qcom,mdss_dsi_pll@ae96900",
        "/soc/qcom,cam-cpas@ac40000", "/soc/qcom,cam-cdm-intf", "/soc/qcom,cpas-cdm0@ac4d000",
        "/soc/qcom,cam_smmu", "/soc/qcom,cam-isp", "/soc/qcom,cam-icp", "/soc/qcom,cam-a5@ac00000",
        "/soc/qcom,cam-jpeg", "/soc/qcom,jpegenc@ac53000", "/soc/qcom,jpegdma@ac57000",
        "/soc/qcom,cam-bps", "/soc/qcom,ipe0", "/soc/qcom,cam-req-mgr",
        "/soc/qcom,cci@ac4f000", "/soc/qcom,cci@ac50000",
    };
    core::Emulator emu(std::move(cfg));

    cpu::UnicornOptions uo;
    uo.stop_on_unmapped = false;
    uo.kmshim = true;            // non-faithful keymaster/QSEE SCM shim
    uo.timeout_us = 0;           // run until done / process exit
    uo.host_backed_ram = true;
    emu.backend = std::make_unique<cpu::UnicornCpu>(uo);

    gui::bridge().emu_running = true;
    try {
        core::BootPipeline pipeline(emu);
        pipeline.run();
    } catch (const std::exception& e) {
        gui::bridge().push_log(std::string("!! emulator exception: ") + e.what());
    }
    gui::bridge().emu_running = false;
    gui::bridge().emu_done = true;
    gui::bridge().push_log("== boot pipeline returned ==");
}

// ---- upload the guest framebuffer (if any) to a DX11 texture ----
static void UpdateFramebufferTexture() {
    auto& b = hw::gui::bridge();
    std::lock_guard<std::mutex> g(b.fb_mu);
    if (b.fb_w <= 0 || b.fb_h <= 0 || b.fb.empty()) return;
    if (!g_fbTex || g_fbTexW != b.fb_w || g_fbTexH != b.fb_h) {
        if (g_fbSrv) { g_fbSrv->Release(); g_fbSrv = nullptr; }
        if (g_fbTex) { g_fbTex->Release(); g_fbTex = nullptr; }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = b.fb_w; td.Height = b.fb_h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (g_dev->CreateTexture2D(&td, nullptr, &g_fbTex) != S_OK) return;
        g_dev->CreateShaderResourceView(g_fbTex, nullptr, &g_fbSrv);
        g_fbTexW = b.fb_w; g_fbTexH = b.fb_h;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (g_ctx->Map(g_fbTex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m) == S_OK) {
        for (int y = 0; y < b.fb_h; y++)
            memcpy((uint8_t*)m.pData + y * m.RowPitch, &b.fb[(size_t)y * b.fb_w], (size_t)b.fb_w * 4);
        g_ctx->Unmap(g_fbTex, 0);
    }
}

int main(int, char**) {
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, GetModuleHandle(nullptr),
                       nullptr, nullptr, nullptr, nullptr, L"hollywood_gui", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"hollywood_emu -- Quest 2 (Snapdragon XR2)",
                              WS_OVERLAPPEDWINDOW, 80, 60, 1500, 940,
                              nullptr, nullptr, wc.hInstance, nullptr);
    if (!CreateDeviceD3D(hwnd)) { CleanupDeviceD3D(); UnregisterClassW(wc.lpszClassName, wc.hInstance); return 1; }
    ShowWindow(hwnd, SW_SHOWDEFAULT); UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    std::thread emu(emu_thread);
    emu.detach();

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        auto& b = hw::gui::bridge();

        // --- Status ---
        ImGui::SetNextWindowPos(ImVec2(956, 8), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(524, 182), ImGuiCond_FirstUseEver);
        ImGui::Begin("Status");
        ImGui::Text("Emulator: %s", b.emu_done ? "FINISHED" : (b.emu_running ? "RUNNING" : "starting..."));
        ImGui::Text("Instructions: %.3f G", (double)hw::cpu::g_live_insns.load() / 1e9);
        ImGui::Text("PC: 0x%016llx", (unsigned long long)hw::cpu::g_live_pc.load());
        { std::lock_guard<std::mutex> g(b.log_mu); ImGui::TextWrapped("%s", b.status_line.c_str()); }
        ImGui::Text("Log lines: %llu", (unsigned long long)b.total_lines);
        ImGui::Text("FPS: %.0f", io.Framerate);
        ImGui::End();

        // --- Guest Display (framebuffer, or boot console fallback) ---
        ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(940, 900), ImGuiCond_FirstUseEver);
        ImGui::Begin("Guest Display");
        UpdateFramebufferTexture();
        if (g_fbSrv && g_fbTexW > 0) {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            float sc = (avail.x / g_fbTexW < avail.y / g_fbTexH) ? avail.x / g_fbTexW : avail.y / g_fbTexH;
            ImGui::Image((ImTextureID)(intptr_t)g_fbSrv, ImVec2(g_fbTexW * sc, g_fbTexH * sc));
        } else {
            ImGui::TextDisabled("(no guest framebuffer yet -- display device not up; showing boot console)");
            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 255));
            ImGui::BeginChild("console", ImVec2(0, 0), true);
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(120, 255, 120, 255));
            std::lock_guard<std::mutex> g(b.log_mu);
            size_t start = b.log.size() > 40 ? b.log.size() - 40 : 0;
            for (size_t i = start; i < b.log.size(); i++) ImGui::TextUnformatted(b.log[i].c_str());
            ImGui::PopStyleColor();
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
        ImGui::End();

        // --- Boot Log (full scroll) ---
        ImGui::SetNextWindowPos(ImVec2(956, 198), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(524, 710), ImGuiCond_FirstUseEver);
        ImGui::Begin("Boot Log");
        ImGui::BeginChild("logscroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
        {
            std::lock_guard<std::mutex> g(b.log_mu);
            for (const auto& s : b.log) ImGui::TextUnformatted(s.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        ImGui::End();

        ImGui::Render();
        const float clear[4] = { 0.06f, 0.07f, 0.09f, 1.0f };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_sc->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    // The detached emulator thread is torn down with the process.
    return 0;
}
