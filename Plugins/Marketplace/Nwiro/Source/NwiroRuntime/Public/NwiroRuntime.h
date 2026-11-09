// Copyright 2026 Nwiro. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

class FNwiroRuntimeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
