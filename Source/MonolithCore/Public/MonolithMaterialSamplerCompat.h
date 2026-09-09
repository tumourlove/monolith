#pragma once

#include "Runtime/Launch/Resources/Version.h"

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
#include "Materials/MaterialExpressionUtils.h"
#else
#include "Materials/MaterialExpressionTextureBase.h"
#endif

class UTexture;

namespace MonolithMaterialSamplerCompat
{
#if WITH_EDITOR
/**
 * Return Unreal's recommended sampler type without exposing engine-version API
 * drift to each material or index consumer.
 *
 * UE 5.8 moved GetSamplerTypeForTexture from UMaterialExpressionTextureBase to
 * the MaterialExpressionUtils namespace and deprecated the former entry point.
 * UE 5.7 does not ship MaterialExpressionUtils.h, so the version gate lives here
 * rather than at every call site.
 */
FORCEINLINE EMaterialSamplerType GetSamplerTypeForTexture(
	const UTexture* Texture,
	bool bForceNoVirtualTexture = false)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	return MaterialExpressionUtils::GetSamplerTypeForTexture(Texture, bForceNoVirtualTexture);
#else
	return UMaterialExpressionTextureBase::GetSamplerTypeForTexture(Texture, bForceNoVirtualTexture);
#endif
}
#endif
}
