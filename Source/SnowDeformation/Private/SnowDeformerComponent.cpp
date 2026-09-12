// Copyright Matin. All Rights Reserved.

#include "SnowDeformerComponent.h"

#include "CollisionQueryParams.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "SnowDeformationSubsystem.h"

USnowDeformerComponent::USnowDeformerComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	bAutoActivate = true;
}

void USnowDeformerComponent::BeginPlay()
{
	Super::BeginPlay();

	if (UWorld* World = GetWorld())
	{
		if (USnowDeformationSubsystem* Subsystem = World->GetSubsystem<USnowDeformationSubsystem>())
		{
			Subsystem->RegisterDeformer(this);
		}
	}
}

void USnowDeformerComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		if (USnowDeformationSubsystem* Subsystem = World->GetSubsystem<USnowDeformationSubsystem>())
		{
			Subsystem->UnregisterDeformer(this);
		}
	}

	Super::EndPlay(EndPlayReason);
}

USkeletalMeshComponent* USnowDeformerComponent::FindSkeletalMesh() const
{
	const AActor* Owner = GetOwner();
	return Owner ? Owner->FindComponentByClass<USkeletalMeshComponent>() : nullptr;
}

void USnowDeformerComponent::GetActiveContacts(TArray<FSnowDeformationContact>& OutContacts) const
{
	AActor* Owner = GetOwner();
	UWorld* World = GetWorld();
	if (!Owner || !World)
	{
		return;
	}

	const float Speed = Owner->GetVelocity().Size2D();
	const float SpeedAlpha = MinSpeedForFullStrength > KINDA_SMALL_NUMBER
		? FMath::Clamp(Speed / MinSpeedForFullStrength, 0.0f, 1.0f)
		: 1.0f;
	// Never fully fade out: a planted foot should still leave a mark even
	// while standing still, just a lighter one than a running stride.
	const float Strength = FMath::Max(SpeedAlpha, 0.35f);

	USkeletalMeshComponent* SkelMesh = FindSkeletalMesh();
	bool bAnyFootContact = false;

	if (SkelMesh)
	{
		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(SnowDeformerFootTrace), /*bTraceComplex=*/false, Owner);
		QueryParams.AddIgnoredActor(Owner);

		for (const FName& Socket : FootSocketNames)
		{
			if (!SkelMesh->DoesSocketExist(Socket))
			{
				continue;
			}

			const FVector SocketLoc = SkelMesh->GetSocketLocation(Socket);
			const FVector TraceEnd = SocketLoc - FVector(0.0f, 0.0f, TraceDownDistance);

			FHitResult Hit;
			if (World->LineTraceSingleByChannel(Hit, SocketLoc, TraceEnd, TraceChannel, QueryParams))
			{
				const float HeightAboveGround = SocketLoc.Z - Hit.Location.Z;
				if (HeightAboveGround <= ContactHeight)
				{
					bAnyFootContact = true;

					FSnowDeformationContact Contact;
					Contact.WorldLocation = Hit.Location;
					Contact.Radius = FootprintRadius;
					Contact.Depth = FootprintDepth;
					Contact.Falloff = FootprintFalloff;
					Contact.RimWidth = RimWidth;
					Contact.RimHeight = RimHeight;
					Contact.Strength = Strength;
					OutContacts.Add(Contact);
				}
			}
		}
	}

	if (!bAnyFootContact && SimpleContactFallbackRadius > 0.0f)
	{
		FVector Origin, BoxExtent;
		Owner->GetActorBounds(false, Origin, BoxExtent);

		FSnowDeformationContact Contact;
		Contact.WorldLocation = FVector(Origin.X, Origin.Y, Origin.Z - BoxExtent.Z);
		Contact.Radius = SimpleContactFallbackRadius;
		Contact.Depth = FootprintDepth;
		Contact.Falloff = FootprintFalloff;
		Contact.RimWidth = RimWidth;
		Contact.RimHeight = RimHeight;
		Contact.Strength = Strength;
		OutContacts.Add(Contact);
	}
}
