#include <cstdio>
#include <chrono>
#include <iostream>
#include <fstream>

// libfive
#include <libfive/tree/tree.hpp>
#include <libfive/tree/archive.hpp>
#include <libfive/render/discrete/heightmap.hpp>

// Old implementation
#include "renderable.hpp"

// New implementation
#include "tape.hpp"
#include "context.hpp"

#include "stats.hpp"

int main(int argc, char **argv)
{
    libfive::Tree t = libfive::Tree::X();
    if (argc == 2) {
        std::ifstream ifs;
        ifs.open(argv[1]);
        if (ifs.is_open()) {
            auto a = libfive::Archive::deserialize(ifs);
            t = a.shapes.front().tree;
        } else {
            fprintf(stderr, "Could not open file %s\n", argv[1]);
            exit(1);
        }
    } else {
        auto X = libfive::Tree::X();
        auto Y = libfive::Tree::Y();
        auto Z = libfive::Tree::Z();
        t = min(sqrt((X + 0.5)*(X + 0.5) + Y*Y + Z*Z) - 0.25,
                sqrt((X - 0.5)*(X - 0.5) + Y*Y + Z*Z) - 0.25);
    }

    auto tape = libfive::cuda::Tape(t);

    const std::vector<int> sizes = {256, 512, 1024, 2048, 3074, 4096};
    std::cout << "Rendering with new architecture:" << std::endl;
    for (auto size: sizes) {
        auto ctx = libfive::cuda::Context(size);
        std::cout << size << " ";
        get_stats([&](){ ctx.render2D(tape, Eigen::Matrix3f::Identity()); });

        libfive::Heightmap out(size, size);
        uint32_t i = 0;
        for (int x=0; x < size; ++x) {
            for (int y=0; y < size; ++y) {
                out.depth(x, y) = ctx.stages[3].filled.get()[i++];
            }
        }
        out.savePNG("out_gpu_ctx2d_" + std::to_string(size) + ".png");
    }
    std::cout << "Rendering with original architecture:" << std::endl;
    for (auto size: sizes) {
        auto r = Renderable::build(t, size, 2);
        std::cout << size << " ";
        get_stats([&](){ r->run({Eigen::Matrix4f::Identity()}, Renderable::MODE_HEIGHTMAP); });

        libfive::Heightmap out(size, size);
        for (int x=0; x < size; ++x) {
            for (int y=0; y < size; ++y) {
                out.depth(y, x) = r->heightAt(y, x);
            }
        }
        out.savePNG("out_gpu_orig_" + std::to_string(size) + ".png");
    }
    return 0;
}

