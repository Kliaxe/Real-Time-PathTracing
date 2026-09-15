#ifndef RTPT_SHADER_TYPES_H
#define RTPT_SHADER_TYPES_H

// Language bridge
// This is the single language bridge for every CPU/GPU ABI header owned by the renderer.
// Shared structs stay ordinary C-like declarations; language-specific syntax is confined to these macros.

#ifdef __cplusplus

#include <cstdint>
#include <glm/glm.hpp>

// C++ side
// Shared structs live in the shaderio namespace, member defaults become initializers, and device addresses are typed pointers.

#define NAMESPACE_SHADERIO_BEGIN() namespace shaderio {
#define NAMESPACE_SHADERIO_END() }  // namespace shaderio
#define RTPT_DEFAULT(value) = (value)
#define RTPT_SCENE_ADDRESS(type) type*
#define RTPT_BUFFER_POINTER(type) type*

NAMESPACE_SHADERIO_BEGIN()

// HLSL type names
// Aliases map HLSL spellings onto GLM so shared declarations compile unchanged in C++.

using float4x4 = glm::mat4;
using float4x3 = glm::mat4x3;
using float3x4 = glm::mat3x4;
using float3x3 = glm::mat3;
using float2x2 = glm::mat2;
using float2x3 = glm::mat2x3;
using float3x2 = glm::mat3x2;

using float2 = glm::vec2;
using float3 = glm::vec3;
using float4 = glm::vec4;
using int2   = glm::ivec2;
using int3   = glm::ivec3;
using int4   = glm::ivec4;
using uint   = unsigned int;
using uint2  = glm::uvec2;
using uint3  = glm::uvec3;
using uint4  = glm::uvec4;
using bool2  = glm::bvec2;
using bool3  = glm::bvec3;
using bool4  = glm::bvec4;

// HLSL intrinsics
// CPU versions of HLSL functions used by shared code. mul reverses GLM's operand order, so an expression written in HLSL order has the same meaning in both languages.
// That holds only because DXC compiles with -Zpr (CMake/CompileHlsl.cmake): HLSL then reads GLM's column-major memory as rows, so a shader matrix is the transpose of its GLM counterpart and HLSL's mul(v, M) equals GLM's M * v.

template <typename T>
T lerp(T a, T b, T t)
{
  return glm::mix(a, b, t);
}

template <glm::length_t N, typename ScalarType, glm::qualifier Precision>
glm::vec<N, ScalarType, Precision> mul(glm::vec<N, ScalarType, Precision> vector, glm::mat<N, N, ScalarType, Precision> matrix)
{
  return matrix * vector;
}

template <glm::length_t N, typename ScalarType, glm::qualifier Precision>
glm::vec<N, ScalarType, Precision> mul(glm::mat<N, N, ScalarType, Precision> matrix, glm::vec<N, ScalarType, Precision> vector)
{
  return vector * matrix;
}

template <glm::length_t N, typename ScalarType, glm::qualifier Precision>
glm::mat<N, N, ScalarType, Precision> mul(glm::mat<N, N, ScalarType, Precision> left, glm::mat<N, N, ScalarType, Precision> right)
{
  return right * left;
}

NAMESPACE_SHADERIO_END()

#elif defined(RTPT_HLSL)

// HLSL side
// No namespace and no member defaults; device addresses are raw 64-bit integers and buffer pointers use DXC's vk::BufferPointer.

#define NAMESPACE_SHADERIO_BEGIN()
#define NAMESPACE_SHADERIO_END()
#define RTPT_DEFAULT(value)
#define RTPT_SCENE_ADDRESS(type) uint64_t
#define RTPT_BUFFER_POINTER(type) vk::BufferPointer<type>

// Shared headers spell unsigned integers as uint32_t, which HLSL does not define.
#ifndef uint32_t
#define uint32_t uint
#endif

#else
#error "ShaderTypes.h requires C++ or DXC HLSL."
#endif

#endif  // RTPT_SHADER_TYPES_H
