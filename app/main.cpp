// SPDX-License-Identifier: GPL-2.0-only
//
// Window, OpenGL and Dear ImGui setup. Everything else lives in App.
#include "App.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#elif defined(_WIN32)
#include <windows.h>
#include <GL/gl.h>
#else
#include <GL/gl.h>
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "font_roboto.h"   // generated at build time

#include <cstdio>
#include <vector>

static void glfwError(int code, const char* msg) {
    std::fprintf(stderr, "GLFW error %d: %s\n", code, msg);
}

static bool savePng(const std::string& path, int w, int h) {
    std::vector<unsigned char> px(size_t(w) * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (auto i = px.begin() + 3; i < px.end(); i += 4) *i = 255;   // opaque
    stbi_flip_vertically_on_write(1);
    return stbi_write_png(path.c_str(), w, h, 4, px.data(), w * 4) != 0;
}

static void applyStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0;
    s.FrameRounding = 4;
    s.GrabRounding = 4;
    s.ChildRounding = 4;
    s.PopupRounding = 4;
    s.TabRounding = 4;
    s.FramePadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(8, 6);
    s.WindowPadding = ImVec2(10, 10);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.105f, 0.11f, 0.12f, 1);
    c[ImGuiCol_ChildBg] = ImVec4(0.125f, 0.13f, 0.145f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.19f, 0.21f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.25f, 0.28f, 1);
    c[ImGuiCol_Button] = ImVec4(0.22f, 0.24f, 0.28f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.33f, 0.38f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.36f, 0.40f, 0.46f, 1);
    c[ImGuiCol_Tab] = ImVec4(0.16f, 0.17f, 0.19f, 1);
    c[ImGuiCol_TabHovered] = ImVec4(0.30f, 0.33f, 0.38f, 1);
    c[ImGuiCol_TabSelected] = ImVec4(0.24f, 0.27f, 0.32f, 1);
    c[ImGuiCol_Header] = ImVec4(0.22f, 0.24f, 0.28f, 1);
    c[ImGuiCol_CheckMark] = ImVec4(0.98f, 0.76f, 0.20f, 1);
    c[ImGuiCol_SliderGrab] = ImVec4(0.98f, 0.76f, 0.20f, 1);
    c[ImGuiCol_PlotHistogram] = ImVec4(0.98f, 0.76f, 0.20f, 1);
    ImPlot::StyleColorsDark();
}

int main(int, char**) {
    glfwSetErrorCallback(glfwError);
    if (!glfwInit()) return 1;

#if defined(__APPLE__)
    const char* glsl = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char* glsl = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

    GLFWwindow* win = glfwCreateWindow(1280, 800, "HP 8903 Audio Analyzer", nullptr, nullptr);
    if (!win) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    glfwSetWindowSizeLimits(win, 960, 600, GLFW_DONT_CARE, GLFW_DONT_CARE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;   // layout is fixed; settings live in App

    applyStyle();
    float xs = 1, ys = 1;
#if !defined(__APPLE__)
    // Windows / Linux: scale for high-DPI monitors (macOS does it natively).
    glfwGetWindowContentScale(win, &xs, &ys);
    ImGui::GetStyle().ScaleAllSizes(xs);
    ImGui::GetStyle().FontScaleDpi = xs;
#endif
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF((void*)kRobotoTtf, int(sizeof kRobotoTtf), 15.0f, &fc);

    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init(glsl);

    {
        App app;
        while (!glfwWindowShouldClose(win)) {
            glfwPollEvents();
            if (glfwGetWindowAttrib(win, GLFW_ICONIFIED)) {
                ImGui_ImplGlfw_Sleep(50);
                continue;
            }
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            app.frame();
            ImGui::Render();

            int w, h;
            glfwGetFramebufferSize(win, &w, &h);
            glViewport(0, 0, w, h);
            glClearColor(0.105f, 0.11f, 0.12f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            if (app.wantScreenshot()) {
                std::string p = app.screenshotPath();
                if (savePng(p, w, h)) app.screenshotSaved(p);
            }
            if (app.selfTestDone()) glfwSetWindowShouldClose(win, 1);
            glfwSwapBuffers(win);
        }
    }   // App destructor stops jobs and releases the instrument

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
