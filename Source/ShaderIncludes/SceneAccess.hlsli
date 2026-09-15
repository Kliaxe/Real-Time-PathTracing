#ifndef RTPT_SCENE_ACCESS_HLSLI
#define RTPT_SCENE_ACCESS_HLSLI

#include "Common/IoGltf.h"

// Reads one element of a scene array uploaded as a device buffer.
template<typename T>
T LoadDeviceArrayElement(uint64_t baseAddress, uint index)
{
  // Scene arrays are uploaded with at least 8-byte base alignment, and the CPU asserts that every shared element size is a multiple of eight.
  return vk::RawBufferLoad<T>(baseAddress + uint64_t(index) * uint64_t(sizeof(T)), 8);
}

// Reads one vertex attribute straight from the uploaded glTF byte buffer through its buffer view.
template<typename T>
T LoadAttribute(uint64_t dataAddress, BufferView view, uint attributeIndex)
{
  // An absent attribute reads as all ones instead of dereferencing an empty view.
  if(view.count == 0)
  {
    return (T)1.0f;
  }

  const uint64_t address = dataAddress + uint64_t(view.offset) + uint64_t(attributeIndex) * uint64_t(view.byteStride);

  return vk::RawBufferLoad<T>(address);
}

// Reads a triangle's three vertex indices. The importer stores the index element size as byteStride, so one triangle spans three strides.
uint3 LoadTriangleIndices(uint64_t dataAddress, TriangleMesh mesh, uint primitiveIndex)
{
  const uint64_t address = dataAddress + uint64_t(mesh.indices.offset) + uint64_t(primitiveIndex) * uint64_t(mesh.indices.byteStride) * 3u;

  // glTF only guarantees that a 16-bit index view starts on a 2-byte boundary, so its load must not promise the default alignment of 4.
  if(mesh.indices.byteStride == 2u)
  {
    return uint3(vk::RawBufferLoad<uint16_t3>(address, 2));
  }

  if(mesh.indices.byteStride == 4u)
  {
    return vk::RawBufferLoad<uint3>(address, 4);
  }

  // Any other index width is unsupported and collapses to a degenerate triangle.
  return uint3(0u, 0u, 0u);
}

// Interpolates a vertex attribute across a triangle with the hit's barycentric weights.
template<typename T>
T LoadTriangleAttribute(uint64_t dataAddress, BufferView view, uint3 indices, float3 barycentrics)
{
  const T attribute0 = LoadAttribute<T>(dataAddress, view, indices.x);
  const T attribute1 = LoadAttribute<T>(dataAddress, view, indices.y);
  const T attribute2 = LoadAttribute<T>(dataAddress, view, indices.z);

  return (T)barycentrics.x * attribute0 + (T)barycentrics.y * attribute1 + (T)barycentrics.z * attribute2;
}

#endif  // RTPT_SCENE_ACCESS_HLSLI
