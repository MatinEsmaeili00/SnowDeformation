// Copyright Matin. All Rights Reserved.

#include "SnowDeformationSubsystem.h"

#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameterCollection.h"
#include "SnowDeformation.h"
#include "SnowDeformationSettings.h"
#include "SnowDeformerComponent.h"
#include "TextureResource.h"

void USnowDeformationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	const USnowDeformationSettings& Settings = USnowDeformationSettings::Get();

	TextureResolution = Settings.TextureResolution;
	RegionSizeWorld   = Settings.RegionSizeWorld;
	MaxDepthWorld     = Settings.MaxDepthWorld;
	RefillRate        = Settings.RefillRate;
	NormalStrength    = Settings.NormalStrength;

	RegionParameterName   = Settings.RegionParameterName;
	SnowDataParameterName = Settings.SnowDataParameterName;

	// Both wiring assets are soft references, so a project that doesn't use
	// them never pays to load them. Resolving and validating here rather than
	// per frame is also what keeps a mis-configured asset to one warning.
	if (!Settings.SnowDataRenderTarget.IsNull())
	{
		ConfiguredOutputAsset = Settings.SnowDataRenderTarget.LoadSynchronous();
		if (ConfiguredOutputAsset)
		{
			PrepareOutputAsset(ConfiguredOutputAsset);
		}
	}

	if (!Settings.ParameterCollection.IsNull())
	{
		ParameterCollection = Settings.ParameterCollection.LoadSynchronous();
		if (ParameterCollection)
		{
			bRegionParameterValid = ParameterCollection->GetVectorParameterByName(RegionParameterName) != nullptr;
			if (!bRegionParameterValid)
			{
				UE_LOG(LogSnowDeformation, Warning,
					TEXT("Material Parameter Collection %s has no vector parameter named %s. Add one ")
					TEXT("(RG = region centre XY, B = region size, A = max depth), or clear the collection ")
					TEXT("reference in Project Settings > Plugins > Snow Deformation."),
					*ParameterCollection->GetName(), *RegionParameterName.ToString());
			}
		}
	}
}

