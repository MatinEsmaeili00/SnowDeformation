// Copyright Matin. All Rights Reserved.

#pragma once

#include "Logging/LogMacros.h"
#include "Modules/ModuleManager.h"

SNOWDEFORMATION_API DECLARE_LOG_CATEGORY_EXTERN(LogSnowDeformation, Log, All);

/**
 * Module entry point. All it does is map the plugin's Shaders/ folder onto
 * the virtual "/SnowDeformationShaders" path so SnowDeformation.usf can be
 * found by the shader compiler - see SnowDeformationComputePass.cpp for
 * where that virtual path is actually used.
 */
class FSnowDeformationModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
