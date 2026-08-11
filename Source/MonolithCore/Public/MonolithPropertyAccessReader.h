#pragma once

#include "CoreMinimal.h"
#include "UObject/TextProperty.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "UObject/UnrealType.h"
#include "UObject/Field.h"
#include "UObject/PropertyPortFlags.h"

// ============================================================
//  MonolithPropertyAccessReader  (Gap 1 — resolved PropertyAccess path)
//
//  UK2Node_PropertyAccess is UCLASS(MinimalAPI) with its header in
//  Engine/Plugins/Developer/PropertyAccessNode/Source/PropertyAccessNode/Private
//  — not includable, not linkable. The write side (add_property_access_node in
//  MonolithBlueprintNodeActions.cpp) already spawns the node and sets its private
//  `Path` UPROPERTY via FProperty reflection; this is the const read-side inverse:
//  match by class-name string (the class symbol is unreachable) then reflectively
//  read the private UPROPERTYs Path / TextPath / ContextId / ResolvedPinType.
//
//  Lives here in MonolithCore/Public so the K2 node serializer
//  (MonolithBlueprintInternal.h) and the anim node serializer
//  (MonolithAnimationActions.cpp) share ONE implementation — no duplication.
//  Header-only inline; the FEdGraphPinType / UEdGraphSchema_K2 references compile
//  at the include site (both including modules already dep BlueprintGraph), so no
//  MonolithCore Build.cs dependency is required.
//
//  Degrades gracefully: optional fields (TextPath / ContextId / ResolvedPinType)
//  that do not resolve are simply omitted; `resolved` tracks the Path property
//  specifically and is emitted as false (rather than erroring) when Path cannot
//  be read — the reflective field names are an engine-layout dependency that
//  could rename across engine versions.
// ============================================================

namespace MonolithPropertyAccessReader
{
	// Flat pin-type string, matching MonolithBlueprintInternal::PinTypeToString /
	// SerializePin's "type" shape so the emitted resolved_pin_type has shape parity
	// with every other pin-type the blueprint reads surface.
	inline FString PinTypeToString(const FEdGraphPinType& PinType)
	{
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			return TEXT("exec");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
			return TEXT("bool");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int)
			return TEXT("int");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int64)
			return TEXT("int64");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Real)
			return PinType.PinSubCategory == TEXT("double") ? TEXT("double") : TEXT("float");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_String)
			return TEXT("string");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Name)
			return TEXT("name");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Text)
			return TEXT("text");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Byte)
			return TEXT("byte");

		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Object ||
			PinType.PinCategory == UEdGraphSchema_K2::PC_Class ||
			PinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject ||
			PinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass ||
			PinType.PinCategory == UEdGraphSchema_K2::PC_Interface)
		{
			FString TypeName = PinType.PinCategory.ToString();
			if (PinType.PinSubCategoryObject.IsValid())
			{
				TypeName += TEXT(":") + PinType.PinSubCategoryObject->GetName();
			}
			return TypeName;
		}
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			if (PinType.PinSubCategoryObject.IsValid())
			{
				return TEXT("struct:") + PinType.PinSubCategoryObject->GetName();
			}
			return TEXT("struct");
		}
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Enum)
		{
			if (PinType.PinSubCategoryObject.IsValid())
			{
				return TEXT("enum:") + PinType.PinSubCategoryObject->GetName();
			}
			return TEXT("enum");
		}
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
			return TEXT("wildcard");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate)
			return TEXT("delegate");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
			return TEXT("multicast_delegate");

		return PinType.PinCategory.ToString();
	}

	inline FString ContainerPrefix(const FEdGraphPinType& PinType)
	{
		switch (PinType.ContainerType)
		{
		case EPinContainerType::Array: return TEXT("array:");
		case EPinContainerType::Set:   return TEXT("set:");
		case EPinContainerType::Map:   return TEXT("map:");
		default:                       return TEXT("");
		}
	}

	// Returns true and sets a "property_access" object on OutNode when Node is a
	// UK2Node_PropertyAccess; returns false (no mutation) for any other node.
	inline bool SerializePropertyAccessBlock(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& OutNode)
	{
		if (!Node || !OutNode.IsValid()) return false;

		UClass* NodeClass = Node->GetClass();
		if (!NodeClass || NodeClass->GetName() != TEXT("K2Node_PropertyAccess"))
		{
			return false;
		}

		TSharedPtr<FJsonObject> PAObj = MakeShared<FJsonObject>();
		bool bResolved = true;

		// Path — TArray<FString>, FStrProperty inner (mirrors the established write idiom).
		if (FArrayProperty* PathProp = FindFProperty<FArrayProperty>(NodeClass, TEXT("Path")))
		{
			if (FStrProperty* InnerStr = CastField<FStrProperty>(PathProp->Inner))
			{
				const void* ArrayValuePtr = PathProp->ContainerPtrToValuePtr<void>(Node);
				FScriptArrayHelper Helper(PathProp, ArrayValuePtr);
				TArray<TSharedPtr<FJsonValue>> PathOut;
				for (int32 i = 0; i < Helper.Num(); ++i)
				{
					PathOut.Add(MakeShared<FJsonValueString>(InnerStr->GetPropertyValue(Helper.GetRawPtr(i))));
				}
				PAObj->SetArrayField(TEXT("path"), PathOut);
			}
			else
			{
				bResolved = false;
			}
		}
		else
		{
			bResolved = false;
		}

		// TextPath — FText display form.
		if (FTextProperty* TextProp = FindFProperty<FTextProperty>(NodeClass, TEXT("TextPath")))
		{
			PAObj->SetStringField(TEXT("text_path"),
				TextProp->GetPropertyValue_InContainer(Node).ToString());
		}

		// ContextId — FName (often empty).
		if (FNameProperty* ContextProp = FindFProperty<FNameProperty>(NodeClass, TEXT("ContextId")))
		{
			PAObj->SetStringField(TEXT("context_id"),
				ContextProp->GetPropertyValue_InContainer(Node).ToString());
		}

		// ResolvedPinType — FEdGraphPinType; serialize with the same flat-string shape
		// the blueprint read surface uses for every other pin type.
		if (FStructProperty* PinTypeProp = FindFProperty<FStructProperty>(NodeClass, TEXT("ResolvedPinType")))
		{
			if (PinTypeProp->Struct == TBaseStructure<FEdGraphPinType>::Get())
			{
				const FEdGraphPinType* PinTypePtr =
					PinTypeProp->ContainerPtrToValuePtr<FEdGraphPinType>(Node);
				if (PinTypePtr)
				{
					PAObj->SetStringField(TEXT("resolved_pin_type"),
						ContainerPrefix(*PinTypePtr) + PinTypeToString(*PinTypePtr));
				}
			}
		}

		PAObj->SetBoolField(TEXT("resolved"), bResolved);
		OutNode->SetObjectField(TEXT("property_access"), PAObj);
		return true;
	}
}