void USnowDeformationSubsystem::Deinitialize()
{
	RegisteredDeformers.Reset();
	RegisteredMaterials.Reset();
	PendingOneShotDeformers.Reset();
	ReleaseRenderTargets();
	ConfiguredOutputAsset = nullptr;
	ParameterCollection = nullptr;

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
	// Banked in world space: the region may still move before the next Tick,
	// which is where this gets rebased into region-local space.
	Deformer.LocalCenter = FVector2f(FVector2D(WorldLocation));
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

// ---------------------------------------------------------------------------
// Material wiring
// ---------------------------------------------------------------------------

FLinearColor USnowDeformationSubsystem::GetSnowRegionParameter() const
{
	return FLinearColor(
		static_cast<float>(RegionCenterWorld.X),
		static_cast<float>(RegionCenterWorld.Y),
		RegionSizeWorld,
		MaxDepthWorld);
}

void USnowDeformationSubsystem::ApplySnowParametersToMaterial(UMaterialInstanceDynamic* Material) const
{
	if (!Material)
	{
		return;
	}

	if (SnowDataRenderTarget)
	{
		Material->SetTextureParameterValue(SnowDataParameterName, SnowDataRenderTarget);
	}
	Material->SetVectorParameterValue(RegionParameterName, GetSnowRegionParameter());
}

void USnowDeformationSubsystem::RegisterSnowMaterial(UMaterialInstanceDynamic* Material)
{
	if (Material)
	{
		RegisteredMaterials.AddUnique(Material);
		ApplySnowParametersToMaterial(Material);
	}
}

void USnowDeformationSubsystem::UnregisterSnowMaterial(UMaterialInstanceDynamic* Material)
{
	RegisteredMaterials.RemoveAll([Material](const TWeakObjectPtr<UMaterialInstanceDynamic>& WeakMaterial)
	{
		return WeakMaterial.Get() == Material;
	});
}

void USnowDeformationSubsystem::PushMaterialParameters()
{
	if (ParameterCollection && bRegionParameterValid)
	{
		UKismetMaterialLibrary::SetVectorParameterValue(this, ParameterCollection, RegionParameterName, GetSnowRegionParameter());
	}

	for (auto It = RegisteredMaterials.CreateIterator(); It; ++It)
	{
		if (UMaterialInstanceDynamic* Material = It->Get())
		{
			ApplySnowParametersToMaterial(Material);
		}
		else
		{
			It.RemoveCurrent();
		}
	}
}

// ---------------------------------------------------------------------------
// Render targets
// ---------------------------------------------------------------------------

void USnowDeformationSubsystem::PrepareOutputAsset(UTextureRenderTarget2D* Asset)
{
	check(Asset);

	if (Asset->RenderTargetFormat != RTF_RGBA16f)
	{
		UE_LOG(LogSnowDeformation, Warning,
			TEXT("Snow output render target %s is not RGBA16f. Heights are signed - the rim of displaced ")
			TEXT("snow around a print is negative - so a fixed-point format clips it away. Set Render ")
			TEXT("Target Format to RGBA16f on the asset."),
			*Asset->GetName());
	}

	// Without this the compute pass has no UAV to write through. Fixing it up
	// here stops a mis-configured asset from silently producing a blank field.
	if (!Asset->bCanCreateUAV)
	{
		UE_LOG(LogSnowDeformation, Warning,
			TEXT("Snow output render target %s did not have Can Create UAV set; enabling it for this ")
			TEXT("session. Tick it on the asset to avoid the runtime fixup."),
			*Asset->GetName());
		Asset->bCanCreateUAV = true;
		Asset->UpdateResource();
	}
}

void USnowDeformationSubsystem::ReleaseRenderTargets()
{
	HeightRenderTargets[0] = nullptr;
	HeightRenderTargets[1] = nullptr;
	SnowDataRenderTarget = nullptr;
	bUsingExternalOutput = false;
	bTargetsInitialised = false;
	bFirstFrame = true;
}

void USnowDeformationSubsystem::EnsureRenderTargets()
{
	// A user-supplied output asset decides the simulation resolution. Resizing
	// someone else's asset out from under them would be a nasty surprise, and
	// the height buffers have to match it texel for texel anyway.
	UTextureRenderTarget2D* OutputAsset = ConfiguredOutputAsset;
	if (OutputAsset)
	{
		TextureResolution = FIntPoint(OutputAsset->SizeX, OutputAsset->SizeY);
	}

	TextureResolution.X = FMath::Max(TextureResolution.X, 1);
	TextureResolution.Y = FMath::Max(TextureResolution.Y, 1);

	const bool bMatchesRequest =
		bTargetsInitialised
		&& HeightRenderTargets[0] && HeightRenderTargets[1] && SnowDataRenderTarget
		&& bUsingExternalOutput == (OutputAsset != nullptr)
		&& (!OutputAsset || SnowDataRenderTarget == OutputAsset)
		&& HeightRenderTargets[0]->SizeX == TextureResolution.X
		&& HeightRenderTargets[0]->SizeY == TextureResolution.Y;

	if (bMatchesRequest)
	{
		return;
	}

	// NAME_None rather than a fixed name: re-allocating on a resolution change
	// would otherwise collide with the outgoing object still holding the name.
	auto MakeRenderTarget = [this](ETextureRenderTargetFormat Format) -> UTextureRenderTarget2D*
	{
		UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(this);
		RT->RenderTargetFormat = Format;
		RT->ClearColor = FLinearColor::Black;
		RT->bAutoGenerateMips = false;
		RT->bCanCreateUAV = true;
		RT->InitAutoFormat(TextureResolution.X, TextureResolution.Y);
		RT->UpdateResourceImmediate(true);
		return RT;
	};

	HeightRenderTargets[0] = MakeRenderTarget(RTF_R32f);
	HeightRenderTargets[1] = MakeRenderTarget(RTF_R32f);

	if (OutputAsset)
	{
		SnowDataRenderTarget = OutputAsset;
		bUsingExternalOutput = true;
	}
	else
	{
		SnowDataRenderTarget = MakeRenderTarget(RTF_RGBA16f);
		bUsingExternalOutput = false;
	}

	CurrentHeightIndex = 0;
	bFirstFrame = true;
	bTargetsInitialised = true;

	UE_LOG(LogSnowDeformation, Log,
		TEXT("Snow height field allocated at %dx%d over %.0f world units; output target: %s."),
		TextureResolution.X, TextureResolution.Y, RegionSizeWorld,
		bUsingExternalOutput ? *SnowDataRenderTarget->GetName() : TEXT("transient"));
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

void USnowDeformationSubsystem::Tick(float DeltaTime)
{
	if (!IsInitialized())
	{
		return;
	}

	EnsureRenderTargets();
	if (!bTargetsInitialised)
	{
		return;
	}

	const AActor* Focus = ResolveFocusActor();
	const FVector2D NewRegionCenter = Focus ? FVector2D(Focus->GetActorLocation()) : RegionCenterWorld;

	TArray<FSnowDeformationContact> Contacts;
	for (auto It = RegisteredDeformers.CreateIterator(); It; ++It)
	{
		if (const USnowDeformerComponent* Component = It->Get())
		{
			Component->GetActiveContacts(Contacts);
		}
		else
		{
			It.RemoveCurrent();
		}
	}

	const int32 NumPresses = Contacts.Num() + PendingOneShotDeformers.Num();

	// Anything pressing in means the field has to keep relaxing afterwards
	// until it is flat again. With RefillRate at 0 tracks never heal, so once
	// the region stops moving there is genuinely nothing left to compute.
	if (NumPresses > 0)
	{
		IdleRelaxRemaining = RefillRate > KINDA_SMALL_NUMBER ? (1.0f / RefillRate) : 0.0f;
	}
	else
	{
		IdleRelaxRemaining = FMath::Max(IdleRelaxRemaining - DeltaTime, 0.0f);
	}

	// Skipping is only safe while the region is stationary: the texture is
	// anchored to RegionCenterWorld, so a move has to be reprojected or every
	// existing track would slide along with the player.
	const bool bRegionMoved = !NewRegionCenter.Equals(RegionCenterWorld, 0.01);
	const bool bNeedsDispatch = bFirstFrame || bRegionMoved || NumPresses > 0 || IdleRelaxRemaining > 0.0f;

	if (!bNeedsDispatch)
	{
		// Parameters still go out: a material instance created this frame needs
		// them even while the simulation itself is idle.
		PushMaterialParameters();
		return;
	}

	FTextureRenderTargetResource* PrevResource = HeightRenderTargets[CurrentHeightIndex]->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* NextResource = HeightRenderTargets[1 - CurrentHeightIndex]->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* DataResource = SnowDataRenderTarget->GameThread_GetRenderTargetResource();
	if (!PrevResource || !NextResource || !DataResource)
	{
		// Resources are created on the render thread; nothing to do until they
		// land. Checked before any state is advanced so no motion is lost.
		return;
	}

	const FVector2D OffsetFromPrev = NewRegionCenter - RegionCenterWorld;
	RegionCenterWorld = NewRegionCenter;

	TArray<FSnowDeformerGPU> GPUDeformers;
	GPUDeformers.Reserve(NumPresses);
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

	const FVector2f RegionCenterFloat(RegionCenterWorld);
	for (FSnowDeformerGPU& OneShot : PendingOneShotDeformers)
	{
		OneShot.LocalCenter -= RegionCenterFloat;
		GPUDeformers.Add(OneShot);
	}
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

	// The RHI textures are resolved on the render thread rather than here: a
	// resource can swap the texture underneath a game-thread read (a resize, a
	// device reset) between now and when the command actually runs.
	ENQUEUE_RENDER_COMMAND(SnowDeformationDispatch)(
		[Params, PrevResource, NextResource, DataResource](FRHICommandListImmediate& RHICmdList) mutable
		{
			Params.PrevHeightTexture = PrevResource->GetRenderTargetTexture();
			Params.NextHeightTexture = NextResource->GetRenderTargetTexture();
			Params.SnowDataTexture   = DataResource->GetRenderTargetTexture();

			SnowDeformation::Dispatch_RenderThread(RHICmdList, Params);
		});

	CurrentHeightIndex = 1 - CurrentHeightIndex;
	bFirstFrame = false;

	PushMaterialParameters();
}
