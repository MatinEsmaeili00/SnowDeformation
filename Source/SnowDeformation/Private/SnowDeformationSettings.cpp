// Copyright Matin. All Rights Reserved.

#include "SnowDeformationSettings.h"

USnowDeformationSettings::USnowDeformationSettings()
	: TextureResolution(1024, 1024)
	, RegionSizeWorld(4096.0f)
	, MaxDepthWorld(20.0f)
	, RefillRate(0.02f)
	, NormalStrength(1.5f)
	, RegionParameterName(TEXT("SnowRegion"))
	, SnowDataParameterName(TEXT("SnowData"))
{
}

FName USnowDeformationSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}

const USnowDeformationSettings& USnowDeformationSettings::Get()
{
	const USnowDeformationSettings* Settings = GetDefault<USnowDeformationSettings>();
	check(Settings);
	return *Settings;
}
