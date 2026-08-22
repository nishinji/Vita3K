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

#include <renderer/software/spirv_interp.h>

#include <renderer/software/common.h>
#include <util/log.h>

#include <SPIRV/GLSL.std.450.h>
// For spv::HasResultAndType, which tells which operands of an arbitrary instruction are its result.
#define SPV_ENABLE_UTILITY_CODE
#include <SPIRV/spirv.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace renderer::software {

namespace {

constexpr uint32_t SPIRV_MAGIC = 0x07230203;
constexpr uint32_t MAX_CALL_DEPTH = 64;

// A pointer value as it is kept inside a register. Pointers are ordinary SSA
// values in SPIR-V, so they need a byte representation like everything else.
struct PointerRepr {
    uint8_t *base;
    uint32_t type_id;
    uint32_t remaining;
};

// An image or sampled image value: the descriptor it was loaded from. Sampling
// is done by the caller, so this is all the interpreter needs to carry around.
struct ImageRepr {
    uint32_t descriptor_set;
    uint32_t binding;
};

constexpr uint32_t POINTER_SIZE = sizeof(PointerRepr);
constexpr uint32_t IMAGE_SIZE = sizeof(ImageRepr);
// Ids with no register of their own all share this many zeroed bytes.
constexpr uint32_t NULL_REGISTER_SIZE = 64;

uint32_t opcode_of(uint32_t word) {
    return word & 0xFFFFu;
}

uint32_t word_count_of(uint32_t word) {
    return word >> 16;
}

uint32_t align_up(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

bool is_bound_buffer(uint32_t storage_class) {
    return storage_class == spv::StorageClassUniform
        || storage_class == spv::StorageClassStorageBuffer
        || storage_class == spv::StorageClassPushConstant;
}

std::string read_string(const std::vector<uint32_t> &words, uint32_t offset, uint32_t max_words) {
    std::string result;
    for (uint32_t i = 0; i < max_words; i++) {
        const uint32_t word = words[offset + i];
        for (uint32_t byte = 0; byte < 4; byte++) {
            const char character = static_cast<char>((word >> (byte * 8)) & 0xFF);
            if (character == '\0')
                return result;
            result.push_back(character);
        }
    }
    return result;
}

// A vector insert at an index the compile step found constant, operands as OpVectorInsertDynamic.
constexpr uint16_t SPIRV_OP_INSERT_AT = 0xFFF0;

void result_and_type(uint32_t op, bool &has_result, bool &has_type) {
    if (op == SPIRV_OP_INSERT_AT) {
        has_result = has_type = true;
        return;
    }
    spv::HasResultAndType(static_cast<spv::Op>(op), &has_result, &has_type);
}

// Calls function with the operand position of every value an instruction
// reads, for the instructions the compile step rewrites; literals and labels
// are left out, and an instruction not listed is never rewritten.
template <typename F>
void for_each_value_operand(uint32_t op, uint32_t word_count, F &&function) {
    const uint32_t operand_count = word_count - 1;
    const auto range = [&](uint32_t first, uint32_t last) {
        for (uint32_t i = first; i < last && i < operand_count; i++)
            function(i);
    };

    switch (op) {
    case spv::OpLoad:
    case spv::OpCompositeExtract:
    case spv::OpImage:
        range(2, 3);
        break;
    case spv::OpStore:
    case spv::OpCopyMemory:
        range(0, 2);
        break;
    case spv::OpReturnValue:
    case spv::OpBranchConditional:
    case spv::OpSwitch:
        range(0, 1);
        break;
    case spv::OpCompositeInsert:
    case spv::OpVectorShuffle:
    case spv::OpVectorExtractDynamic:
    case spv::OpSampledImage:
        range(2, 4);
        break;
    case SPIRV_OP_INSERT_AT:
    case spv::OpVectorInsertDynamic:
    case spv::OpSelect:
        range(2, 5);
        break;
    case spv::OpExtInst:
        range(4, operand_count);
        break;
    case spv::OpFunctionCall:
        range(3, operand_count);
        break;
    case spv::OpPhi:
        for (uint32_t i = 2; i < operand_count; i += 2)
            function(i);
        break;
    case spv::OpImageSampleImplicitLod:
    case spv::OpImageSampleExplicitLod:
    case spv::OpImageSampleProjImplicitLod:
    case spv::OpImageSampleProjExplicitLod:
    case spv::OpImageFetch:
    case spv::OpImageRead:
        // The image operands mask sits between the coordinate and the operand ids.
        range(2, 4);
        range(5, operand_count);
        break;
    case spv::OpAccessChain:
    case spv::OpInBoundsAccessChain:
    case spv::OpCompositeConstruct:
    case spv::OpCopyObject:
    case spv::OpBitcast:
    case spv::OpFNegate:
    case spv::OpSNegate:
    case spv::OpNot:
    case spv::OpFAdd:
    case spv::OpFSub:
    case spv::OpFMul:
    case spv::OpFDiv:
    case spv::OpFMod:
    case spv::OpFRem:
    case spv::OpIAdd:
    case spv::OpISub:
    case spv::OpIMul:
    case spv::OpSDiv:
    case spv::OpUDiv:
    case spv::OpSMod:
    case spv::OpSRem:
    case spv::OpUMod:
    case spv::OpShiftLeftLogical:
    case spv::OpShiftRightLogical:
    case spv::OpShiftRightArithmetic:
    case spv::OpBitwiseAnd:
    case spv::OpBitwiseOr:
    case spv::OpBitwiseXor:
    case spv::OpLogicalAnd:
    case spv::OpLogicalOr:
    case spv::OpLogicalNot:
    case spv::OpLogicalEqual:
    case spv::OpLogicalNotEqual:
    case spv::OpFOrdEqual:
    case spv::OpFUnordEqual:
    case spv::OpFOrdNotEqual:
    case spv::OpFUnordNotEqual:
    case spv::OpFOrdLessThan:
    case spv::OpFUnordLessThan:
    case spv::OpFOrdGreaterThan:
    case spv::OpFUnordGreaterThan:
    case spv::OpFOrdLessThanEqual:
    case spv::OpFUnordLessThanEqual:
    case spv::OpFOrdGreaterThanEqual:
    case spv::OpFUnordGreaterThanEqual:
    case spv::OpIEqual:
    case spv::OpINotEqual:
    case spv::OpSLessThan:
    case spv::OpULessThan:
    case spv::OpSGreaterThan:
    case spv::OpUGreaterThan:
    case spv::OpSLessThanEqual:
    case spv::OpULessThanEqual:
    case spv::OpSGreaterThanEqual:
    case spv::OpUGreaterThanEqual:
    case spv::OpConvertFToS:
    case spv::OpConvertFToU:
    case spv::OpConvertSToF:
    case spv::OpConvertUToF:
    case spv::OpFConvert:
    case spv::OpSConvert:
    case spv::OpUConvert:
    case spv::OpIsNan:
    case spv::OpIsInf:
    case spv::OpAny:
    case spv::OpAll:
    case spv::OpDot:
    case spv::OpVectorTimesScalar:
    case spv::OpMatrixTimesVector:
    case spv::OpVectorTimesMatrix:
    case spv::OpMatrixTimesMatrix:
    case spv::OpMatrixTimesScalar:
    case spv::OpTranspose:
    case spv::OpBitFieldInsert:
    case spv::OpBitFieldSExtract:
    case spv::OpBitFieldUExtract:
    case spv::OpBitReverse:
    case spv::OpBitCount:
        range(2, operand_count);
        break;
    default:
        break;
    }
}

// std::countr_zero picks its instruction at run time on MSVC, which costs more than the lane work it guards.
inline uint32_t lowest_lane(uint32_t mask) {
#ifdef _MSC_VER
    unsigned long index;
    _BitScanForward(&index, mask);
    return static_cast<uint32_t>(index);
#else
    return static_cast<uint32_t>(__builtin_ctz(mask));
#endif
}

template <typename F>
inline void for_each_lane(uint32_t mask, F &&function) {
    // Batches fill their lanes from the bottom, so a mask is usually one solid run from lane 0.
    if ((mask & (mask + 1)) == 0) {
        for (uint32_t lane = 0; mask; lane++, mask >>= 1)
            function(lane);
        return;
    }
    while (mask) {
        const uint32_t lane = lowest_lane(mask);
        mask &= mask - 1;
        function(lane);
    }
}

// Fixed-size copies compile to plain moves, which matters on paths that run for
// every lane of every instruction.
inline void copy_value(uint8_t *destination, const uint8_t *source, uint32_t size) {
    switch (size) {
    case 4:
        std::memcpy(destination, source, 4);
        break;
    case 8:
        std::memcpy(destination, source, 8);
        break;
    case 12:
        std::memcpy(destination, source, 12);
        break;
    case 16:
        std::memcpy(destination, source, 16);
        break;
    default:
        std::memcpy(destination, source, size);
        break;
    }
}

// The flattened scalar view of a type, copied out so the lane loops read it
// from the stack.
struct Scalars {
    SpirvTypeKind kind = SpirvTypeKind::Unknown;
    uint32_t width = 32;
    uint32_t count = 1;
    uint32_t size = 4;
    bool is_signed = false;
};

Scalars scalars_of(const SpirvModule &module, uint32_t type_id) {
    const SpirvType *info = module.type(type_id);
    if (!info)
        return {};
    return { info->scalar_kind, info->scalar_width, info->scalar_count, info->scalar_size, info->scalar_signed };
}

inline float load_float(const uint8_t *data, const Scalars &info, uint32_t index) {
    const uint8_t *element = data + static_cast<size_t>(index) * info.size;
    if (info.width == 32) {
        float value;
        std::memcpy(&value, element, sizeof(value));
        return value;
    }
    if (info.width == 16) {
        uint16_t bits;
        std::memcpy(&bits, element, sizeof(bits));
        return half_to_float(bits);
    }
    double value;
    std::memcpy(&value, element, sizeof(value));
    return static_cast<float>(value);
}

inline void store_float(uint8_t *data, const Scalars &info, uint32_t index, float value) {
    uint8_t *element = data + static_cast<size_t>(index) * info.size;
    if (info.width == 32) {
        std::memcpy(element, &value, sizeof(value));
    } else if (info.width == 16) {
        const uint16_t bits = float_to_half(value);
        std::memcpy(element, &bits, sizeof(bits));
    } else {
        const double wide = value;
        std::memcpy(element, &wide, sizeof(wide));
    }
}

inline uint32_t load_uint(const uint8_t *data, const Scalars &info, uint32_t index) {
    const uint8_t *element = data + static_cast<size_t>(index) * info.size;
    if (info.width == 32 || info.width > 32) {
        uint32_t bits;
        std::memcpy(&bits, element, sizeof(bits));
        return bits;
    }
    if (info.width == 16) {
        uint16_t bits;
        std::memcpy(&bits, element, sizeof(bits));
        return bits;
    }
    return *element;
}

inline int32_t load_int(const uint8_t *data, const Scalars &info, uint32_t index) {
    const uint32_t raw = load_uint(data, info, index);
    if (info.width == 16)
        return static_cast<int16_t>(raw);
    if (info.width == 8)
        return static_cast<int8_t>(raw);
    return static_cast<int32_t>(raw);
}

inline void store_uint(uint8_t *data, const Scalars &info, uint32_t index, uint32_t value) {
    uint8_t *element = data + static_cast<size_t>(index) * info.size;
    if (info.width == 32 || info.width > 32) {
        std::memcpy(element, &value, sizeof(value));
    } else if (info.width == 16) {
        const uint16_t bits = static_cast<uint16_t>(value);
        std::memcpy(element, &bits, sizeof(bits));
    } else {
        *element = static_cast<uint8_t>(value);
    }
}

// Comparisons and the reductions produce bools, stored as one 32-bit word each.
constexpr Scalars bool_scalars(uint32_t count) {
    return { SpirvTypeKind::Bool, 32, count, 4, false };
}

} // namespace

bool SpirvModule::parse(const std::vector<uint32_t> &code_words) {
    if (code_words.size() < 5 || code_words[0] != SPIRV_MAGIC) {
        LOG_ERROR("Software renderer: the shader binary is not SPIR-V");
        return false;
    }

    words = code_words;
    id_bound = code_words[3];
    type_table.assign(id_bound, nullptr);

    const auto add_type = [&](uint32_t id, const SpirvType &info) {
        if (id >= id_bound)
            return;
        SpirvType &slot = types[id];
        slot = info;
        type_table[id] = &slot;
    };

    // Decorations are gathered first because a type or a variable can be
    // decorated before or after the instruction that declares it.
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>> decorations;
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>>> member_decorations;
    std::unordered_map<uint32_t, std::string> names;

    uint32_t position = 5;
    while (position < words.size()) {
        const uint32_t header = words[position];
        const uint32_t count = word_count_of(header);
        if (count == 0 || position + count > words.size())
            break;

        const uint32_t op = opcode_of(header);
        // words.data() rather than &words[...]: an instruction with no operands
        // that ends the module puts this one past the end, which is a valid
        // pointer to form but not a valid element to reference.
        const uint32_t *operands = words.data() + position + 1;

        switch (op) {
        case spv::OpDecorate:
            if (count >= 3)
                decorations[operands[0]][operands[1]] = (count >= 4) ? operands[2] : 1;
            break;
        case spv::OpMemberDecorate:
            if (count >= 4)
                member_decorations[operands[0]][operands[1]][operands[2]] = (count >= 5) ? operands[3] : 1;
            break;
        case spv::OpName:
            if (count >= 3)
                names[operands[0]] = read_string(words, position + 2, count - 2);
            break;
        default:
            break;
        }

        position += count;
    }

    const auto decoration_of = [&](uint32_t id, uint32_t decoration) -> uint32_t {
        const auto target = decorations.find(id);
        if (target == decorations.end())
            return SPIRV_NO_VALUE;
        const auto value = target->second.find(decoration);
        return (value == target->second.end()) ? SPIRV_NO_VALUE : value->second;
    };

    uint32_t current_function = 0;

    position = 5;
    while (position < words.size()) {
        const uint32_t header = words[position];
        const uint32_t count = word_count_of(header);
        if (count == 0 || position + count > words.size())
            break;

        const uint32_t op = opcode_of(header);
        const uint32_t *operands = words.data() + position + 1;

        switch (op) {
        case spv::OpEntryPoint:
            entry_point = operands[1];
            is_fragment = (operands[0] == spv::ExecutionModelFragment);
            break;

        case spv::OpTypeVoid: {
            SpirvType info;
            info.kind = SpirvTypeKind::Void;
            add_type(operands[0], info);
            break;
        }
        case spv::OpTypeBool: {
            SpirvType info;
            info.kind = SpirvTypeKind::Bool;
            info.width = 32;
            info.byte_size = 4;
            add_type(operands[0], info);
            break;
        }
        case spv::OpTypeInt: {
            SpirvType info;
            info.kind = SpirvTypeKind::Int;
            info.width = operands[1];
            info.is_signed = operands[2] != 0;
            info.byte_size = std::max(1u, info.width / 8);
            add_type(operands[0], info);
            break;
        }
        case spv::OpTypeFloat: {
            SpirvType info;
            info.kind = SpirvTypeKind::Float;
            info.width = operands[1];
            info.byte_size = std::max(1u, info.width / 8);
            add_type(operands[0], info);
            break;
        }
        case spv::OpTypeVector: {
            SpirvType info;
            info.kind = SpirvTypeKind::Vector;
            info.element_type = operands[1];
            info.element_count = operands[2];
            info.byte_size = byte_size_of(info.element_type) * info.element_count;
            add_type(operands[0], info);
            break;
        }
        case spv::OpTypeMatrix: {
            SpirvType info;
            info.kind = SpirvTypeKind::Matrix;
            info.element_type = operands[1];
            info.element_count = operands[2];
            const uint32_t decorated_stride = decoration_of(operands[0], spv::DecorationMatrixStride);
            info.stride = (decorated_stride == SPIRV_NO_VALUE) ? byte_size_of(info.element_type) : decorated_stride;
            info.byte_size = info.stride * info.element_count;
            add_type(operands[0], info);
            break;
        }
        case spv::OpTypeImage: {
            SpirvType info;
            info.kind = SpirvTypeKind::Image;
            info.element_type = operands[1];
            info.dim = operands[2];
            info.arrayed = operands[4] != 0;
            info.byte_size = IMAGE_SIZE;
            add_type(operands[0], info);
            break;
        }
        case spv::OpTypeSampler: {
            SpirvType info;
            info.kind = SpirvTypeKind::Sampler;
            info.byte_size = IMAGE_SIZE;
            add_type(operands[0], info);
            break;
        }
        case spv::OpTypeSampledImage: {
            SpirvType info;
            info.kind = SpirvTypeKind::SampledImage;
            info.element_type = operands[1];
            info.byte_size = IMAGE_SIZE;
            add_type(operands[0], info);
            break;
        }
        case spv::OpTypeArray: {
            SpirvType info;
            info.kind = SpirvTypeKind::Array;
            info.element_type = operands[1];

            // The length is a constant, which SPIR-V requires to appear before
            // the array type that uses it.
            uint32_t length = 0;
            const auto constant = constants.find(operands[2]);
            if (constant != constants.end() && constant->second.size() >= sizeof(uint32_t))
                std::memcpy(&length, constant->second.data(), sizeof(uint32_t));
            info.element_count = length;

            const uint32_t decorated_stride = decoration_of(operands[0], spv::DecorationArrayStride);
            info.stride = (decorated_stride == SPIRV_NO_VALUE) ? byte_size_of(info.element_type) : decorated_stride;
            info.byte_size = info.stride * info.element_count;
            add_type(operands[0], info);
            break;
        }
        case spv::OpTypeRuntimeArray: {
            SpirvType info;
            info.kind = SpirvTypeKind::RuntimeArray;
            info.element_type = operands[1];
            const uint32_t decorated_stride = decoration_of(operands[0], spv::DecorationArrayStride);
            info.stride = (decorated_stride == SPIRV_NO_VALUE) ? byte_size_of(info.element_type) : decorated_stride;
            info.element_count = 0;
            info.byte_size = 0;
            add_type(operands[0], info);
            break;
        }
        case spv::OpTypeStruct: {
            SpirvType info;
            info.kind = SpirvTypeKind::Struct;

            const uint32_t member_count = count - 2;
            info.member_types.reserve(member_count);
            info.member_offsets.reserve(member_count);

            const auto members = member_decorations.find(operands[0]);
            uint32_t packed_offset = 0;
            uint32_t total = 0;

            for (uint32_t i = 0; i < member_count; i++) {
                const uint32_t member_type = operands[1 + i];
                uint32_t offset = SPIRV_NO_VALUE;

                if (members != member_decorations.end()) {
                    const auto member = members->second.find(i);
                    if (member != members->second.end()) {
                        const auto decoration = member->second.find(spv::DecorationOffset);
                        if (decoration != member->second.end())
                            offset = decoration->second;
                    }
                }

                if (offset == SPIRV_NO_VALUE)
                    offset = packed_offset;

                const uint32_t size = byte_size_of(member_type);
                info.member_types.push_back(member_type);
                info.member_offsets.push_back(offset);

                packed_offset = offset + size;
                total = std::max(total, offset + size);
            }

            info.byte_size = total;
            add_type(operands[0], info);
            break;
        }
        case spv::OpTypePointer: {
            SpirvType info;
            info.kind = SpirvTypeKind::Pointer;
            info.storage_class = operands[1];
            info.element_type = operands[2];
            info.byte_size = POINTER_SIZE;
            add_type(operands[0], info);
            break;
        }
        case spv::OpTypeFunction: {
            SpirvType info;
            info.kind = SpirvTypeKind::Function;
            info.element_type = operands[1];
            add_type(operands[0], info);
            break;
        }

        case spv::OpConstantTrue:
        case spv::OpSpecConstantTrue: {
            std::vector<uint8_t> value(4);
            const uint32_t one = 1;
            std::memcpy(value.data(), &one, sizeof(one));
            constants[operands[1]] = std::move(value);
            constant_types[operands[1]] = operands[0];
            break;
        }
        case spv::OpConstantFalse:
        case spv::OpSpecConstantFalse:
            constants[operands[1]] = std::vector<uint8_t>(4, 0);
            constant_types[operands[1]] = operands[0];
            break;

        case spv::OpConstant:
        case spv::OpSpecConstant: {
            const uint32_t literal_words = count - 3;
            const uint32_t size = std::max(byte_size_of(operands[0]), 1u);
            std::vector<uint8_t> value(std::max(size, literal_words * 4u), 0);
            std::memcpy(value.data(), &operands[2], static_cast<size_t>(literal_words) * sizeof(uint32_t));
            value.resize(size);
            constants[operands[1]] = std::move(value);
            constant_types[operands[1]] = operands[0];

            if (op == spv::OpSpecConstant) {
                const uint32_t spec_id = decoration_of(operands[1], spv::DecorationSpecId);
                if (spec_id != SPIRV_NO_VALUE)
                    spec_constant_ids[spec_id] = operands[1];
            }
            break;
        }
        case spv::OpConstantComposite:
        case spv::OpSpecConstantComposite: {
            const SpirvType *composite = type(operands[0]);
            std::vector<uint8_t> value(composite ? composite->byte_size : 0, 0);

            if (composite) {
                const uint32_t member_count = count - 3;
                for (uint32_t i = 0; i < member_count; i++) {
                    const auto member = constants.find(operands[2 + i]);
                    if (member == constants.end())
                        continue;

                    uint32_t offset = 0;
                    if (composite->kind == SpirvTypeKind::Struct && i < composite->member_offsets.size())
                        offset = composite->member_offsets[i];
                    else if (composite->kind == SpirvTypeKind::Array || composite->kind == SpirvTypeKind::Matrix)
                        offset = composite->stride * i;
                    else
                        offset = byte_size_of(composite->element_type) * i;

                    if (offset >= value.size())
                        continue;

                    const uint32_t copy = std::min<uint32_t>(static_cast<uint32_t>(member->second.size()),
                        static_cast<uint32_t>(value.size()) - offset);
                    std::memcpy(value.data() + offset, member->second.data(), copy);
                }
            }

            constants[operands[1]] = std::move(value);
            constant_types[operands[1]] = operands[0];
            break;
        }
        case spv::OpConstantNull:
        case spv::OpUndef:
            constants[operands[1]] = std::vector<uint8_t>(std::max(byte_size_of(operands[0]), 4u), 0);
            constant_types[operands[1]] = operands[0];
            break;

        case spv::OpVariable: {
            SpirvVariable variable;
            variable.id = operands[1];
            variable.storage_class = operands[2];
            variable.initializer = (count >= 5) ? operands[3] : 0;
            variable.is_local = current_function != 0;

            const SpirvType *pointer = type(operands[0]);
            variable.pointer_type_id = operands[0];
            variable.type_id = pointer ? pointer->element_type : 0;
            variable.byte_size = byte_size_of(variable.type_id);

            variable.location = decoration_of(variable.id, spv::DecorationLocation);
            variable.binding = decoration_of(variable.id, spv::DecorationBinding);
            variable.descriptor_set = decoration_of(variable.id, spv::DecorationDescriptorSet);
            variable.builtin = decoration_of(variable.id, spv::DecorationBuiltIn);

            const auto name = names.find(variable.id);
            if (name != names.end())
                variable.name = name->second;

            switch (variable.storage_class) {
            case spv::StorageClassInput:
                input_variables.push_back(variable.id);
                break;
            case spv::StorageClassOutput:
                output_variables.push_back(variable.id);
                break;
            case spv::StorageClassUniform:
            case spv::StorageClassStorageBuffer:
            case spv::StorageClassPushConstant:
                uniform_variables.push_back(variable.id);
                break;
            case spv::StorageClassUniformConstant:
                image_variables.push_back(variable.id);
                if (variable.descriptor_set == 1)
                    reads_destination = true;
                break;
            default:
                if (!variable.is_local)
                    private_variables.push_back(variable.id);
                break;
            }

            variables[variable.id] = std::move(variable);
            break;
        }

        case spv::OpFunction:
            current_function = operands[1];
            function_parameters[current_function] = {};
            break;

        case spv::OpFunctionEnd:
            current_function = 0;
            break;

        case spv::OpFunctionParameter:
            function_parameters[current_function].push_back(operands[1]);
            break;

        case spv::OpKill:
        case spv::OpTerminateInvocation:
            may_discard = true;
            break;

        default:
            break;
        }

        position += count;
    }

    if (entry_point == 0) {
        LOG_ERROR("Software renderer: the shader has no entry point");
        return false;
    }

    // Nothing the recompiler emits comes close to these, so anything that does
    // means the decode went wrong somewhere and the tables cannot be trusted.
    constexpr uint32_t MAX_REASONABLE_TYPE_SIZE = 1 << 20;
    for (const auto &[id, info] : types) {
        if (info.byte_size > MAX_REASONABLE_TYPE_SIZE) {
            LOG_ERROR("Software renderer: type {} decoded to {} bytes, refusing the module",
                id, info.byte_size);
            return false;
        }
    }

    for (const auto &[id, variable] : variables) {
        if (variable.byte_size > MAX_REASONABLE_TYPE_SIZE) {
            LOG_ERROR("Software renderer: variable {} decoded to {} bytes, refusing the module",
                id, variable.byte_size);
            return false;
        }
    }

    // Flatten vectors and matrices down to their scalars once, instead of
    // walking the element types for every arithmetic instruction.
    for (auto &[id, info] : types) {
        const SpirvType *current = &info;
        uint32_t scalar_count = 1;
        while (current && (current->kind == SpirvTypeKind::Vector || current->kind == SpirvTypeKind::Matrix)) {
            scalar_count *= current->element_count;
            current = type(current->element_type);
        }

        info.scalar_count = scalar_count;
        if (current) {
            info.scalar_kind = current->kind;
            info.scalar_width = (current->width == 0) ? 32 : current->width;
            info.scalar_signed = current->is_signed;
            info.scalar_size = std::max(1u, current->byte_size);
        }
    }

    return compile();
}

bool SpirvModule::compile() {
    const auto register_size_of = [&](uint32_t type_id) -> uint32_t {
        const SpirvType *info = type(type_id);
        if (!info)
            return 4;

        switch (info->kind) {
        case SpirvTypeKind::Void:
        case SpirvTypeKind::Function:
            return 0;
        case SpirvTypeKind::Pointer:
            return POINTER_SIZE;
        case SpirvTypeKind::Image:
        case SpirvTypeKind::Sampler:
        case SpirvTypeKind::SampledImage:
            return IMAGE_SIZE;
        default:
            return std::max(4u, align_up(info->byte_size, 4));
        }
    };

    const auto constant_uint = [&](uint32_t id, uint32_t &value) {
        const auto constant = constants.find(id);
        if (constant == constants.end() || constant->second.size() < sizeof(uint32_t))
            return false;
        std::memcpy(&value, constant->second.data(), sizeof(value));
        return true;
    };

    // Byte offset of a member picked by literal indices, as composite
    // extraction and insertion name them.
    const auto composite_offset = [&](uint32_t type_id, const uint32_t *indices, uint32_t index_count) {
        uint32_t offset = 0;
        for (uint32_t i = 0; i < index_count; i++) {
            const SpirvType *current = type(type_id);
            if (!current)
                break;

            const uint32_t index = indices[i];
            switch (current->kind) {
            case SpirvTypeKind::Struct:
                if (index < current->member_offsets.size()) {
                    offset += current->member_offsets[index];
                    type_id = current->member_types[index];
                }
                break;
            case SpirvTypeKind::Array:
            case SpirvTypeKind::Matrix:
                offset += current->stride * index;
                type_id = current->element_type;
                break;
            case SpirvTypeKind::Vector:
                offset += byte_size_of(current->element_type) * index;
                type_id = current->element_type;
                break;
            default:
                break;
            }
        }
        return offset;
    };

    value_type.assign(id_bound, 0);
    value_size.assign(id_bound, 0);
    static_pointers.assign(id_bound, {});
    materialized.assign(id_bound, 0);
    label_index.assign(id_bound, SPIRV_NO_VALUE);
    function_index.assign(id_bound, SPIRV_NO_VALUE);
    function_number.assign(id_bound, SPIRV_NO_VALUE);
    code.clear();
    callee_parameters.clear();

    for (const auto &[id, type_id] : constant_types) {
        if (id >= id_bound)
            continue;
        value_type[id] = type_id;
        value_size[id] = std::max(register_size_of(type_id),
            align_up(static_cast<uint32_t>(constants.at(id).size()), 4));
    }

    for (const auto &[id, variable] : variables) {
        if (id >= id_bound)
            continue;
        value_type[id] = variable.pointer_type_id;
        value_size[id] = POINTER_SIZE;
        static_pointers[id] = { id, 0, variable.type_id };
    }

    uint32_t current_function = 0;
    bool function_started = false;
    std::vector<uint8_t> returns_void(id_bound, 0);

    uint32_t position = 5;
    while (position < words.size()) {
        const uint32_t header = words[position];
        const uint32_t count = word_count_of(header);
        if (count == 0 || position + count > words.size())
            break;

        const uint32_t op = opcode_of(header);
        const uint32_t *operands = words.data() + position + 1;
        position += count;

        if (op == spv::OpFunction) {
            current_function = (operands[1] < id_bound) ? operands[1] : 0;
            function_started = false;
            if (current_function) {
                const SpirvType *return_type = type(operands[0]);
                returns_void[current_function] = return_type && return_type->kind == SpirvTypeKind::Void;
                function_number[current_function] = static_cast<uint32_t>(callee_parameters.size());
                const auto parameters = function_parameters.find(current_function);
                callee_parameters.push_back((parameters != function_parameters.end()) ? parameters->second
                                                                                      : std::vector<uint32_t>{});
            }
            continue;
        }

        if (current_function == 0)
            continue;

        switch (op) {
        case spv::OpFunctionParameter:
            if (operands[1] < id_bound) {
                value_type[operands[1]] = operands[0];
                value_size[operands[1]] = register_size_of(operands[0]);
            }
            continue;

        // Structure hints and debug info mean nothing to a lockstep batch that
        // splits whenever its lanes disagree.
        case spv::OpLine:
        case spv::OpNoLine:
        case spv::OpNop:
        case spv::OpSelectionMerge:
        case spv::OpLoopMerge:
        case spv::OpUndef:
            continue;

        default:
            break;
        }

        if (!function_started) {
            function_index[current_function] = static_cast<uint32_t>(code.size());
            function_started = true;
        }

        if (op == spv::OpLabel && operands[0] < id_bound)
            label_index[operands[0]] = static_cast<uint32_t>(code.size());

        SpirvInstruction instruction;
        instruction.op = static_cast<uint16_t>(op);
        instruction.word_count = static_cast<uint16_t>(count);
        instruction.operands = position - count + 1;

        bool has_result = false;
        bool has_type = false;
        result_and_type(op, has_result, has_type);
        if (has_result && has_type && count >= 3 && operands[1] < id_bound) {
            value_type[operands[1]] = operands[0];
            value_size[operands[1]] = register_size_of(operands[0]);
        }

        switch (op) {
        case spv::OpAccessChain:
        case spv::OpInBoundsAccessChain: {
            if (operands[1] >= id_bound || operands[2] >= id_bound)
                break;

            SpirvStaticPointer pointer = static_pointers[operands[2]];
            bool resolved = pointer.valid();
            for (uint32_t i = 0; resolved && i + 4 < count; i++) {
                uint32_t index = 0;
                const SpirvType *current = type(pointer.type_id);
                if (!constant_uint(operands[3 + i], index) || !current) {
                    resolved = false;
                    break;
                }

                switch (current->kind) {
                case SpirvTypeKind::Struct:
                    if (index >= current->member_offsets.size()) {
                        resolved = false;
                        break;
                    }
                    pointer.offset += current->member_offsets[index];
                    pointer.type_id = current->member_types[index];
                    break;
                case SpirvTypeKind::Array:
                case SpirvTypeKind::RuntimeArray:
                case SpirvTypeKind::Matrix:
                    pointer.offset += current->stride * index;
                    pointer.type_id = current->element_type;
                    break;
                case SpirvTypeKind::Vector:
                    pointer.offset += byte_size_of(current->element_type) * index;
                    pointer.type_id = current->element_type;
                    break;
                default:
                    resolved = false;
                    break;
                }
            }

            if (resolved)
                static_pointers[operands[1]] = pointer;
            break;
        }

        case spv::OpCompositeExtract:
            if (operands[2] < id_bound)
                instruction.aux = composite_offset(value_type[operands[2]], operands + 3, count - 4);
            break;

        case spv::OpCompositeInsert:
            instruction.aux = composite_offset(operands[0], operands + 4, count - 5);
            break;

        // A vector access at a constant index is a composite access at a fixed offset.
        case spv::OpVectorExtractDynamic:
        case spv::OpVectorInsertDynamic: {
            const bool extract = op == spv::OpVectorExtractDynamic;
            const SpirvType *vector_type = (operands[2] < id_bound) ? type(value_type[operands[2]]) : nullptr;
            uint32_t index = 0;
            if (vector_type && constant_uint(operands[extract ? 3 : 4], index) && index < vector_type->scalar_count) {
                instruction.aux = index * vector_type->scalar_size;
                instruction.op = extract ? static_cast<uint16_t>(spv::OpCompositeExtract) : SPIRV_OP_INSERT_AT;
            }
            break;
        }

        default:
            break;
        }

        code.push_back(instruction);

        if (op == spv::OpFunctionEnd)
            current_function = 0;
    }

    // The recompiler reloads registers it has just stored and rebuilds vectors
    // only to pick them apart again. A load of what the same block last stored
    // or loaded, and an extract from a construct, shuffle or insert, are
    // replaced with the value already in a register; dead code removal then
    // takes the instructions that computed them.
    {
        std::vector<uint32_t> owner(code.size(), 0);
        for (uint32_t id = 0; id < id_bound; id++) {
            if (function_index[id] == SPIRV_NO_VALUE)
                continue;
            for (uint32_t pc = function_index[id]; pc < code.size(); pc++) {
                owner[pc] = id;
                if (code[pc].op == spv::OpFunctionEnd)
                    break;
            }
        }

        std::vector<uint32_t> chain_base(id_bound, 0);
        for (const SpirvInstruction &instruction : code) {
            if (instruction.op != spv::OpAccessChain && instruction.op != spv::OpInBoundsAccessChain)
                continue;
            const uint32_t *operands = words.data() + instruction.operands;
            if (operands[1] < id_bound)
                chain_base[operands[1]] = operands[2];
        }
        const auto root_variable = [&](uint32_t pointer) -> uint32_t {
            for (uint32_t depth = 0; depth < 16 && pointer && pointer < id_bound; depth++) {
                if (static_pointers[pointer].valid())
                    return static_pointers[pointer].variable;
                pointer = chain_base[pointer];
            }
            return 0;
        };

        // The variables a call may store to, so the values known across it can be kept.
        std::vector<std::vector<uint32_t>> writes(id_bound);
        std::vector<uint8_t> writes_anything(id_bound, 0);
        std::vector<std::vector<uint32_t>> callees(id_bound);
        for (uint32_t pc = 0; pc < code.size(); pc++) {
            const uint32_t function = owner[pc];
            const uint32_t *operands = words.data() + code[pc].operands;
            switch (code[pc].op) {
            case spv::OpStore: {
                const uint32_t variable = root_variable(operands[0]);
                if (variable)
                    writes[function].push_back(variable);
                else
                    writes_anything[function] = 1;
                break;
            }
            case spv::OpCopyMemory:
                writes_anything[function] = 1;
                break;
            case spv::OpFunctionCall:
                if (operands[2] < id_bound)
                    callees[function].push_back(operands[2]);
                else
                    writes_anything[function] = 1;
                break;
            default:
                break;
            }
        }
        std::vector<uint8_t> visit_state(id_bound, 0);
        const auto gather_writes = [&](auto &self, uint32_t function) -> void {
            if (visit_state[function])
                return;
            visit_state[function] = 1;
            for (const uint32_t callee : callees[function]) {
                self(self, callee);
                if (visit_state[callee] == 1 || writes_anything[callee])
                    writes_anything[function] = 1;
                else
                    writes[function].insert(writes[function].end(), writes[callee].begin(), writes[callee].end());
            }
            visit_state[function] = 2;
        };
        for (uint32_t id = 0; id < id_bound; id++) {
            if (function_index[id] != SPIRV_NO_VALUE)
                gather_writes(gather_writes, id);
        }

        std::vector<uint32_t> alias(id_bound, 0);
        const auto resolve = [&](uint32_t id) {
            while (id < id_bound && alias[id])
                id = alias[id];
            return id;
        };
        std::vector<uint32_t> producer_of(id_bound, SPIRV_NO_VALUE);

        struct Known {
            uint32_t variable;
            uint32_t offset;
            uint32_t size;
            uint32_t value;
        };
        std::vector<Known> known;
        const auto forget = [&](uint32_t variable, uint32_t offset, uint32_t size) {
            std::erase_if(known, [&](const Known &entry) {
                return entry.variable == variable && entry.offset < offset + size && offset < entry.offset + entry.size;
            });
        };

        for (uint32_t pc = 0; pc < code.size(); pc++) {
            SpirvInstruction &instruction = code[pc];
            uint32_t *operands = words.data() + instruction.operands;

            for_each_value_operand(instruction.op, instruction.word_count, [&](uint32_t position) {
                operands[position] = resolve(operands[position]);
            });

            bool has_result = false;
            bool has_type = false;
            result_and_type(instruction.op, has_result, has_type);
            if (has_result && has_type && instruction.word_count >= 3 && operands[1] < id_bound)
                producer_of[operands[1]] = pc;

            switch (instruction.op) {
            case spv::OpLabel:
            case spv::OpCopyMemory:
                known.clear();
                break;

            case spv::OpVariable:
                forget(operands[1], 0, UINT32_MAX / 2);
                break;

            case spv::OpFunctionCall:
                if (operands[2] >= id_bound || writes_anything[operands[2]]) {
                    known.clear();
                } else {
                    for (const uint32_t variable : writes[operands[2]])
                        forget(variable, 0, UINT32_MAX / 2);
                }
                break;

            case spv::OpLoad: {
                const SpirvStaticPointer &fixed = static_pointers[operands[2]];
                if (!fixed.valid() || operands[1] >= id_bound)
                    break;
                const uint32_t size = byte_size_of(fixed.type_id);
                const auto entry = std::find_if(known.begin(), known.end(), [&](const Known &candidate) {
                    return candidate.variable == fixed.variable && candidate.offset == fixed.offset && candidate.size == size
                        && value_type[candidate.value] == operands[0];
                });
                if (entry != known.end())
                    alias[operands[1]] = entry->value;
                else
                    known.push_back({ fixed.variable, fixed.offset, size, operands[1] });
                break;
            }

            case spv::OpStore: {
                const SpirvStaticPointer &fixed = static_pointers[operands[0]];
                if (!fixed.valid()) {
                    const uint32_t variable = root_variable(operands[0]);
                    if (variable)
                        forget(variable, 0, UINT32_MAX / 2);
                    else
                        known.clear();
                    break;
                }
                const uint32_t size = byte_size_of(fixed.type_id);
                forget(fixed.variable, fixed.offset, size);
                // A store into a bound buffer is dropped at run time, so it must not be read back either.
                const auto variable = variables.find(fixed.variable);
                const bool bound = variable != variables.end() && is_bound_buffer(variable->second.storage_class);
                if (!bound && operands[1] < id_bound && value_type[operands[1]] == fixed.type_id)
                    known.push_back({ fixed.variable, fixed.offset, size, operands[1] });
                break;
            }

            case spv::OpCompositeExtract: {
                if (operands[1] >= id_bound)
                    break;
                const uint32_t size = byte_size_of(operands[0]);
                uint32_t source = operands[2];
                uint32_t offset = instruction.aux;
                uint32_t replacement = 0;

                for (uint32_t depth = 0; depth < 32 && !replacement; depth++) {
                    if (source >= id_bound || producer_of[source] == SPIRV_NO_VALUE)
                        break;
                    const SpirvInstruction &producer = code[producer_of[source]];
                    const uint32_t *inputs = words.data() + producer.operands;
                    const uint32_t input_count = producer.word_count - 1u;
                    bool moved = false;

                    switch (producer.op) {
                    case spv::OpCompositeConstruct: {
                        uint32_t start = 0;
                        for (uint32_t k = 2; k < input_count; k++) {
                            const uint32_t part = inputs[k];
                            const uint32_t part_size = byte_size_of(value_type[part]);
                            if (offset >= start && offset + size <= start + part_size) {
                                if (offset == start && size == part_size && value_type[part] == operands[0]) {
                                    replacement = part;
                                } else {
                                    source = part;
                                    offset -= start;
                                    moved = true;
                                }
                                break;
                            }
                            start += part_size;
                        }
                        break;
                    }
                    case spv::OpVectorShuffle: {
                        const SpirvType *result_type = type(inputs[0]);
                        const SpirvType *first_type = type(value_type[inputs[2]]);
                        const uint32_t element = result_type ? result_type->scalar_size : 0;
                        if (!element || !first_type || size != element || offset % element || 4 + offset / element >= input_count)
                            break;
                        const uint32_t selector = inputs[4 + offset / element];
                        if (selector == 0xFFFFFFFFu)
                            break;
                        const bool from_first = selector < first_type->scalar_count;
                        source = from_first ? inputs[2] : inputs[3];
                        offset = (from_first ? selector : selector - first_type->scalar_count) * element;
                        moved = true;
                        break;
                    }
                    case spv::OpCompositeInsert:
                    case SPIRV_OP_INSERT_AT: {
                        const bool insert_at = producer.op == SPIRV_OP_INSERT_AT;
                        const uint32_t object = insert_at ? inputs[3] : inputs[2];
                        const uint32_t composite = insert_at ? inputs[2] : inputs[3];
                        const uint32_t object_size = byte_size_of(value_type[object]);
                        if (producer.aux == offset && object_size == size && value_type[object] == operands[0]) {
                            replacement = object;
                        } else if (offset + size <= producer.aux || producer.aux + object_size <= offset) {
                            source = composite;
                            moved = true;
                        }
                        break;
                    }
                    case spv::OpCompositeExtract:
                        source = inputs[2];
                        offset += producer.aux;
                        moved = true;
                        break;
                    case spv::OpCopyObject:
                        source = inputs[2];
                        moved = true;
                        break;
                    default:
                        break;
                    }

                    if (!moved)
                        break;
                }

                if (replacement) {
                    alias[operands[1]] = resolve(replacement);
                } else if (source != operands[2]) {
                    operands[2] = source;
                    instruction.aux = offset;
                }
                break;
            }

            default:
                break;
            }
        }
    }

    const auto is_pointer_value = [&](uint32_t id) {
        if (id >= id_bound)
            return false;
        const SpirvType *info = type(value_type[id]);
        return info && info->kind == SpirvTypeKind::Pointer;
    };

    // Pointers read as values by anything but a load, a store or an access
    // chain need their per-lane value computed.
    for (const SpirvInstruction &instruction : code) {
        const uint32_t *operands = words.data() + instruction.operands;
        const uint32_t count = instruction.word_count;

        switch (instruction.op) {
        case spv::OpFunctionCall:
            for (uint32_t i = 3; i < count - 1u; i++) {
                if (is_pointer_value(operands[i]))
                    materialized[operands[i]] = 1;
            }
            break;
        case spv::OpPhi:
            for (uint32_t i = 2; i + 1 < count - 1u; i += 2) {
                if (is_pointer_value(operands[i]))
                    materialized[operands[i]] = 1;
            }
            break;
        case spv::OpSelect:
            for (uint32_t i = 3; i < 5 && i < count - 1u; i++) {
                if (is_pointer_value(operands[i]))
                    materialized[operands[i]] = 1;
            }
            break;
        case spv::OpCopyObject:
            if (is_pointer_value(operands[2]))
                materialized[operands[2]] = 1;
            break;
        case spv::OpReturnValue:
            if (is_pointer_value(operands[0]))
                materialized[operands[0]] = 1;
            break;
        default:
            break;
        }
    }

    // Dead code. The recompiler leaves plenty of loads and extracts nothing
    // reads; with no side effects they can go. Any operand word that happens to
    // equal an id counts as a use, which can only keep something alive.
    const auto result_of = [&](const SpirvInstruction &instruction) -> uint32_t {
        bool has_result = false;
        bool has_type = false;
        result_and_type(instruction.op, has_result, has_type);
        if (!has_result || instruction.word_count < (has_type ? 3u : 2u))
            return 0;
        const uint32_t id = words[instruction.operands + (has_type ? 1 : 0)];
        return (id < id_bound) ? id : 0;
    };
    const auto for_each_operand_id = [&](const SpirvInstruction &instruction, auto &&function) {
        bool has_result = false;
        bool has_type = false;
        result_and_type(instruction.op, has_result, has_type);
        const uint32_t first = (has_type ? 1u : 0u) + (has_result ? 1u : 0u);
        for (uint32_t i = first; i + 1 < instruction.word_count; i++) {
            const uint32_t word = words[instruction.operands + i];
            if (word < id_bound)
                function(word);
        }
    };
    const auto is_pure = [](uint32_t op) {
        switch (op) {
        case spv::OpLoad:
        case spv::OpAccessChain:
        case spv::OpInBoundsAccessChain:
        case spv::OpCompositeExtract:
        case spv::OpCompositeInsert:
        case spv::OpCompositeConstruct:
        case spv::OpVectorShuffle:
        case spv::OpVectorExtractDynamic:
        case spv::OpVectorInsertDynamic:
        case spv::OpSelect:
        case spv::OpCopyObject:
        case spv::OpBitcast:
        case spv::OpSampledImage:
        case spv::OpImage:
        case spv::OpImageSampleImplicitLod:
        case spv::OpImageSampleExplicitLod:
        case spv::OpImageSampleProjImplicitLod:
        case spv::OpImageSampleProjExplicitLod:
        case spv::OpImageFetch:
        case spv::OpImageRead:
        case spv::OpPhi:
        case spv::OpExtInst:
        case spv::OpFNegate:
        case spv::OpSNegate:
        case spv::OpNot:
        case spv::OpFAdd:
        case spv::OpFSub:
        case spv::OpFMul:
        case spv::OpFDiv:
        case spv::OpFMod:
        case spv::OpFRem:
        case spv::OpIAdd:
        case spv::OpISub:
        case spv::OpIMul:
        case spv::OpSDiv:
        case spv::OpUDiv:
        case spv::OpSMod:
        case spv::OpSRem:
        case spv::OpUMod:
        case spv::OpShiftLeftLogical:
        case spv::OpShiftRightLogical:
        case spv::OpShiftRightArithmetic:
        case spv::OpBitwiseAnd:
        case spv::OpBitwiseOr:
        case spv::OpBitwiseXor:
        case spv::OpLogicalAnd:
        case spv::OpLogicalOr:
        case spv::OpLogicalNot:
        case spv::OpLogicalEqual:
        case spv::OpLogicalNotEqual:
        case spv::OpFOrdEqual:
        case spv::OpFUnordEqual:
        case spv::OpFOrdNotEqual:
        case spv::OpFUnordNotEqual:
        case spv::OpFOrdLessThan:
        case spv::OpFUnordLessThan:
        case spv::OpFOrdGreaterThan:
        case spv::OpFUnordGreaterThan:
        case spv::OpFOrdLessThanEqual:
        case spv::OpFUnordLessThanEqual:
        case spv::OpFOrdGreaterThanEqual:
        case spv::OpFUnordGreaterThanEqual:
        case spv::OpIEqual:
        case spv::OpINotEqual:
        case spv::OpSLessThan:
        case spv::OpULessThan:
        case spv::OpSGreaterThan:
        case spv::OpUGreaterThan:
        case spv::OpSLessThanEqual:
        case spv::OpULessThanEqual:
        case spv::OpSGreaterThanEqual:
        case spv::OpUGreaterThanEqual:
        case spv::OpConvertFToS:
        case spv::OpConvertFToU:
        case spv::OpConvertSToF:
        case spv::OpConvertUToF:
        case spv::OpFConvert:
        case spv::OpSConvert:
        case spv::OpUConvert:
        case spv::OpIsNan:
        case spv::OpIsInf:
        case spv::OpAny:
        case spv::OpAll:
        case spv::OpDot:
        case spv::OpVectorTimesScalar:
        case spv::OpMatrixTimesVector:
        case spv::OpVectorTimesMatrix:
        case spv::OpMatrixTimesMatrix:
        case spv::OpMatrixTimesScalar:
        case spv::OpTranspose:
        case spv::OpBitFieldInsert:
        case spv::OpBitFieldSExtract:
        case spv::OpBitFieldUExtract:
        case spv::OpBitReverse:
        case spv::OpBitCount:
        case SPIRV_OP_INSERT_AT:
            return true;
        default:
            return false;
        }
    };

    std::vector<uint32_t> use_count(id_bound, 0);
    std::vector<uint32_t> definition(id_bound, SPIRV_NO_VALUE);
    for (uint32_t i = 0; i < code.size(); i++) {
        for_each_operand_id(code[i], [&](uint32_t id) { use_count[id]++; });
        const uint32_t result = result_of(code[i]);
        if (result)
            definition[result] = i;
    }

    std::vector<uint8_t> dead(code.size(), 0);
    std::vector<uint32_t> worklist;
    for (uint32_t i = 0; i < code.size(); i++) {
        const uint32_t result = result_of(code[i]);
        if (result && is_pure(code[i].op) && use_count[result] == 0)
            worklist.push_back(i);
    }
    while (!worklist.empty()) {
        const uint32_t index = worklist.back();
        worklist.pop_back();
        if (dead[index])
            continue;
        dead[index] = 1;
        for_each_operand_id(code[index], [&](uint32_t id) {
            if (use_count[id] > 0 && --use_count[id] == 0) {
                const uint32_t producer = definition[id];
                if (producer != SPIRV_NO_VALUE && !dead[producer] && is_pure(code[producer].op))
                    worklist.push_back(producer);
            }
        });
    }

    // Compact what survives, dropping the access chains loads and stores
    // resolve on their own, and turning constant-index vector accesses into
    // fixed offsets.
    std::vector<uint32_t> new_index(code.size() + 1, SPIRV_NO_VALUE);
    std::vector<SpirvInstruction> compacted;
    compacted.reserve(code.size());
    std::vector<uint8_t> dead_value(id_bound, 0);

    for (uint32_t i = 0; i < code.size(); i++) {
        SpirvInstruction instruction = code[i];
        const uint32_t result = result_of(instruction);

        if (dead[i]) {
            if (result)
                dead_value[result] = 1;
            continue;
        }

        if ((instruction.op == spv::OpAccessChain || instruction.op == spv::OpInBoundsAccessChain) && result
            && static_pointers[result].valid() && !materialized[result])
            continue;

        if (result && value_type[result])
            instruction.size = byte_size_of(value_type[result]);

        new_index[i] = static_cast<uint32_t>(compacted.size());
        compacted.push_back(instruction);
    }
    new_index[code.size()] = static_cast<uint32_t>(compacted.size());
    code = std::move(compacted);

    const auto remap = [&](std::vector<uint32_t> &table) {
        for (uint32_t &index : table) {
            if (index != SPIRV_NO_VALUE)
                index = (index < new_index.size()) ? new_index[index] : SPIRV_NO_VALUE;
        }
    };
    remap(label_index);
    remap(function_index);

    // Branch targets and callees can come later in the module than the
    // instruction naming them, so they are resolved once every index is known.
    const auto index_of = [&](const std::vector<uint32_t> &table, uint32_t id) {
        return (id < table.size()) ? table[id] : SPIRV_NO_VALUE;
    };

    for (SpirvInstruction &instruction : code) {
        const uint32_t *operands = words.data() + instruction.operands;
        switch (instruction.op) {
        case spv::OpBranch:
            instruction.target = index_of(label_index, operands[0]);
            break;
        case spv::OpBranchConditional:
            instruction.target = index_of(label_index, operands[1]);
            instruction.second_target = index_of(label_index, operands[2]);
            break;
        case spv::OpFunctionCall:
            instruction.target = index_of(function_index, operands[2]);
            instruction.aux = index_of(function_number, operands[2]);
            break;
        default:
            break;
        }
    }

    find_memoizable_functions(returns_void);

    // Registers. Constants come first and are stored once; every other value
    // gets one slot per lane, except pointers that were resolved statically and
    // are never needed as values.
    register_offset.assign(id_bound, SPIRV_NO_VALUE);
    register_stride.assign(id_bound, 0);

    uint32_t cursor = 0;
    for (const auto &[id, bytes] : constants) {
        if (id >= id_bound)
            continue;
        register_offset[id] = cursor;
        cursor += align_up(std::max(value_size[id], 4u), 16);
    }

    constant_registers.assign(cursor, 0);
    for (const auto &[id, bytes] : constants) {
        if (id < id_bound && !bytes.empty())
            std::memcpy(constant_registers.data() + register_offset[id], bytes.data(), bytes.size());
    }

    const uint32_t null_register = cursor;
    cursor += NULL_REGISTER_SIZE;

    for (uint32_t id = 0; id < id_bound; id++) {
        if (register_offset[id] != SPIRV_NO_VALUE || value_size[id] == 0 || dead_value[id])
            continue;
        if (static_pointers[id].valid() && !materialized[id])
            continue;

        register_offset[id] = cursor;
        register_stride[id] = value_size[id];
        cursor += align_up(value_size[id] * SPIRV_MAX_LANES, 16);
    }

    // Whatever is left reads as zero instead of pointing into the unknown.
    for (uint32_t id = 0; id < id_bound; id++) {
        if (register_offset[id] == SPIRV_NO_VALUE)
            register_offset[id] = null_register;
    }

    register_bytes = cursor;

    // Storage of the variables the interpreter owns; bound buffers live with
    // the caller.
    variable_offset.assign(id_bound, SPIRV_NO_VALUE);
    variable_stride.assign(id_bound, 0);

    cursor = 0;
    for (const auto &[id, variable] : variables) {
        if (id >= id_bound || is_bound_buffer(variable.storage_class))
            continue;

        const uint32_t stride = align_up(std::max(variable.byte_size, 4u), 16);
        variable_offset[id] = cursor;
        variable_stride[id] = stride;
        cursor += stride * SPIRV_MAX_LANES;
    }
    variable_bytes = cursor;

    if (entry_point >= id_bound || function_index[entry_point] == SPIRV_NO_VALUE) {
        LOG_ERROR("Software renderer: the shader entry point has no body");
        return false;
    }

    return true;
}

void SpirvModule::find_memoizable_functions(const std::vector<uint8_t> &returns_void) {
    const uint32_t function_count = static_cast<uint32_t>(callee_parameters.size());
    memoizable.assign(function_count, 0);
    memo_state.assign(function_count, {});

    // Access chains still in the code are the dynamic ones; following their
    // bases leads back to the variable a load or store lands in.
    std::vector<uint32_t> chain_base(id_bound, 0);
    for (const SpirvInstruction &instruction : code) {
        if (instruction.op != spv::OpAccessChain && instruction.op != spv::OpInBoundsAccessChain)
            continue;
        const uint32_t *operands = words.data() + instruction.operands;
        if (operands[1] < id_bound)
            chain_base[operands[1]] = operands[2];
    }
    const auto root_variable = [&](uint32_t pointer) -> uint32_t {
        for (uint32_t depth = 0; depth < 16 && pointer && pointer < id_bound; depth++) {
            if (static_pointers[pointer].valid())
                return static_pointers[pointer].variable;
            pointer = chain_base[pointer];
        }
        return 0;
    };

    struct Summary {
        // Reads or writes something that differs between lanes, or that the
        // analysis cannot follow.
        bool per_lane = false;
        std::vector<uint32_t> callees;
        std::vector<Range> ranges;
    };
    std::vector<Summary> summaries(function_count);
    std::vector<uint32_t> function_of_number(function_count, 0);

    for (uint32_t id = 0; id < id_bound; id++) {
        const uint32_t number = function_number[id];
        if (number >= function_count || function_index[id] == SPIRV_NO_VALUE)
            continue;
        function_of_number[number] = id;

        Summary &summary = summaries[number];
        for (uint32_t pc = function_index[id]; pc < code.size() && code[pc].op != spv::OpFunctionEnd; pc++) {
            const SpirvInstruction &instruction = code[pc];
            const uint32_t *operands = words.data() + instruction.operands;

            switch (instruction.op) {
            case spv::OpLoad:
            case spv::OpStore: {
                const uint32_t pointer = operands[(instruction.op == spv::OpLoad) ? 2 : 0];
                const SpirvStaticPointer &fixed = static_pointers[pointer];
                const auto variable = variables.find(fixed.valid() ? fixed.variable : root_variable(pointer));
                if (variable == variables.end()) {
                    summary.per_lane = true;
                    break;
                }
                // Bound buffers are the same for every lane, and a function's own locals start afresh.
                if (is_bound_buffer(variable->second.storage_class) || variable->second.is_local)
                    break;
                // Outputs are lane storage like private variables; inputs are what makes lanes differ.
                if (variable->second.storage_class != spv::StorageClassPrivate
                    && variable->second.storage_class != spv::StorageClassOutput) {
                    summary.per_lane = true;
                    break;
                }
                if (fixed.valid())
                    summary.ranges.push_back({ variable->first, fixed.offset, byte_size_of(fixed.type_id) });
                else
                    summary.ranges.push_back({ variable->first, 0, std::max(variable->second.byte_size, 4u) });
                break;
            }
            case spv::OpFunctionCall:
                if (instruction.aux < function_count)
                    summary.callees.push_back(instruction.aux);
                else
                    summary.per_lane = true;
                break;
            case spv::OpCopyMemory:
            case spv::OpImageSampleImplicitLod:
            case spv::OpImageSampleExplicitLod:
            case spv::OpImageSampleProjImplicitLod:
            case spv::OpImageSampleProjExplicitLod:
            case spv::OpImageFetch:
            case spv::OpImageRead:
            case spv::OpKill:
            case spv::OpTerminateInvocation:
                summary.per_lane = true;
                break;
            default:
                break;
            }
        }
    }

    // Fold every callee into its callers; the recompiler never recurses, and
    // a module that does is simply not memoized.
    std::vector<uint8_t> visit_state(function_count, 0);
    const auto visit = [&](auto &self, uint32_t number) -> void {
        if (visit_state[number])
            return;
        visit_state[number] = 1;
        Summary &summary = summaries[number];
        for (const uint32_t callee : summary.callees) {
            self(self, callee);
            if (visit_state[callee] == 1 || summaries[callee].per_lane) {
                summary.per_lane = true;
                continue;
            }
            summary.ranges.insert(summary.ranges.end(), summaries[callee].ranges.begin(), summaries[callee].ranges.end());
        }
        visit_state[number] = 2;
    };

    for (uint32_t number = 0; number < function_count; number++) {
        visit(visit, number);

        Summary &summary = summaries[number];
        const uint32_t function = function_of_number[number];
        if (summary.per_lane || !callee_parameters[number].empty() || !function || !returns_void[function])
            continue;

        // Overlapping ranges of a variable merge into one.
        std::sort(summary.ranges.begin(), summary.ranges.end(), [](const Range &a, const Range &b) {
            return (a.variable != b.variable) ? (a.variable < b.variable) : (a.offset < b.offset);
        });
        std::vector<Range> merged;
        for (const Range &range : summary.ranges) {
            if (!merged.empty() && merged.back().variable == range.variable
                && range.offset <= merged.back().offset + merged.back().size) {
                const uint32_t end = std::max(merged.back().offset + merged.back().size, range.offset + range.size);
                merged.back().size = end - merged.back().offset;
            } else {
                merged.push_back(range);
            }
        }

        // An invocation starts with its private variables undefined, so all the
        // entry point can share between lanes is what it outputs.
        if (function == entry_point) {
            std::erase_if(merged, [&](const Range &range) {
                const auto variable = variables.find(range.variable);
                return variable == variables.end() || variable->second.storage_class != spv::StorageClassOutput;
            });
        }

        memoizable[number] = 1;
        memo_state[number] = std::move(merged);
    }
}

// -----------------------------------------------------------------------------
// Interpreter
// -----------------------------------------------------------------------------

void SpirvInterpreter::set_program(const SpirvModule *module, const SpirvBindings *bindings) {
    m_module = module;
    m_bindings = bindings;
    m_discarded = 0;

    m_location_to_input.clear();
    m_location_to_output.clear();
    m_builtin_to_variable.clear();
    m_private_inits.clear();

    if (!m_module) {
        m_registers = nullptr;
        return;
    }

    m_register_file.assign(m_module->register_bytes, 0);
    m_registers = m_register_file.data();

    // A new program means new bound buffers, which a remembered run may have read.
    m_memo.resize(m_module->memoizable.size());
    for (Memo &memo : m_memo)
        memo.valid = false;
    std::memcpy(m_registers, m_module->constant_registers.data(), m_module->constant_registers.size());

    // Specialization constants the caller overrode take priority.
    if (m_bindings) {
        for (const auto &[spec_id, value] : m_bindings->spec_constants) {
            const auto constant = m_module->spec_constant_ids.find(spec_id);
            if (constant != m_module->spec_constant_ids.end() && constant->second < m_module->id_bound)
                std::memcpy(reg(constant->second, 0), &value, sizeof(value));
        }
    }

    m_variable_file.assign(m_module->variable_bytes, 0);
    m_variables.assign(m_module->id_bound, {});

    for (const auto &[id, variable] : m_module->variables) {
        if (id >= m_module->id_bound)
            continue;

        VariableStorage &storage = m_variables[id];
        if (is_bound_buffer(variable.storage_class)) {
            if (m_bindings && variable.binding != SPIRV_NO_VALUE && variable.binding < m_bindings->uniform_buffers.size()) {
                const SpirvBindings::Buffer &buffer = m_bindings->uniform_buffers[variable.binding];
                if (buffer.data && buffer.size) {
                    storage.base = const_cast<uint8_t *>(buffer.data);
                    storage.size = buffer.size;
                }
            }
        } else {
            storage.base = m_variable_file.data() + m_module->variable_offset[id];
            storage.stride = m_module->variable_stride[id];
            storage.size = std::max(variable.byte_size, 4u);
        }

        if (variable.storage_class == spv::StorageClassInput && variable.location != SPIRV_NO_VALUE)
            m_location_to_input[variable.location] = id;
        if (variable.storage_class == spv::StorageClassOutput && variable.location != SPIRV_NO_VALUE)
            m_location_to_output[variable.location] = id;
        if (variable.builtin != SPIRV_NO_VALUE)
            m_builtin_to_variable[variable.builtin] = id;

        // Image variables never change once bound, so their descriptor is
        // written now and then loaded like any other value.
        if (variable.storage_class == spv::StorageClassUniformConstant && storage.base) {
            ImageRepr image{};
            image.descriptor_set = (variable.descriptor_set == SPIRV_NO_VALUE) ? 0 : variable.descriptor_set;
            image.binding = (variable.binding == SPIRV_NO_VALUE) ? 0 : variable.binding;
            for (uint32_t lane = 0; lane < SPIRV_MAX_LANES; lane++)
                std::memcpy(storage.base + static_cast<size_t>(lane) * storage.stride, &image, sizeof(image));
        }

        if (m_module->materialized[id] && m_module->register_stride[id]) {
            for (uint32_t lane = 0; lane < SPIRV_MAX_LANES; lane++) {
                PointerRepr repr{};
                repr.base = storage.base ? storage.base + static_cast<size_t>(lane) * storage.stride : nullptr;
                repr.type_id = variable.type_id;
                repr.remaining = storage.base ? storage.size : 0;
                std::memcpy(reg(id, lane), &repr, sizeof(repr));
            }
        }
    }

    for (const uint32_t id : m_module->private_variables) {
        const VariableStorage &storage = m_variables[id];
        if (!storage.base)
            continue;

        const uint32_t initializer = m_module->variables.at(id).initializer;
        const auto constant = m_module->constants.find(initializer);
        if (initializer == 0 || constant == m_module->constants.end())
            continue;

        PrivateInit init;
        init.storage = storage;
        init.initializer = constant->second.data();
        init.initializer_size = static_cast<uint32_t>(std::min<size_t>(constant->second.size(), storage.size));
        m_private_inits.push_back(init);
    }
}

SpirvLaneStorage SpirvInterpreter::variable_lanes(uint32_t variable_id) const {
    if (!m_module || variable_id >= m_variables.size())
        return {};
    const VariableStorage &storage = m_variables[variable_id];
    return { storage.base, storage.stride };
}

SpirvLaneStorage SpirvInterpreter::input_by_location(uint32_t location) const {
    const auto it = m_location_to_input.find(location);
    return (it == m_location_to_input.end()) ? SpirvLaneStorage{} : variable_lanes(it->second);
}

SpirvLaneStorage SpirvInterpreter::output_by_location(uint32_t location) const {
    const auto it = m_location_to_output.find(location);
    return (it == m_location_to_output.end()) ? SpirvLaneStorage{} : variable_lanes(it->second);
}

SpirvLaneStorage SpirvInterpreter::builtin_storage(uint32_t builtin) const {
    const auto it = m_builtin_to_variable.find(builtin);
    return (it == m_builtin_to_variable.end()) ? SpirvLaneStorage{} : variable_lanes(it->second);
}

uint32_t SpirvInterpreter::execute(uint32_t lane_mask) {
    if (!m_module || !m_registers || lane_mask == 0)
        return 0;

    m_discarded = 0;

    // A private variable with an initializer starts every invocation from it.
    // One without is undefined until written, and like GPU registers keeps
    // whatever the previous batch left: clearing it for every batch cost more
    // than some of the shaders themselves.
    for (const PrivateInit &init : m_private_inits) {
        for_each_lane(lane_mask, [&](uint32_t lane) {
            std::memcpy(init.storage.base + static_cast<size_t>(lane) * init.storage.stride, init.initializer,
                init.initializer_size);
        });
    }

    // A shader that reads no input gives every lane the same outputs, so one lane can run it for all.
    const uint32_t entry = m_module->function_index[m_module->entry_point];
    const uint32_t number = m_module->function_number[m_module->entry_point];
    if (number < m_module->memoizable.size() && m_module->memoizable[number] && run_shared(number, entry, lane_mask, 0))
        return lane_mask;

    run(entry, lane_mask, 0, 0, 0);
    return lane_mask & ~m_discarded;
}

void SpirvInterpreter::copy_lanes(uint32_t destination, uint32_t source, uint32_t mask) {
    const uint32_t size = std::min(m_module->value_size[destination], m_module->value_size[source]);
    if (size == 0 || m_module->register_stride[destination] == 0)
        return;

    const Lanes to = lanes(destination);
    const Lanes from = lanes(source);
    for_each_lane(mask, [&](uint32_t lane) { copy_value(to[lane], from[lane], size); });
}

void SpirvInterpreter::run(uint32_t pc, uint32_t mask, uint32_t current_label, uint32_t result_id, uint32_t depth) {
    const SpirvModule &module = *m_module;
    const std::vector<SpirvInstruction> &code = module.code;
    const uint32_t *words = module.words.data();
    uint32_t previous_label = 0;

    while (pc < code.size() && mask) {
        const SpirvInstruction &instruction = code[pc];
        const uint32_t *operands = words + instruction.operands;
        const uint32_t count = instruction.word_count;

        switch (instruction.op) {
        case spv::OpFunctionEnd:
        case spv::OpReturn:
        case spv::OpUnreachable:
            return;

        case spv::OpReturnValue:
            if (result_id)
                copy_lanes(result_id, operands[0], mask);
            return;

        case spv::OpKill:
        case spv::OpTerminateInvocation:
            m_discarded |= mask;
            return;

        case spv::OpLabel:
            previous_label = current_label;
            current_label = operands[0];
            break;

        case spv::OpBranch:
            pc = instruction.target;
            continue;

        case spv::OpBranchConditional: {
            uint32_t taken = 0;
            for_each_lane(mask, [&](uint32_t lane) {
                uint32_t condition;
                std::memcpy(&condition, reg(operands[0], lane), sizeof(condition));
                if (condition)
                    taken |= 1u << lane;
            });

            const uint32_t not_taken = mask & ~taken;
            if (!not_taken) {
                pc = instruction.target;
                continue;
            }
            if (!taken) {
                pc = instruction.second_target;
                continue;
            }

            run(instruction.target, taken, current_label, result_id, depth);
            run(instruction.second_target, not_taken, current_label, result_id, depth);
            return;
        }

        case spv::OpSwitch: {
            const Scalars selector_info = scalars_of(module, module.value_type[operands[0]]);
            uint32_t targets[SPIRV_MAX_LANES] = {};
            for_each_lane(mask, [&](uint32_t lane) {
                const uint32_t selector = load_uint(reg(operands[0], lane), selector_info, 0);
                uint32_t target = operands[1];
                for (uint32_t i = 2; i + 1 < count - 1; i += 2) {
                    if (operands[i] == selector) {
                        target = operands[i + 1];
                        break;
                    }
                }
                targets[lane] = target;
            });

            uint32_t remaining = mask;
            const uint32_t first_target = targets[lowest_lane(mask)];
            uint32_t same = 0;
            for_each_lane(mask, [&](uint32_t lane) {
                if (targets[lane] == first_target)
                    same |= 1u << lane;
            });

            if (same == mask) {
                pc = (first_target < module.label_index.size()) ? module.label_index[first_target] : SPIRV_NO_VALUE;
                continue;
            }

            while (remaining) {
                const uint32_t target = targets[lowest_lane(remaining)];
                uint32_t group = 0;
                for_each_lane(remaining, [&](uint32_t lane) {
                    if (targets[lane] == target)
                        group |= 1u << lane;
                });
                remaining &= ~group;
                run((target < module.label_index.size()) ? module.label_index[target] : SPIRV_NO_VALUE, group,
                    current_label, result_id, depth);
            }
            return;
        }

        case spv::OpPhi: {
            // The phis heading a block read their inputs as they were on entry,
            // so every one of them is gathered before any is written.
            uint32_t end = pc;
            while (end < code.size() && code[end].op == spv::OpPhi)
                end++;

            const auto source_of = [&](const SpirvInstruction &phi) -> uint32_t {
                const uint32_t *phi_operands = words + phi.operands;
                for (uint32_t i = 2; i + 1 < phi.word_count - 1; i += 2) {
                    if (phi_operands[i + 1] == previous_label)
                        return phi_operands[i];
                }
                return 0;
            };

            if (end - pc == 1) {
                const uint32_t source = source_of(instruction);
                if (source)
                    copy_lanes(operands[1], source, mask);
            } else {
                m_phi_scratch.clear();
                for (uint32_t k = pc; k < end; k++) {
                    const uint32_t result = words[code[k].operands + 1];
                    const uint32_t source = source_of(code[k]);
                    const uint32_t size = source ? std::min(module.value_size[result], module.value_size[source]) : 0;
                    for_each_lane(mask, [&](uint32_t lane) {
                        const size_t at = m_phi_scratch.size();
                        m_phi_scratch.resize(at + size);
                        if (size)
                            std::memcpy(m_phi_scratch.data() + at, reg(source, lane), size);
                    });
                }

                size_t at = 0;
                for (uint32_t k = pc; k < end; k++) {
                    const uint32_t result = words[code[k].operands + 1];
                    const uint32_t source = source_of(code[k]);
                    const uint32_t size = source ? std::min(module.value_size[result], module.value_size[source]) : 0;
                    for_each_lane(mask, [&](uint32_t lane) {
                        if (size && module.register_stride[result])
                            std::memcpy(reg(result, lane), m_phi_scratch.data() + at, size);
                        at += size;
                    });
                }
            }

            pc = end;
            continue;
        }

        case spv::OpFunctionCall:
            call(instruction, operands, mask, depth);
            // A callee that discarded lanes takes them out of the caller too.
            mask &= ~m_discarded;
            break;

        case spv::OpVariable: {
            // A function scope local starts from its initializer on every call.
            const VariableStorage &storage = m_variables[operands[1]];
            if (!storage.base)
                break;

            const uint32_t initializer = (count >= 5) ? operands[3] : 0;
            const uint32_t initial_size = initializer ? std::min(storage.size, module.value_size[initializer]) : 0;
            for_each_lane(mask, [&](uint32_t lane) {
                uint8_t *target = storage.base + static_cast<size_t>(lane) * storage.stride;
                std::memset(target, 0, storage.size);
                if (initial_size)
                    std::memcpy(target, reg(initializer, lane), initial_size);
            });
            break;
        }

        case spv::OpLoad:
            execute_load(operands, mask);
            break;

        case spv::OpStore:
            execute_store(operands, mask);
            break;

        case spv::OpCopyMemory:
            for_each_lane(mask, [&](uint32_t lane) {
                uint32_t destination_room = 0;
                uint32_t source_room = 0;
                uint32_t type_id = 0;
                uint32_t source_type = 0;
                uint8_t *destination = resolve_pointer(operands[0], lane, destination_room, type_id);
                const uint8_t *source = resolve_pointer(operands[1], lane, source_room, source_type);
                const uint32_t bytes = std::min({ module.byte_size_of(type_id), destination_room, source_room });
                if (destination && source && bytes)
                    std::memmove(destination, source, bytes);
            });
            break;

        case spv::OpAccessChain:
        case spv::OpInBoundsAccessChain:
            // Loads and stores resolve a static chain themselves.
            if (module.static_pointers[operands[1]].valid() && !module.materialized[operands[1]])
                break;
            execute_access_chain(operands, count, mask);
            break;

        case spv::OpCopyObject:
            copy_lanes(operands[1], operands[2], mask);
            break;

        case spv::OpCompositeExtract: {
            const Lanes result = lanes(operands[1]);
            const uint32_t size = instruction.size;
            const uint32_t offset = instruction.aux;
            if (offset + size > module.value_size[operands[2]]) {
                for_each_lane(mask, [&](uint32_t lane) { std::memset(result[lane], 0, module.value_size[operands[1]]); });
                break;
            }
            const Lanes composite = lanes(operands[2]);
            if (size == 4) {
                for_each_lane(mask, [&](uint32_t lane) { std::memcpy(result[lane], composite[lane] + offset, 4); });
                break;
            }
            for_each_lane(mask, [&](uint32_t lane) { copy_value(result[lane], composite[lane] + offset, size); });
            break;
        }

        case spv::OpCompositeInsert: {
            const Lanes result = lanes(operands[1]);
            const Lanes object = lanes(operands[2]);
            const Lanes composite = lanes(operands[3]);
            const uint32_t size = module.value_size[operands[1]];
            const uint32_t object_size = module.byte_size_of(module.value_type[operands[2]]);
            const uint32_t composite_size = std::min(size, module.value_size[operands[3]]);
            const uint32_t offset = instruction.aux;
            const bool fits = offset + object_size <= size;
            for_each_lane(mask, [&](uint32_t lane) {
                copy_value(result[lane], composite[lane], composite_size);
                if (fits)
                    copy_value(result[lane] + offset, object[lane], object_size);
            });
            break;
        }

        case SPIRV_OP_INSERT_AT: {
            const Lanes result = lanes(operands[1]);
            const Lanes vector = lanes(operands[2]);
            const Lanes component = lanes(operands[3]);
            const uint32_t size = std::min(module.value_size[operands[1]], module.value_size[operands[2]]);
            const uint32_t component_size = module.byte_size_of(module.value_type[operands[3]]);
            const uint32_t offset = instruction.aux;
            for_each_lane(mask, [&](uint32_t lane) {
                copy_value(result[lane], vector[lane], size);
                copy_value(result[lane] + offset, component[lane], component_size);
            });
            break;
        }

        case spv::OpCompositeConstruct: {
            const Lanes result = lanes(operands[1]);
            const uint32_t size = module.value_size[operands[1]];
            const uint32_t constituent_count = std::min<uint32_t>(count - 3, 16);

            // Where each constituent lands is the same for every lane.
            Lanes sources[16];
            uint32_t bytes[16];
            uint32_t offsets[16];
            uint32_t used = 0;
            uint32_t offset = 0;
            for (uint32_t i = 0; i < constituent_count && offset < size; i++) {
                sources[used] = lanes(operands[2 + i]);
                bytes[used] = std::min(module.byte_size_of(module.value_type[operands[2 + i]]), size - offset);
                offsets[used] = offset;
                offset += bytes[used];
                used++;
            }

            for_each_lane(mask, [&](uint32_t lane) {
                uint8_t *destination = result[lane];
                for (uint32_t i = 0; i < used; i++)
                    copy_value(destination + offsets[i], sources[i][lane], bytes[i]);
            });
            break;
        }

        case spv::OpVectorShuffle: {
            const Lanes result = lanes(operands[1]);
            const Lanes first = lanes(operands[2]);
            const Lanes second = lanes(operands[3]);
            const Scalars result_info = scalars_of(module, operands[0]);
            const uint32_t first_count = scalars_of(module, module.value_type[operands[2]]).count;
            const uint32_t element_size = result_info.size;
            const uint32_t component_count = std::min<uint32_t>(count - 5, 16);

            // Resolve every selector to a source and an offset once.
            const Lanes *sources[16];
            uint32_t offsets[16];
            for (uint32_t i = 0; i < component_count; i++) {
                const uint32_t selector = operands[4 + i];
                if (selector == 0xFFFFFFFFu) {
                    sources[i] = nullptr;
                    offsets[i] = 0;
                } else if (selector < first_count) {
                    sources[i] = &first;
                    offsets[i] = selector * element_size;
                } else {
                    sources[i] = &second;
                    offsets[i] = (selector - first_count) * element_size;
                }
            }

            for_each_lane(mask, [&](uint32_t lane) {
                uint8_t *destination = result[lane];
                for (uint32_t i = 0; i < component_count; i++) {
                    uint8_t *target = destination + static_cast<size_t>(i) * element_size;
                    if (sources[i])
                        copy_value(target, (*sources[i])[lane] + offsets[i], element_size);
                    else
                        std::memset(target, 0, element_size);
                }
            });
            break;
        }

        case spv::OpVectorExtractDynamic: {
            const Lanes result = lanes(operands[1]);
            const Lanes vector = lanes(operands[2]);
            const Lanes index_lanes = lanes(operands[3]);
            const uint32_t element_size = instruction.size;
            const uint32_t vector_count = scalars_of(module, module.value_type[operands[2]]).count;
            const Scalars index_info = scalars_of(module, module.value_type[operands[3]]);
            for_each_lane(mask, [&](uint32_t lane) {
                const uint32_t index = load_uint(index_lanes[lane], index_info, 0);
                if (index < vector_count)
                    copy_value(result[lane], vector[lane] + static_cast<size_t>(index) * element_size, element_size);
                else
                    std::memset(result[lane], 0, element_size);
            });
            break;
        }

        case spv::OpVectorInsertDynamic: {
            const Lanes result = lanes(operands[1]);
            const Lanes vector = lanes(operands[2]);
            const Lanes component = lanes(operands[3]);
            const Lanes index_lanes = lanes(operands[4]);
            const uint32_t size = std::min(module.value_size[operands[1]], module.value_size[operands[2]]);
            const Scalars info = scalars_of(module, operands[0]);
            const Scalars index_info = scalars_of(module, module.value_type[operands[4]]);
            for_each_lane(mask, [&](uint32_t lane) {
                copy_value(result[lane], vector[lane], size);
                const uint32_t index = load_uint(index_lanes[lane], index_info, 0);
                if (index < info.count)
                    copy_value(result[lane] + static_cast<size_t>(index) * info.size, component[lane], info.size);
            });
            break;
        }

        case spv::OpSelect: {
            const uint32_t result = operands[1];
            const Scalars info = scalars_of(module, operands[0]);
            const Scalars condition_info = scalars_of(module, module.value_type[operands[2]]);
            const uint32_t size = module.value_size[result];
            for_each_lane(mask, [&](uint32_t lane) {
                uint8_t *destination = reg(result, lane);
                const uint8_t *condition = reg(operands[2], lane);
                const uint8_t *when_true = reg(operands[3], lane);
                const uint8_t *when_false = reg(operands[4], lane);

                // The condition is either a single bool or one per component.
                if (condition_info.count <= 1) {
                    copy_value(destination, load_uint(condition, condition_info, 0) ? when_true : when_false, size);
                    return;
                }
                for (uint32_t i = 0; i < info.count; i++) {
                    const uint8_t *chosen = load_uint(condition, condition_info, i) ? when_true : when_false;
                    copy_value(destination + static_cast<size_t>(i) * info.size,
                        chosen + static_cast<size_t>(i) * info.size, info.size);
                }
            });
            break;
        }

        case spv::OpBitcast: {
            const uint32_t size = std::min(module.value_size[operands[1]], module.value_size[operands[2]]);
            for_each_lane(mask, [&](uint32_t lane) {
                copy_value(reg(operands[1], lane), reg(operands[2], lane), size);
            });
            break;
        }

        case spv::OpSampledImage:
        case spv::OpImage:
            for_each_lane(mask, [&](uint32_t lane) {
                copy_value(reg(operands[1], lane), reg(operands[2], lane), IMAGE_SIZE);
            });
            break;

        case spv::OpFNegate:
        case spv::OpSNegate:
        case spv::OpNot:
        case spv::OpFAdd:
        case spv::OpFSub:
        case spv::OpFMul:
        case spv::OpFDiv:
        case spv::OpFMod:
        case spv::OpFRem:
        case spv::OpIAdd:
        case spv::OpISub:
        case spv::OpIMul:
        case spv::OpSDiv:
        case spv::OpUDiv:
        case spv::OpSMod:
        case spv::OpSRem:
        case spv::OpUMod:
        case spv::OpShiftLeftLogical:
        case spv::OpShiftRightLogical:
        case spv::OpShiftRightArithmetic:
        case spv::OpBitwiseAnd:
        case spv::OpBitwiseOr:
        case spv::OpBitwiseXor:
        case spv::OpLogicalAnd:
        case spv::OpLogicalOr:
        case spv::OpLogicalNot:
        case spv::OpLogicalEqual:
        case spv::OpLogicalNotEqual:
        case spv::OpFOrdEqual:
        case spv::OpFUnordEqual:
        case spv::OpFOrdNotEqual:
        case spv::OpFUnordNotEqual:
        case spv::OpFOrdLessThan:
        case spv::OpFUnordLessThan:
        case spv::OpFOrdGreaterThan:
        case spv::OpFUnordGreaterThan:
        case spv::OpFOrdLessThanEqual:
        case spv::OpFUnordLessThanEqual:
        case spv::OpFOrdGreaterThanEqual:
        case spv::OpFUnordGreaterThanEqual:
        case spv::OpIEqual:
        case spv::OpINotEqual:
        case spv::OpSLessThan:
        case spv::OpULessThan:
        case spv::OpSGreaterThan:
        case spv::OpUGreaterThan:
        case spv::OpSLessThanEqual:
        case spv::OpULessThanEqual:
        case spv::OpSGreaterThanEqual:
        case spv::OpUGreaterThanEqual:
        case spv::OpConvertFToS:
        case spv::OpConvertFToU:
        case spv::OpConvertSToF:
        case spv::OpConvertUToF:
        case spv::OpFConvert:
        case spv::OpSConvert:
        case spv::OpUConvert:
        case spv::OpIsNan:
        case spv::OpIsInf:
        case spv::OpAny:
        case spv::OpAll:
        case spv::OpDot:
        case spv::OpVectorTimesScalar:
            execute_scalar_op(instruction.op, operands, count, mask);
            break;

        case spv::OpMatrixTimesVector:
        case spv::OpVectorTimesMatrix:
        case spv::OpMatrixTimesMatrix:
        case spv::OpMatrixTimesScalar:
        case spv::OpTranspose:
            execute_matrix_op(instruction.op, operands, count, mask);
            break;

        case spv::OpBitFieldInsert:
        case spv::OpBitFieldSExtract:
        case spv::OpBitFieldUExtract:
        case spv::OpBitReverse:
        case spv::OpBitCount:
            execute_bit_op(instruction.op, operands, mask);
            break;

        case spv::OpExtInst:
            execute_ext_inst(operands, count, mask);
            break;

        case spv::OpImageSampleImplicitLod:
        case spv::OpImageSampleExplicitLod:
        case spv::OpImageSampleProjImplicitLod:
        case spv::OpImageSampleProjExplicitLod:
        case spv::OpImageFetch:
        case spv::OpImageRead:
            execute_image_op(instruction.op, operands, count, mask);
            break;

        default:
            LOG_WARN_ONCE("Software renderer: unimplemented SPIR-V opcode {}", instruction.op);
            if (count >= 3 && module.register_stride[operands[1]]) {
                for_each_lane(mask, [&](uint32_t lane) {
                    std::memset(reg(operands[1], lane), 0, module.value_size[operands[1]]);
                });
            }
            break;
        }

        pc++;
    }
}

void SpirvInterpreter::call(const SpirvInstruction &instruction, const uint32_t *operands, uint32_t mask, uint32_t depth) {
    // Shaders out of the recompiler are not recursive, but a malformed module
    // must not take the emulator down with it.
    if (depth >= MAX_CALL_DEPTH) {
        LOG_ERROR_ONCE("Software renderer: shader call depth exceeded");
        return;
    }
    if (instruction.target == SPIRV_NO_VALUE || instruction.aux >= m_module->callee_parameters.size())
        return;

    if (m_module->memoizable[instruction.aux] && run_shared(instruction.aux, instruction.target, mask, depth))
        return;

    const std::vector<uint32_t> &parameters = m_module->callee_parameters[instruction.aux];
    const uint32_t argument_count = instruction.word_count - 4u;
    for (uint32_t i = 0; i < parameters.size() && i < argument_count; i++)
        copy_lanes(parameters[i], operands[3 + i], mask);

    run(instruction.target, mask, 0, (m_module->value_size[operands[1]] > 0) ? operands[1] : 0, depth + 1);
}

bool SpirvInterpreter::run_shared(uint32_t number, uint32_t target, uint32_t mask, uint32_t depth) {
    const std::vector<SpirvModule::Range> &ranges = m_module->memo_state[number];
    const uint32_t first = lowest_lane(mask);
    const uint32_t others = mask & ~(1u << first);

    // One run can only stand in for lanes that all start from the same state.
    m_memo_scratch.clear();
    for (const SpirvModule::Range &range : ranges) {
        const VariableStorage &storage = m_variables[range.variable];
        if (!storage.base || range.offset + range.size > storage.size)
            return false;

        const uint8_t *reference = storage.base + first * storage.stride + range.offset;
        bool same = true;
        for_each_lane(others, [&](uint32_t lane) {
            same = same && std::memcmp(reference, storage.base + lane * storage.stride + range.offset, range.size) == 0;
        });
        if (!same)
            return false;

        m_memo_scratch.insert(m_memo_scratch.end(), reference, reference + range.size);
    }

    Memo &memo = m_memo[number];
    if (!memo.valid || memo.input != m_memo_scratch) {
        run(target, 1u << first, 0, 0, depth + 1);

        memo.valid = true;
        memo.input.swap(m_memo_scratch);
        memo.output.clear();
        for (const SpirvModule::Range &range : ranges) {
            const VariableStorage &storage = m_variables[range.variable];
            const uint8_t *source = storage.base + first * storage.stride + range.offset;
            memo.output.insert(memo.output.end(), source, source + range.size);
        }
    }

    size_t at = 0;
    for (const SpirvModule::Range &range : ranges) {
        const VariableStorage &storage = m_variables[range.variable];
        for_each_lane(mask, [&](uint32_t lane) {
            std::memcpy(storage.base + lane * storage.stride + range.offset, memo.output.data() + at, range.size);
        });
        at += range.size;
    }
    return true;
}

uint8_t *SpirvInterpreter::resolve_pointer(uint32_t pointer_id, uint32_t lane, uint32_t &room, uint32_t &type_id) const {
    room = 0;
    const SpirvStaticPointer &fixed = m_module->static_pointers[pointer_id];
    if (fixed.valid()) {
        type_id = fixed.type_id;
        const VariableStorage &storage = m_variables[fixed.variable];
        if (!storage.base || fixed.offset >= storage.size)
            return nullptr;
        room = storage.size - fixed.offset;
        return storage.base + static_cast<size_t>(lane) * storage.stride + fixed.offset;
    }

    PointerRepr repr;
    std::memcpy(&repr, reg(pointer_id, lane), sizeof(repr));
    type_id = repr.type_id;
    if (!repr.base)
        return nullptr;
    room = repr.remaining;
    return repr.base;
}

void SpirvInterpreter::execute_load(const uint32_t *operands, uint32_t mask) {
    const uint32_t result = operands[1];
    const uint32_t pointer = operands[2];
    if (!m_module->register_stride[result])
        return;

    const uint32_t size = m_module->byte_size_of(operands[0]);
    const uint32_t register_size = m_module->value_size[result];

    const SpirvStaticPointer &fixed = m_module->static_pointers[pointer];
    if (fixed.valid()) {
        const VariableStorage &storage = m_variables[fixed.variable];
        const uint32_t readable = (storage.base && fixed.offset < storage.size)
            ? std::min(size, storage.size - fixed.offset)
            : 0;

        if (readable == size) {
            const uint8_t *source = storage.base + fixed.offset;
            const size_t stride = storage.stride;
            const Lanes destination = lanes(result);
            if (size == 16) {
                for_each_lane(mask, [&](uint32_t lane) { std::memcpy(destination[lane], source + lane * stride, 16); });
                return;
            }
            if (size == 4) {
                for_each_lane(mask, [&](uint32_t lane) { std::memcpy(destination[lane], source + lane * stride, 4); });
                return;
            }
            for_each_lane(mask, [&](uint32_t lane) { copy_value(destination[lane], source + lane * stride, size); });
            return;
        }

        LOG_WARN_ONCE("Software renderer: a shader load of {} bytes was clamped to {}", size, readable);
        for_each_lane(mask, [&](uint32_t lane) {
            uint8_t *destination = reg(result, lane);
            std::memset(destination, 0, register_size);
            if (readable)
                std::memcpy(destination, storage.base + static_cast<size_t>(lane) * storage.stride + fixed.offset, readable);
        });
        return;
    }

    for_each_lane(mask, [&](uint32_t lane) {
        PointerRepr repr;
        std::memcpy(&repr, reg(pointer, lane), sizeof(repr));
        uint8_t *destination = reg(result, lane);
        const uint32_t readable = repr.base ? std::min(size, repr.remaining) : 0;
        if (readable == size) {
            copy_value(destination, repr.base, size);
            return;
        }
        std::memset(destination, 0, register_size);
        if (readable)
            std::memcpy(destination, repr.base, readable);
    });
}

void SpirvInterpreter::execute_store(const uint32_t *operands, uint32_t mask) {
    const uint32_t pointer = operands[0];
    const uint32_t value = operands[1];
    const uint32_t value_bytes = m_module->byte_size_of(m_module->value_type[value]);

    const SpirvStaticPointer &fixed = m_module->static_pointers[pointer];
    if (fixed.valid()) {
        const VariableStorage &storage = m_variables[fixed.variable];
        // Bound buffers are shared by every lane and read only to a shader.
        if (!storage.base || !storage.stride || fixed.offset >= storage.size)
            return;

        const uint32_t size = std::min({ m_module->byte_size_of(fixed.type_id), value_bytes, storage.size - fixed.offset });
        uint8_t *target = storage.base + fixed.offset;
        const size_t stride = storage.stride;
        const Lanes source = lanes(value);
        if (size == 16) {
            for_each_lane(mask, [&](uint32_t lane) { std::memcpy(target + lane * stride, source[lane], 16); });
            return;
        }
        for_each_lane(mask, [&](uint32_t lane) { copy_value(target + lane * stride, source[lane], size); });
        return;
    }

    for_each_lane(mask, [&](uint32_t lane) {
        PointerRepr repr;
        std::memcpy(&repr, reg(pointer, lane), sizeof(repr));
        if (!repr.base)
            return;
        const uint32_t size = std::min({ m_module->byte_size_of(repr.type_id), value_bytes, repr.remaining });
        if (size)
            std::memcpy(repr.base, reg(value, lane), size);
    });
}

void SpirvInterpreter::execute_access_chain(const uint32_t *operands, uint32_t count, uint32_t mask) {
    const SpirvModule &module = *m_module;
    const uint32_t result = operands[1];
    const uint32_t base = operands[2];
    const uint32_t index_count = count - 4;
    if (!module.register_stride[result])
        return;

    const SpirvStaticPointer &fixed_base = module.static_pointers[base];

    for_each_lane(mask, [&](uint32_t lane) {
        uint8_t *address = nullptr;
        uint32_t remaining = 0;
        uint32_t type_id = 0;

        if (fixed_base.valid()) {
            address = resolve_pointer(base, lane, remaining, type_id);
        } else {
            PointerRepr repr;
            std::memcpy(&repr, reg(base, lane), sizeof(repr));
            address = repr.base;
            remaining = repr.remaining;
            type_id = repr.type_id;
        }

        // Every step forward eats into what is still addressable. Once the
        // walk would leave the object the pointer is dropped instead of being
        // allowed to escape into whatever follows it in memory.
        const auto step = [&](uint32_t bytes) {
            if (bytes > remaining) {
                address = nullptr;
                remaining = 0;
                return;
            }
            address += bytes;
            remaining -= bytes;
        };

        for (uint32_t i = 0; i < index_count && address; i++) {
            const SpirvType *current = module.type(type_id);
            if (!current)
                break;

            const uint32_t index_id = operands[3 + i];
            const uint32_t index = load_uint(reg(index_id, lane), scalars_of(module, module.value_type[index_id]), 0);

            switch (current->kind) {
            case SpirvTypeKind::Struct:
                if (index < current->member_offsets.size()) {
                    step(current->member_offsets[index]);
                    type_id = current->member_types[index];
                } else {
                    address = nullptr;
                }
                break;
            case SpirvTypeKind::Array:
            case SpirvTypeKind::RuntimeArray:
            case SpirvTypeKind::Matrix:
                step(current->stride * index);
                type_id = current->element_type;
                break;
            case SpirvTypeKind::Vector:
                step(module.byte_size_of(current->element_type) * index);
                type_id = current->element_type;
                break;
            default:
                break;
            }
        }

        PointerRepr repr{};
        repr.base = address;
        repr.type_id = type_id;
        repr.remaining = address ? remaining : 0;
        std::memcpy(reg(result, lane), &repr, sizeof(repr));
    });
}

void SpirvInterpreter::execute_scalar_op(uint32_t op, const uint32_t *operands, uint32_t count, uint32_t mask) {
    const SpirvModule &module = *m_module;
    const uint32_t result = operands[1];
    if (!module.register_stride[result])
        return;

    const Scalars r = scalars_of(module, operands[0]);
    const uint32_t first_id = operands[2];
    const uint32_t second_id = (count > 4) ? operands[3] : first_id;
    const Scalars a = scalars_of(module, module.value_type[first_id]);
    const Scalars b = scalars_of(module, module.value_type[second_id]);
    const uint32_t a_step = (a.count > 1) ? 1 : 0;
    const uint32_t b_step = (b.count > 1) ? 1 : 0;
    const bool all_32 = r.width == 32 && a.width == 32 && b.width == 32;
    const Scalars bools = bool_scalars(r.count);
    const Lanes out = lanes(result);
    const Lanes in_a = lanes(first_id);
    const Lanes in_b = lanes(second_id);

    // One loop per operation shape, with the operation itself inlined into it.
    const auto float_unary = [&](auto &&function) {
        for_each_lane(mask, [&](uint32_t lane) {
            uint8_t *destination = out[lane];
            const uint8_t *x = in_a[lane];
            for (uint32_t i = 0; i < r.count; i++)
                store_float(destination, r, i, function(load_float(x, a, i * a_step)));
        });
    };
    const auto float_binary = [&](auto &&function) {
        if (all_32) {
            for_each_lane(mask, [&](uint32_t lane) {
                uint8_t *destination = out[lane];
                const uint8_t *x = in_a[lane];
                const uint8_t *y = in_b[lane];
                for (uint32_t i = 0; i < r.count; i++) {
                    float left, right;
                    std::memcpy(&left, x + i * a_step * 4, 4);
                    std::memcpy(&right, y + i * b_step * 4, 4);
                    const float value = function(left, right);
                    std::memcpy(destination + i * 4, &value, 4);
                }
            });
            return;
        }
        for_each_lane(mask, [&](uint32_t lane) {
            uint8_t *destination = out[lane];
            const uint8_t *x = in_a[lane];
            const uint8_t *y = in_b[lane];
            for (uint32_t i = 0; i < r.count; i++)
                store_float(destination, r, i, function(load_float(x, a, i * a_step), load_float(y, b, i * b_step)));
        });
    };
    const auto uint_unary = [&](auto &&function) {
        for_each_lane(mask, [&](uint32_t lane) {
            uint8_t *destination = out[lane];
            const uint8_t *x = in_a[lane];
            for (uint32_t i = 0; i < r.count; i++)
                store_uint(destination, r, i, function(load_uint(x, a, i * a_step)));
        });
    };
    const auto uint_binary = [&](auto &&function) {
        if (all_32) {
            for_each_lane(mask, [&](uint32_t lane) {
                uint8_t *destination = out[lane];
                const uint8_t *x = in_a[lane];
                const uint8_t *y = in_b[lane];
                for (uint32_t i = 0; i < r.count; i++) {
                    uint32_t left, right;
                    std::memcpy(&left, x + i * a_step * 4, 4);
                    std::memcpy(&right, y + i * b_step * 4, 4);
                    const uint32_t value = function(left, right);
                    std::memcpy(destination + i * 4, &value, 4);
                }
            });
            return;
        }
        for_each_lane(mask, [&](uint32_t lane) {
            uint8_t *destination = out[lane];
            const uint8_t *x = in_a[lane];
            const uint8_t *y = in_b[lane];
            for (uint32_t i = 0; i < r.count; i++)
                store_uint(destination, r, i, function(load_uint(x, a, i * a_step), load_uint(y, b, i * b_step)));
        });
    };
    const auto int_binary = [&](auto &&function) {
        for_each_lane(mask, [&](uint32_t lane) {
            uint8_t *destination = out[lane];
            const uint8_t *x = in_a[lane];
            const uint8_t *y = in_b[lane];
            for (uint32_t i = 0; i < r.count; i++)
                store_uint(destination, r, i,
                    static_cast<uint32_t>(function(load_int(x, a, i * a_step), load_int(y, b, i * b_step))));
        });
    };
    const auto compare_float = [&](auto &&function) {
        for_each_lane(mask, [&](uint32_t lane) {
            uint8_t *destination = out[lane];
            const uint8_t *x = in_a[lane];
            const uint8_t *y = in_b[lane];
            for (uint32_t i = 0; i < r.count; i++)
                store_uint(destination, bools, i,
                    function(load_float(x, a, i * a_step), load_float(y, b, i * b_step)) ? 1u : 0u);
        });
    };
    const auto compare_uint = [&](auto &&function) {
        for_each_lane(mask, [&](uint32_t lane) {
            uint8_t *destination = out[lane];
            const uint8_t *x = in_a[lane];
            const uint8_t *y = in_b[lane];
            for (uint32_t i = 0; i < r.count; i++)
                store_uint(destination, bools, i,
                    function(load_uint(x, a, i * a_step), load_uint(y, b, i * b_step)) ? 1u : 0u);
        });
    };
    const auto compare_int = [&](auto &&function) {
        for_each_lane(mask, [&](uint32_t lane) {
            uint8_t *destination = out[lane];
            const uint8_t *x = in_a[lane];
            const uint8_t *y = in_b[lane];
            for (uint32_t i = 0; i < r.count; i++)
                store_uint(destination, bools, i,
                    function(load_int(x, a, i * a_step), load_int(y, b, i * b_step)) ? 1u : 0u);
        });
    };

    // The unordered forms are true whenever an operand is NaN, the ordered
    // ones are false; C++ comparisons already give the ordered answer.
    const auto unordered = [](auto &&function) {
        return [function](float x, float y) { return std::isnan(x) || std::isnan(y) || function(x, y); };
    };

    switch (op) {
    case spv::OpDot:
        for_each_lane(mask, [&](uint32_t lane) {
            const uint8_t *x = in_a[lane];
            const uint8_t *y = in_b[lane];
            float sum = 0.0f;
            for (uint32_t i = 0; i < a.count; i++)
                sum += load_float(x, a, i) * load_float(y, b, i);
            store_float(out[lane], r, 0, sum);
        });
        return;
    case spv::OpVectorTimesScalar:
        for_each_lane(mask, [&](uint32_t lane) {
            uint8_t *destination = out[lane];
            const uint8_t *x = in_a[lane];
            const float factor = load_float(in_b[lane], b, 0);
            for (uint32_t i = 0; i < r.count; i++)
                store_float(destination, r, i, load_float(x, a, i) * factor);
        });
        return;
    case spv::OpAny:
    case spv::OpAll: {
        const bool all = (op == spv::OpAll);
        for_each_lane(mask, [&](uint32_t lane) {
            const uint8_t *x = in_a[lane];
            bool value = all;
            for (uint32_t i = 0; i < a.count; i++) {
                const bool component = load_uint(x, a, i) != 0;
                value = all ? (value && component) : (value || component);
            }
            store_uint(out[lane], bools, 0, value ? 1u : 0u);
        });
        return;
    }

    case spv::OpFNegate:
        float_unary([](float x) { return -x; });
        return;
    case spv::OpFAdd:
        float_binary([](float x, float y) { return x + y; });
        return;
    case spv::OpFSub:
        float_binary([](float x, float y) { return x - y; });
        return;
    case spv::OpFMul:
        float_binary([](float x, float y) { return x * y; });
        return;
    case spv::OpFDiv:
        float_binary([](float x, float y) { return x / y; });
        return;
    case spv::OpFRem:
        float_binary([](float x, float y) { return std::fmod(x, y); });
        return;
    case spv::OpFMod:
        // Unlike OpFRem, OpFMod takes the sign of the divisor.
        float_binary([](float x, float y) { return x - y * std::floor(x / y); });
        return;

    case spv::OpSNegate:
        uint_unary([](uint32_t x) { return 0u - x; });
        return;
    case spv::OpNot:
        uint_unary([](uint32_t x) { return ~x; });
        return;
    case spv::OpLogicalNot:
        uint_unary([](uint32_t x) { return x ? 0u : 1u; });
        return;
    case spv::OpIAdd:
        uint_binary([](uint32_t x, uint32_t y) { return x + y; });
        return;
    case spv::OpISub:
        uint_binary([](uint32_t x, uint32_t y) { return x - y; });
        return;
    case spv::OpIMul:
        uint_binary([](uint32_t x, uint32_t y) { return x * y; });
        return;
    case spv::OpUDiv:
        uint_binary([](uint32_t x, uint32_t y) { return y ? x / y : 0u; });
        return;
    case spv::OpUMod:
        uint_binary([](uint32_t x, uint32_t y) { return y ? x % y : 0u; });
        return;
    case spv::OpSDiv:
        int_binary([](int32_t x, int32_t y) { return (y && !(x == INT32_MIN && y == -1)) ? x / y : 0; });
        return;
    case spv::OpSRem:
        int_binary([](int32_t x, int32_t y) { return (y && !(x == INT32_MIN && y == -1)) ? x % y : 0; });
        return;
    case spv::OpSMod:
        int_binary([](int32_t x, int32_t y) {
            if (!y || (x == INT32_MIN && y == -1))
                return 0;
            int32_t value = x % y;
            if (value != 0 && ((value < 0) != (y < 0)))
                value += y;
            return value;
        });
        return;
    case spv::OpShiftLeftLogical:
        uint_binary([](uint32_t x, uint32_t y) { return (y >= 32) ? 0u : (x << y); });
        return;
    case spv::OpShiftRightLogical:
        uint_binary([](uint32_t x, uint32_t y) { return (y >= 32) ? 0u : (x >> y); });
        return;
    case spv::OpShiftRightArithmetic:
        int_binary([](int32_t x, int32_t y) { return (y < 0 || y >= 32) ? (x < 0 ? -1 : 0) : (x >> y); });
        return;
    case spv::OpBitwiseAnd:
    case spv::OpLogicalAnd:
        uint_binary([](uint32_t x, uint32_t y) { return x & y; });
        return;
    case spv::OpBitwiseOr:
    case spv::OpLogicalOr:
        uint_binary([](uint32_t x, uint32_t y) { return x | y; });
        return;
    case spv::OpBitwiseXor:
        uint_binary([](uint32_t x, uint32_t y) { return x ^ y; });
        return;

    case spv::OpLogicalEqual:
    case spv::OpIEqual:
        compare_uint([](uint32_t x, uint32_t y) { return x == y; });
        return;
    case spv::OpLogicalNotEqual:
    case spv::OpINotEqual:
        compare_uint([](uint32_t x, uint32_t y) { return x != y; });
        return;
    case spv::OpULessThan:
        compare_uint([](uint32_t x, uint32_t y) { return x < y; });
        return;
    case spv::OpUGreaterThan:
        compare_uint([](uint32_t x, uint32_t y) { return x > y; });
        return;
    case spv::OpULessThanEqual:
        compare_uint([](uint32_t x, uint32_t y) { return x <= y; });
        return;
    case spv::OpUGreaterThanEqual:
        compare_uint([](uint32_t x, uint32_t y) { return x >= y; });
        return;
    case spv::OpSLessThan:
        compare_int([](int32_t x, int32_t y) { return x < y; });
        return;
    case spv::OpSGreaterThan:
        compare_int([](int32_t x, int32_t y) { return x > y; });
        return;
    case spv::OpSLessThanEqual:
        compare_int([](int32_t x, int32_t y) { return x <= y; });
        return;
    case spv::OpSGreaterThanEqual:
        compare_int([](int32_t x, int32_t y) { return x >= y; });
        return;

    case spv::OpFOrdEqual:
        compare_float([](float x, float y) { return x == y; });
        return;
    case spv::OpFUnordEqual:
        compare_float(unordered([](float x, float y) { return x == y; }));
        return;
    case spv::OpFOrdNotEqual:
        compare_float([](float x, float y) { return x != y && !std::isnan(x) && !std::isnan(y); });
        return;
    case spv::OpFUnordNotEqual:
        compare_float([](float x, float y) { return x != y; });
        return;
    case spv::OpFOrdLessThan:
        compare_float([](float x, float y) { return x < y; });
        return;
    case spv::OpFUnordLessThan:
        compare_float(unordered([](float x, float y) { return x < y; }));
        return;
    case spv::OpFOrdGreaterThan:
        compare_float([](float x, float y) { return x > y; });
        return;
    case spv::OpFUnordGreaterThan:
        compare_float(unordered([](float x, float y) { return x > y; }));
        return;
    case spv::OpFOrdLessThanEqual:
        compare_float([](float x, float y) { return x <= y; });
        return;
    case spv::OpFUnordLessThanEqual:
        compare_float(unordered([](float x, float y) { return x <= y; }));
        return;
    case spv::OpFOrdGreaterThanEqual:
        compare_float([](float x, float y) { return x >= y; });
        return;
    case spv::OpFUnordGreaterThanEqual:
        compare_float(unordered([](float x, float y) { return x >= y; }));
        return;

    case spv::OpIsNan:
        for_each_lane(mask, [&](uint32_t lane) {
            const uint8_t *x = in_a[lane];
            for (uint32_t i = 0; i < r.count; i++)
                store_uint(out[lane], bools, i, std::isnan(load_float(x, a, i * a_step)) ? 1u : 0u);
        });
        return;
    case spv::OpIsInf:
        for_each_lane(mask, [&](uint32_t lane) {
            const uint8_t *x = in_a[lane];
            for (uint32_t i = 0; i < r.count; i++)
                store_uint(out[lane], bools, i, std::isinf(load_float(x, a, i * a_step)) ? 1u : 0u);
        });
        return;

    case spv::OpConvertFToS:
        for_each_lane(mask, [&](uint32_t lane) {
            const uint8_t *x = in_a[lane];
            for (uint32_t i = 0; i < r.count; i++) {
                const float value = load_float(x, a, i * a_step);
                // Out of range is undefined in SPIR-V; saturating keeps it from being undefined in C++ too.
                const int32_t converted = std::isnan(value) ? 0
                    : (value >= 2147483647.0f)              ? INT32_MAX
                    : (value <= -2147483648.0f)             ? INT32_MIN
                                                            : static_cast<int32_t>(value);
                store_uint(out[lane], r, i, static_cast<uint32_t>(converted));
            }
        });
        return;
    case spv::OpConvertFToU:
        for_each_lane(mask, [&](uint32_t lane) {
            const uint8_t *x = in_a[lane];
            for (uint32_t i = 0; i < r.count; i++) {
                const float value = load_float(x, a, i * a_step);
                const uint32_t converted = (std::isnan(value) || value <= 0.0f) ? 0u
                    : (value >= 4294967295.0f)                                  ? UINT32_MAX
                                                                                : static_cast<uint32_t>(value);
                store_uint(out[lane], r, i, converted);
            }
        });
        return;
    case spv::OpConvertSToF:
        for_each_lane(mask, [&](uint32_t lane) {
            const uint8_t *x = in_a[lane];
            for (uint32_t i = 0; i < r.count; i++)
                store_float(out[lane], r, i, static_cast<float>(load_int(x, a, i * a_step)));
        });
        return;
    case spv::OpConvertUToF:
        for_each_lane(mask, [&](uint32_t lane) {
            const uint8_t *x = in_a[lane];
            for (uint32_t i = 0; i < r.count; i++)
                store_float(out[lane], r, i, static_cast<float>(load_uint(x, a, i * a_step)));
        });
        return;
    case spv::OpFConvert:
        float_unary([](float x) { return x; });
        return;
    case spv::OpSConvert:
        for_each_lane(mask, [&](uint32_t lane) {
            const uint8_t *x = in_a[lane];
            for (uint32_t i = 0; i < r.count; i++)
                store_uint(out[lane], r, i, static_cast<uint32_t>(load_int(x, a, i * a_step)));
        });
        return;
    case spv::OpUConvert:
        uint_unary([](uint32_t x) { return x; });
        return;

    default:
        return;
    }
}

void SpirvInterpreter::execute_bit_op(uint32_t op, const uint32_t *operands, uint32_t mask) {
    const SpirvModule &module = *m_module;
    const uint32_t result = operands[1];
    if (!module.register_stride[result])
        return;

    const Scalars r = scalars_of(module, operands[0]);
    const Scalars base_info = scalars_of(module, module.value_type[operands[2]]);
    const uint32_t base_step = (base_info.count > 1) ? 1 : 0;
    const Lanes out = lanes(result);
    const Lanes base = lanes(operands[2]);

    // Bits [offset, offset + bits) of a 32-bit word; the field is cut off at the top rather than shifted out of range.
    const auto field_mask = [](uint32_t offset, uint32_t bits) -> uint32_t {
        if (bits == 0 || offset >= 32)
            return 0;
        const uint32_t low = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
        return low << offset;
    };

    switch (op) {
    case spv::OpBitFieldInsert: {
        const Scalars insert_info = scalars_of(module, module.value_type[operands[3]]);
        const Scalars offset_info = scalars_of(module, module.value_type[operands[4]]);
        const Scalars bits_info = scalars_of(module, module.value_type[operands[5]]);
        const uint32_t insert_step = (insert_info.count > 1) ? 1 : 0;
        const Lanes insert = lanes(operands[3]);
        const Lanes offset = lanes(operands[4]);
        const Lanes bits = lanes(operands[5]);
        for_each_lane(mask, [&](uint32_t lane) {
            const uint32_t at = load_uint(offset[lane], offset_info, 0);
            const uint32_t field = field_mask(at, load_uint(bits[lane], bits_info, 0));
            for (uint32_t i = 0; i < r.count; i++) {
                const uint32_t shifted = field ? (load_uint(insert[lane], insert_info, i * insert_step) << at) : 0;
                const uint32_t value = (load_uint(base[lane], base_info, i * base_step) & ~field) | (shifted & field);
                store_uint(out[lane], r, i, value);
            }
        });
        return;
    }

    case spv::OpBitFieldSExtract:
    case spv::OpBitFieldUExtract: {
        const bool sign_extend = op == spv::OpBitFieldSExtract;
        const Scalars offset_info = scalars_of(module, module.value_type[operands[3]]);
        const Scalars bits_info = scalars_of(module, module.value_type[operands[4]]);
        const Lanes offset = lanes(operands[3]);
        const Lanes bits = lanes(operands[4]);
        for_each_lane(mask, [&](uint32_t lane) {
            const uint32_t at = load_uint(offset[lane], offset_info, 0);
            const uint32_t count = load_uint(bits[lane], bits_info, 0);
            const uint32_t field = field_mask(at, count);
            for (uint32_t i = 0; i < r.count; i++) {
                uint32_t value = field ? ((load_uint(base[lane], base_info, i * base_step) & field) >> at) : 0;
                const uint32_t width = std::min(count, 32u - std::min(at, 32u));
                if (sign_extend && width > 0 && width < 32 && (value & (1u << (width - 1))))
                    value |= ~((1u << width) - 1u);
                store_uint(out[lane], r, i, value);
            }
        });
        return;
    }

    case spv::OpBitReverse:
        for_each_lane(mask, [&](uint32_t lane) {
            for (uint32_t i = 0; i < r.count; i++) {
                uint32_t value = load_uint(base[lane], base_info, i * base_step);
                uint32_t reversed = 0;
                for (uint32_t bit = 0; bit < 32; bit++, value >>= 1)
                    reversed = (reversed << 1) | (value & 1u);
                // A narrower integer reverses within its own width.
                store_uint(out[lane], r, i, (r.width < 32) ? (reversed >> (32 - r.width)) : reversed);
            }
        });
        return;

    case spv::OpBitCount:
        for_each_lane(mask, [&](uint32_t lane) {
            for (uint32_t i = 0; i < r.count; i++)
                store_uint(out[lane], r, i, static_cast<uint32_t>(std::popcount(load_uint(base[lane], base_info, i * base_step))));
        });
        return;

    default:
        return;
    }
}

void SpirvInterpreter::execute_matrix_op(uint32_t op, const uint32_t *operands, uint32_t count, uint32_t mask) {
    const SpirvModule &module = *m_module;
    const uint32_t result_type = operands[0];
    const uint32_t result = operands[1];
    if (!module.register_stride[result])
        return;

    const uint32_t first_id = operands[2];
    const uint32_t second_id = (count > 4) ? operands[3] : first_id;

    // Matrices are column major: a column sits contiguously, columns follow one
    // another with the matrix stride between them.
    struct Shape {
        uint32_t columns = 0;
        uint32_t rows = 0;
        uint32_t stride = 0;
    };
    const auto shape_of = [&](uint32_t type_id, Shape &shape) {
        const SpirvType *matrix = module.type(type_id);
        if (!matrix || matrix->kind != SpirvTypeKind::Matrix)
            return false;
        const SpirvType *column = module.type(matrix->element_type);
        shape.columns = matrix->element_count;
        shape.rows = column ? column->element_count : 0;
        shape.stride = matrix->stride;
        return true;
    };

    const auto element = [](const uint8_t *base, uint32_t stride, uint32_t column, uint32_t row) {
        float value;
        std::memcpy(&value, base + static_cast<size_t>(column) * stride + static_cast<size_t>(row) * sizeof(float),
            sizeof(value));
        return value;
    };
    const auto set_element = [](uint8_t *base, uint32_t stride, uint32_t column, uint32_t row, float value) {
        std::memcpy(base + static_cast<size_t>(column) * stride + static_cast<size_t>(row) * sizeof(float), &value,
            sizeof(value));
    };
    const auto vector_element = [](const uint8_t *base, uint32_t index) {
        float value;
        std::memcpy(&value, base + static_cast<size_t>(index) * sizeof(float), sizeof(value));
        return value;
    };

    switch (op) {
    case spv::OpMatrixTimesVector: {
        Shape shape;
        if (!shape_of(module.value_type[first_id], shape))
            return;
        for_each_lane(mask, [&](uint32_t lane) {
            const uint8_t *matrix = reg(first_id, lane);
            const uint8_t *vector = reg(second_id, lane);
            uint8_t *destination = reg(result, lane);
            for (uint32_t row = 0; row < shape.rows; row++) {
                float sum = 0.0f;
                for (uint32_t column = 0; column < shape.columns; column++)
                    sum += element(matrix, shape.stride, column, row) * vector_element(vector, column);
                std::memcpy(destination + static_cast<size_t>(row) * sizeof(float), &sum, sizeof(sum));
            }
        });
        break;
    }

    case spv::OpVectorTimesMatrix: {
        Shape shape;
        if (!shape_of(module.value_type[second_id], shape))
            return;
        for_each_lane(mask, [&](uint32_t lane) {
            const uint8_t *vector = reg(first_id, lane);
            const uint8_t *matrix = reg(second_id, lane);
            uint8_t *destination = reg(result, lane);
            for (uint32_t column = 0; column < shape.columns; column++) {
                float sum = 0.0f;
                for (uint32_t row = 0; row < shape.rows; row++)
                    sum += element(matrix, shape.stride, column, row) * vector_element(vector, row);
                std::memcpy(destination + static_cast<size_t>(column) * sizeof(float), &sum, sizeof(sum));
            }
        });
        break;
    }

    case spv::OpMatrixTimesMatrix: {
        Shape left, right, output;
        if (!shape_of(module.value_type[first_id], left) || !shape_of(module.value_type[second_id], right)
            || !shape_of(result_type, output))
            return;
        for_each_lane(mask, [&](uint32_t lane) {
            const uint8_t *x = reg(first_id, lane);
            const uint8_t *y = reg(second_id, lane);
            uint8_t *destination = reg(result, lane);
            for (uint32_t column = 0; column < output.columns; column++) {
                for (uint32_t row = 0; row < output.rows; row++) {
                    float sum = 0.0f;
                    for (uint32_t k = 0; k < left.columns; k++)
                        sum += element(x, left.stride, k, row) * element(y, right.stride, column, k);
                    set_element(destination, output.stride, column, row, sum);
                }
            }
        });
        break;
    }

    case spv::OpMatrixTimesScalar: {
        Shape shape;
        if (!shape_of(result_type, shape))
            return;
        for_each_lane(mask, [&](uint32_t lane) {
            const uint8_t *matrix = reg(first_id, lane);
            const float factor = vector_element(reg(second_id, lane), 0);
            uint8_t *destination = reg(result, lane);
            for (uint32_t column = 0; column < shape.columns; column++) {
                for (uint32_t row = 0; row < shape.rows; row++)
                    set_element(destination, shape.stride, column, row, element(matrix, shape.stride, column, row) * factor);
            }
        });
        break;
    }

    case spv::OpTranspose: {
        Shape source, output;
        if (!shape_of(module.value_type[first_id], source) || !shape_of(result_type, output))
            return;
        for_each_lane(mask, [&](uint32_t lane) {
            const uint8_t *matrix = reg(first_id, lane);
            uint8_t *destination = reg(result, lane);
            for (uint32_t column = 0; column < output.columns; column++) {
                for (uint32_t row = 0; row < output.rows; row++)
                    set_element(destination, output.stride, column, row, element(matrix, source.stride, row, column));
            }
        });
        break;
    }

    default:
        break;
    }
}

void SpirvInterpreter::execute_ext_inst(const uint32_t *operands, uint32_t count, uint32_t mask) {
    const SpirvModule &module = *m_module;
    const uint32_t result = operands[1];
    const uint32_t instruction = operands[3];
    if (!module.register_stride[result])
        return;

    const Scalars r = scalars_of(module, operands[0]);

    // Up to three arguments cover every GLSL.std.450 instruction the recompiler
    // emits.
    const uint32_t argument_count = count - 5;
    uint32_t argument_ids[3] = { 0, 0, 0 };
    Scalars argument_info[3];
    uint32_t steps[3] = { 0, 0, 0 };
    for (uint32_t i = 0; i < 3 && i < argument_count; i++) {
        argument_ids[i] = operands[4 + i];
        argument_info[i] = scalars_of(module, module.value_type[argument_ids[i]]);
        steps[i] = (argument_info[i].count > 1) ? 1 : 0;
    }
    if (argument_count == 0)
        return;
    const Lanes out = lanes(result);
    const Lanes args[3] = { lanes(argument_ids[0]), lanes(argument_ids[1]), lanes(argument_ids[2]) };

    const auto unary = [&](auto &&function) {
        const bool fast = r.width == 32 && argument_info[0].width == 32;
        for_each_lane(mask, [&](uint32_t lane) {
            uint8_t *destination = out[lane];
            const uint8_t *x = args[0][lane];
            if (fast) {
                for (uint32_t i = 0; i < r.count; i++) {
                    float value;
                    std::memcpy(&value, x + i * steps[0] * 4, 4);
                    value = function(value);
                    std::memcpy(destination + i * 4, &value, 4);
                }
                return;
            }
            for (uint32_t i = 0; i < r.count; i++)
                store_float(destination, r, i, function(load_float(x, argument_info[0], i * steps[0])));
        });
    };
    const auto binary = [&](auto &&function) {
        for_each_lane(mask, [&](uint32_t lane) {
            uint8_t *destination = out[lane];
            const uint8_t *x = args[0][lane];
            const uint8_t *y = args[1][lane];
            for (uint32_t i = 0; i < r.count; i++)
                store_float(destination, r, i,
                    function(load_float(x, argument_info[0], i * steps[0]), load_float(y, argument_info[1], i * steps[1])));
        });
    };
    const auto ternary = [&](auto &&function) {
        for_each_lane(mask, [&](uint32_t lane) {
            uint8_t *destination = out[lane];
            const uint8_t *x = args[0][lane];
            const uint8_t *y = args[1][lane];
            const uint8_t *z = args[2][lane];
            for (uint32_t i = 0; i < r.count; i++)
                store_float(destination, r, i,
                    function(load_float(x, argument_info[0], i * steps[0]), load_float(y, argument_info[1], i * steps[1]),
                        load_float(z, argument_info[2], i * steps[2])));
        });
    };
    const auto int_unary = [&](auto &&function) {
        for_each_lane(mask, [&](uint32_t lane) {
            uint8_t *destination = out[lane];
            const uint8_t *x = args[0][lane];
            for (uint32_t i = 0; i < r.count; i++)
                store_uint(destination, r, i, static_cast<uint32_t>(function(load_int(x, argument_info[0], i * steps[0]))));
        });
    };
    const auto int_binary = [&](auto &&function) {
        for_each_lane(mask, [&](uint32_t lane) {
            uint8_t *destination = out[lane];
            const uint8_t *x = args[0][lane];
            const uint8_t *y = args[1][lane];
            for (uint32_t i = 0; i < r.count; i++)
                store_uint(destination, r, i,
                    static_cast<uint32_t>(function(load_int(x, argument_info[0], i * steps[0]),
                        load_int(y, argument_info[1], i * steps[1]))));
        });
    };
    const auto int_ternary = [&](auto &&function) {
        for_each_lane(mask, [&](uint32_t lane) {
            uint8_t *destination = out[lane];
            const uint8_t *x = args[0][lane];
            const uint8_t *y = args[1][lane];
            const uint8_t *z = args[2][lane];
            for (uint32_t i = 0; i < r.count; i++)
                store_uint(destination, r, i,
                    static_cast<uint32_t>(function(load_int(x, argument_info[0], i * steps[0]),
                        load_int(y, argument_info[1], i * steps[1]), load_int(z, argument_info[2], i * steps[2]))));
        });
    };
    const auto length_of = [&](const uint8_t *x) {
        float sum = 0.0f;
        for (uint32_t i = 0; i < argument_info[0].count; i++) {
            const float value = load_float(x, argument_info[0], i);
            sum += value * value;
        }
        return std::sqrt(sum);
    };

    switch (instruction) {
    case GLSLstd450Length:
        for_each_lane(mask, [&](uint32_t lane) {
            store_float(out[lane], r, 0, length_of(args[0][lane]));
        });
        return;
    case GLSLstd450Distance:
        for_each_lane(mask, [&](uint32_t lane) {
            const uint8_t *x = args[0][lane];
            const uint8_t *y = args[1][lane];
            float sum = 0.0f;
            for (uint32_t i = 0; i < argument_info[0].count; i++) {
                const float difference = load_float(x, argument_info[0], i) - load_float(y, argument_info[1], i);
                sum += difference * difference;
            }
            store_float(out[lane], r, 0, std::sqrt(sum));
        });
        return;
    case GLSLstd450Normalize:
        for_each_lane(mask, [&](uint32_t lane) {
            const uint8_t *x = args[0][lane];
            const float length = length_of(x);
            const float scale = (length > 0.0f) ? (1.0f / length) : 0.0f;
            for (uint32_t i = 0; i < r.count; i++)
                store_float(out[lane], r, i, load_float(x, argument_info[0], i) * scale);
        });
        return;
    case GLSLstd450Cross:
        for_each_lane(mask, [&](uint32_t lane) {
            const uint8_t *x = args[0][lane];
            const uint8_t *y = args[1][lane];
            const float ax = load_float(x, argument_info[0], 0), ay = load_float(x, argument_info[0], 1), az = load_float(x, argument_info[0], 2);
            const float bx = load_float(y, argument_info[1], 0), by = load_float(y, argument_info[1], 1), bz = load_float(y, argument_info[1], 2);
            uint8_t *destination = out[lane];
            store_float(destination, r, 0, ay * bz - az * by);
            store_float(destination, r, 1, az * bx - ax * bz);
            store_float(destination, r, 2, ax * by - ay * bx);
        });
        return;
    case GLSLstd450Reflect:
        for_each_lane(mask, [&](uint32_t lane) {
            const uint8_t *x = args[0][lane];
            const uint8_t *y = args[1][lane];
            float dot = 0.0f;
            for (uint32_t i = 0; i < argument_info[0].count; i++)
                dot += load_float(x, argument_info[0], i) * load_float(y, argument_info[1], i);
            for (uint32_t i = 0; i < r.count; i++)
                store_float(out[lane], r, i,
                    load_float(x, argument_info[0], i) - 2.0f * dot * load_float(y, argument_info[1], i));
        });
        return;
    case GLSLstd450FaceForward:
        for_each_lane(mask, [&](uint32_t lane) {
            const uint8_t *n = args[0][lane];
            const uint8_t *incident = args[1][lane];
            const uint8_t *reference = args[2][lane];
            float dot = 0.0f;
            for (uint32_t i = 0; i < argument_info[1].count; i++)
                dot += load_float(incident, argument_info[1], i) * load_float(reference, argument_info[2], i);
            for (uint32_t i = 0; i < r.count; i++) {
                const float value = load_float(n, argument_info[0], i);
                store_float(out[lane], r, i, (dot < 0.0f) ? value : -value);
            }
        });
        return;
    case GLSLstd450PackHalf2x16:
        for_each_lane(mask, [&](uint32_t lane) {
            const uint8_t *x = args[0][lane];
            const uint32_t packed = float_to_half(load_float(x, argument_info[0], 0))
                | (static_cast<uint32_t>(float_to_half(load_float(x, argument_info[0], 1))) << 16);
            store_uint(out[lane], r, 0, packed);
        });
        return;
    case GLSLstd450UnpackHalf2x16:
        for_each_lane(mask, [&](uint32_t lane) {
            const uint32_t packed = load_uint(args[0][lane], argument_info[0], 0);
            uint8_t *destination = out[lane];
            store_float(destination, r, 0, half_to_float(static_cast<uint16_t>(packed & 0xFFFF)));
            store_float(destination, r, 1, half_to_float(static_cast<uint16_t>(packed >> 16)));
        });
        return;

    case GLSLstd450FAbs:
        unary([](float x) { return std::fabs(x); });
        return;
    case GLSLstd450FSign:
        unary([](float x) { return (x > 0.0f) ? 1.0f : ((x < 0.0f) ? -1.0f : 0.0f); });
        return;
    case GLSLstd450Floor:
        unary([](float x) { return std::floor(x); });
        return;
    case GLSLstd450Ceil:
        unary([](float x) { return std::ceil(x); });
        return;
    case GLSLstd450Fract:
        unary([](float x) { return x - std::floor(x); });
        return;
    case GLSLstd450Trunc:
        unary([](float x) { return std::trunc(x); });
        return;
    case GLSLstd450Round:
    case GLSLstd450RoundEven:
        unary([](float x) { return std::nearbyint(x); });
        return;
    case GLSLstd450Radians:
        unary([](float x) { return x * 0.01745329251f; });
        return;
    case GLSLstd450Degrees:
        unary([](float x) { return x * 57.2957795131f; });
        return;
    case GLSLstd450Sin:
        unary([](float x) { return std::sin(x); });
        return;
    case GLSLstd450Cos:
        unary([](float x) { return std::cos(x); });
        return;
    case GLSLstd450Tan:
        unary([](float x) { return std::tan(x); });
        return;
    case GLSLstd450Asin:
        unary([](float x) { return std::asin(std::clamp(x, -1.0f, 1.0f)); });
        return;
    case GLSLstd450Acos:
        unary([](float x) { return std::acos(std::clamp(x, -1.0f, 1.0f)); });
        return;
    case GLSLstd450Atan:
        unary([](float x) { return std::atan(x); });
        return;
    case GLSLstd450Sinh:
        unary([](float x) { return std::sinh(x); });
        return;
    case GLSLstd450Cosh:
        unary([](float x) { return std::cosh(x); });
        return;
    case GLSLstd450Tanh:
        unary([](float x) { return std::tanh(x); });
        return;
    case GLSLstd450Exp:
        unary([](float x) { return std::exp(x); });
        return;
    case GLSLstd450Exp2:
        unary([](float x) { return std::exp2(x); });
        return;
    case GLSLstd450Log:
        unary([](float x) { return std::log(x); });
        return;
    case GLSLstd450Log2:
        unary([](float x) { return std::log2(x); });
        return;
    case GLSLstd450Sqrt:
        unary([](float x) { return std::sqrt(x); });
        return;
    case GLSLstd450InverseSqrt:
        unary([](float x) { return (x > 0.0f) ? (1.0f / std::sqrt(x)) : 0.0f; });
        return;

    case GLSLstd450Atan2:
        binary([](float y, float x) { return std::atan2(y, x); });
        return;
    case GLSLstd450Pow:
        binary([](float x, float y) { return std::pow(x, y); });
        return;
    case GLSLstd450FMin:
    case GLSLstd450NMin:
        binary([](float x, float y) { return std::min(x, y); });
        return;
    case GLSLstd450FMax:
    case GLSLstd450NMax:
        binary([](float x, float y) { return std::max(x, y); });
        return;
    case GLSLstd450Step:
        binary([](float edge, float x) { return (x < edge) ? 0.0f : 1.0f; });
        return;

    case GLSLstd450FClamp:
    case GLSLstd450NClamp:
        // std::clamp asserts on an inverted range, which a shader is allowed to hand over.
        ternary([](float x, float low, float high) { return std::min(std::max(x, low), high); });
        return;
    case GLSLstd450FMix:
        ternary([](float x, float y, float factor) { return x * (1.0f - factor) + y * factor; });
        return;
    case GLSLstd450SmoothStep:
        ternary([](float edge0, float edge1, float x) {
            const float value = std::min(std::max((x - edge0) / (edge1 - edge0), 0.0f), 1.0f);
            return value * value * (3.0f - 2.0f * value);
        });
        return;
    case GLSLstd450Fma:
        ternary([](float x, float y, float z) { return x * y + z; });
        return;

    case GLSLstd450SAbs:
        int_unary([](int32_t x) { return (x < 0) ? static_cast<int32_t>(0u - static_cast<uint32_t>(x)) : x; });
        return;
    case GLSLstd450SSign:
        int_unary([](int32_t x) { return (x > 0) - (x < 0); });
        return;
    case GLSLstd450SMin:
        int_binary([](int32_t x, int32_t y) { return std::min(x, y); });
        return;
    case GLSLstd450SMax:
        int_binary([](int32_t x, int32_t y) { return std::max(x, y); });
        return;
    case GLSLstd450UMin:
        int_binary([](int32_t x, int32_t y) {
            return static_cast<int32_t>(std::min(static_cast<uint32_t>(x), static_cast<uint32_t>(y)));
        });
        return;
    case GLSLstd450UMax:
        int_binary([](int32_t x, int32_t y) {
            return static_cast<int32_t>(std::max(static_cast<uint32_t>(x), static_cast<uint32_t>(y)));
        });
        return;
    case GLSLstd450SClamp:
        int_ternary([](int32_t x, int32_t low, int32_t high) { return std::min(std::max(x, low), high); });
        return;
    case GLSLstd450UClamp:
        int_ternary([](int32_t x, int32_t low, int32_t high) {
            return static_cast<int32_t>(std::min(std::max(static_cast<uint32_t>(x), static_cast<uint32_t>(low)),
                static_cast<uint32_t>(high)));
        });
        return;
    case GLSLstd450FindUMsb:
        int_unary([](int32_t x) { return x ? 31 - std::countl_zero(static_cast<uint32_t>(x)) : -1; });
        return;
    case GLSLstd450FindSMsb:
        // For a negative value the answer is the highest bit that differs from the sign.
        int_unary([](int32_t x) {
            const uint32_t bits = static_cast<uint32_t>((x < 0) ? ~x : x);
            return bits ? 31 - std::countl_zero(bits) : -1;
        });
        return;
    case GLSLstd450FindILsb:
        int_unary([](int32_t x) { return x ? std::countr_zero(static_cast<uint32_t>(x)) : -1; });
        return;

    default:
        LOG_WARN_ONCE("Software renderer: unimplemented GLSL.std.450 instruction {}", instruction);
        for_each_lane(mask, [&](uint32_t lane) { std::memset(out[lane], 0, module.value_size[result]); });
        return;
    }
}

void SpirvInterpreter::execute_image_op(uint32_t op, const uint32_t *operands, uint32_t count, uint32_t mask) {
    const SpirvModule &module = *m_module;
    const uint32_t result = operands[1];
    if (!module.register_stride[result])
        return;

    const Scalars result_info = scalars_of(module, operands[0]);
    const uint32_t image_id = operands[2];
    const uint32_t coordinate_id = operands[3];
    const Scalars coordinate_info = scalars_of(module, module.value_type[coordinate_id]);
    const uint32_t coordinate_count = std::min(coordinate_info.count, 4u);
    const bool projective = (op == spv::OpImageSampleProjImplicitLod || op == spv::OpImageSampleProjExplicitLod);

    // An explicit level of detail arrives as an image operand.
    uint32_t lod_id = 0;
    if (count > 6 && (operands[4] & spv::ImageOperandsLodMask))
        lod_id = operands[5];
    const Scalars lod_info = lod_id ? scalars_of(module, module.value_type[lod_id]) : Scalars{};

    // Fetches and subpass reads address texels directly, which the sampler
    // callback tells apart by the missing normalized coordinates.
    const bool direct = (op == spv::OpImageFetch || op == spv::OpImageRead);

    float coordinates[SPIRV_MAX_LANES][4] = {};
    float lods[SPIRV_MAX_LANES] = {};
    float sampled[SPIRV_MAX_LANES][4] = {};
    ImageRepr descriptors[SPIRV_MAX_LANES] = {};
    uint32_t used = coordinate_count;

    for_each_lane(mask, [&](uint32_t lane) {
        std::memcpy(&descriptors[lane], reg(image_id, lane), sizeof(ImageRepr));

        const uint8_t *coordinate = reg(coordinate_id, lane);
        for (uint32_t i = 0; i < coordinate_count; i++) {
            coordinates[lane][i] = (coordinate_info.kind == SpirvTypeKind::Float)
                ? load_float(coordinate, coordinate_info, i)
                : static_cast<float>(load_int(coordinate, coordinate_info, i));
        }

        // The projective forms divide through by the last coordinate.
        if (projective && coordinate_count > 0) {
            const float divisor = coordinates[lane][coordinate_count - 1];
            if (divisor != 0.0f) {
                for (uint32_t i = 0; i + 1 < coordinate_count; i++)
                    coordinates[lane][i] /= divisor;
            }
        }

        if (lod_id) {
            const uint8_t *lod_data = reg(lod_id, lane);
            lods[lane] = (lod_info.kind == SpirvTypeKind::Float) ? load_float(lod_data, lod_info, 0)
                                                                 : static_cast<float>(load_int(lod_data, lod_info, 0));
        }
        sampled[lane][3] = 1.0f;
    });
    if (projective && used > 0)
        used--;

    // Lanes normally all name the same image; any that do not are sampled in a group of their own.
    if (m_bindings && m_bindings->sample_texture) {
        uint32_t remaining = mask;
        while (remaining) {
            const ImageRepr &descriptor = descriptors[lowest_lane(remaining)];
            uint32_t group = 0;
            for_each_lane(remaining, [&](uint32_t lane) {
                if (descriptors[lane].descriptor_set == descriptor.descriptor_set && descriptors[lane].binding == descriptor.binding)
                    group |= 1u << lane;
            });
            remaining &= ~group;
            m_bindings->sample_texture(descriptor.descriptor_set, descriptor.binding, group, &coordinates[0][0], used, lods,
                lod_id != 0 || direct, &sampled[0][0]);
        }
    }

    const Lanes destination = lanes(result);
    const uint32_t components = std::min(result_info.count, 4u);
    for_each_lane(mask, [&](uint32_t lane) {
        std::memset(destination[lane], 0, module.value_size[result]);
        if (result_info.kind == SpirvTypeKind::Float) {
            for (uint32_t i = 0; i < components; i++)
                store_float(destination[lane], result_info, i, sampled[lane][i]);
        } else {
            for (uint32_t i = 0; i < components; i++)
                store_uint(destination[lane], result_info, i, static_cast<uint32_t>(sampled[lane][i]));
        }
    });
}

} // namespace renderer::software
