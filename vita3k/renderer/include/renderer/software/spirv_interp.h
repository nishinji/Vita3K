// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace renderer::software {

// Decorations and storage classes are kept as plain integers so that the
// glslang SPIR-V headers stay confined to the interpreter implementation.
constexpr uint32_t SPIRV_NO_VALUE = ~0u;

// Invocations run in lockstep batches this wide, so one decode of an
// instruction is shared by up to this many pixels or vertices.
constexpr uint32_t SPIRV_MAX_LANES = 16;

enum class SpirvTypeKind : uint8_t {
    Unknown,
    Void,
    Bool,
    Int,
    Float,
    Vector,
    Matrix,
    Array,
    RuntimeArray,
    Struct,
    Pointer,
    Function,
    Image,
    Sampler,
    SampledImage,
};

struct SpirvType {
    SpirvTypeKind kind = SpirvTypeKind::Unknown;
    // Scalar width in bits, for Int and Float.
    uint32_t width = 0;
    bool is_signed = false;
    // Element type of a vector, matrix, array or pointer.
    uint32_t element_type = 0;
    uint32_t element_count = 0;
    // Struct members, with the byte offset each one was decorated with.
    std::vector<uint32_t> member_types;
    std::vector<uint32_t> member_offsets;
    // Size of one value of this type, in bytes, in the interpreter layout.
    uint32_t byte_size = 0;
    // ArrayStride / MatrixStride decoration, when one was given.
    uint32_t stride = 0;
    uint32_t storage_class = 0;
    // Image dimensionality, for Image types.
    uint32_t dim = 0;
    bool arrayed = false;

    // Vectors and matrices seen as a flat run of scalars, which is how every
    // arithmetic instruction walks them.
    SpirvTypeKind scalar_kind = SpirvTypeKind::Unknown;
    uint32_t scalar_width = 32;
    uint32_t scalar_count = 1;
    uint32_t scalar_size = 4;
    bool scalar_signed = false;
};

struct SpirvVariable {
    uint32_t id = 0;
    // Type of the value the variable points at, not of the pointer itself.
    uint32_t type_id = 0;
    // Type of the pointer the variable itself is, which is what an OpLoad or an
    // OpAccessChain on it sees.
    uint32_t pointer_type_id = 0;
    uint32_t storage_class = 0;
    uint32_t location = SPIRV_NO_VALUE;
    uint32_t binding = SPIRV_NO_VALUE;
    uint32_t descriptor_set = SPIRV_NO_VALUE;
    uint32_t builtin = SPIRV_NO_VALUE;
    uint32_t initializer = 0;
    uint32_t byte_size = 0;
    // Declared inside a function body rather than at module scope.
    bool is_local = false;
    std::string name;
};

// A pointer whose target the compile step could pin down: a variable and a
// byte offset into it. Loads and stores through one never touch a register.
struct SpirvStaticPointer {
    uint32_t variable = 0;
    uint32_t offset = 0;
    // Type of the value pointed at.
    uint32_t type_id = 0;

    bool valid() const {
        return variable != 0;
    }
};

// One instruction of a function body, with what could be worked out ahead of
// time so the dispatch loop does not have to.
struct SpirvInstruction {
    uint16_t op = 0;
    uint16_t word_count = 0;
    // Where the operands start inside the module words.
    uint32_t operands = 0;
    // Instruction index of a branch target or of a callee entry.
    uint32_t target = 0;
    uint32_t second_target = 0;
    // Op specific: byte offset of a composite access, or a callee number.
    uint32_t aux = 0;
    // Bytes of the value the instruction produces, 0 when it produces none.
    uint32_t size = 0;
};

// A SPIR-V binary produced by the shader recompiler, decoded into the tables the
// interpreter walks. Parsing is done once per program and the result is shared
// by every invocation, so this stays immutable while shaders are running.
class SpirvModule {
public:
    SpirvModule() = default;
    // The type table points into `types`, so a copy would dangle.
    SpirvModule(const SpirvModule &) = delete;
    SpirvModule &operator=(const SpirvModule &) = delete;

    bool parse(const std::vector<uint32_t> &code_words);

    const SpirvType *type(uint32_t id) const {
        return (id < type_table.size()) ? type_table[id] : nullptr;
    }

    uint32_t byte_size_of(uint32_t type_id) const {
        const SpirvType *info = type(type_id);
        return info ? info->byte_size : 0;
    }

