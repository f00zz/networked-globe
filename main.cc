#include "panic.h"

#include "geometry.h"
#include "rand.h"
#include "shader_program.h"
#include "globe_geometry.h"
#include "cities.h"
#include "blur_effect.h"
#include "util.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <iostream>
#include <memory>
#include <algorithm>
#include <cstdlib>

#define GLOW

namespace
{
using connection_vertex = std::tuple<glm::vec3, float>;

glm::vec3 to_position(float latitude, float longitude)
{
    const auto y = std::sin(latitude);
    const auto r = std::cos(latitude);
    const auto z = r * std::sin(longitude);
    const auto x = - r * std::cos(longitude);
    return {x, y, z};
}

std::unique_ptr<geometry<connection_vertex>> build_connection_geometry(const glm::vec3 &from_normal, const glm::vec3 &to_normal, float max_height)
{
    std::vector<connection_vertex> verts;

    constexpr const auto num_steps = 256;

    for (int i = 0; i < num_steps; ++i)
    {
        const float t = static_cast<float>(i) / (num_steps - 1);
        auto v = glm::normalize(from_normal + t * (to_normal - from_normal));
        const auto s = 1.0 + max_height * (1.0 - 4.0 * (t - 0.5) * (t - 0.5));
        v *= s;
        verts.emplace_back(v, t);
    }

    auto g = std::make_unique<geometry<connection_vertex>>();
    g->set_data(verts);
    return g;
}
}

class demo
{
public:
    demo(int window_width, int window_height)
        : window_width_(window_width)
        , window_height_(window_height)
        , blur_(window_width/2, window_height/2)
    {
        initialize_graph();
        initialize_meshes();
        initialize_connections();
        initialize_shader();
    }

    void render_and_step(float dt)
    {
        render();
        step_connections();
        cur_time_ += dt;
    }

private:
    void initialize_graph()
    {
        const auto &cities = get_cities();

        graph_.reserve(cities.size());
        std::transform(cities.begin(), cities.end(), std::back_inserter(graph_), [](const auto &city) -> graph_vertex {
            return {to_position(city.latitude, city.longitude), {}};
        });
    }

    void initialize_meshes()
    {
        globe_ = build_globe_geometry(6, "assets/map.png");

        cities_ = std::make_unique<geometry<city_vertex>>();
        cities_->set_data(nullptr, graph_.size());
    }

    void initialize_connections()
    {
        constexpr const auto num_connections = 5;
        constexpr const auto min_distance = 0.75;

        constexpr const auto min_height = 0.05;
        constexpr const auto max_height = 0.3;

        constexpr const auto max_per_city = 3;

        for (int i = 0; i < graph_.size(); ++i)
        {
            for (int j = 0; j < graph_.size(); ++j)
            {
                if (i == j)
                    continue;

                const auto &from = graph_[i].position;
                const auto &to = graph_[j].position;
                const auto d = glm::distance(from, to);
                if (d < min_distance)
                {
                    auto conn = std::make_unique<connection>();

                    conn->active = (std::rand() % 150) == 0;
                    conn->elapsed = 0;
                    conn->target_vertex = j;

                    const auto height = min_height + (d / min_distance) * (max_height - min_height);
                    conn->mesh = build_connection_geometry(from, to, height);

                    graph_[i].connections.push_back(std::move(conn));
                }
            }
        }
    }

    void initialize_shader()
    {
        globe_program_.add_shader(GL_VERTEX_SHADER, "shaders/sphere.vert");
        globe_program_.add_shader(GL_FRAGMENT_SHADER, "shaders/sphere.frag");
        globe_program_.link();

        cities_program_.add_shader(GL_VERTEX_SHADER, "shaders/cities.vert");
        cities_program_.add_shader(GL_GEOMETRY_SHADER, "shaders/cities.geom");
        cities_program_.add_shader(GL_FRAGMENT_SHADER, "shaders/cities.frag");
        cities_program_.link();

        connection_program_.add_shader(GL_VERTEX_SHADER, "shaders/connection.vert");
        connection_program_.add_shader(GL_FRAGMENT_SHADER, "shaders/connection.frag");
        connection_program_.link();
    }

    void render() const
    {
        update_cities_mesh();

        const auto projection =
            glm::perspective(glm::radians(45.0f), static_cast<float>(window_width_) / window_height_, 0.1f, 100.f);
        const auto view_pos = glm::vec3(0, 1.3, 3);
        const auto view_up = glm::vec3(0, 1, 0);
        const auto view = glm::lookAt(view_pos, glm::vec3(0, 0, 0), view_up);

        const auto angle = -0.2f * 0.75f * cur_time_ + 1.5f;
        const auto model = glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0, 1, 0));
        const auto mvp = projection * view * model;

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

#ifdef GLOW
        blur_.bind();

        glViewport(0, 0, blur_.width(), blur_.height());
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glDisable(GL_MULTISAMPLE);
        glLineWidth(3.0);

        render_planet(mvp, glm::vec4(0), glm::vec4(0));
        render_connections(mvp, glm::vec4(0.5, 0.35, 0.0, 1.0));
        framebuffer::unbind();
#endif

        glViewport(0, 0, window_width_, window_height_);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glEnable(GL_MULTISAMPLE);
        glLineWidth(3.0);

        render_planet(mvp, glm::vec4(0.6), glm::vec4(0.4));
        render_connections(mvp, glm::vec4(1.0, 0.35, 0.0, 1.0));

