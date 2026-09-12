// Copyright Matin. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

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