    // Entry point of the single shader this module holds.
    uint32_t entry_point = 0;
    // Parameter ids of every function, in declaration order.
    std::unordered_map<uint32_t, std::vector<uint32_t>> function_parameters;

    std::unordered_map<uint32_t, SpirvType> types;
    std::unordered_map<uint32_t, SpirvVariable> variables;
    // Constants, already materialized into their byte representation.
    std::unordered_map<uint32_t, std::vector<uint8_t>> constants;
    std::unordered_map<uint32_t, uint32_t> constant_types;
    // Maps a SpecId decoration to the id of the specialization constant.
    std::unordered_map<uint32_t, uint32_t> spec_constant_ids;

    // Variables sorted by the interface they belong to, for quick binding.
    std::vector<uint32_t> input_variables;
    std::vector<uint32_t> output_variables;
    std::vector<uint32_t> uniform_variables;
    std::vector<uint32_t> image_variables;
    // Module scope variables every invocation starts afresh.
    std::vector<uint32_t> private_variables;

    std::vector<uint32_t> words;
    uint32_t id_bound = 0;
    bool is_fragment = false;
    // Set when the shader can throw a fragment away. Without it the rasterizer
    // is free to test depth before running the shader at all.
    bool may_discard = false;
    // Set when the shader reads the pixel it is about to write (descriptor set 1).
    bool reads_destination = false;

    // What the compile step derives from the tables above.
    std::vector<const SpirvType *> type_table;
    std::vector<SpirvInstruction> code;
    // Instruction index of every label and of every function's first
    // instruction, by id; SPIRV_NO_VALUE when the id is neither.
    std::vector<uint32_t> label_index;
    std::vector<uint32_t> function_index;
    // Callee number of every function, indexing callee_parameters.
    std::vector<uint32_t> function_number;
    std::vector<std::vector<uint32_t>> callee_parameters;

    // A byte range of a variable, the same range in every lane.
    struct Range {
        uint32_t variable = 0;
        uint32_t offset = 0;
        uint32_t size = 0;
    };

    // Callees that touch only private variables, bound buffers and constants,
    // by callee number. When every lane agrees on the ranges one reads or
    // writes, a single lane can run it for the batch; memo_state lists them.
    std::vector<uint8_t> memoizable;
    std::vector<std::vector<Range>> memo_state;

    // Type and per-lane byte size of every value, by id.
    std::vector<uint32_t> value_type;
    std::vector<uint32_t> value_size;
    // Where every value lives in the register file. Constants are stored once
    // and have a zero lane stride; everything else has one copy per lane.
    std::vector<uint32_t> register_offset;
    std::vector<uint32_t> register_stride;
    std::vector<uint8_t> constant_registers;
    uint32_t register_bytes = 0;

    std::vector<SpirvStaticPointer> static_pointers;
    // Pointers something other than a load, a store or an access chain reads,
    // which therefore need their per-lane value computed.
    std::vector<uint8_t> materialized;

    // Storage of the variables the interpreter owns, by id: lane 0 offset and
    // the distance between lanes.
    std::vector<uint32_t> variable_offset;
    std::vector<uint32_t> variable_stride;
    uint32_t variable_bytes = 0;

private:
    bool compile();
    void find_memoizable_functions(const std::vector<uint8_t> &returns_void);
};

// Texture sampling is delegated to the caller: the interpreter knows which
// binding an image variable has, but not how the texture cache stores pixels.
// One call serves every lane in lane_mask: coords holds 4 floats per lane,
// lods one per lane, and out_rgba receives 4 floats per lane.
using SamplerCallback = std::function<void(uint32_t descriptor_set, uint32_t binding, uint32_t lane_mask,
    const float *coords, uint32_t coord_count, const float *lods, bool has_lod, float *out_rgba)>;

// Everything an invocation reads from outside its own registers.
struct SpirvBindings {
    struct Buffer {
        const uint8_t *data = nullptr;
        uint32_t size = 0;
    };

    // Uniform buffers of descriptor set 0, indexed by binding.
    std::vector<Buffer> uniform_buffers;
    SamplerCallback sample_texture;
    // Value of every specialization constant the caller wants to override.
    std::unordered_map<uint32_t, uint32_t> spec_constants;
};

// Host storage of one interface variable across a batch: lane i lives at
// base + i * stride.
struct SpirvLaneStorage {
    uint8_t *base = nullptr;
    uint32_t stride = 0;

    explicit operator bool() const {
        return base != nullptr;
    }

    uint8_t *lane(uint32_t index) const {
        return base + static_cast<size_t>(index) * stride;
    }
};

