#ifndef RESTIR_PT_SHIFT_H
#define RESTIR_PT_SHIFT_H

#include "ShaderIncludes/ReSTIR/PT/Candidate.h.slang"
#include "ShaderIncludes/ReSTIR/Common/RayQueryVisibility.h.slang"

bool AreSurfacesCompatibleForReuse(ReSTIRPTPrimarySurface sourceSurface, ReSTIRPTPrimarySurface targetSurface)
{
  // The thesis baseline keeps the receiver compatibility test simple and
  // explicit instead of reproducing multiple sample-specific modes.
  if(!IsValidPrimarySurface(sourceSurface) || !IsValidPrimarySurface(targetSurface))
  {
    return false;
  }

  if(sourceSurface.materialIndex != targetSurface.materialIndex)
  {
    return false;
  }

  if(abs(sourceSurface.roughness - targetSurface.roughness) > pushConst.reconnection.roughnessThreshold)
  {
    return false;
  }

  if(pushConst.reconnection.distanceThreshold > 0.0
     && distance(sourceSurface.worldPosition, targetSurface.worldPosition) > pushConst.reconnection.distanceThreshold)
  {
    return false;
  }

  return true;
}

float ComputeReconnectionJacobian(float3 sourceToPoint, float3 targetToPoint, float3 reconnectionNormal)
{
  // Reconnection changes the geometry term between receiver and reused path
  // endpoint, so the shifted candidate needs a Jacobian correction.
  const float sourceDistanceSquared = max(dot(sourceToPoint, sourceToPoint), 1.0e-6);
  const float targetDistanceSquared = max(dot(targetToPoint, targetToPoint), 1.0e-6);
  const float sourceCos             = abs(dot(normalize(reconnectionNormal), -normalize(sourceToPoint)));
  const float targetCos             = abs(dot(normalize(reconnectionNormal), -normalize(targetToPoint)));
  if(sourceCos <= 1.0e-5 || targetCos <= 1.0e-5)
  {
    return 0.0;
  }

  return (targetCos / targetDistanceSquared) / (sourceCos / sourceDistanceSquared);
}

ReSTIRPTReservoir ShiftReservoirToSurface(ReSTIRPTReservoir sourceReservoir,
                                          ReSTIRPTPrimarySurface sourceSurface,
                                          ReSTIRPTPrimarySurface targetSurface,
                                          float3 cameraPosition,
                                          out float shiftJacobian,
                                          out uint shiftStatus)
{
  // Temporal and spatial reuse both pass through this function so the shift
  // contract stays identical across reuse dimensions.
  shiftJacobian = 1.0;
  shiftStatus   = uint(ReSTIRShiftStatus::eReSTIRShiftStatusAccepted);

  ReSTIRPTReservoir shiftedReservoir = sourceReservoir;
  if(!IsValidReservoir(sourceReservoir) || !IsValidPrimarySurface(sourceSurface) || !IsValidPrimarySurface(targetSurface))
  {
    shiftStatus = uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedSurface);
    shiftedReservoir.valid = 0;
    return shiftedReservoir;
  }

  if(!AreSurfacesCompatibleForReuse(sourceSurface, targetSurface))
  {
    shiftStatus = uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedSurface);
    shiftedReservoir.valid = 0;
    return shiftedReservoir;
  }

  float3 shiftedWorldDirection = GetReservoirWorldOutgoingDirection(sourceSurface, sourceReservoir);
  if(dot(shiftedWorldDirection, shiftedWorldDirection) <= 0.0)
  {
    shiftStatus = uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedDirection);
    shiftedReservoir.valid = 0;
    return shiftedReservoir;
  }

  if(sourceReservoir.reconnectionValid != 0u)
  {
    // If the candidate stores a first-bounce endpoint, reconnect from the
    // target receiver to that endpoint instead of reusing the old direction.
    const float3 sourceToPoint = sourceReservoir.reconnectionPoint - sourceSurface.worldPosition;
    const float3 targetToPoint = sourceReservoir.reconnectionPoint - targetSurface.worldPosition;
    const float  targetDistanceSquared = dot(targetToPoint, targetToPoint);
    if(targetDistanceSquared <= 1.0e-8)
    {
      shiftStatus = uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedDirection);
      shiftedReservoir.valid = 0;
      return shiftedReservoir;
    }

    shiftedWorldDirection = normalize(targetToPoint);
    if(abs(dot(targetSurface.geometricNormal, shiftedWorldDirection)) <= 1.0e-6)
    {
      shiftStatus = uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedDirection);
      shiftedReservoir.valid = 0;
      return shiftedReservoir;
    }

    const float targetDistance = sqrt(targetDistanceSquared);
    if(IsShiftVisibilityValidationEnabled()
       && !TraceReSTIRPTVisibility(targetSurface.worldPosition, targetSurface.geometricNormal, shiftedWorldDirection,
                                   max(targetDistance - 1.0e-3, 0.0)))
    {
      // Rejection here keeps occluded shifted paths out of the reservoir before
      // they can bias temporal or spatial reuse.
      shiftStatus = uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedVisibility);
      shiftedReservoir.valid = 0;
      return shiftedReservoir;
    }

    shiftJacobian = ComputeReconnectionJacobian(sourceToPoint, targetToPoint, sourceReservoir.reconnectionNormal);
    if(!(shiftJacobian > 0.0) || isnan(shiftJacobian) || isinf(shiftJacobian))
    {
      shiftStatus = uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedTargetPdf);
      shiftedReservoir.valid = 0;
      return shiftedReservoir;
    }

    shiftedReservoir.cachedIncidentRadiance *= shiftJacobian;
  }

  shiftedReservoir.localOutgoingDirection = normalize(ToLocal(targetSurface.shadingNormal, targetSurface.tangent, targetSurface.bitangent,
                                                              shiftedWorldDirection));
  // After geometric shifting, the candidate must be re-scored for the target
  // receiver before RIS can combine it.
  shiftedReservoir.targetPdf = EvaluateReservoirCandidateTarget(targetSurface, shiftedReservoir, cameraPosition);
  shiftedReservoir.valid     = shiftedReservoir.targetPdf > 0.0 ? 1u : 0u;
  if(shiftedReservoir.valid == 0)
  {
    shiftStatus = uint(ReSTIRShiftStatus::eReSTIRShiftStatusRejectedTargetPdf);
  }

  return shiftedReservoir;
}

#endif
