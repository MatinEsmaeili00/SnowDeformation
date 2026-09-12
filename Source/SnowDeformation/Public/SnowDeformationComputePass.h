// Copyright Matin. All Rights Reserved.
//
// Snow deformation compute passes.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

class FRHICommandListImmediate;
class FRHITexture;

/**
 * One thing pressing into the snow this frame. Layout must match
 * FSnowDeformerGPU in SnowDeformation.usf - all floats, tightly packed.
 */
struct FSnowDeformerGPU
{
	FVector2f	LocalCenter = FVector2f::ZeroVector;
	float		Radius = 24.0f;
	float		Depth = 1.0f;
	float		Falloff = 2.0f;
	float		RimWidth = 8.0f;
	float		RimHeight = 0.25f;
	float		Strength = 1.0f;
};
static_assert(sizeof(FSnowDeformerGPU) == 32, "FSnowDeformerGPU must stay tightly packed to match the HLSL struct");

/** Everything the render thread needs for one frame of snow simulation. */
struct FSnowDeformationDispatchParams
{
	FRHITexture*	PrevHeightTexture = nullptr;
	FRHITexture*	NextHeightTexture = nullptr;
	FRHITexture*	SnowDataTexture = nullptr;

	FIntPoint		TextureSize = FIntPoint(1024, 1024);
	float			RegionSize = 4096.0f;
	FVector2f		RegionOffsetFromPrev = FVector2f::ZeroVector;
	float			MaxDepth = 20.0f;
	float			RefillRate = 0.0f;
	float			DeltaTime = 0.0f;
	float			NormalStrength = 1.0f;
	float			ClearMask = 1.0f;

	TArray<FSnowDeformerGPU> Deformers;
};

// ---------------------------------------------------------------------------
// Pass 1: reproject + relax + stamp
// ---------------------------------------------------------------------------

BEGIN_SHADER_PARAMETER_STRUCT(FSnowAccumulateParams, )
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, PrevHeightTexture)
	SHADER_PARAMETER_SAMPLER(SamplerState, PrevHeightSampler)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutHeightTexture)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FSnowDeformerGPU>, Deformers)
	SHADER_PARAMETER(FIntPoint, TextureSize)
	SHADER_PARAMETER(float, RegionSize)
	SHADER_PARAMETER(FVector2f, RegionOffsetFromPrev)
	SHADER_PARAMETER(int32, NumDeformers)
	SHADER_PARAMETER(float, RefillRate)
	SHADER_PARAMETER(float, DeltaTime)
	SHADER_PARAMETER(float, ClearMask)
END_SHADER_PARAMETER_STRUCT()

class FSnowAccumulateCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSnowAccumulateCS);
	SHADER_USE_PARAMETER_STRUCT(FSnowAccumulateCS, FGlobalShader);

	using FParameters = FSnowAccumulateParams;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		SET_SHADER_DEFINE(OutEnvironment, THREADS_X, 8);
		SET_SHADER_DEFINE(OutEnvironment, THREADS_Y, 8);
	}
};

// ---------------------------------------------------------------------------
// Pass 2: height field -> packed height + normal texture for the material
// ---------------------------------------------------------------------------

BEGIN_SHADER_PARAMETER_STRUCT(FSnowNormalsParams, )
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, HeightTexture)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutSnowDataTexture)
	SHADER_PARAMETER(FIntPoint, TextureSize)
	SHADER_PARAMETER(float, RegionSize)
	SHADER_PARAMETER(float, MaxDepth)
	SHADER_PARAMETER(float, NormalStrength)
END_SHADER_PARAMETER_STRUCT()

class FSnowNormalsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSnowNormalsCS);
	SHADER_USE_PARAMETER_STRUCT(FSnowNormalsCS, FGlobalShader);

	using FParameters = FSnowNormalsParams;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		SET_SHADER_DEFINE(OutEnvironment, THREADS_X, 8);
		SET_SHADER_DEFINE(OutEnvironment, THREADS_Y, 8);
	}
};

namespace SnowDeformation
{
	/** Builds and executes the render graph for one simulation step. Render thread only. */
	SNOWDEFORMATION_API void Dispatch_RenderThread(FRHICommandListImmediate& RHICmdList, const FSnowDeformationDispatchParams& Params);
}