#ifdef GLOW
        blur_.render(window_width_, window_height_);
#endif
    }

    void render_planet(const glm::mat4 &mvp, const glm::vec4 &front_color, const glm::vec4 &back_color) const
    {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);

        glEnable(GL_CULL_FACE);
        globe_program_.bind();
        globe_program_.set_uniform(globe_program_.uniform_location("mvp"), mvp);
        globe_program_.set_uniform(globe_program_.uniform_location("color"), front_color);
        glCullFace(GL_BACK);

        globe_->render(GL_TRIANGLES);
        globe_program_.set_uniform(globe_program_.uniform_location("color"), back_color);
        glCullFace(GL_FRONT);
        globe_->render(GL_TRIANGLES);
    }

    void render_connections(const glm::mat4 &mvp, const glm::vec4 &color) const
    {
        glDisable(GL_CULL_FACE);
        glDepthMask(GL_FALSE);

        cities_program_.bind();
        cities_program_.set_uniform(cities_program_.uniform_location("mvp"), mvp);
        cities_program_.set_uniform(connection_program_.uniform_location("color"), glm::vec4(1.0, 0.35, 0.0, 1.0));
        cities_->render(GL_POINTS, num_active_cities_);

        const float tex_offset = 0.5f * cur_time_;

        connection_program_.bind();
        connection_program_.set_uniform(connection_program_.uniform_location("mvp"), mvp);
        connection_program_.set_uniform(connection_program_.uniform_location("color"), color);

        for (const auto &vertex : graph_)
        {
            for (const auto &conn : vertex.connections)
            {
                if (conn->active)
                {
                    connection_program_.set_uniform(connection_program_.uniform_location("tex_offset"), conn->elapsed);
                    conn->mesh->render(GL_LINE_STRIP);
                }
            }
        }

        glDepthMask(GL_TRUE);
    }

    void update_cities_mesh() const
    {
        num_active_cities_ = 0;

        auto *vert_data = cities_->map_vertex_data();

        for (const auto &vertex : graph_)
        {
            const auto &connections = vertex.connections;
            if (connections.empty())
                continue;
            auto it = std::max_element(connections.begin(), connections.end(),
                                       [](const auto &a, const auto &b) { return a->elapsed < b->elapsed; });
            const auto elapsed = (*it)->elapsed;
            if (elapsed == 0.f)
                continue;
            const float alpha = std::min(1.0f, elapsed * 2.0f);
            *vert_data++ = {vertex.position, alpha};
            ++num_active_cities_;
        }

        cities_->unmap_vertex_data();
    }

    void step_connections()
    {
        for (auto &vertex : graph_)
        {
            for (auto &conn : vertex.connections)
            {
                if (!conn->active || conn->elapsed >= 1.0)
                    continue;
                conn->elapsed += 0.02;
                if (conn->elapsed >= 1.0)
                {
                    auto &target_vertex = graph_[conn->target_vertex];
                    int activated = 0;
                    for (auto &next_conn : target_vertex.connections)
                    {
                        if (!next_conn->active)
                        {
                            next_conn->active = true;
                            if (++activated == 3)
                                break;
                        }
                    }
                }
            }
        }
    }

    int window_width_;
    int window_height_;
    float cur_time_ = 0;
    std::unique_ptr<geometry<globe_vertex>> globe_;

    using city_vertex = std::tuple<glm::vec3, float>;
    std::unique_ptr<geometry<city_vertex>> cities_;

    struct connection
    {
        bool active;
        float elapsed = 0.0;;
        int target_vertex;
        std::unique_ptr<geometry<connection_vertex>> mesh;
    };

    struct graph_vertex
    {
        glm::vec3 position;
        std::vector<std::unique_ptr<connection>> connections;
    };

    std::vector<graph_vertex> graph_;

    mutable int num_active_cities_;

    shader_program globe_program_;
    shader_program cities_program_;
    shader_program connection_program_;
    blur_effect blur_;
};

int main()
{
    constexpr auto window_width = 1000;
    constexpr auto window_height = 1000;

    if (!glfwInit())
        panic("glfwInit failed\n");

    glfwSetErrorCallback([](int error, const char *description) { panic("GLFW error: %s\n", description); });

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 16);
    auto *window = glfwCreateWindow(window_width, window_height, "demo", nullptr, nullptr);
    if (!window)
        panic("glfwCreateWindow failed\n");

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    glewInit();

    glfwSetKeyCallback(window, [](GLFWwindow *window, int key, int scancode, int action, int mode) {
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
            glfwSetWindowShouldClose(window, GL_TRUE);
    });

#ifdef DUMP_FRAMES
    constexpr auto total_frames = 500;
    auto frame_num = 0;
#endif

    {
        demo d(window_width, window_height);

        while (!glfwWindowShouldClose(window))
        {
#ifdef DUMP_FRAMES
            const auto dt = 1.0f / 40;
#else
            const auto dt = 1.0f / 60;
#endif
            d.render_and_step(dt);

#ifdef DUMP_FRAMES
            char path[80];
            std::sprintf(path, "%05d.ppm", frame_num);
            dump_frame_to_file(path, window_width, window_height);

            if (++frame_num == total_frames)
                break;
#endif

            glfwSwapBuffers(window);
            glfwPollEvents();
        }
    }

    glfwDestroyWindow(window);
    glfwTerminate();
}
