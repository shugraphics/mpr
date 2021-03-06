/*
Reference implementation for
"Massively Parallel Rendering of Complex Closed-Form Implicit Surfaces"
(SIGGRAPH 2020)

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this file,
You can obtain one at http://mozilla.org/MPL/2.0/.

Copyright (C) 2019-2020  Matt Keeter
*/

#include <chrono>
#include <fstream>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "TextEditor.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include "context.hpp"
#include "effects.hpp"
#include "tape.hpp"

#include "interpreter.hpp"
#include "tex.hpp"

#define TEXTURE_SIZE 2048

static void glfw_error_callback(int error, const char* description)
{
    fprintf(stderr, "glfw Error %d: %s\n", error, description);
}

struct Shape {
    mpr::Tape tape;
    libfive::Tree tree;
};

////////////////////////////////////////////////////////////////////////////////

int main(int argc, char** argv)
{
    // Setup window
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return 1;

    // Decide GL+GLSL versions
    // GL 3.2 + GLSL 150
    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // Required on Mac

    // Create window with graphics context
    GLFWwindow* window = glfwCreateWindow(1280, 720, "demo", NULL, NULL);
    if (window == NULL) {
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    // Initialize OpenGL loader
    bool err = glewInit() != GLEW_OK;
    if (err) {
        fprintf(stderr, "Failed to initialize OpenGL loader!\n");
        return 1;
    }

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer bindings
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    io.Fonts->AddFontFromFileTTF("../gui/Inconsolata.ttf", 16.0f);

    // Create our text editor
    TextEditor editor;
    bool from_file = false;
    if (argc > 1) {
        std::ifstream input(argv[1]);
        if (input.is_open()) {
            std::vector<std::string> lines;
            std::string line;
            while (std::getline(input, line)) {
                lines.emplace_back(std::move(line));
            }
            input.close();
            editor.SetTextLines(lines);
            from_file = true;
        } else {
            std::cerr << "Could not open file '" << argv[1] << "'\n";
        }
    }

    // Create the interpreter
    Interpreter interpreter;
    bool needs_eval = true;

    // Our state
    bool show_demo_window = false;
    ImVec4 clear_color = ImVec4(0.0f, 0.0f, 0.0f, 1.00f);

    // View matrix, as it were
    Eigen::Vector3f view_center{0.f, 0.0f, 0.0f};
    float view_scale = 2.0f;
    float view_pitch = 0.0f;
    float view_yaw = 0.0f;

    // Function to pack view_center and view_scale into the matrix
    Eigen::Affine3f model;
    Eigen::Affine3f view;
    auto update_mats = [&]() {
        model = Eigen::Affine3f::Identity();
        model.translate(view_center);
        model.scale(view_scale);
        model.rotate(Eigen::AngleAxisf(view_yaw, Eigen::Vector3f::UnitZ()));
        model.rotate(Eigen::AngleAxisf(view_pitch, Eigen::Vector3f::UnitX()));

        view = Eigen::Affine3f::Identity();
        float s = 2.0f / fmax(io.DisplaySize.x, io.DisplaySize.y);
        view.scale(Eigen::Vector3f{s, -s, 1.0f});
        view.translate(Eigen::Vector3f{-io.DisplaySize.x / 2.0f, -io.DisplaySize.y / 2.0f, 0.0f});
    };
    update_mats();

    std::map<libfive::Tree::Id, Shape> shapes;

    // Generate a texture which we'll draw into
    GLuint gl_tex;
    glGenTextures(1, &gl_tex);
    glBindTexture(GL_TEXTURE_2D, gl_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 TEXTURE_SIZE,
                 TEXTURE_SIZE, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    auto cuda_tex = register_texture(gl_tex);

    bool just_saved = false;

    // Main loop
    int render_size = 256;
    int render_dimension = 3;
    int render_mode = RENDER_MODE_NORMALS;

    mpr::Context ctx(render_size);
    mpr::Effects effects;

    while (!glfwWindowShouldClose(window))
    {
        // Poll and handle events (inputs, window resize, etc.)
        // You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
        // - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application.
        // - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application.
        // Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
        glfwWaitEventsTimeout(0.1f);

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Rebuild the transform matrix, in case the window size has changed
        update_mats();

        // Handle panning
        if (!io.WantCaptureMouse) {
            // Start position in world coordinates
            const Eigen::Vector3f mouse = Eigen::Vector3f{
                io.MousePos.x, io.MousePos.y, 0.0f};

            if (ImGui::IsMouseDragging(0)) {
                const auto d = ImGui::GetMouseDragDelta(0);
                const Eigen::Vector3f drag(d.x, d.y, 0.0f);
                view_center += (model * view * (mouse - drag)) -
                               (model * view * mouse);
                update_mats();
                ImGui::ResetMouseDragDelta(0);
            }

            if (ImGui::IsMouseDragging(1)) {
                const auto d = ImGui::GetMouseDragDelta(1);
                view_yaw   -= d.x / 100.0f;
                view_pitch -= d.y / 100.0f;
                view_pitch = fmax(-M_PI / 2, view_pitch);
                view_pitch = fmin( M_PI / 2, view_pitch);
                view_yaw = fmod(view_yaw, 2 * M_PI);
                update_mats();
                ImGui::ResetMouseDragDelta(1);
            }

            // Handle scrolling
            const auto scroll = io.MouseWheel;
            if (scroll) {
                // Reset accumulated scroll
                io.MouseWheel = 0.0f;

                // Start position in world coordinates
                const Eigen::Vector3f start = (model * view * mouse);

                // Update matrix
                view_scale *= powf(1.01f, scroll);
                update_mats();

                const Eigen::Vector3f end = (model * view * mouse);

                // Shift so that world position is constant
                view_center -= (end - start);
                update_mats();
            }
        }

        if (!io.WantCaptureKeyboard) {
            if (io.KeySuper && io.KeysDown[GLFW_KEY_S]) {
                if (!just_saved && from_file) {
                    std::ofstream output(argv[1]);
                    if (output.is_open()) {
                        for (auto& line: editor.GetTextLines()) {
                            output << line << "\n";
                        }
                        output.close();
                    } else {
                        std::cerr << "Failed to save to '" << argv[1] << "'\n";
                    }
                    just_saved = true;
                }
            } else {
                just_saved = false;
            }
        }

        // Draw main menu
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("View")) {
                ImGui::Checkbox("Show demo window", &show_demo_window);
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        if (show_demo_window) {
            ImGui::ShowDemoWindow(&show_demo_window);
        }

        // Draw the interpreter window and handle re-evaluation as needed
        ImGui::Begin("Text editor");
            if (needs_eval) {
                interpreter.eval(editor.GetText());

                // Erase shapes that are no longer in the script
                auto itr = shapes.begin();
                while (itr != shapes.end()) {
                    if (interpreter.shapes.find(itr->first) == interpreter.shapes.end()) {
                        itr = shapes.erase(itr);
                    } else {
                        ++itr;
                    }
                }
                // Create new shapes from the script
                for (auto& t : interpreter.shapes) {
                    if (shapes.find(t.first) == shapes.end()) {
                        Shape s = { mpr::Tape(t.second), t.second };
                        shapes.emplace(t.first, std::move(s));
                    }
                }
            }

            float size = ImGui::GetContentRegionAvail().y;
            if (interpreter.result_valid) {
                size -= ImGui::GetFrameHeight() *
                        (std::count(interpreter.result_str.begin(),
                                    interpreter.result_str.end(), '\n') + 1);
            } else {
                size -= ImGui::GetFrameHeight() *
                        (std::count(interpreter.result_err_str.begin(),
                                    interpreter.result_err_str.end(), '\n') + 1);
            }

            needs_eval = editor.Render("TextEditor", ImVec2(0, size));
            if (interpreter.result_valid) {
                ImGui::Text("%s", interpreter.result_str.c_str());
            } else {
                ImGui::Text("%s", interpreter.result_err_str.c_str());
            }
        ImGui::End();

        ImGui::Begin("Settings");
            ImGui::Text("Render size:");
            ImGui::RadioButton("256", &render_size, 256);
            ImGui::SameLine();
            ImGui::RadioButton("512", &render_size, 512);
            ImGui::SameLine();
            ImGui::RadioButton("1024", &render_size, 1024);
            ImGui::SameLine();
            ImGui::RadioButton("2048", &render_size, 2048);

            // Update the render context if size has changed
            if (render_size != ctx.image_size_px) {
                ctx = mpr::Context(render_size);
            }

            ImGui::Text("Dimension:");
            ImGui::RadioButton("2D", &render_dimension, 2);
            ImGui::SameLine();
            ImGui::RadioButton("3D", &render_dimension, 3);

            if (render_dimension == 3) {
                ImGui::Text("Render mode:");
                ImGui::RadioButton("Heightmap", &render_mode, RENDER_MODE_DEPTH);
                ImGui::SameLine();
                ImGui::RadioButton("Normals", &render_mode, RENDER_MODE_NORMALS);
                ImGui::SameLine();
                ImGui::RadioButton("SSAO", &render_mode, RENDER_MODE_SSAO);
                ImGui::SameLine();
                ImGui::RadioButton("Shaded", &render_mode, RENDER_MODE_SHADED);
            } else {
                render_mode = RENDER_MODE_2D;
            }
        ImGui::End();

        // Draw the shapes, and add them to the draw list
        auto background = ImGui::GetBackgroundDrawList();

        ImGui::Begin("Shapes");
            bool append = false;

            for (auto& s : shapes) {
                ImGui::Text("Shape at %p", (void*)s.first);
                ImGui::Columns(2);
                //ImGui::Text("%u clauses", s.second.handle->tape.num_clauses);
                //ImGui::Text("%u slots", s.second.handle->tape.num_regs);
                ImGui::NextColumn();
                //ImGui::Text("%u constants", s.second.handle->tape.num_constants);
                //ImGui::Text("%u CSG nodes", s.second.handle->tape.num_csg_choices);
                ImGui::Columns(1);

                {   // Timed rendering pass
                    using namespace std::chrono;
                    auto start = high_resolution_clock::now();
                    if (render_dimension == 2) {
                        Eigen::Matrix4f mat = model.matrix();
                        Eigen::Matrix3f mat2d;
                        mat2d.block<2, 2>(0, 0) = mat.block<2, 2>(0, 0);
                        mat2d.block<2, 1>(0, 2) = mat.block<2, 1>(0, 3);
                        mat2d.block<1, 2>(2, 0) = mat.block<1, 2>(3, 0);
                        mat2d.block<1, 1>(2, 2) = mat.block<1, 1>(3, 3);
                        ctx.render2D(s.second.tape, mat2d);
                    } else {
                        ctx.render3D(s.second.tape, model.matrix());
                    }
                    auto end = high_resolution_clock::now();
                    auto dt = duration_cast<microseconds>(end - start);
                    ImGui::Text("Render time: %f s", dt.count() / 1e6);

                    if (render_mode == RENDER_MODE_SSAO) {
                        start = high_resolution_clock::now();
                        effects.drawSSAO(ctx);
                        end = high_resolution_clock::now();
                        auto dt = duration_cast<microseconds>(end - start);
                        ImGui::Text("SSAO time: %f s", dt.count() / 1e6);
                    } else if (render_mode == RENDER_MODE_SHADED) {
                        start = high_resolution_clock::now();
                        effects.drawShaded(ctx);
                        end = high_resolution_clock::now();
                        auto dt = duration_cast<microseconds>(end - start);
                        ImGui::Text("SSAO + shading time: %f s", dt.count() / 1e6);
                    }

                    start = high_resolution_clock::now();
                    copy_to_texture(ctx, effects, cuda_tex, TEXTURE_SIZE,
                                    append, (Mode)render_mode);
                    end = high_resolution_clock::now();
                    dt = duration_cast<microseconds>(end - start);
                    ImGui::Text("Texture load time: %f s", dt.count() / 1e6);
                }

                if (ImGui::Button("Save shape.frep")) {
                    auto a = libfive::Archive();
                    a.addShape(s.second.tree);
                    std::ofstream out("shape.frep");
                    if (out.is_open()) {
                        a.serialize(out);
                    } else {
                        std::cerr << "Could not open shape.frep\n";
                    }
                }

                ImGui::Separator();

                // Later render passes will only append to the texture,
                // instead of writing both filled and empty pixels.
                append = true;
            }

            const float max_pixels = fmax(io.DisplaySize.x, io.DisplaySize.y);
            background->AddImage((void*)(intptr_t)gl_tex,
                    {io.DisplaySize.x / 2.0f - max_pixels / 2.0f,
                     io.DisplaySize.y / 2.0f + max_pixels / 2.0f},
                    {io.DisplaySize.x / 2.0f + max_pixels / 2.0f,
                     io.DisplaySize.y / 2.0f - max_pixels / 2.0f});


            // Draw XY axes based on current position
            {
                Eigen::Vector3f center = Eigen::Vector3f::Zero();
                Eigen::Vector3f ax = Eigen::Vector3f{1.0f, 0.0f, 0.0f};
                Eigen::Vector3f ay = Eigen::Vector3f{0.0f, 1.0f, 0.0f};
                Eigen::Vector3f az = Eigen::Vector3f{0.0f, 0.0f, 1.0f};

                for (auto pt : {&center, &ax, &ay, &az}) {
                    *pt = (model * view).inverse() * (*pt);
                }
                background->AddLine({center.x(), center.y()},
                                    {ax.x(), ax.y()}, 0xFF0000FF);
                background->AddLine({center.x(), center.y()},
                                    {ay.x(), ay.y()}, 0xFF00FF00);
                background->AddLine({center.x(), center.y()},
                                    {az.x(), az.y()}, 0xFFFF0000);
            }

        ImGui::End();

        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
