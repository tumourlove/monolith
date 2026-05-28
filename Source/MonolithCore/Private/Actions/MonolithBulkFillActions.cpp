// SPDX-License-Identifier: MIT
// Central dispatcher actions for the bulk_fill + describe namespaces.
// Routes incoming JSON-RPC params to the per-namespace adapter table.
// Phase 0 — registered from FMonolithCoreModule::StartupModule.

#include "Actions/MonolithBulkFillActions.h"
#include "MonolithBulkFillRegistry.h"
#include "MonolithBulkFillTypes.h"
#include "Reflection/MonolithDryRunGuard.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "Dom/JsonObject.h"

namespace MonolithBulkFillActionsInternal
{
	static TSharedPtr<FJsonObject> BuildBulkFillSchema()
	{
		return FParamSchemaBuilder()
			.Required(TEXT("target_namespace"), TEXT("string"),
				TEXT("Adapter namespace ('blueprint', 'gas', 'inventory', 'ui', 'ai', 'niagara', 'material', 'audio', 'mesh', 'animation', 'logicdriver', 'combograph')."))
			.Required(TEXT("target"), TEXT("string"),
				TEXT("Asset path or adapter-defined target identifier (e.g. '/Game/Items/DA_HealingPotion')."))
			.Required(TEXT("tree"), TEXT("object"),
				TEXT("Nested JSON object of properties to walk against the target's reflection schema."))
			.Optional(TEXT("dry_run"), TEXT("boolean"),
				TEXT("If true, validate only — emit the would-be writes but do not persist."),
				TEXT("false"))
			.Optional(TEXT("strict"), TEXT("boolean"),
				TEXT("If true, promote silent drops / clamps / unknown-fields to hard errors."),
				TEXT("false"))
			.Build();
	}

	static TSharedPtr<FJsonObject> BuildBulkFillListNamespacesSchema()
	{
		return FParamSchemaBuilder().Build();
	}

	static TSharedPtr<FJsonObject> BuildDescribeSchema()
	{
		return FParamSchemaBuilder()
			.Required(TEXT("target_namespace"), TEXT("string"),
				TEXT("Adapter namespace whose schema should be introspected."))
			.Required(TEXT("target"), TEXT("string"),
				TEXT("Asset path or action name to describe."))
			.Build();
	}

	static TSharedPtr<FJsonObject> BuildDescribeListTargetsSchema()
	{
		return FParamSchemaBuilder()
			.Required(TEXT("target_namespace"), TEXT("string"),
				TEXT("Adapter namespace whose introspection inventory should be listed."))
			.Build();
	}

