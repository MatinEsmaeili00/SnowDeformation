// Copyright Matin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "SnowDeformerComponent.generated.h"

class USkeletalMeshComponent;

/**
 * One world-space press into the snow, produced by a component this frame.
 * The subsystem converts these into GPU-ready FSnowDeformerGPU entries once
 * it knows where the simulated region is currently centred.
 */
USTRUCT(BlueprintType)
struct FSnowDeformationContact
{
	GENERATED_BODY()

	/** Where the print lands, in world space. Only X/Y matter to the simulation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Deformation")
	FVector WorldLocation = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Deformation")
	float Radius = 24.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Deformation")
	float Depth = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Deformation")
	float Falloff = 2.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Deformation")
	float RimWidth = 9.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Deformation")
	float RimHeight = 0.3f;

	/** 0..1 master multiplier - fades a print out as the foot lifts off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Deformation")
	float Strength = 1.0f;
};

/**
 * Attach to a character (or any actor that should press into the snow).
 * Every frame the SnowDeformationSubsystem asks this component for its
 * currently active contacts via GetActiveContacts() - there is no ticking
 * here, so an idle component costs nothing beyond the registration.
 *
 * Two contact modes:
 *   - Skeletal: traces straight down from each name in FootSocketNames and
 *     treats a socket as "planted" whenever the trace hits ground within
 *     ContactHeight of the socket. This is what makes footprints land where
 *     the feet actually are instead of under the capsule's centre.
 *   - Fallback: if no skeletal mesh / sockets are found and
 *     SimpleContactFallbackRadius > 0, stamps a single print under the
 *     actor's collision bounds instead. Useful for simple pawns, vehicles,
 *     or props that don't have a foot-bone rig.
 */
UCLASS(ClassGroup = (SnowDeformation), meta = (BlueprintSpawnableComponent))
class SNOWDEFORMATION_API USnowDeformerComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	USnowDeformerComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Appends this component's currently active prints, in world space. Called by the subsystem; safe to call any time. */
	void GetActiveContacts(TArray<FSnowDeformationContact>& OutContacts) const;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Deformation")
	TArray<FName> FootSocketNames = { TEXT("foot_l"), TEXT("foot_r") };

	/** How far below each foot socket to trace looking for ground. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Deformation", meta = (ClampMin = "1.0"))
	float TraceDownDistance = 40.0f;

	/** A foot counts as "planted" when the ground hit is within this many units of the socket. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Deformation", meta = (ClampMin = "0.0"))
	float ContactHeight = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Deformation|Footprint")
	float FootprintRadius = 22.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Deformation|Footprint")
	float FootprintDepth = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Deformation|Footprint")
	float FootprintFalloff = 2.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Deformation|Footprint")
	float RimWidth = 9.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Deformation|Footprint")
	float RimHeight = 0.3f;

	/** Horizontal speed (cm/s) at which prints reach full strength. Below this they fade but never fully vanish. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Deformation|Footprint", meta = (ClampMin = "1.0"))
	float MinSpeedForFullStrength = 80.0f;

	/** Used only when no skeletal mesh with the named sockets is found. 0 disables the fallback. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Deformation|Fallback", meta = (ClampMin = "0.0"))
	float SimpleContactFallbackRadius = 0.0f;

	/** Which collision channel the ground trace uses. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Snow Deformation")
	TEnumAsByte<ECollisionChannel> TraceChannel = ECC_WorldStatic;

private:
	USkeletalMeshComponent* FindSkeletalMesh() const;
};
