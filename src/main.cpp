// Multi-Channel UV Player: plays media onto the UV channels of an FBX mesh.
//
// Usage: MultiChannelUVPlayer [mesh.fbx] [images or folders...]
//        [--led] [--grid N] [--wireframe] [--size WxH] [--screenshot out.png]
// --screenshot renders a few frames, saves the window to a PNG and exits (used for testing).

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// windows.h comes before GLFW so its APIENTRY definition is the one used.
#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#endif

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <stb_image_write.h>

#include "app.h"

namespace {

void onDrop(GLFWwindow* window, int count, const char** paths) {
    App* app = static_cast<App*>(glfwGetWindowUserPointer(window));
    for (int i = 0; i < count; ++i) app->importPath(paths[i]);
}

void setupStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 2.0f;
    style.WindowBorderSize = 0.0f;
    style.FramePadding = ImVec2(8, 4);
    style.ItemSpacing = ImVec2(8, 6);
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.11f, 0.115f, 0.13f, 1.0f);
    colors[ImGuiCol_ChildBg] = colors[ImGuiCol_WindowBg];
    colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.085f, 0.095f, 1.0f);
    colors[ImGuiCol_TitleBgActive] = colors[ImGuiCol_TitleBg];
    colors[ImGuiCol_Tab] = ImVec4(0.14f, 0.15f, 0.17f, 1.0f);
    colors[ImGuiCol_TabSelected] = ImVec4(0.20f, 0.22f, 0.26f, 1.0f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.18f, 0.21f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.20f, 0.22f, 0.27f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.42f, 0.66f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.22f, 0.36f, 0.56f, 0.8f);
}

void loadFont() {
    ImGuiIO& io = ImGui::GetIO();
    const char* candidates[] = {"C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
    for (const char* path : candidates) {
        if (FILE* file = std::fopen(path, "rb")) {
            std::fclose(file);
            io.Fonts->AddFontFromFileTTF(path, 17.0f);
            return;
        }
    }
    io.Fonts->AddFontDefault();
}

// Command-line arguments as UTF-8. On Windows argv uses the ANSI code page, so
// file names with accents are read from the wide command line instead.
std::vector<std::string> utf8Arguments(int argc, char** argv) {
    std::vector<std::string> args;
#ifdef _WIN32
    (void)argc;
    (void)argv;
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int i = 1; i < count; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string text(size > 0 ? size - 1 : 0, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, text.data(), size, nullptr, nullptr);
        args.push_back(text);
    }
    LocalFree(wide);
#else
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
#endif
    return args;
}

bool saveScreenshot(GLFWwindow* window, const std::string& path) {
    int width = 0, height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    std::vector<unsigned char> pixels((size_t)width * height * 4);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    stbi_flip_vertically_on_write(1);
    return stbi_write_png(path.c_str(), width, height, 4, pixels.data(), width * 4) != 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> inputs;
    std::string screenshotPath;
    int gridChannel = 0;  // 1-based on the command line, 0 = show the layers
    bool led = false;
    bool wireframe = false;
    int windowWidth = 1600, windowHeight = 900;
    const std::vector<std::string> args = utf8Arguments(argc, argv);
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        const bool hasValue = i + 1 < args.size();
        if (arg == "--screenshot" && hasValue) screenshotPath = args[++i];
        else if (arg == "--grid" && hasValue) gridChannel = std::atoi(args[++i].c_str());
        else if (arg == "--led") led = true;
        else if (arg == "--wireframe") wireframe = true;
        else if (arg == "--size" && hasValue) std::sscanf(args[++i].c_str(), "%dx%d", &windowWidth, &windowHeight);
        else inputs.push_back(arg);
    }

#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
#endif

    if (!glfwInit()) {
        std::fprintf(stderr, "Could not start GLFW.\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(windowWidth, windowHeight, "Multi-Channel UV Player", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Could not create an OpenGL 3.3 window.\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    if (!gladLoadGL(glfwGetProcAddress)) {
        std::fprintf(stderr, "Could not load OpenGL functions.\n");
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.IniFilename = screenshotPath.empty() ? "imgui_layout.ini" : nullptr;
    setupStyle();
    loadFont();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    GLint maxTextureSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);

    App app;
    if (!app.init(maxTextureSize)) {
        std::fprintf(stderr, "Could not set up the renderer.\n");
        return 1;
    }
    glfwSetWindowUserPointer(window, &app);
    glfwSetDropCallback(window, onDrop);
    if (led) app.setMode(MappingMode::Led);
    for (const std::string& input : inputs) app.importPath(input);
    if (gridChannel > 0) app.showTestGrid(gridChannel - 1);
    if (wireframe) app.setWireframe(true);

    int frameCount = 0;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        app.frame();
        ImGui::Render();

        int width = 0, height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.08f, 0.085f, 0.095f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (!screenshotPath.empty() && ++frameCount == 5) {
            const bool saved = saveScreenshot(window, screenshotPath);
            std::fprintf(stderr, saved ? "Saved %s\n" : "Could not save %s\n", screenshotPath.c_str());
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        glfwSwapBuffers(window);
    }

    app.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