	// Serialise an FSchemaDescriptor tree to JSON for the response payload.
	static TSharedPtr<FJsonObject> DescriptorToJson(const FSchemaDescriptor& Desc)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("field_path"), Desc.FieldPath);
		O->SetStringField(TEXT("type_name"), Desc.TypeName);
		O->SetStringField(TEXT("import_text_form"), Desc.ImportTextForm);
		O->SetBoolField(TEXT("required"), Desc.bRequired);
		O->SetBoolField(TEXT("set_once"), Desc.bSetOnce);
		O->SetBoolField(TEXT("pie_blocked"), Desc.bPieBlocked);
		if (!Desc.ConditionalOn.IsEmpty())
		{
			O->SetStringField(TEXT("conditional_on"), Desc.ConditionalOn);
		}
		O->SetNumberField(TEXT("range_min"), Desc.RangeMin);
		O->SetNumberField(TEXT("range_max"), Desc.RangeMax);
		if (Desc.EnumValues.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Vals;
			for (const FString& E : Desc.EnumValues) { Vals.Add(MakeShared<FJsonValueString>(E)); }
			O->SetArrayField(TEXT("enum_values"), Vals);
		}
		if (Desc.Children.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Kids;
			for (const FSchemaDescriptor& C : Desc.Children)
			{
				Kids.Add(MakeShared<FJsonValueObject>(DescriptorToJson(C)));
			}
			O->SetArrayField(TEXT("children"), Kids);
		}
		return O;
	}

	// --- bulk_fill.apply ---
	static FMonolithActionResult HandleBulkFillApply(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
		{
			return FMonolithActionResult::Error(TEXT("bulk_fill.apply requires params"));
		}

		FBulkFillSpec Spec;
		Params->TryGetStringField(TEXT("target_namespace"), Spec.TargetNamespace);
		Params->TryGetStringField(TEXT("target"), Spec.TargetAsset);
		Spec.Tree = Params->GetObjectField(TEXT("tree"));
		Params->TryGetBoolField(TEXT("dry_run"), Spec.bDryRun);
		Params->TryGetBoolField(TEXT("strict"), Spec.bStrict);

		if (Spec.TargetNamespace.IsEmpty() || Spec.TargetAsset.IsEmpty() || !Spec.Tree.IsValid())
		{
			return FMonolithActionResult::Error(
				TEXT("bulk_fill.apply requires target_namespace, target, and tree (JSON object)"),
				FMonolithJsonUtils::ErrInvalidParams);
		}

		if (!FMonolithBulkFillRegistry::Get().HasAdapter(Spec.TargetNamespace))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("no bulk_fill adapter registered for namespace '%s' (Phase 0 ships dispatcher only; per-namespace adapters land in Phases 1-5)"),
					*Spec.TargetNamespace),
				FMonolithJsonUtils::ErrOptionalDepUnavailable);
		}

		const FDryRunReport Report = FMonolithBulkFillRegistry::Get().DispatchBulkFill(Spec);
		return FMonolithActionResult::Success(FMonolithDryRunGuard::ReportToJson(Report));
	}

	// --- bulk_fill.list_namespaces ---
	static FMonolithActionResult HandleBulkFillListNamespaces(const TSharedPtr<FJsonObject>& /*Params*/)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		const TArray<FString> Namespaces = FMonolithBulkFillRegistry::Get().GetRegisteredNamespaces();

		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FString& Ns : Namespaces)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("namespace"), Ns);
			Entry->SetBoolField(TEXT("available"), true);
			Arr.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Out->SetArrayField(TEXT("namespaces"), Arr);
		Out->SetNumberField(TEXT("count"), Namespaces.Num());
		return FMonolithActionResult::Success(Out);
	}

	// --- describe.schema ---
	static FMonolithActionResult HandleDescribeSchema(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
		{
			return FMonolithActionResult::Error(TEXT("describe.schema requires params"));
		}

		FString TargetNamespace;
		FString Target;
		Params->TryGetStringField(TEXT("target_namespace"), TargetNamespace);
		Params->TryGetStringField(TEXT("target"), Target);

		if (TargetNamespace.IsEmpty() || Target.IsEmpty())
		{
			return FMonolithActionResult::Error(
				TEXT("describe.schema requires target_namespace and target"),
				FMonolithJsonUtils::ErrInvalidParams);
		}

		if (!FMonolithBulkFillRegistry::Get().HasAdapter(TargetNamespace))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("no describe adapter registered for namespace '%s'"), *TargetNamespace),
				FMonolithJsonUtils::ErrOptionalDepUnavailable);
		}

		const FSchemaDescriptor Root = FMonolithBulkFillRegistry::Get().DispatchDescribe(TargetNamespace, Target);
		return FMonolithActionResult::Success(DescriptorToJson(Root));
	}

	// --- describe.list_targets ---
	static FMonolithActionResult HandleDescribeListTargets(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
		{
			return FMonolithActionResult::Error(TEXT("describe.list_targets requires params"));
		}

		FString TargetNamespace;
		Params->TryGetStringField(TEXT("target_namespace"), TargetNamespace);

		if (TargetNamespace.IsEmpty())
		{
			return FMonolithActionResult::Error(
				TEXT("describe.list_targets requires target_namespace"),
				FMonolithJsonUtils::ErrInvalidParams);
		}

		const TArray<FString> Targets = FMonolithBulkFillRegistry::Get().DispatchListTargets(TargetNamespace);

		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FString& T : Targets) { Arr.Add(MakeShared<FJsonValueString>(T)); }
		Out->SetArrayField(TEXT("targets"), Arr);
		Out->SetStringField(TEXT("namespace"), TargetNamespace);
		Out->SetNumberField(TEXT("count"), Targets.Num());
		return FMonolithActionResult::Success(Out);
	}

	// --- describe::action_schema (gap #5) ---
	// Surfaces a registered ACTION's param schema (names / types / required / defaults /
	// aliases / descriptions) so callers stop trial-and-erroring param names. The data already
	// lives in FMonolithActionInfo.ParamSchema (the same object discover serializes as "params");
	// this just reads it back by (namespace, action). Closes the cause behind gaps #4/#12/#13.
	static TSharedPtr<FJsonObject> BuildDescribeActionSchemaSchema()
	{
		return FParamSchemaBuilder()
			.Required(TEXT("target_namespace"), TEXT("string"), TEXT("Namespace that owns the action (e.g. \"blueprint\", \"ui\")"))
			.Required(TEXT("action"), TEXT("string"), TEXT("Action name whose param schema to return (e.g. \"add_nodes_bulk\")"))
			.Build();
	}

	static FMonolithActionResult HandleDescribeActionSchema(const TSharedPtr<FJsonObject>& Params)
	{
		FString TargetNamespace;
		if (!Params->TryGetStringField(TEXT("target_namespace"), TargetNamespace) || TargetNamespace.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("missing required parameter: target_namespace"));
		}
		FString ActionName;
		if (!Params->TryGetStringField(TEXT("action"), ActionName) || ActionName.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("missing required parameter: action"));
		}

		FMonolithToolRegistry& Reg = FMonolithToolRegistry::Get();
		const TArray<FMonolithActionInfo> Actions = Reg.GetActions(TargetNamespace);
		const FMonolithActionInfo* Found = Actions.FindByPredicate(
			[&ActionName](const FMonolithActionInfo& Info){ return Info.Action == ActionName; });

		if (!Found)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("action '%s' not found in namespace '%s'. Use monolith_discover(\"%s\") to list available actions."),
				*ActionName, *TargetNamespace, *TargetNamespace));
		}

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("namespace"), Found->Namespace);
		Out->SetStringField(TEXT("action"), Found->Action);
		Out->SetStringField(TEXT("description"), Found->Description);
		if (!Found->Category.IsEmpty())
		{
			Out->SetStringField(TEXT("category"), Found->Category);
		}
		// Same JSON-Schema "properties" object discover serializes as "params".
		Out->SetObjectField(TEXT("params"),
			Found->ParamSchema.IsValid() ? Found->ParamSchema : MakeShared<FJsonObject>());
		return FMonolithActionResult::Success(Out);
	}
} // namespace MonolithBulkFillActionsInternal

