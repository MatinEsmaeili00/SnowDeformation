// Copyright Matin. All Rights Reserved.

#include "SnowDeformationSubsystem.h"

#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "SnowDeformerComponent.h"

void USnowDeformationSubsystem::Deinitialize()
{
	RegisteredDeformers.Reset();
	Super::Deinitialize();
}

TStatId USnowDeformationSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(USnowDeformationSubsystem, STATGROUP_Tickables);
}

bool USnowDeformationSubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void USnowDeformationSubsystem::RegisterDeformer(USnowDeformerComponent* Component)
{
	if (Component)
	{
		RegisteredDeformers.AddUnique(Component);
	}
}

void USnowDeformationSubsystem::UnregisterDeformer(USnowDeformerComponent* Component)
{
	RegisteredDeformers.RemoveAll([Component](const TWeakObjectPtr<USnowDeformerComponent>& WeakComponent)
	{
		return WeakComponent.Get() == Component;
	});
}

void USnowDeformationSubsystem::StampSnowAt(FVector WorldLocation, float Radius, float Depth, float Falloff, float RimWidth, float RimHeight, float Strength)
{
	FSnowDeformerGPU Deformer;
	Deformer.LocalCenter = FVector2f(FVector2D(WorldLocation) - RegionCenterWorld);
	Deformer.Radius = Radius;
	Deformer.Depth = Depth;
	Deformer.Falloff = Falloff;
	Deformer.RimWidth = RimWidth;
	Deformer.RimHeight = RimHeight;
	Deformer.Strength = Strength;
	PendingOneShotDeformers.Add(Deformer);
}

void USnowDeformationSubsystem::SetFocusActor(AActor* NewFocusActor)
{
	FocusActor = NewFocusActor;
}

AActor* USnowDeformationSubsystem::ResolveFocusActor() const
{
	if (AActor* Focus = FocusActor.Get())
	{
		return Focus;
	}

	if (const UWorld* World = GetWorld())
	{
		return UGameplayStatics::GetPlayerPawn(World, 0);
	}

	return nullptr;
}

void USnowDeformationSubsystem::EnsureRenderTargets()
{
	if (bTargetsInitialised
		&& HeightRenderTargets[0]
		&& HeightRenderTargets[0]->SizeX == TextureResolution.X
		&& HeightRenderTargets[0]->SizeY == TextureResolution.Y)
	{
		return;
	}

	auto MakeRenderTarget = [this](ETextureRenderTargetFormat Format, const TCHAR* DebugName) -> UTextureRenderTarget2D*
	{
		UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(this, DebugName);
		RT->RenderTargetFormat = Format;
		RT->ClearColor = FLinearColor::Black;
		RT->bAutoGenerateMips = false;
		RT->bCanCreateUAV = true;
		RT->InitAutoFormat(TextureResolution.X, TextureResolution.Y);
		RT->UpdateResourceImmediate(true);
		return RT;
	};

	HeightRenderTargets[0] = MakeRenderTarget(RTF_R32f, TEXT("SnowHeightA"));
	HeightRenderTargets[1] = MakeRenderTarget(RTF_R32f, TEXT("SnowHeightB"));
	SnowDataRenderTarget   = MakeRenderTarget(RTF_RGBA16f, TEXT("SnowData"));

	CurrentHeightIndex = 0;
	bFirstFrame = true;
	bTargetsInitialised = true;
}

void USnowDeformationSubsystem::Tick(float DeltaTime)
{
	EnsureRenderTargets();

	AActor* Focus = ResolveFocusActor();
	const FVector2D NewRegionCenter = Focus ? FVector2D(Focus->GetActorLocation()) : RegionCenterWorld;
	const FVector2D OffsetFromPrev = NewRegionCenter - RegionCenterWorld;
	RegionCenterWorld = NewRegionCenter;

	TArray<FSnowDeformationContact> Contacts;
	for (auto It = RegisteredDeformers.CreateIterator(); It; ++It)
	{
		if (USnowDeformerComponent* Component = It->Get())
		{
			Component->GetActiveContacts(Contacts);
		}
		else
		{
			It.RemoveCurrent();
		}
	}

	TArray<FSnowDeformerGPU> GPUDeformers;
	GPUDeformers.Reserve(Contacts.Num() + PendingOneShotDeformers.Num());
	for (const FSnowDeformationContact& Contact : Contacts)
	{
		FSnowDeformerGPU Deformer;
		Deformer.LocalCenter = FVector2f(FVector2D(Contact.WorldLocation) - RegionCenterWorld);
		Deformer.Radius = Contact.Radius;
		Deformer.Depth = Contact.Depth;
		Deformer.Falloff = Contact.Falloff;
		Deformer.RimWidth = Contact.RimWidth;
		Deformer.RimHeight = Contact.RimHeight;
		Deformer.Strength = Contact.Strength;
		GPUDeformers.Add(Deformer);
	}
	GPUDeformers.Append(PendingOneShotDeformers);
	PendingOneShotDeformers.Reset();

	FSnowDeformationDispatchParams Params;
	Params.TextureSize = TextureResolution;
	Params.RegionSize = RegionSizeWorld;
	Params.RegionOffsetFromPrev = FVector2f(OffsetFromPrev);
	Params.MaxDepth = MaxDepthWorld;
	Params.RefillRate = RefillRate;
	Params.DeltaTime = DeltaTime;
	Params.NormalStrength = NormalStrength;
	Params.ClearMask = bFirstFrame ? 0.0f : 1.0f;
	Params.Deformers = MoveTemp(GPUDeformers);
	Params.PrevHeightTexture = HeightRenderTargets[CurrentHeightIndex]->GameThread_GetRenderTargetResource()->GetRenderTargetTexture();
	Params.NextHeightTexture = HeightRenderTargets[1 - CurrentHeightIndex]->GameThread_GetRenderTargetResource()->GetRenderTargetTexture();
	Params.SnowDataTexture = SnowDataRenderTarget->GameThread_GetRenderTargetResource()->GetRenderTargetTexture();

	ENQUEUE_RENDER_COMMAND(SnowDeformationDispatch)(
		[Params](FRHICommandListImmediate& RHICmdList)
		{
			SnowDeformation::Dispatch_RenderThread(RHICmdList, Params);
		});

	CurrentHeightIndex = 1 - CurrentHeightIndex;
	bFirstFrame = false;
}
