// Copyright Matin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "SnowDeformationComputePass.h"
#include "SnowDeformationSubsystem.generated.h"

class UTextureRenderTarget2D;
class USnowDeformerComponent;

/**
 * Runs the snow simulation for a world: one persistent, world-anchored
 * height field that follows a focus actor (the local player by default),
 * fed every frame by whatever USnowDeformerComponents are registered.
 *
 * Usage:
 *   - Add a USnowDeformerComponent to any character that should leave
 *     footprints; it registers itself automatically.
 *   - Read GetSnowDataRenderTarget() (R = height, GB = normal.xy) into a
 *     material driving world-position-offset / normal on your snow mesh or
 *     landscape, using GetRegionCenter() / GetRegionSize() to compute UVs:
 *       UV = (AbsoluteWorldPosition.xy - RegionCenter) / RegionSize + 0.5
 *   - Call StampSnowAt() for one-off presses that aren't tied to a
 *     component, e.g. an explosion or a vehicle wheel.
 */
UCLASS()
class SNOWDEFORMATION_API USnowDeformationSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Deinitialize() override;

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

	/** Called by USnowDeformerComponent::BeginPlay - no need to call directly. */
	void RegisterDeformer(USnowDeformerComponent* Component);
	void UnregisterDeformer(USnowDeformerComponent* Component);

	/** One-off press not tied to a component, e.g. an explosion crater or a vehicle wheel. Lands next tick. */
	UFUNCTION(BlueprintCallable, Category = "Snow Deformation")
	void StampSnowAt(FVector WorldLocation, float Radius, float Depth, float Falloff = 2.0f, float RimWidth = 8.0f, float RimHeight = 0.25f, float Strength = 1.0f);

	/** The region tracks this actor's XY every frame. Defaults to player pawn 0 when unset. */
	UFUNCTION(BlueprintCallable, Category = "Snow Deformation")
	void SetFocusActor(AActor* NewFocusActor);

	/** R = height (0 flat, 1 = fully pressed to MaxDepthWorld, negative = displaced rim), GB = normal.xy. */
	UFUNCTION(BlueprintPure, Category = "Snow Deformation")
	UTextureRenderTarget2D* GetSnowDataRenderTarget() const { return SnowDataRenderTarget; }

	/** World-space XY the simulated region is currently centred on. */
	UFUNCTION(BlueprintPure, Category = "Snow Deformation")
	FVector2D GetRegionCenter() const { return RegionCenterWorld; }

	UFUNCTION(BlueprintPure, Category = "Snow Deformation")
	float GetRegionSize() const { return RegionSizeWorld; }

	UFUNCTION(BlueprintPure, Category = "Snow Deformation")
	float GetMaxDepth() const { return MaxDepthWorld; }

	/** Resolution of the simulated height field. Changing this at runtime reallocates the render targets. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Deformation|Settings")
	FIntPoint TextureResolution = FIntPoint(1024, 1024);

	/** World units covered by the whole texture, centred on the focus actor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Deformation|Settings", meta = (ClampMin = "1.0"))
	float RegionSizeWorld = 4096.0f;

	/** World units a fully-pressed footprint (height == 1) sinks down. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Deformation|Settings", meta = (ClampMin = "0.1"))
	float MaxDepthWorld = 20.0f;

	/** Normalised height recovered per second - how quickly fresh snowfall fills old tracks back in. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Deformation|Settings", meta = (ClampMin = "0.0"))
	float RefillRate = 0.02f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Deformation|Settings", meta = (ClampMin = "0.0"))
	float NormalStrength = 1.5f;

private:
	void EnsureRenderTargets();
	AActor* ResolveFocusActor() const;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> HeightRenderTargets[2] = { nullptr, nullptr };

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> SnowDataRenderTarget = nullptr;

	UPROPERTY(Transient)
	TWeakObjectPtr<AActor> FocusActor;

	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<USnowDeformerComponent>> RegisteredDeformers;

	TArray<FSnowDeformerGPU> PendingOneShotDeformers;

	FVector2D RegionCenterWorld = FVector2D::ZeroVector;
	int32 CurrentHeightIndex = 0;
	bool bTargetsInitialised = false;
	bool bFirstFrame = true;
};
