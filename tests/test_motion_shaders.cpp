// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the actual SPIR-V backend without opening a window or loading the game.
// Output is validated by spirv-val (see docs/upscaler.md).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <setjmp.h>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"


// Whether the module decorates something with Location = location (OpDecorate, opcode 71).
static bool HasLocation(const std::vector<u32>& code, u32 location) {
    for (std::size_t at = 5; at < code.size();) {
        const u32 words = code[at] >> 16, opcode = code[at] & 0xffff;
        if (words == 0) {
            break;
        }
        if (opcode == 71 && words >= 4 && code[at + 2] == 30 /* Location */ && code[at + 3] == location) {
            return true;
        }
        at += words;
    }
    return false;
}

int main(int argc, char** argv) {
    using namespace Shader;
    const std::filesystem::path dir = argc > 1 ? argv[1] : ".";
    std::filesystem::create_directories(dir);
    // Without the motion buffer (object motion off) neither stage may have the motion varyings:
    // a pipeline saved with them meets such a session through the pipeline cache.
    for (bool buffer : {true, false}) {
    MotionVectors::params_address = buffer ? 0x10000 : 0;
    MotionVectors::positions_address = buffer ? 0x20000 : 0;
    for (bool vertex : {true, false}) {
        for (bool motion : {false, true}) {
            Info info{};
            info.hw_stage = vertex ? HwStage::Vertex : HwStage::Fragment;
            info.sw_stage = vertex ? SwStage::Vertex : SwStage::Fragment;
            RuntimeInfo runtime{};
            runtime.Initialize(info.hw_stage, info.sw_stage);
            if (vertex) {
                runtime.hw.vs.motion_vectors = motion;
            } else {
                runtime.hw.fs.motion_vectors = motion;
                runtime.hw.fs.color_buffers[0].num_format = AmdGpu::NumberFormat::Float;
            }
            Common::ObjectPool<IR::Inst> pool;
            IR::Block block(pool);
            IR::IREmitter ir(block);
            const auto output = vertex ? IR::Attribute::Position0 : IR::Attribute::RenderTarget0;
            for (u32 component = 0; component < 4; ++component) {
                info.stores.Set(output, component);
                if (!vertex && component < 2) {
                    info.loads.Set(IR::Attribute::FragCoord, component);
                    ir.SetAttribute(output, ir.GetAttribute(IR::Attribute::FragCoord, component), component);
                } else {
                    ir.SetAttribute(output, ir.Imm32(component == 3 ? 1.0f : 0.0f), component);
                }
            }
            ir.Epilogue();
            IR::Program program(info);
            program.blocks.push_back(&block);
            program.syntax_list.push_back({.data = {.block = &block},
                                           .type = IR::AbstractSyntaxNode::Type::Block});
            program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
            Profile profile{};
            profile.supported_spirv = 0x00010600;
            profile.support_int64 = true;
            Backend::Bindings bindings{};
            const auto code = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
            // Checked without assert: this target keeps NDEBUG.
            if (HasLocation(code, MotionVectors::CurrentLocation) != (motion && buffer)) {
                std::printf("FAIL: %s shader, motion %d, buffer %d: motion varyings %s\n",
                            vertex ? "vertex" : "fragment", motion, buffer,
                            motion && buffer ? "missing" : "present");
                return 1;
            }
            if (!buffer) {
                continue;
            }
            const auto path = dir / (std::string(vertex ? "vertex" : "fragment") +
                                      (motion ? "-motion.spv" : "-plain.spv"));
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(u32));
            if (!out) { return 1; }
        }
    }
    }
    std::puts("Motion shaders: varyings in both stages or in neither (no buffer) PASS");
}
