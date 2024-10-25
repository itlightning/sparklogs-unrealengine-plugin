// Copyright (C) 2024 IT Lightning, LLC. All rights reserved.
// Licensed software - see LICENSE

using System.IO;
using UnrealBuildTool;

public class itlightning : ModuleRules
{
	public itlightning(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
                Path.Combine(ModuleDirectory, "Public")
            }
			);


        PrivateIncludePaths.AddRange(
			new string[] {
                Path.Combine(ModuleDirectory, "Private"),
                Path.Combine(EngineDirectory, "Source/Runtime/Tracelog/Private")
            }
			);

		//AddEngineThirdPartyPrivateStaticDependencies(Target, "zlib");
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"HTTP"
			}
			);

		/** Already set and managed in UE PCH file...
        if (Target.Configuration != UnrealTargetConfiguration.Shipping)
        {
            PublicDefinitions.Add("WITH_AUTOMATION_TESTS=1");
        }
        else
        {
            PublicDefinitions.Add("WITH_AUTOMATION_TESTS=0");
        }
		*/

        PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",
				// ... add private dependencies that you statically link with here ...	
			}
			);
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}
