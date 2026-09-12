// Copyright Matin. All Rights Reserved.

#include "SnowDeformationComputePass.h"

#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderTargetPool.h"
#include "RHIStaticStates.h"

IMPLEMENT_GLOBAL_SHADER(FSnowAccumulateCS, "/SnowDeformationShaders/Private/SnowDeformation.usf", "SnowAccumulateCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSnowNormalsCS, "/SnowDeformationShaders/Private/SnowDeformation.usf", "SnowNormalsCS", SF_Compute);

void SnowDeformation::Dispatch_RenderThread(FRHICommandListImmediate& RHICmdList, const FSnowDeformationDispatchParams& Params)
{
	check(IsInRenderingThread());

	if (!Params.PrevHeightTexture || !Params.NextHeightTexture || !Params.SnowDataTexture)
	{
		return;
	}

	FRDGBuilder GraphBuilder(RHICmdList);

	const FRDGTextureRef PrevHeightRDG = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Params.PrevHeightTexture, TEXT("SnowPrevHeight")));
	const FRDGTextureRef NextHeightRDG = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Params.NextHeightTexture, TEXT("SnowNextHeight")));
	const FRDGTextureRef SnowDataRDG   = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Params.SnowDataTexture, TEXT("SnowData")));

	const FRDGTextureUAVRef NextHeightUAV = GraphBuilder.CreateUAV(NextHeightRDG);
	const FRDGTextureUAVRef SnowDataUAV   = GraphBuilder.CreateUAV(SnowDataRDG);

	// Structured buffers can't be zero-sized, so fall back to a single inert
	// entry when nothing is pressing into the snow this frame.
	const int32 NumDeformers = Params.Deformers.Num();
	FRDGBufferRef DeformerBuffer;
	if (NumDeformers > 0)
	{
		DeformerBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("SnowDeformers"),
			sizeof(FSnowDeformerGPU),
			NumDeformers,
			Params.Deformers.GetData(),
			sizeof(FSnowDeformerGPU) * NumDeformers);
	}
	else
	{
		static const FSnowDeformerGPU DummyDeformer;
		DeformerBuffer = CreateStructuredBuffer(
			GraphBuilder,
			TEXT("SnowDeformersDummy"),
			sizeof(FSnowDeformerGPU),
			1,
			&DummyDeformer,
			sizeof(FSnowDeformerGPU));
	}
	const FRDGBufferSRVRef DeformerSRV = GraphBuilder.CreateSRV(DeformerBuffer);

	// --- Pass 1: reproject + relax + stamp -------------------------------
	{
		FSnowAccumulateCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSnowAccumulateCS::FParameters>();
		PassParameters->PrevHeightTexture = PrevHeightRDG;
		PassParameters->PrevHeightSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		PassParameters->OutHeightTexture = NextHeightUAV;
		PassParameters->Deformers = DeformerSRV;
		PassParameters->TextureSize = Params.TextureSize;
		PassParameters->RegionSize = Params.RegionSize;
		PassParameters->RegionOffsetFromPrev = Params.RegionOffsetFromPrev;
		PassParameters->NumDeformers = NumDeformers;
		PassParameters->RefillRate = Params.RefillRate;
		PassParameters->DeltaTime = Params.DeltaTime;
		PassParameters->ClearMask = Params.ClearMask;

		TShaderMapRef<FSnowAccumulateCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("SnowAccumulate(%dx%d, %d deformers)", Params.TextureSize.X, Params.TextureSize.Y, NumDeformers),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(Params.TextureSize, FIntPoint(8, 8)));
	}

	// --- Pass 2: height field -> height+normal texture --------------------
	{
		FSnowNormalsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSnowNormalsCS::FParameters>();
		PassParameters->HeightTexture = NextHeightRDG;
		PassParameters->OutSnowDataTexture = SnowDataUAV;
		PassParameters->TextureSize = Params.TextureSize;
		PassParameters->RegionSize = Params.RegionSize;
		PassParameters->MaxDepth = Params.MaxDepth;
		PassParameters->NormalStrength = Params.NormalStrength;

		TShaderMapRef<FSnowNormalsCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("SnowNormals(%dx%d)", Params.TextureSize.X, Params.TextureSize.Y),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(Params.TextureSize, FIntPoint(8, 8)));
	}

	GraphBuilder.Execute();
}
