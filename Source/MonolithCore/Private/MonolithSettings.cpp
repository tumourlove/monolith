#include "MonolithSettings.h"

UMonolithSettings::UMonolithSettings()
{
}

const UMonolithSettings* UMonolithSettings::Get()
{
	return GetDefault<UMonolithSettings>();
}

TArray<FName> UMonolithSettings::GetIndexedContentPaths()
{
	TArray<FName> Paths;
	Paths.Add(FName(TEXT("/Game")));

	if (const UMonolithSettings* Settings = Get())
	{
		for (const FString& Path : Settings->AdditionalContentPaths)
		{
			if (!Path.IsEmpty())
			{
				Paths.AddUnique(FName(*Path));
			}
		}
	}

	return Paths;
}

bool UMonolithSettings::IsIndexedContentPath(const FString& PackagePath)
{
	if (PackagePath.StartsWith(TEXT("/Game/")))
	{
		return true;
	}

	if (const UMonolithSettings* Settings = Get())
	{
		for (const FString& ContentPath : Settings->AdditionalContentPaths)
		{
			if (!ContentPath.IsEmpty() && PackagePath.StartsWith(ContentPath + TEXT("/")))
			{
				return true;
			}
		}
	}

	return false;
}
