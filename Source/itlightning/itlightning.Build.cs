// Copyright (C) 2024 IT Lightning, LLC. All rights reserved.
// Licensed software - see LICENSE

using UnrealBuildTool;

public class itlightning : ModuleRules
{
	public itlightning(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
				"itlightning/Public"
			}
			);


        PrivateIncludePaths.AddRange(
			new string[] {
                "itlightning/Private"
            }
			);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				// ... add other public dependencies that you statically link with here ...
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
