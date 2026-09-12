// Copyright Matin. All Rights Reserved.

#include "SnowDeformation.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "FSnowDeformationModule"

DEFINE_LOG_CATEGORY(LogSnowDeformation);

void FSnowDeformationModule::StartupModule()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SnowDeformation"));
	if (!Plugin.IsValid())
	{
		return;
	}

	const FString PluginShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));

	if (!AllShaderSourceDirectoryMappings().Contains(TEXT("/SnowDeformationShaders")))
	{
		AddShaderSourceDirectoryMapping(TEXT("/SnowDeformationShaders"), PluginShaderDir);
	}
}

void FSnowDeformationModule::ShutdownModule()
{
	// Nothing to clean up: shader directory mappings are process-global and
	// other modules may still rely on the mapping table during shutdown.
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FSnowDeformationModule, SnowDeformation)
