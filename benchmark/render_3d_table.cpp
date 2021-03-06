/*
Reference implementation for
"Massively Parallel Rendering of Complex Closed-Form Implicit Surfaces"
(SIGGRAPH 2020)

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this file,
You can obtain one at http://mozilla.org/MPL/2.0/.

Copyright (C) 2019-2020  Matt Keeter
*/
#include <cstdio>
#include <chrono>
#include <iostream>
#include <fstream>

// libfive
#include <libfive/tree/tree.hpp>
#include <libfive/tree/archive.hpp>
#include <libfive/render/discrete/heightmap.hpp>

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

    Eigen::Matrix4f T = Eigen::Matrix4f::Identity();
    T(3,2) = 0.3f;

    const std::vector<int> sizes = {256, 512, 1024, 1536, 2048};
    for (auto size: sizes) {
        auto tape = mpr::Tape(t);
        auto c = mpr::Context(size);

        std::cout << size << " ";
        auto mean = get_stats([&](){ c.render3D(tape, T); });

        libfive::Heightmap out(size, size);
        uint32_t i = 0;
        for (int x=0; x < size; ++x) {
            for (int y=0; y < size; ++y) {
                out.depth(x, y) = c.stages[3].filled[i];
                out.norm(x, y) = c.normals[i];
                ++i;
            }
        }
        out.savePNG("out_gpu_depth_ctx_" + std::to_string(size) + ".png");
        out.saveNormalPNG("out_gpu_norm_ctx_" + std::to_string(size) + ".png");

        if (mean > 750) {
            break;
        }
    }
    return 0;
}
