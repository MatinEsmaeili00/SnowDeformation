// Copyright Matin. All Rights Reserved.

using UnrealBuildTool;

public class SnowDeformation : ModuleRules
{
	public SnowDeformation(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				// USnowDeformationSettings derives from UDeveloperSettings, so
				// anything including our public headers needs this too.
				"DeveloperSettings",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Projects",
				"RHI",
				"RenderCore",
			}
		);
	}
}
