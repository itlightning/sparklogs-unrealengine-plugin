// Copyright (C) 2024-2025 IT Lightning, LLC. All rights reserved.
// Licensed software - see LICENSE

using System.IO;
using UnrealBuildTool;

public class sparklogs : ModuleRules
{
	public sparklogs(ReadOnlyTargetRules Target) : base(Target)
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
				"HTTP",
				"Json",
				"Projects"
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
                "Analytics",
            }
            );

        PrivateIncludePathModuleNames.AddRange(
            new string[] {
                "Settings",
            });

        DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}
