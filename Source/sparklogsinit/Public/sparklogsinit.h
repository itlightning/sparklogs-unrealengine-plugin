// Copyright (C) 2024-2025 IT Lightning, LLC. All rights reserved.
// Licensed software - see LICENSE

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FsparklogsinitModule : public IModuleInterface
{

public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
