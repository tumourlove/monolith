// MonolithCommonUIHelpers.cpp
// Implementation of shared CommonUI authoring / mutation / runtime helpers.
#include "MonolithCommonUIHelpers.h"

#if WITH_COMMONUI

#include "MonolithJsonUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Engine/World.h"
#include "Editor.h"
#include "MonolithAssetUtils.h"

namespace MonolithCommonUI
{
	FMonolithActionResult CreateAsset(
		UClass* AssetClass,
		const FString& PackagePath,
		const FString& AssetName,
		bool bSkipSave,
		UObject*& OutAsset)
	{
		OutAsset = nullptr;

		if (!AssetClass)
		{
			return FMonolithActionResult::Error(TEXT("CreateAsset: AssetClass is null"));
		}
		if (PackagePath.IsEmpty() || AssetName.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("CreateAsset: PackagePath and AssetName required"));
		}

		const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *AssetName);

		UPackage* Package = CreatePackage(*FullPath);
		if (!Package)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("CreateAsset: CreatePackage failed for '%s'"), *FullPath));
		}

		// Collision check — both in-memory (FindObject) and on-disk (AssetRegistry).
		if (FindObject<UObject>(Package, *AssetName))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("CreateAsset: asset already exists at '%s'"), *FullPath));
		}
		const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *FullPath, *AssetName);
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		if (ARM.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid())
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("CreateAsset: asset already exists at '%s' (on disk)"), *FullPath));
		}

		UObject* NewAsset = NewObject<UObject>(Package, AssetClass, FName(*AssetName), RF_Public | RF_Standalone);
		if (!NewAsset)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("CreateAsset: NewObject returned null for class '%s'"), *AssetClass->GetName()));
		}

		FAssetRegistryModule::AssetCreated(NewAsset);
		Package->MarkPackageDirty();

		if (!bSkipSave)
		{
			FMonolithAssetUtils::SaveAssetPackage(Package);
		}

		OutAsset = NewAsset;

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_path"), FullPath);
		Result->SetStringField(TEXT("asset_name"), AssetName);
		Result->SetStringField(TEXT("class"), AssetClass->GetName());
		return FMonolithActionResult::Success(Result);
	}

	FMonolithActionResult LoadWidgetForMutation(
		const FString& WbpPath,
		const FName& WidgetName,
		UWidgetBlueprint*& OutWbp,
		UWidget*& OutWidget)
	{
		OutWbp = nullptr;
		OutWidget = nullptr;

		UWidgetBlueprint* Wbp = LoadObject<UWidgetBlueprint>(nullptr, *WbpPath);
		if (!Wbp)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("LoadWidgetForMutation: failed to load WBP '%s'"), *WbpPath));
		}
		if (!Wbp->WidgetTree)
		{
			return FMonolithActionResult::Error(TEXT("LoadWidgetForMutation: WBP has no WidgetTree"));
		}

		// Traverse the widget tree looking for the named widget.
		UWidget* Found = nullptr;
		Wbp->WidgetTree->ForEachWidget([&Found, &WidgetName](UWidget* Widget)
		{
			if (!Found && Widget && Widget->GetFName() == WidgetName)
			{
				Found = Widget;
			}
		});

		if (!Found)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("LoadWidgetForMutation: widget '%s' not found in '%s'"),
				*WidgetName.ToString(), *WbpPath));
		}

		OutWbp = Wbp;
		OutWidget = Found;

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), WbpPath);
		Result->SetStringField(TEXT("widget_name"), WidgetName.ToString());
		Result->SetStringField(TEXT("widget_class"), Found->GetClass()->GetName());
		return FMonolithActionResult::Success(Result);
	}

	FMonolithActionResult CompileAndSaveWidgetBlueprint(UWidgetBlueprint* Wbp)
	{
		if (!Wbp)
		{
			return FMonolithActionResult::Error(TEXT("CompileAndSaveWidgetBlueprint: Wbp is null"));
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Wbp);
		FKismetEditorUtilities::CompileBlueprint(Wbp);

		UPackage* Package = Wbp->GetOutermost();
		Package->MarkPackageDirty();

		const FString PackageName = Package->GetName();
		FMonolithAssetUtils::SaveAssetPackage(Package);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("wbp_path"), PackageName);
		Result->SetBoolField(TEXT("compiled"), true);
		Result->SetBoolField(TEXT("saved"), true);
		return FMonolithActionResult::Success(Result);
	}

	FMonolithActionResult InvokeRuntimeFunction(
		UObject* Target,
		const FName& FunctionName,
		const TSharedPtr<FJsonObject>& ParamsJson,
		TSharedPtr<FJsonObject>& OutResult)
	{
		OutResult = MakeShared<FJsonObject>();

		if (!Target)
		{
			return FMonolithActionResult::Error(TEXT("InvokeRuntimeFunction: Target is null"));
		}

		UFunction* Func = Target->FindFunction(FunctionName);
		if (!Func)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("InvokeRuntimeFunction: function '%s' not found on '%s'"),
				*FunctionName.ToString(), *Target->GetClass()->GetName()));
		}

		// Allocate parameter frame. Zero-init ensures defaults for any unfilled params.
		TArray<uint8> ParmFrame;
		ParmFrame.SetNumZeroed(Func->ParmsSize);

		// Marshal JSON → frame.
		if (ParamsJson.IsValid())
		{
			for (TFieldIterator<FProperty> ParmIt(Func); ParmIt && ParmIt->HasAnyPropertyFlags(CPF_Parm); ++ParmIt)
			{
				FProperty* Parm = *ParmIt;
				if (Parm->HasAnyPropertyFlags(CPF_ReturnParm)) continue;
				if (!ParamsJson->HasField(Parm->GetName())) continue;

				TSharedPtr<FJsonValue> JsonVal = ParamsJson->TryGetField(Parm->GetName());
				if (JsonVal.IsValid())
				{
					// ImportText into the parm's slot in the frame.
					void* ParmPtr = Parm->ContainerPtrToValuePtr<void>(ParmFrame.GetData(), 0);
					FString TextVal;
					if (JsonVal->TryGetString(TextVal))
					{
						Parm->ImportText_Direct(*TextVal, ParmPtr, nullptr, 0);
					}
				}
			}
		}

		Target->ProcessEvent(Func, ParmFrame.GetData());

		// Extract return-value parameter (if any) back to JSON.
		for (TFieldIterator<FProperty> ParmIt(Func); ParmIt && ParmIt->HasAnyPropertyFlags(CPF_Parm); ++ParmIt)
		{
			if (ParmIt->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				FString ExportedText;
				void* ReturnPtr = ParmIt->ContainerPtrToValuePtr<void>(ParmFrame.GetData(), 0);
				ParmIt->ExportTextItem_Direct(ExportedText, ReturnPtr, nullptr, nullptr, PPF_None);
				OutResult->SetStringField(TEXT("return_value"), ExportedText);
				break;
			}
		}

		return FMonolithActionResult::Success(OutResult);
	}

	bool SetPropertyFromJson(UObject* Container, FProperty* Property, const TSharedPtr<FJsonValue>& JsonValue)
	{
		if (!Container || !Property || !JsonValue.IsValid())
		{
			return false;
		}

		void* Ptr = Property->ContainerPtrToValuePtr<void>(Container);

		// Object pointer (TObjectPtr<UObject>) — accept class path string, LoadObject.
		if (FObjectProperty* ObjectProp = CastField<FObjectProperty>(Property))
		{
			FString Path;
			if (JsonValue->TryGetString(Path))
			{
				UObject* Resolved = LoadObject<UObject>(nullptr, *Path);
				if (Resolved && Resolved->IsA(ObjectProp->PropertyClass))
				{
					ObjectProp->SetObjectPropertyValue(Ptr, Resolved);
					return true;
				}
			}
			return false;
		}

		// Class pointer (TSubclassOf<T>) — accept class path string, LoadClass.
		if (FClassProperty* ClassProp = CastField<FClassProperty>(Property))
		{
			FString Path;
			if (JsonValue->TryGetString(Path))
			{
				UClass* ResolvedClass = LoadClass<UObject>(nullptr, *Path);
				if (ResolvedClass && (!ClassProp->MetaClass || ResolvedClass->IsChildOf(ClassProp->MetaClass)))
				{
					ClassProp->SetObjectPropertyValue(Ptr, ResolvedClass);
					return true;
				}
			}
			return false;
		}

		// Default path: string-serialize + ImportText. Works for primitives, enums, simple structs
		// (FLinearColor, FMargin, FVector2D all accept text form).
		FString TextVal;
		if (JsonValue->TryGetString(TextVal))
		{
			return Property->ImportText_Direct(*TextVal, Ptr, nullptr, PPF_None) != nullptr;
		}

		// Try numeric coerce for non-string JSON values.
		double NumVal = 0.0;
		if (JsonValue->TryGetNumber(NumVal))
		{
			FString NumStr = FString::SanitizeFloat(NumVal);
			return Property->ImportText_Direct(*NumStr, Ptr, nullptr, PPF_None) != nullptr;
		}

		bool BoolVal = false;
		if (JsonValue->TryGetBool(BoolVal))
		{
			FString BoolStr = BoolVal ? TEXT("true") : TEXT("false");
			return Property->ImportText_Direct(*BoolStr, Ptr, nullptr, PPF_None) != nullptr;
		}

		return false;
	}

	UWorld* GetPIEWorld()
	{
		if (!GEditor) return nullptr;

		for (const FWorldContext& Ctx : GEditor->GetWorldContexts())
		{
			if (Ctx.WorldType == EWorldType::PIE && Ctx.World())
			{
				return Ctx.World();
			}
		}
		return nullptr;
	}
}

#endif // WITH_COMMONUI
