// Copyright Matin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "SnowDeformationComputePass.h"
#include "SnowDeformationSubsystem.generated.h"

class UMaterialInstanceDynamic;
class UMaterialParameterCollection;
class UTextureRenderTarget2D;
class USnowDeformerComponent;

/**
 * Runs the snow simulation for a world: one persistent, world-anchored
 * height field that follows a focus actor (the local player by default),
 * fed every frame by whatever USnowDeformerComponents are registered.
 *
 * Tuning lives in Project Settings > Plugins > Snow Deformation
 * (USnowDeformationSettings); the values below are seeded from there on
 * Initialize and can be overridden at runtime from Blueprint.
 *
 * Usage:
 *   - Add a USnowDeformerComponent to any character that should leave
 *     footprints; it registers itself automatically.
 *   - Wire the output into a material one of three ways (see README):
 *       1. Assign a render target asset + a Material Parameter Collection in
 *          Project Settings and sample them directly - no Blueprint at all.
 *       2. Call RegisterSnowMaterial() with a dynamic material instance and
 *          the subsystem keeps its SnowData / SnowRegion parameters current.
 *       3. Read GetSnowDataRenderTarget() / GetSnowRegionParameter() yourself.
 *   - Call StampSnowAt() for one-off presses that aren't tied to a
 *     component, e.g. an explosion or a vehicle wheel.
 */
UCLASS()
class SNOWDEFORMATION_API USnowDeformationSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
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

	// --- Material wiring --------------------------------------------------

	/** R = height (0 flat, 1 = fully pressed to MaxDepthWorld, negative = displaced rim), GB = normal.xy. */
	UFUNCTION(BlueprintPure, Category = "Snow Deformation|Material")
	UTextureRenderTarget2D* GetSnowDataRenderTarget() const { return SnowDataRenderTarget; }

	/**
	 * Everything a material needs to place the snow texture in the world,
	 * packed into one vector: RG = region centre XY, B = region size,
	 * A = max depth. This is what gets pushed into the parameter collection.
	 */
	UFUNCTION(BlueprintPure, Category = "Snow Deformation|Material")
	FLinearColor GetSnowRegionParameter() const;

	/** Pushes SnowData + SnowRegion onto a dynamic material instance once. */
	UFUNCTION(BlueprintCallable, Category = "Snow Deformation|Material")
	void ApplySnowParametersToMaterial(UMaterialInstanceDynamic* Material) const;

	/** Keeps a dynamic material instance's parameters current every frame. Held weakly. */
	UFUNCTION(BlueprintCallable, Category = "Snow Deformation|Material")
	void RegisterSnowMaterial(UMaterialInstanceDynamic* Material);

	UFUNCTION(BlueprintCallable, Category = "Snow Deformation|Material")
	void UnregisterSnowMaterial(UMaterialInstanceDynamic* Material);

	/** World-space XY the simulated region is currently centred on. */
	UFUNCTION(BlueprintPure, Category = "Snow Deformation")
	FVector2D GetRegionCenter() const { return RegionCenterWorld; }

	UFUNCTION(BlueprintPure, Category = "Snow Deformation")
	float GetRegionSize() const { return RegionSizeWorld; }

	UFUNCTION(BlueprintPure, Category = "Snow Deformation")
	float GetMaxDepth() const { return MaxDepthWorld; }

	// --- Runtime overrides (seeded from USnowDeformationSettings) ----------

	/** Resolution of the simulated height field. Changing this at runtime reallocates the render targets. */
	UPROPERTY(BlueprintReadWrite, Category = "Snow Deformation|Settings")
	FIntPoint TextureResolution = FIntPoint(1024, 1024);

	/** World units covered by the whole texture, centred on the focus actor. */
	UPROPERTY(BlueprintReadWrite, Category = "Snow Deformation|Settings")
	float RegionSizeWorld = 4096.0f;

	/** World units a fully-pressed footprint (height == 1) sinks down. */
	UPROPERTY(BlueprintReadWrite, Category = "Snow Deformation|Settings")
	float MaxDepthWorld = 20.0f;

	/** Normalised height recovered per second - how quickly fresh snowfall fills old tracks back in. */
	UPROPERTY(BlueprintReadWrite, Category = "Snow Deformation|Settings")
	float RefillRate = 0.02f;

	UPROPERTY(BlueprintReadWrite, Category = "Snow Deformation|Settings")
	float NormalStrength = 1.5f;

private:
	void EnsureRenderTargets();
	void ReleaseRenderTargets();
	AActor* ResolveFocusActor() const;

	/** Forces the UAV flag on a user-supplied output asset and warns about a lossy format. */
	void PrepareOutputAsset(UTextureRenderTarget2D* Asset);

	/** Pushes the region vector into the collection and every registered dynamic material. */
	void PushMaterialParameters();

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> HeightRenderTargets[2] = { nullptr, nullptr };

	/** Either the asset from the settings, or a transient one we made ourselves. */
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> SnowDataRenderTarget = nullptr;

	/**
	 * The output asset named in the settings, resolved and validated once on
	 * Initialize. Cached rather than re-resolved per frame so a mis-configured
	 * asset warns exactly once instead of every tick.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> ConfiguredOutputAsset = nullptr;

	/** Set when SnowDataRenderTarget came from the settings asset rather than being ours to resize. */
	bool bUsingExternalOutput = false;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialParameterCollection> ParameterCollection = nullptr;

	UPROPERTY(Transient)
	TWeakObjectPtr<AActor> FocusActor;

	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<USnowDeformerComponent>> RegisteredDeformers;

	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<UMaterialInstanceDynamic>> RegisteredMaterials;

	FName RegionParameterName;
	FName SnowDataParameterName;
	bool bRegionParameterValid = false;

	TArray<FSnowDeformerGPU> PendingOneShotDeformers;

	FVector2D RegionCenterWorld = FVector2D::ZeroVector;
	int32 CurrentHeightIndex = 0;
	bool bTargetsInitialised = false;
	bool bFirstFrame = true;

	/**
	 * Seconds of simulation still owed after the last thing pressed into the
	 * snow, so tracks finish healing before we stop dispatching. Derived from
	 * RefillRate - a zero refill rate means tracks are permanent and there is
	 * nothing left to simulate once the region stops moving.
	 */
	float IdleRelaxRemaining = 0.0f;
};