// Runs a batch of up to SPIRV_MAX_LANES invocations in lockstep. An interpreter
// owns the mutable state of one batch, so one is kept per worker thread.
class SpirvInterpreter {
public:
    void set_program(const SpirvModule *module, const SpirvBindings *bindings);

    // Storage backing an input/output/builtin variable. The caller writes each
    // lane's inputs before execute() and reads the outputs back after it.
    SpirvLaneStorage input_by_location(uint32_t location) const;
    SpirvLaneStorage output_by_location(uint32_t location) const;
    SpirvLaneStorage builtin_storage(uint32_t builtin) const;

    // Executes the entry point for every lane set in lane_mask and returns the
    // lanes that were not discarded by OpKill.
    uint32_t execute(uint32_t lane_mask);

    const SpirvModule *module() const {
        return m_module;
    }

private:
    struct VariableStorage {
        uint8_t *base = nullptr;
        uint32_t stride = 0;
        // Bytes addressable from the lane base.
        uint32_t size = 0;
    };

    struct PrivateInit {
        VariableStorage storage;
        const uint8_t *initializer = nullptr;
        uint32_t initializer_size = 0;
    };

    uint8_t *reg(uint32_t id, uint32_t lane) const {
        return m_registers + m_module->register_offset[id]
            + static_cast<size_t>(lane) * m_module->register_stride[id];
    }

    // A value's lanes, looked up once so a lane loop is only pointer arithmetic.
    struct Lanes {
        uint8_t *base;
        size_t stride;

        uint8_t *operator[](uint32_t lane) const {
            return base + lane * stride;
        }
    };

    Lanes lanes(uint32_t id) const {
        return { m_registers + m_module->register_offset[id], m_module->register_stride[id] };
    }

    SpirvLaneStorage variable_lanes(uint32_t variable_id) const;
    void copy_lanes(uint32_t destination, uint32_t source, uint32_t mask);
    // Where a pointer value points for one lane, with the bytes left behind it and the pointee type.
    uint8_t *resolve_pointer(uint32_t pointer_id, uint32_t lane, uint32_t &room, uint32_t &type_id) const;

    // Runs from the instruction at pc until the function returns. A branch the
    // lanes disagree on splits them, each group finishing the function alone.
    void run(uint32_t pc, uint32_t mask, uint32_t current_label, uint32_t result_id, uint32_t depth);
    void call(const SpirvInstruction &instruction, const uint32_t *operands, uint32_t mask, uint32_t depth);
    // Runs a memoizable function once for the whole batch; false when the lanes disagree and it has to run normally.
    bool run_shared(uint32_t number, uint32_t target, uint32_t mask, uint32_t depth);

    void execute_scalar_op(uint32_t op, const uint32_t *operands, uint32_t count, uint32_t mask);
    void execute_bit_op(uint32_t op, const uint32_t *operands, uint32_t mask);
    void execute_matrix_op(uint32_t op, const uint32_t *operands, uint32_t count, uint32_t mask);
    void execute_ext_inst(const uint32_t *operands, uint32_t count, uint32_t mask);
    void execute_image_op(uint32_t op, const uint32_t *operands, uint32_t count, uint32_t mask);
    void execute_access_chain(const uint32_t *operands, uint32_t count, uint32_t mask);
    void execute_load(const uint32_t *operands, uint32_t mask);
    void execute_store(const uint32_t *operands, uint32_t mask);

    const SpirvModule *m_module = nullptr;
    const SpirvBindings *m_bindings = nullptr;

    std::vector<uint8_t> m_register_file;
    uint8_t *m_registers = nullptr;
    std::vector<uint8_t> m_variable_file;
    // Lane 0 base, stride and size of every variable, bound buffers included.
    std::vector<VariableStorage> m_variables;
    std::vector<PrivateInit> m_private_inits;
    std::vector<uint8_t> m_phi_scratch;

    // The last state a memoizable callee started from and what it left, per
    // callee number; bound buffers stay put for a draw, so this does too.
    struct Memo {
        bool valid = false;
        std::vector<uint8_t> input;
        std::vector<uint8_t> output;
    };
    std::vector<Memo> m_memo;
    std::vector<uint8_t> m_memo_scratch;

    std::unordered_map<uint32_t, uint32_t> m_location_to_input;
    std::unordered_map<uint32_t, uint32_t> m_location_to_output;
    std::unordered_map<uint32_t, uint32_t> m_builtin_to_variable;

    uint32_t m_discarded = 0;
};

} // namespace renderer::software