void FMonolithBulkFillActions::RegisterAll()
{
	using namespace MonolithBulkFillActionsInternal;

	FMonolithToolRegistry& Reg = FMonolithToolRegistry::Get();

	// Reminder per ref_monolith_action_registry_api.md:
	//   RegisterAction(namespace, action, DESCRIPTION, handler, ParamSchema, Category)
	// Description is the THIRD param (NOT a keyword arg; positional).

	Reg.RegisterAction(
		TEXT("bulk_fill"),
		TEXT("apply"),
		TEXT("Apply a JSON-tree fill to an asset via the target namespace's adapter. Supports dry_run + strict."),
		FMonolithActionHandler::CreateStatic(&HandleBulkFillApply),
		BuildBulkFillSchema());

	Reg.RegisterAction(
		TEXT("bulk_fill"),
		TEXT("list_namespaces"),
		TEXT("List target_namespace values the bulk_fill registry currently knows about (one row per registered adapter)."),
		FMonolithActionHandler::CreateStatic(&HandleBulkFillListNamespaces),
		BuildBulkFillListNamespacesSchema());

	Reg.RegisterAction(
		TEXT("describe"),
		TEXT("schema"),
		TEXT("Return a rich FSchemaDescriptor tree (type names, ImportText forms, enum-value lists, clamp ranges, nested children) for an asset/action via its namespace adapter."),
		FMonolithActionHandler::CreateStatic(&HandleDescribeSchema),
		BuildDescribeSchema());

	Reg.RegisterAction(
		TEXT("describe"),
		TEXT("list_targets"),
		TEXT("List the asset paths / action names the describe adapter can introspect for a given target_namespace."),
		FMonolithActionHandler::CreateStatic(&HandleDescribeListTargets),
		BuildDescribeListTargetsSchema());

	Reg.RegisterAction(
		TEXT("describe"),
		TEXT("action_schema"),
		TEXT("Return a registered ACTION's param schema (names, types, required, defaults, aliases, descriptions) by (target_namespace, action). Closes param-name discoverability so callers stop trial-and-erroring param names."),
		FMonolithActionHandler::CreateStatic(&HandleDescribeActionSchema),
		BuildDescribeActionSchemaSchema());

	UE_LOG(LogMonolith, Log, TEXT("MonolithBulkFillActions: registered 2 namespaces (bulk_fill + describe) with 5 actions total"));
}

void FMonolithBulkFillActions::UnregisterAll()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("bulk_fill"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("describe"));
}
