// Copyright Matin. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class SnowDeformation : ModuleRules
{
	public SnowDeformation(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicIncludePaths.AddRange(
			new string[]
			{
				// CreateRenderTarget() / render-target-pool helpers used by SnowDeformationComputePass.cpp
				// live in the Renderer module's private headers.
				Path.Combine(GetModuleDirectory("Renderer"), "Private"),
			}
		);

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"Engine",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Projects",
				"RHI",
				"RenderCore",
				"Renderer",
			}
		);
	}
}
