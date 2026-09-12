// Copyright Matin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "SnowDeformationSettings.generated.h"

class UMaterialParameterCollection;
class UTextureRenderTarget2D;

/**
 * Project-wide snow simulation defaults, plus the two optional assets that
 * wire the simulation into a material. Lives under
 * Project Settings > Plugins > Snow Deformation.
 *
 * A world subsystem has no details panel of its own, so this is where the
 * simulation is actually tuned: USnowDeformationSubsystem copies these values
 * when it initialises, and they stay writable at runtime from Blueprint for
 * anything that needs to change mid-game.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Snow Deformation"))
class SNOWDEFORMATION_API USnowDeformationSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	USnowDeformationSettings();

	virtual FName GetCategoryName() const override;

	static const USnowDeformationSettings& Get();

	// --- Simulation -------------------------------------------------------

	/** Resolution of the simulated height field. Ignored when SnowDataRenderTarget is set - that asset's size wins. */
	UPROPERTY(config, EditAnywhere, Category = "Simulation")
	FIntPoint TextureResolution;

	/** World units covered by the whole texture, centred on the focus actor. */
	UPROPERTY(config, EditAnywhere, Category = "Simulation", meta = (ClampMin = "1.0"))
	float RegionSizeWorld;

	/** World units a fully-pressed footprint (height == 1) sinks down. */
	UPROPERTY(config, EditAnywhere, Category = "Simulation", meta = (ClampMin = "0.1"))
	float MaxDepthWorld;

	/** Normalised height recovered per second - how quickly fresh snowfall fills old tracks back in. 0 = permanent tracks. */
	UPROPERTY(config, EditAnywhere, Category = "Simulation", meta = (ClampMin = "0.0"))
	float RefillRate;

	/** Scales the slope baked into the output normals. */
	UPROPERTY(config, EditAnywhere, Category = "Simulation", meta = (ClampMin = "0.0"))
	float NormalStrength;

	// --- Material wiring --------------------------------------------------

	/**
	 * Optional, and the thing that makes material wiring possible at all: a
	 * material cannot reference a texture that only exists at runtime, so when
	 * this is set the simulation writes its height+normal output into this
	 * asset instead of an auto-created transient one. Your material can then
	 * sample it with a plain Texture Sample node.
	 *
	 * Create it as a Texture Render Target 2D with Render Target Format
	 * RGBA16f and "Can Create UAV" ticked. The subsystem fixes up the UAV flag
	 * at runtime if you forget, and warns about the format.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Material Wiring", meta = (AllowedClasses = "/Script/Engine.TextureRenderTarget2D"))
	TSoftObjectPtr<UTextureRenderTarget2D> SnowDataRenderTarget;

	/**
	 * Optional. When set, the region vector below is pushed into this
	 * collection every frame, so a material can convert world position into
	 * snow-texture UVs without any Blueprint of its own.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Material Wiring", meta = (AllowedClasses = "/Script/Engine.MaterialParameterCollection"))
	TSoftObjectPtr<UMaterialParameterCollection> ParameterCollection;

	/**
	 * Name of the vector parameter carrying everything a material needs to
	 * place the texture in the world:
	 *   R = region centre X, G = region centre Y,
	 *   B = region size (world units), A = max depth (world units).
	 * Used both for the collection above and for ApplySnowParametersToMaterial.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Material Wiring")
	FName RegionParameterName;

	/** Texture parameter name used by ApplySnowParametersToMaterial / RegisterSnowMaterial. */
	UPROPERTY(config, EditAnywhere, Category = "Material Wiring")
	FName SnowDataParameterName;
};
