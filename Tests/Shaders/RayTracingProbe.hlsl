// RayTracingProbe
// Minimal ray tracing library for RtptVulkanBootstrapTests. Traces two rays against a single triangle and writes a marker per ray, so the test can confirm that hits and misses reach the right shader.

// Top-level acceleration structure containing one instance of the triangle spanning (0,0,0), (1,0,0), and (0,1,0).
[[vk::binding(0, 0)]] RaytracingAccelerationStructure Scene;

// One marker per ray, read back by the test and compared against 0x1234 and 0x5678.
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> Results;

// Payload
// Carries the marker from whichever shader handled the ray back to raygenMain.

struct Payload
{
  // 0x1234 when the closest hit shader ran, 0x5678 when the miss shader ran.
  uint marker;
};

[shader("raygeneration")]
void raygenMain()
{
  // Rays
  // Ray 0 starts over the triangle's interior and must hit it; ray 1 starts outside its bounds and must miss.
  // Both travel along +Z from z = -1 toward the triangle in the z = 0 plane.

  const uint index = DispatchRaysIndex().x;

  RayDesc ray;

  ray.Origin    = index == 0 ? float3(0.25F, 0.25F, -1.0F) : float3(2.0F, 2.0F, -1.0F);
  ray.Direction = float3(0.0F, 0.0F, 1.0F);
  ray.TMin      = 0.001F;
  ray.TMax      = 100.0F;

  // Trace
  // Shader binding table offset, geometry multiplier, and miss index are all zero because the probe has a single hit group and a single miss shader.

  Payload payload = { 0 };

  TraceRay(Scene, RAY_FLAG_NONE, 0xff, 0, 0, 0, ray, payload);

  Results[index] = payload.marker;
}

[shader("miss")]
void missMain(inout Payload payload)
{
  payload.marker = 0x5678u;
}

[shader("closesthit")]
void closestHitMain(inout Payload payload, in BuiltInTriangleIntersectionAttributes attributes)
{
  payload.marker = 0x1234u;
}
