#include "MonolithMaterialActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionCustomOutput.h"
#include "Materials/MaterialExpressionMaterialAttributeLayers.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionMaterialLayer.h"
#include "Materials/MaterialFunctionMaterialLayerBlend.h"
#include "Materials/MaterialFunctionInstance.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionStaticBoolParameter.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "MaterialExpressionIO.h"
#include "Materials/MaterialInstanceConstant.h"
#include "EditorAssetLibrary.h"
#include "MaterialEditingLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"
#include "MaterialShared.h"
#include "RHIShaderPlatform.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "ObjectTools.h"
#include "ImageUtils.h"
#include "UObject/SavePackage.h"
#include "AssetToolsModule.h"
#include "AssetImportTask.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCube.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/EngineTypes.h"
#include "Engine/TextureCollection.h"
#include "Engine/Font.h"
#include "VT/RuntimeVirtualTexture.h"
#include "SparseVolumeTexture/SparseVolumeTexture.h"
#include "Materials/MaterialParameterCollection.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "IMonolithGraphFormatter.h"

// ============================================================================
// Pin name normalization — UE's GetShortenPinName converts raw names to
// shortened forms, so we must do the same when matching by name.
// ============================================================================

static FString NormalizeInputPinName(const FString& PinName)
{
	if (PinName == TEXT("Input"))          return TEXT("");
	if (PinName == TEXT("Coordinates"))    return TEXT("UVs");
	if (PinName == TEXT("TextureObject"))  return TEXT("Tex");
	if (PinName == TEXT("Exponent"))       return TEXT("Exp");
	if (PinName == TEXT("AGreaterThanB"))  return TEXT("A > B");
	if (PinName == TEXT("AEqualsB"))       return TEXT("A == B");
	if (PinName == TEXT("ALessThanB"))     return TEXT("A < B");
	if (PinName == TEXT("MipLevel"))       return TEXT("Level");
	if (PinName == TEXT("MipBias"))        return TEXT("Bias");

	// Strip type hints from material function call inputs, e.g. "BaseColor (V3)" -> "BaseColor"
	// UE's GetInputNameWithType(index, false) returns names without these suffixes,
	// so we must strip them to match correctly.
	FString Result = PinName;
	static const TArray<FString> TypeSuffixes = {
		TEXT(" (V3)"), TEXT(" (V2)"), TEXT(" (V4)"),
		TEXT(" (S)"), TEXT(" (T2d)"), TEXT(" (TCube)"),
		TEXT(" (T2dArr)"), TEXT(" (TVol)"), TEXT(" (SB)"),
		TEXT(" (MA)"), TEXT(" (TExt)"), TEXT(" (B)"),
		TEXT(" (Stra)"), TEXT(" (MCL)")
	};
	for (const FString& Suffix : TypeSuffixes)
	{
		if (Result.EndsWith(Suffix))
		{
			Result.LeftChopInline(Suffix.Len());
			break;
		}
	}
	return Result;
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithMaterialActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("material"), TEXT("get_all_expressions"),
		TEXT("Get all expression nodes in a base material"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::GetAllExpressions),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("get_expression_details"),
		TEXT("Get full property reflection, inputs, and outputs for a single expression"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::GetExpressionDetails),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Required(TEXT("expression_name"), TEXT("string"), TEXT("Name of the expression node"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("get_full_connection_graph"),
		TEXT("Get the complete connection graph (all wires) of a material"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::GetFullConnectionGraph),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("disconnect_expression"),
		TEXT("Disconnect inputs or outputs on a named expression"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::DisconnectExpression),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Required(TEXT("expression_name"), TEXT("string"), TEXT("Name of the expression to disconnect"))
			.Optional(TEXT("input_name"), TEXT("string"), TEXT("Specific input to disconnect"))
			.Optional(TEXT("disconnect_outputs"), TEXT("bool"), TEXT("Also disconnect outputs"), TEXT("false"))
			.Optional(TEXT("target_expression"), TEXT("string"), TEXT("Only disconnect from this specific downstream expression (requires disconnect_outputs=true)"))
			.Optional(TEXT("output_index"), TEXT("integer"), TEXT("Only disconnect connections using this output index"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("build_material_graph"),
		TEXT("Build entire material graph from JSON spec in a single undo transaction"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::BuildMaterialGraph),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Required(TEXT("graph_spec"), TEXT("object"), TEXT("JSON specification of the material graph"))
			.Optional(TEXT("clear_existing"), TEXT("bool"), TEXT("Clear existing expressions before building (default: true)"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("begin_transaction"),
		TEXT("Begin a named undo transaction for batching edits"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::BeginTransaction),
		FParamSchemaBuilder()
			.Required(TEXT("transaction_name"), TEXT("string"), TEXT("Name for the undo transaction"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("end_transaction"),
		TEXT("End the current undo transaction"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::EndTransaction),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("material"), TEXT("export_material_graph"),
		TEXT("Export complete material graph to JSON (round-trippable with build_material_graph)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::ExportMaterialGraph),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Optional(TEXT("include_properties"), TEXT("bool"), TEXT("Include full property data"), TEXT("true"))
			.Optional(TEXT("include_positions"), TEXT("bool"), TEXT("Include node positions"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("import_material_graph"),
		TEXT("Import material graph from JSON string. Mode: overwrite or merge"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::ImportMaterialGraph),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Required(TEXT("graph_json"), TEXT("string"), TEXT("JSON string of the material graph"))
			.Optional(TEXT("mode"), TEXT("string"), TEXT("Import mode: overwrite or merge"), TEXT("overwrite"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("validate_material"),
		TEXT("Validate material graph health and optionally auto-fix issues"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::ValidateMaterial),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Optional(TEXT("fix_issues"), TEXT("bool"), TEXT("Auto-fix detected issues"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("render_preview"),
		TEXT("Render material preview to PNG file. Supports UV tiling to check repetition at scale."),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::RenderPreview),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Optional(TEXT("resolution"), TEXT("integer"), TEXT("Preview resolution in pixels"), TEXT("256"))
			.Optional(TEXT("uv_tiling"), TEXT("number"), TEXT("UV tiling multiplier (e.g. 3.0 to preview at 3x3 tiling)"), TEXT("1.0"))
			.Optional(TEXT("preview_mesh"), TEXT("string"), TEXT("Preview mesh shape: plane, sphere, cube (default: sphere)"), TEXT("sphere"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("get_thumbnail"),
		TEXT("Get material thumbnail as base64-encoded PNG"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::GetThumbnail),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Optional(TEXT("resolution"), TEXT("integer"), TEXT("Thumbnail resolution"), TEXT("256"))
			.Optional(TEXT("save_to_file"), TEXT("string"), TEXT("Optional file path to save PNG to disk"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("create_custom_hlsl_node"),
		TEXT("Create a Custom HLSL expression node with inputs, outputs, and code"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::CreateCustomHLSLNode),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Required(TEXT("code"), TEXT("string"), TEXT("HLSL code for the custom node"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("Node description"))
			.Optional(TEXT("output_type"), TEXT("string"), TEXT("Output type (float, float2, float3, float4)"))
			.Optional(TEXT("pos_x"), TEXT("integer"), TEXT("Node X position in graph"))
			.Optional(TEXT("pos_y"), TEXT("integer"), TEXT("Node Y position in graph"))
			.Optional(TEXT("inputs"), TEXT("array"), TEXT("Array of input pin definitions"))
			.Optional(TEXT("additional_outputs"), TEXT("array"), TEXT("Array of additional output pin definitions"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("get_layer_info"),
		TEXT("Get Material Layer or Material Layer Blend info"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::GetLayerInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material Layer or Layer Blend asset path"))
			.Build());

	// --- Wave 2: Asset creation & properties ---

	Registry.RegisterAction(TEXT("material"), TEXT("create_material"),
		TEXT("Create a new empty material asset at the specified path"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::CreateMaterial),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path e.g. /Game/Materials/M_MyMaterial"))
			.Optional(TEXT("blend_mode"), TEXT("string"), TEXT("Opaque, Masked, Translucent, Additive, Modulate, AlphaComposite"), TEXT("Opaque"))
			.Optional(TEXT("shading_model"), TEXT("string"), TEXT("DefaultLit, Unlit, Subsurface, SubsurfaceProfile, ClearCoat, TwoSidedFoliage"), TEXT("DefaultLit"))
			.Optional(TEXT("material_domain"), TEXT("string"), TEXT("Surface, DeferredDecal, PostProcess, LightFunction, UI"), TEXT("Surface"))
			.Optional(TEXT("two_sided"), TEXT("bool"), TEXT("Enable two-sided rendering"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("create_material_instance"),
		TEXT("Create a material instance constant from a parent material"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::CreateMaterialInstance),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Path for the new instance e.g. /Game/Materials/MI_MyInstance"))
			.Required(TEXT("parent_material"), TEXT("string"), TEXT("Path to parent material or material instance"))
			.Optional(TEXT("scalar_parameters"), TEXT("object"), TEXT("Map of scalar param name to float value"))
			.Optional(TEXT("vector_parameters"), TEXT("object"), TEXT("Map of vector param name to {R,G,B,A} object"))
			.Optional(TEXT("texture_parameters"), TEXT("object"), TEXT("Map of texture param name to texture asset path"))
			.Optional(TEXT("static_switch_parameters"), TEXT("object"), TEXT("Map of static switch param name to bool value"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("set_material_property"),
		TEXT("Set top-level material properties (blend mode, shading model, domain, two-sided, opacity mask clip, usage flags, etc.)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::SetMaterialProperty),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Optional(TEXT("blend_mode"), TEXT("string"), TEXT("Opaque, Masked, Translucent, Additive, Modulate, AlphaComposite"))
			.Optional(TEXT("shading_model"), TEXT("string"), TEXT("DefaultLit, Unlit, Subsurface, SubsurfaceProfile, ClearCoat, TwoSidedFoliage"))
			.Optional(TEXT("material_domain"), TEXT("string"), TEXT("Surface, DeferredDecal, PostProcess, LightFunction, UI"))
			.Optional(TEXT("two_sided"), TEXT("bool"), TEXT("Enable two-sided rendering"))
			.Optional(TEXT("opacity_mask_clip_value"), TEXT("number"), TEXT("Clip value for masked blend mode"))
			.Optional(TEXT("dithered_lod_transition"), TEXT("bool"), TEXT("Enable dithered LOD transition"))
			.Optional(TEXT("fully_rough"), TEXT("bool"), TEXT("Enable fully rough (disables specular)"))
			.Optional(TEXT("cast_shadow_as_masked"), TEXT("bool"), TEXT("Cast shadow as masked"))
			.Optional(TEXT("output_velocity"), TEXT("bool"), TEXT("Output per-pixel velocity"))
			.Optional(TEXT("used_with_skeletal_mesh"), TEXT("bool"), TEXT("Mark as used with skeletal meshes"))
			.Optional(TEXT("used_with_particle_sprites"), TEXT("bool"), TEXT("Mark as used with particle sprites"))
			.Optional(TEXT("used_with_niagara_sprites"), TEXT("bool"), TEXT("Mark as used with Niagara sprites"))
			.Optional(TEXT("used_with_niagara_meshes"), TEXT("bool"), TEXT("Mark as used with Niagara meshes"))
			.Optional(TEXT("used_with_niagara_ribbons"), TEXT("bool"), TEXT("Mark as used with Niagara ribbons"))
			.Optional(TEXT("used_with_morph_targets"), TEXT("bool"), TEXT("Mark as used with morph targets"))
			.Optional(TEXT("used_with_instanced_static_meshes"), TEXT("bool"), TEXT("Mark as used with instanced static meshes"))
			.Optional(TEXT("used_with_static_lighting"), TEXT("bool"), TEXT("Mark as used with static lighting"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("delete_expression"),
		TEXT("Delete a material expression node by name, cleaning up all connections"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::DeleteExpression),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Required(TEXT("expression_name"), TEXT("string"), TEXT("Name of the expression to delete"))
			.Build());

	// --- Wave 2B: Parameter management, recompile, duplicate ---

	Registry.RegisterAction(TEXT("material"), TEXT("get_material_parameters"),
		TEXT("List all parameters in a material or material instance with types, defaults, and groups"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::GetMaterialParameters),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material or material instance asset path"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("set_instance_parameter"),
		TEXT("Set a parameter override on an existing material instance"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::SetInstanceParameter),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material instance asset path"))
			.Required(TEXT("parameter_name"), TEXT("string"), TEXT("Parameter name to set"))
			.Optional(TEXT("scalar_value"), TEXT("number"), TEXT("Float value for scalar parameters"))
			.Optional(TEXT("vector_value"), TEXT("object"), TEXT("{R,G,B,A} object for vector parameters"))
			.Optional(TEXT("texture_value"), TEXT("string"), TEXT("Texture asset path for texture parameters"))
			.Optional(TEXT("switch_value"), TEXT("bool"), TEXT("Boolean for static switch parameters"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("recompile_material"),
		TEXT("Force recompile a material and return success/failure"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::RecompileMaterial),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Optional(TEXT("include_stats"), TEXT("bool"), TEXT("If true, wait for compilation and return errors/instruction counts (default: false)"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("duplicate_material"),
		TEXT("Duplicate an existing material or material instance to a new path"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::DuplicateMaterial),
		FParamSchemaBuilder()
			.Required(TEXT("source_path"), TEXT("string"), TEXT("Path of the material to duplicate"))
			.Required(TEXT("dest_path"), TEXT("string"), TEXT("Destination path for the copy"))
			.Build());

	// --- Wave 2C: Advanced utilities ---

	Registry.RegisterAction(TEXT("material"), TEXT("get_compilation_stats"),
		TEXT("Get shader compilation statistics: instruction count, texture samplers, errors"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::GetCompilationStats),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("set_expression_property"),
		TEXT("Set a property value on an existing material expression node"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::SetExpressionProperty),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Required(TEXT("expression_name"), TEXT("string"), TEXT("Name of the expression node"))
			.Required(TEXT("property_name"), TEXT("string"), TEXT("Property to set (e.g. R, ParameterName, SamplerType, Texture)"))
			.Required(TEXT("value"), TEXT("string"), TEXT("Value as string (parsed via ImportText for complex types) or number"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("connect_expressions"),
		TEXT("Connect an expression output to another expression input or a material output property"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::ConnectExpressions),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Required(TEXT("from_expression"), TEXT("string"), TEXT("Source expression name"))
			.Optional(TEXT("from_output"), TEXT("string"), TEXT("Source output pin name (empty = default). Alias: from_pin"))
			.Optional(TEXT("to_expression"), TEXT("string"), TEXT("Target expression name (for expr-to-expr)"))
			.Optional(TEXT("to_input"), TEXT("string"), TEXT("Target input pin name (empty = default). Alias: to_pin"))
			.Optional(TEXT("to_property"), TEXT("string"), TEXT("Material property name: BaseColor, Roughness, etc. (for expr-to-material)"))
			.Build());

	// --- Wave 3: Free wins (graph utilities & inspection) ---

	Registry.RegisterAction(TEXT("material"), TEXT("auto_layout"),
		TEXT("Auto-layout all expression nodes in a material or material function graph"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::AutoLayout),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material or material function asset path"))
			.Optional(TEXT("formatter"), TEXT("string"),
				TEXT("Formatter: 'auto' (default, prefers Blueprint Assist if available), "
					 "'blueprint_assist' (requires asset open in editor), or 'monolith' (UE built-in layout)"),
				TEXT("auto"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("duplicate_expression"),
		TEXT("Duplicate an expression node within the same material. Output connections are NOT duplicated (input connections are preserved)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::DuplicateExpression),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Required(TEXT("expression_name"), TEXT("string"), TEXT("Name of the expression to duplicate"))
			.Optional(TEXT("offset_x"), TEXT("integer"), TEXT("X offset from original position"), TEXT("50"))
			.Optional(TEXT("offset_y"), TEXT("integer"), TEXT("Y offset from original position"), TEXT("50"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("list_expression_classes"),
		TEXT("List all available material expression classes with pin counts. First call may take 1-2s (cached after)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::ListExpressionClasses),
		FParamSchemaBuilder()
			.Optional(TEXT("filter"), TEXT("string"), TEXT("Substring filter on class name (case-insensitive)"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Filter by MenuCategories substring"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("get_expression_connections"),
		TEXT("Get all input connections and output consumers for a single expression"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::GetExpressionConnections),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Required(TEXT("expression_name"), TEXT("string"), TEXT("Name of the expression"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("move_expression"),
		TEXT("Move one or more expression nodes to new positions"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::MoveExpression),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Optional(TEXT("expression_name"), TEXT("string"), TEXT("Single expression to move"))
			.Optional(TEXT("pos_x"), TEXT("integer"), TEXT("X position (or offset if relative=true)"))
			.Optional(TEXT("pos_y"), TEXT("integer"), TEXT("Y position (or offset if relative=true)"))
			.Optional(TEXT("relative"), TEXT("bool"), TEXT("Treat pos_x/pos_y as offsets from current position"), TEXT("false"))
			.Optional(TEXT("expressions"), TEXT("array"), TEXT("Batch: array of {name, x, y} objects"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("get_material_properties"),
		TEXT("Get top-level material properties: blend mode, shading model, domain, usage flags, etc."),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::GetMaterialProperties),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material or material instance asset path"))
			.Build());

	// --- Wave 4: Instance & property improvements ---

	Registry.RegisterAction(TEXT("material"), TEXT("get_instance_parameters"),
		TEXT("Read all parameter overrides from a material instance, with override detection vs parent"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::GetInstanceParameters),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material instance asset path"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("set_instance_parameters"),
		TEXT("Set multiple parameter overrides on a material instance in one call (batch)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::SetInstanceParameters),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material instance asset path"))
			.Required(TEXT("parameters"), TEXT("array"), TEXT("Array of {name, type (scalar/vector/texture/switch), value}"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("set_instance_parent"),
		TEXT("Reparent a material instance to a new parent material, reporting lost/kept parameters"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::SetInstanceParent),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material instance asset path"))
			.Required(TEXT("new_parent"), TEXT("string"), TEXT("Path to new parent material or material instance"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("clear_instance_parameter"),
		TEXT("Remove a single parameter override (or all overrides) from a material instance"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::ClearInstanceParameter),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material instance asset path"))
			.Optional(TEXT("parameter_name"), TEXT("string"), TEXT("Specific parameter to clear (omit for all)"))
			.Optional(TEXT("parameter_type"), TEXT("string"), TEXT("scalar, vector, texture, switch, or all (default: all)"), TEXT("all"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("save_material"),
		TEXT("Save a material or material instance asset to disk"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::SaveMaterial),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Optional(TEXT("only_if_dirty"), TEXT("bool"), TEXT("Only save if the asset has unsaved changes"), TEXT("true"))
			.Build());

	// --- Wave 5: Graph editing power ---

	Registry.RegisterAction(TEXT("material"), TEXT("update_custom_hlsl_node"),
		TEXT("Update fields on an existing Custom HLSL expression node (code, inputs, outputs, etc.)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::UpdateCustomHlslNode),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Required(TEXT("expression_name"), TEXT("string"), TEXT("Name of the UMaterialExpressionCustom node"))
			.Optional(TEXT("code"), TEXT("string"), TEXT("New HLSL code"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("New description/label"))
			.Optional(TEXT("output_type"), TEXT("string"), TEXT("Output type: Float1, Float2, Float3, Float4"))
			.Optional(TEXT("inputs"), TEXT("array"), TEXT("Array of {name, type?} input pin definitions (replaces all inputs)"))
			.Optional(TEXT("additional_outputs"), TEXT("array"), TEXT("Array of {name, type} output pin definitions (replaces all additional outputs)"))
			.Optional(TEXT("include_file_paths"), TEXT("array"), TEXT("Array of HLSL include file paths"))
			.Optional(TEXT("additional_defines"), TEXT("array"), TEXT("Array of {name, value} preprocessor defines"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("replace_expression"),
		TEXT("Replace an expression with a new one, preserving connections by pin name (index fallback with warnings)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::ReplaceExpression),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Required(TEXT("expression_name"), TEXT("string"), TEXT("Name of the expression to replace"))
			.Required(TEXT("new_class"), TEXT("string"), TEXT("Class name for the new expression (e.g. VectorParameter, Multiply)"))
			.Optional(TEXT("new_properties"), TEXT("object"), TEXT("Properties to set on the new expression via reflection"))
			.Optional(TEXT("preserve_connections"), TEXT("bool"), TEXT("Attempt to reconnect wires by pin name, then index fallback"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("get_expression_pin_info"),
		TEXT("Get input and output pin info for a material expression class (without needing a material asset)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::GetExpressionPinInfo),
		FParamSchemaBuilder()
			.Required(TEXT("class_name"), TEXT("string"), TEXT("Expression class name (e.g. MaterialExpressionMultiply or Multiply)"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("rename_expression"),
		TEXT("Set the user-visible description/label on an expression node in the graph editor"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::RenameExpression),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Required(TEXT("expression_name"), TEXT("string"), TEXT("Name of the expression node"))
			.Required(TEXT("new_desc"), TEXT("string"), TEXT("New description text (visible label in graph editor)"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("list_material_instances"),
		TEXT("Find all material instances that reference a parent material (recursively walks the instance tree)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::ListMaterialInstances),
		FParamSchemaBuilder()
			.Required(TEXT("parent_path"), TEXT("string"), TEXT("Path to the parent material or material instance"))
			.Optional(TEXT("recursive"), TEXT("bool"), TEXT("Recursively find instances of instances"), TEXT("true"))
			.Build());

	// --- Wave 6: Material Functions ---

	Registry.RegisterAction(TEXT("material"), TEXT("create_material_function"),
		TEXT("Create a new UMaterialFunction asset"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::CreateMaterialFunction),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Content path for the new function (e.g. /Game/Materials/Functions/MF_MyFunc)"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("Description shown in the material editor"))
			.Optional(TEXT("expose_to_library"), TEXT("bool"), TEXT("Expose to the material function library"), TEXT("true"))
			.Optional(TEXT("library_categories"), TEXT("array"), TEXT("Array of category strings for library organization"))
			.Optional(TEXT("type"), TEXT("string"), TEXT("Function type: MaterialFunction (default), MaterialLayer, or MaterialLayerBlend"), TEXT("MaterialFunction"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("build_function_graph"),
		TEXT("Build a material function graph from JSON spec — same schema as build_material_graph but with function inputs[] and outputs[] definitions"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::BuildFunctionGraph),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material function asset path"))
			.Required(TEXT("graph_spec"), TEXT("object"), TEXT("JSON specification: { inputs: [{name, type, sort_priority?, preview_value?}], outputs: [{name, sort_priority?}], nodes: [...], custom_hlsl_nodes: [...], connections: [...] }"))
			.Optional(TEXT("clear_existing"), TEXT("bool"), TEXT("Clear existing expressions before building"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("get_function_info"),
		TEXT("Get detailed info about a material function — inputs, outputs, description, categories, expressions"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::GetFunctionInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material function asset path"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("export_function_graph"),
		TEXT("Export complete material function graph to JSON (expressions, connections, inputs, outputs, properties)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::ExportFunctionGraph),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MaterialFunction asset path"))
			.Optional(TEXT("include_properties"), TEXT("bool"), TEXT("Include full property reflection data per expression"), TEXT("true"))
			.Optional(TEXT("include_positions"), TEXT("bool"), TEXT("Include editor node positions"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("set_function_metadata"),
		TEXT("Update material function metadata (description, categories, library exposure)"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::SetFunctionMetadata),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MaterialFunction asset path"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("Function description text"))
			.Optional(TEXT("expose_to_library"), TEXT("bool"), TEXT("Expose in material function library"))
			.Optional(TEXT("library_categories"), TEXT("array"), TEXT("Library category strings"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("update_material_function"),
		TEXT("Recompile material function and cascade changes to all referencing materials/instances"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::UpdateMaterialFunction),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MaterialFunction or MaterialFunctionInstance asset path"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("delete_function_expression"),
		TEXT("Delete expression(s) from a material function"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::DeleteFunctionExpression),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MaterialFunction asset path"))
			.Required(TEXT("expression_name"), TEXT("string"), TEXT("Expression name or comma-separated names to delete"))
			.Build());

	// --- Wave 7: Batch & Advanced ---

	Registry.RegisterAction(TEXT("material"), TEXT("batch_set_material_property"),
		TEXT("Set material properties on multiple materials in a single transaction"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::BatchSetMaterialProperty),
		FParamSchemaBuilder()
			.Required(TEXT("asset_paths"), TEXT("array"), TEXT("Array of material asset paths"))
			.Optional(TEXT("blend_mode"), TEXT("string"), TEXT("Blend mode enum value"))
			.Optional(TEXT("shading_model"), TEXT("string"), TEXT("Shading model enum value"))
			.Optional(TEXT("material_domain"), TEXT("string"), TEXT("Material domain enum value"))
			.Optional(TEXT("two_sided"), TEXT("bool"), TEXT("Enable two-sided rendering"))
			.Optional(TEXT("opacity_mask_clip_value"), TEXT("number"), TEXT("Opacity mask clip value"))
			.Optional(TEXT("dithered_lod_transition"), TEXT("bool"), TEXT("Enable dithered LOD transition"))
			.Optional(TEXT("used_with_skeletal_mesh"), TEXT("bool"), TEXT("Enable SkeletalMesh usage"))
			.Optional(TEXT("used_with_particle_sprites"), TEXT("bool"), TEXT("Enable ParticleSprites usage"))
			.Optional(TEXT("used_with_niagara_sprites"), TEXT("bool"), TEXT("Enable NiagaraSprites usage"))
			.Optional(TEXT("used_with_niagara_meshes"), TEXT("bool"), TEXT("Enable NiagaraMeshes usage"))
			.Optional(TEXT("used_with_niagara_ribbons"), TEXT("bool"), TEXT("Enable NiagaraRibbons usage"))
			.Optional(TEXT("used_with_morph_targets"), TEXT("bool"), TEXT("Enable MorphTargets usage"))
			.Optional(TEXT("used_with_instanced_static_meshes"), TEXT("bool"), TEXT("Enable InstancedStaticMeshes usage"))
			.Optional(TEXT("used_with_static_lighting"), TEXT("bool"), TEXT("Enable StaticLighting usage"))
			.Optional(TEXT("fully_rough"), TEXT("bool"), TEXT("Enable fully rough"))
			.Optional(TEXT("cast_shadow_as_masked"), TEXT("bool"), TEXT("Cast shadow as masked"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("batch_recompile"),
		TEXT("Recompile multiple materials and return per-material instruction counts"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::BatchRecompile),
		FParamSchemaBuilder()
			.Required(TEXT("asset_paths"), TEXT("array"), TEXT("Array of material asset paths to recompile"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("import_texture"),
		TEXT("Import a texture from disk into the project content"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::ImportTexture),
		FParamSchemaBuilder()
			.Required(TEXT("source_file"), TEXT("string"), TEXT("Absolute path to the image file on disk (e.g. D:/Textures/diffuse.png)"))
			.Required(TEXT("dest_path"), TEXT("string"), TEXT("Content path for the imported texture (e.g. /Game/Textures/MyTexture)"))
			.Optional(TEXT("dest_name"), TEXT("string"), TEXT("Override asset name (default: derived from dest_path)"))
			.Optional(TEXT("compression"), TEXT("string"), TEXT("Compression setting: Default, Normalmap, NormalmapBC5, NormalmapLA, Grayscale, Alpha, Masks, HDR, etc. (default: Default)"), TEXT("Default"))
			.Optional(TEXT("srgb"), TEXT("bool"), TEXT("sRGB color space"), TEXT("true"))
			.Optional(TEXT("lod_group"), TEXT("string"), TEXT("LOD group: World, WorldNormalMap, WorldSpecular, Character, CharacterNormalMap, Weapon, UI, etc."))
			.Optional(TEXT("max_size"), TEXT("integer"), TEXT("Max texture dimension (e.g. 2048, 4096). 0 means no limit."))
			.Optional(TEXT("replace_existing"), TEXT("bool"), TEXT("Replace existing asset at dest_path"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("create_pbr_material_from_disk"),
		TEXT("Import PBR textures from disk, create material, build graph, and compile in one action. Handles decals via opacity_from_alpha"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::CreatePbrMaterialFromDisk),
		FParamSchemaBuilder()
			.Required(TEXT("material_path"), TEXT("string"), TEXT("UE asset path for the new material (e.g. /Game/Materials/M_MyMaterial)"))
			.Required(TEXT("texture_folder"), TEXT("string"), TEXT("UE content folder for imported textures (e.g. /Game/Textures/MyMaterial)"))
			.Required(TEXT("maps"), TEXT("object"), TEXT("Map of PBR type to disk path. Keys: basecolor, albedo, normal, roughness, metallic, metalness, ao, height, emissive, opacity"))
			.Optional(TEXT("blend_mode"), TEXT("string"), TEXT("Material blend mode"), TEXT("Opaque"))
			.Optional(TEXT("shading_model"), TEXT("string"), TEXT("Shading model"), TEXT("DefaultLit"))
			.Optional(TEXT("material_domain"), TEXT("string"), TEXT("Material domain"), TEXT("Surface"))
			.Optional(TEXT("two_sided"), TEXT("bool"), TEXT("Two-sided rendering"), TEXT("false"))
			.Optional(TEXT("max_texture_size"), TEXT("integer"), TEXT("Max texture resolution"), TEXT("2048"))
			.Optional(TEXT("opacity_from_alpha"), TEXT("bool"), TEXT("Wire basecolor alpha to Opacity (decals)"), TEXT("false"))
			.Optional(TEXT("replace_existing"), TEXT("bool"), TEXT("Replace existing material and textures"), TEXT("false"))
			.Build());

	// --- Wave 9: Function instances ---

	Registry.RegisterAction(TEXT("material"), TEXT("create_function_instance"),
		TEXT("Create a material function instance with parent and optional parameter overrides"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::CreateFunctionInstance),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path for new MFI"))
			.Required(TEXT("parent"), TEXT("string"), TEXT("Parent material function or instance asset path"))
			.Optional(TEXT("scalar_overrides"), TEXT("object"), TEXT("Scalar param overrides {name: value}"))
			.Optional(TEXT("vector_overrides"), TEXT("object"), TEXT("Vector param overrides {name: {r,g,b,a}}"))
			.Optional(TEXT("texture_overrides"), TEXT("object"), TEXT("Texture param overrides {name: texture_path}"))
			.Optional(TEXT("static_switch_overrides"), TEXT("object"), TEXT("Static switch overrides {name: bool}"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("set_function_instance_parameter"),
		TEXT("Set or update a parameter override on a material function instance"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::SetFunctionInstanceParameter),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MFI asset path"))
			.Required(TEXT("parameter_name"), TEXT("string"), TEXT("Parameter name to override"))
			.Optional(TEXT("scalar_value"), TEXT("number"), TEXT("Scalar parameter value"))
			.Optional(TEXT("vector_value"), TEXT("object"), TEXT("Vector value {r,g,b,a}"))
			.Optional(TEXT("texture_value"), TEXT("string"), TEXT("Texture asset path"))
			.Optional(TEXT("switch_value"), TEXT("bool"), TEXT("Static switch value"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("get_function_instance_info"),
		TEXT("Get material function instance info including parent chain and parameter overrides"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::GetFunctionInstanceInfo),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MFI asset path"))
			.Build());

	// --- Wave 10: Function utilities ---

	Registry.RegisterAction(TEXT("material"), TEXT("layout_function_expressions"),
		TEXT("Auto-arrange expression layout in a material function"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::LayoutFunctionExpressions),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MaterialFunction asset path"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("rename_function_parameter_group"),
		TEXT("Rename a parameter group across all parameters in a material function"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::RenameFunctionParameterGroup),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("MaterialFunction asset path"))
			.Required(TEXT("old_group"), TEXT("string"), TEXT("Current group name"))
			.Required(TEXT("new_group"), TEXT("string"), TEXT("New group name"))
			.Build());

	// --- Wave 11: Material expansion ---

	Registry.RegisterAction(TEXT("material"), TEXT("clear_graph"),
		TEXT("Remove all expressions from a material graph, optionally preserving parameter nodes"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::ClearGraph),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Optional(TEXT("preserve_parameters"), TEXT("bool"), TEXT("Keep parameter expressions (ScalarParameter, VectorParameter, etc.)"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("delete_expressions"),
		TEXT("Delete multiple expression nodes by name in a single transaction"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::DeleteExpressions),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Required(TEXT("expression_names"), TEXT("array"), TEXT("Array of expression name strings to delete"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("get_texture_properties"),
		TEXT("Get comprehensive texture properties including recommended sampler type for material use"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::GetTextureProperties),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Texture asset path"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("preview_texture"),
		TEXT("Render texture preview to PNG and return texture metadata in one call"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::PreviewTexture),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Texture asset path"))
			.Optional(TEXT("resolution"), TEXT("integer"), TEXT("Preview resolution in pixels"), TEXT("256"))
			.Optional(TEXT("output_path"), TEXT("string"), TEXT("Custom output file path (default: Saved/Monolith/previews/)"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("preview_textures"),
		TEXT("Render a contact sheet of multiple textures with metadata"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::PreviewTextures),
		FParamSchemaBuilder()
			.Required(TEXT("asset_paths"), TEXT("array"), TEXT("Array of texture asset path strings"))
			.Optional(TEXT("per_texture_size"), TEXT("integer"), TEXT("Size of each texture tile in pixels"), TEXT("128"))
			.Optional(TEXT("output_path"), TEXT("string"), TEXT("Custom output file path (default: Saved/Monolith/previews/)"))
			.Build());

	Registry.RegisterAction(TEXT("material"), TEXT("check_tiling_quality"),
		TEXT("Analyze material for tiling issues: missing anti-tiling, no macro variation, direct UV usage"),
		FMonolithActionHandler::CreateStatic(&FMonolithMaterialActions::CheckTilingQuality),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Material asset path"))
			.Build());
}

// ============================================================================
// Helpers
// ============================================================================

UMaterial* FMonolithMaterialActions::LoadBaseMaterial(const FString& AssetPath)
{
	// Try UEditorAssetLibrary first (loads from disk)
	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	if (LoadedAsset)
	{
		return Cast<UMaterial>(LoadedAsset);
	}

	// Fallback: check in-memory objects (e.g. freshly created, not yet saved)
	FString FullObjectPath = AssetPath;
	if (!FullObjectPath.Contains(TEXT(".")))
	{
		// Convert "/Game/Path/Name" to "/Game/Path/Name.Name"
		int32 LastSlash;
		if (FullObjectPath.FindLastChar('/', LastSlash))
		{
			FString ObjName = FullObjectPath.Mid(LastSlash + 1);
			FullObjectPath = FString::Printf(TEXT("%s.%s"), *AssetPath, *ObjName);
		}
	}
	UObject* Found = FindFirstObject<UMaterial>(*FullObjectPath, EFindFirstObjectOptions::NativeFirst);
	return Cast<UMaterial>(Found);
}

TSharedPtr<FJsonObject> FMonolithMaterialActions::SerializeExpression(const UMaterialExpression* Expression)
{
	auto ExprJson = MakeShared<FJsonObject>();

	ExprJson->SetStringField(TEXT("name"), Expression->GetName());
	ExprJson->SetStringField(TEXT("class"), Expression->GetClass()->GetName());
	ExprJson->SetNumberField(TEXT("pos_x"), Expression->MaterialExpressionEditorX);
	ExprJson->SetNumberField(TEXT("pos_y"), Expression->MaterialExpressionEditorY);

	if (const auto* TexSampleParam = Cast<UMaterialExpressionTextureSampleParameter>(Expression))
	{
		ExprJson->SetStringField(TEXT("parameter_name"), TexSampleParam->ParameterName.ToString());
		if (TexSampleParam->Texture)
		{
			ExprJson->SetStringField(TEXT("texture"), TexSampleParam->Texture->GetPathName());
		}
	}
	else if (const auto* Param = Cast<UMaterialExpressionParameter>(Expression))
	{
		ExprJson->SetStringField(TEXT("parameter_name"), Param->ParameterName.ToString());
	}
	else if (const auto* TexBase = Cast<UMaterialExpressionTextureBase>(Expression))
	{
		if (TexBase->Texture)
		{
			ExprJson->SetStringField(TEXT("texture"), TexBase->Texture->GetPathName());
		}
	}

	if (const auto* Custom = Cast<UMaterialExpressionCustom>(Expression))
	{
		FString CodePreview = Custom->Code.Left(100);
		ExprJson->SetStringField(TEXT("code"), CodePreview);
	}

	if (const auto* Comment = Cast<UMaterialExpressionComment>(Expression))
	{
		ExprJson->SetStringField(TEXT("text"), Comment->Text);
	}

	if (const auto* FuncCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
	{
		if (FuncCall->MaterialFunction)
		{
			ExprJson->SetStringField(TEXT("function"), FuncCall->MaterialFunction->GetPathName());
		}
	}

	return ExprJson;
}

/** Map string property name to EMaterialProperty. */
static EMaterialProperty ParseMaterialProperty(const FString& PropName)
{
	static const TMap<FString, EMaterialProperty> Map = {
		{ TEXT("BaseColor"),            MP_BaseColor },
		{ TEXT("Base Color"),           MP_BaseColor },
		{ TEXT("Metallic"),             MP_Metallic },
		{ TEXT("Specular"),             MP_Specular },
		{ TEXT("Roughness"),            MP_Roughness },
		{ TEXT("Anisotropy"),           MP_Anisotropy },
		{ TEXT("EmissiveColor"),        MP_EmissiveColor },
		{ TEXT("Emissive Color"),       MP_EmissiveColor },
		{ TEXT("Opacity"),              MP_Opacity },
		{ TEXT("OpacityMask"),          MP_OpacityMask },
		{ TEXT("Opacity Mask"),         MP_OpacityMask },
		{ TEXT("Normal"),               MP_Normal },
		{ TEXT("WorldPositionOffset"),  MP_WorldPositionOffset },
		{ TEXT("World Position Offset"),MP_WorldPositionOffset },
		{ TEXT("SubsurfaceColor"),      MP_SubsurfaceColor },
		{ TEXT("Subsurface Color"),     MP_SubsurfaceColor },
		{ TEXT("AmbientOcclusion"),     MP_AmbientOcclusion },
		{ TEXT("Ambient Occlusion"),    MP_AmbientOcclusion },
		{ TEXT("Refraction"),           MP_Refraction },
		{ TEXT("PixelDepthOffset"),     MP_PixelDepthOffset },
		{ TEXT("Pixel Depth Offset"),   MP_PixelDepthOffset },
		{ TEXT("ShadingModel"),         MP_ShadingModel },
		{ TEXT("Shading Model"),        MP_ShadingModel },
	};

	const EMaterialProperty* Found = Map.Find(PropName);
	return Found ? *Found : MP_MAX;
}

/** Map string to ECustomMaterialOutputType. */
static ECustomMaterialOutputType ParseCustomOutputType(const FString& TypeName)
{
	if (TypeName == TEXT("CMOT_Float1") || TypeName == TEXT("Float1")) return CMOT_Float1;
	if (TypeName == TEXT("CMOT_Float2") || TypeName == TEXT("Float2")) return CMOT_Float2;
	if (TypeName == TEXT("CMOT_Float3") || TypeName == TEXT("Float3")) return CMOT_Float3;
	if (TypeName == TEXT("CMOT_Float4") || TypeName == TEXT("Float4")) return CMOT_Float4;
	return CMOT_Float1;
}

/** Map ECustomMaterialOutputType to string. */
static FString CustomOutputTypeToString(ECustomMaterialOutputType Type)
{
	switch (Type)
	{
	case CMOT_Float1: return TEXT("Float1");
	case CMOT_Float2: return TEXT("Float2");
	case CMOT_Float3: return TEXT("Float3");
	case CMOT_Float4: return TEXT("Float4");
	default: return TEXT("Float1");
	}
}

/** Map EMaterialProperty to string name. */
static FString MaterialPropertyToString(EMaterialProperty Prop)
{
	switch (Prop)
	{
	case MP_BaseColor:            return TEXT("BaseColor");
	case MP_Metallic:             return TEXT("Metallic");
	case MP_Specular:             return TEXT("Specular");
	case MP_Roughness:            return TEXT("Roughness");
	case MP_Anisotropy:           return TEXT("Anisotropy");
	case MP_EmissiveColor:        return TEXT("EmissiveColor");
	case MP_Opacity:              return TEXT("Opacity");
	case MP_OpacityMask:          return TEXT("OpacityMask");
	case MP_Normal:               return TEXT("Normal");
	case MP_WorldPositionOffset:  return TEXT("WorldPositionOffset");
	case MP_SubsurfaceColor:      return TEXT("SubsurfaceColor");
	case MP_AmbientOcclusion:     return TEXT("AmbientOcclusion");
	case MP_Refraction:           return TEXT("Refraction");
	case MP_PixelDepthOffset:     return TEXT("PixelDepthOffset");
	case MP_ShadingModel:         return TEXT("ShadingModel");
	default:                      return TEXT("");
	}
}

/** All material properties for iteration */
static const EMaterialProperty AllMaterialProperties[] =
{
	MP_BaseColor, MP_Metallic, MP_Specular, MP_Roughness, MP_Anisotropy,
	MP_EmissiveColor, MP_Opacity, MP_OpacityMask, MP_Normal,
	MP_WorldPositionOffset, MP_SubsurfaceColor, MP_AmbientOcclusion,
	MP_Refraction, MP_PixelDepthOffset, MP_ShadingModel,
	MP_Tangent, MP_Displacement, MP_CustomData0, MP_CustomData1,
	MP_SurfaceThickness, MP_FrontMaterial, MP_MaterialAttributes,
};

/** Material output entries for connection graph */
struct FMaterialOutputEntry
{
	EMaterialProperty Property;
	const TCHAR* Name;
};

static const FMaterialOutputEntry MaterialOutputEntries[] =
{
	{ MP_BaseColor,              TEXT("BaseColor") },
	{ MP_Metallic,               TEXT("Metallic") },
	{ MP_Specular,               TEXT("Specular") },
	{ MP_Roughness,              TEXT("Roughness") },
	{ MP_Anisotropy,             TEXT("Anisotropy") },
	{ MP_EmissiveColor,          TEXT("EmissiveColor") },
	{ MP_Opacity,                TEXT("Opacity") },
	{ MP_OpacityMask,            TEXT("OpacityMask") },
	{ MP_Normal,                 TEXT("Normal") },
	{ MP_WorldPositionOffset,    TEXT("WorldPositionOffset") },
	{ MP_SubsurfaceColor,        TEXT("SubsurfaceColor") },
	{ MP_AmbientOcclusion,       TEXT("AmbientOcclusion") },
	{ MP_Refraction,             TEXT("Refraction") },
	{ MP_PixelDepthOffset,       TEXT("PixelDepthOffset") },
	{ MP_ShadingModel,           TEXT("ShadingModel") },
	{ MP_Tangent,                TEXT("Tangent") },
	{ MP_Displacement,           TEXT("Displacement") },
	{ MP_CustomData0,            TEXT("ClearCoat") },
	{ MP_CustomData1,            TEXT("ClearCoatRoughness") },
	{ MP_SurfaceThickness,       TEXT("SurfaceThickness") },
	{ MP_FrontMaterial,          TEXT("FrontMaterial") },
	{ MP_MaterialAttributes,     TEXT("MaterialAttributes") },
};

// ============================================================================
// Action: get_all_expressions
// Params: { "asset_path": "/Game/..." }
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::GetAllExpressions(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	// Try UMaterial first, then fall back to UMaterialFunction
	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	UMaterial* Mat = Cast<UMaterial>(LoadedAsset);
	UMaterialFunction* MatFunc = Mat ? nullptr : Cast<UMaterialFunction>(LoadedAsset);

	if (!Mat && !MatFunc)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to load material or material function at '%s'"), *AssetPath));
	}

	TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions =
		Mat ? Mat->GetExpressions() : MatFunc->GetExpressions();

	TArray<TSharedPtr<FJsonValue>> ExpressionsArray;
	for (const TObjectPtr<UMaterialExpression>& Expr : Expressions)
	{
		if (Expr)
		{
			ExpressionsArray.Add(MakeShared<FJsonValueObject>(SerializeExpression(Expr)));
		}
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetStringField(TEXT("asset_type"), Mat ? TEXT("Material") : TEXT("MaterialFunction"));
	ResultJson->SetNumberField(TEXT("expression_count"), ExpressionsArray.Num());
	ResultJson->SetArrayField(TEXT("expressions"), ExpressionsArray);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: get_expression_details
// Params: { "asset_path": "...", "expression_name": "..." }
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::GetExpressionDetails(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString ExpressionName = Params->GetStringField(TEXT("expression_name"));

	// Try UMaterial first, then fall back to UMaterialFunction
	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	UMaterial* Mat = Cast<UMaterial>(LoadedAsset);
	UMaterialFunction* MatFunc = Mat ? nullptr : Cast<UMaterialFunction>(LoadedAsset);

	if (!Mat && !MatFunc)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to load material or material function at '%s'"), *AssetPath));
	}

	TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions =
		Mat ? Mat->GetExpressions() : MatFunc->GetExpressions();

	UMaterialExpression* FoundExpr = nullptr;
	for (const TObjectPtr<UMaterialExpression>& Expr : Expressions)
	{
		if (Expr && Expr->GetName() == ExpressionName)
		{
			FoundExpr = Expr;
			break;
		}
	}

	if (!FoundExpr)
	{
		TArray<FString> AvailableNames;
		for (const TObjectPtr<UMaterialExpression>& Expr : Expressions)
		{
			if (Expr) AvailableNames.Add(Expr->GetName());
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Expression '%s' not found. Available: %s"),
			*ExpressionName, *FString::Join(AvailableNames, TEXT(", "))));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetStringField(TEXT("expression_name"), ExpressionName);
	ResultJson->SetStringField(TEXT("class"), FoundExpr->GetClass()->GetName());

	// Serialize ALL UProperties via reflection
	auto PropsJson = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> PropIt(FoundExpr->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!Prop || Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient))
		{
			continue;
		}
		FString ValueStr;
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(FoundExpr);
		Prop->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, nullptr, PPF_None);
		if (!ValueStr.IsEmpty())
		{
			PropsJson->SetStringField(Prop->GetName(), ValueStr);
		}
	}
	ResultJson->SetObjectField(TEXT("properties"), PropsJson);

	// List input pins
	TArray<TSharedPtr<FJsonValue>> InputsArray;
	for (int32 i = 0; ; ++i)
	{
		FExpressionInput* Input = FoundExpr->GetInput(i);
		if (!Input)
		{
			break;
		}
		auto InputJson = MakeShared<FJsonObject>();
		InputJson->SetStringField(TEXT("name"), FoundExpr->GetInputName(i).ToString());
		InputJson->SetBoolField(TEXT("connected"), Input->Expression != nullptr);
		if (Input->Expression)
		{
			InputJson->SetStringField(TEXT("connected_to"), Input->Expression->GetName());
			InputJson->SetNumberField(TEXT("output_index"), Input->OutputIndex);
		}
		InputsArray.Add(MakeShared<FJsonValueObject>(InputJson));
	}
	ResultJson->SetArrayField(TEXT("inputs"), InputsArray);

	// List output pins
	TArray<TSharedPtr<FJsonValue>> OutputsArray;
	const TArray<FExpressionOutput>& Outputs = FoundExpr->Outputs;
	for (int32 i = 0; i < Outputs.Num(); ++i)
	{
		auto OutputJson = MakeShared<FJsonObject>();
		OutputJson->SetStringField(TEXT("name"), Outputs[i].OutputName.ToString());
		OutputJson->SetNumberField(TEXT("index"), i);
		OutputsArray.Add(MakeShared<FJsonValueObject>(OutputJson));
	}
	ResultJson->SetArrayField(TEXT("outputs"), OutputsArray);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: get_full_connection_graph
// Params: { "asset_path": "..." }
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::GetFullConnectionGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UMaterial* Mat = LoadBaseMaterial(AssetPath);
	if (!Mat)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load base material at '%s'"), *AssetPath));
	}

	TArray<TSharedPtr<FJsonValue>> ConnectionsArray;
	TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = Mat->GetExpressions();

	for (const TObjectPtr<UMaterialExpression>& Expr : Expressions)
	{
		if (!Expr)
		{
			continue;
		}

		for (int32 i = 0; ; ++i)
		{
			FExpressionInput* Input = Expr->GetInput(i);
			if (!Input)
			{
				break;
			}
			if (!Input->Expression)
			{
				continue;
			}

			auto ConnJson = MakeShared<FJsonObject>();
			ConnJson->SetStringField(TEXT("from"), Input->Expression->GetName());
			ConnJson->SetNumberField(TEXT("from_output_index"), Input->OutputIndex);

			FString FromOutputName;
			const TArray<FExpressionOutput>& SourceOutputs = Input->Expression->Outputs;
			if (SourceOutputs.IsValidIndex(Input->OutputIndex))
			{
				FromOutputName = SourceOutputs[Input->OutputIndex].OutputName.ToString();
			}
			ConnJson->SetStringField(TEXT("from_output"), FromOutputName);
			ConnJson->SetStringField(TEXT("to"), Expr->GetName());
			ConnJson->SetStringField(TEXT("to_input"), Expr->GetInputName(i).ToString());

			ConnectionsArray.Add(MakeShared<FJsonValueObject>(ConnJson));
		}
	}

	// Material output connections
	TArray<TSharedPtr<FJsonValue>> MaterialOutputsArray;
	for (const FMaterialOutputEntry& Entry : MaterialOutputEntries)
	{
		FExpressionInput* Input = Mat->GetExpressionInputForProperty(Entry.Property);
		if (Input && Input->Expression)
		{
			auto OutJson = MakeShared<FJsonObject>();
			OutJson->SetStringField(TEXT("property"), Entry.Name);
			OutJson->SetStringField(TEXT("expression"), Input->Expression->GetName());
			OutJson->SetNumberField(TEXT("output_index"), Input->OutputIndex);
			MaterialOutputsArray.Add(MakeShared<FJsonValueObject>(OutJson));
		}
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetArrayField(TEXT("connections"), ConnectionsArray);
	ResultJson->SetArrayField(TEXT("material_outputs"), MaterialOutputsArray);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: disconnect_expression
// Params: { "asset_path": "...", "expression_name": "...", "input_name": "", "disconnect_outputs": false, "target_expression": "", "output_index": -1 }
// When disconnect_outputs=true: disconnects other expressions/material outputs that reference expression_name
//   target_expression: filter to only disconnect from this downstream expression or material property name (e.g. "BaseColor")
//   output_index: filter to only disconnect connections using this output index
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::DisconnectExpression(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString ExpressionName = Params->GetStringField(TEXT("expression_name"));
	FString InputName = Params->HasField(TEXT("input_name")) ? Params->GetStringField(TEXT("input_name")) : TEXT("");
	bool bDisconnectOutputs = Params->HasField(TEXT("disconnect_outputs")) ? Params->GetBoolField(TEXT("disconnect_outputs")) : false;
	// Optional filter: only disconnect from a specific downstream expression (when disconnect_outputs=true)
	FString TargetDownstream = Params->HasField(TEXT("target_expression")) ? Params->GetStringField(TEXT("target_expression")) : TEXT("");
	// Optional filter: only disconnect a specific output index
	int32 TargetOutputIndex = Params->HasField(TEXT("output_index")) ? static_cast<int32>(Params->GetNumberField(TEXT("output_index"))) : -1;

	UMaterial* Mat = LoadBaseMaterial(AssetPath);
	if (!Mat)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load base material at '%s'"), *AssetPath));
	}

	UMaterialExpression* TargetExpr = nullptr;
	TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = Mat->GetExpressions();
	for (const TObjectPtr<UMaterialExpression>& Expr : Expressions)
	{
		if (Expr && Expr->GetName() == ExpressionName)
		{
			TargetExpr = Expr;
			break;
		}
	}

	if (!TargetExpr)
	{
		TArray<FString> AvailableNames;
		for (const TObjectPtr<UMaterialExpression>& Expr : Expressions)
		{
			if (Expr) AvailableNames.Add(Expr->GetName());
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Expression '%s' not found. Available: %s"),
			*ExpressionName, *FString::Join(AvailableNames, TEXT(", "))));
	}

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "DisconnectExpr", "Disconnect Expression"));
	Mat->Modify();

	TArray<TSharedPtr<FJsonValue>> DisconnectedArray;
	int32 DisconnectCount = 0;

	if (!bDisconnectOutputs)
	{
		FString NormalizedInputName = InputName.IsEmpty() ? TEXT("") : NormalizeInputPinName(InputName);

		for (int32 i = 0; ; ++i)
		{
			FExpressionInput* Input = TargetExpr->GetInput(i);
			if (!Input)
			{
				break;
			}

			FString PinName = TargetExpr->GetInputName(i).ToString();
			FString NormalizedPinName = NormalizeInputPinName(PinName);
			if (InputName.IsEmpty() || PinName == InputName || NormalizedPinName == NormalizedInputName)
			{
				if (Input->Expression != nullptr)
				{
					auto DisconnJson = MakeShared<FJsonObject>();
					DisconnJson->SetStringField(TEXT("pin"), PinName);
					DisconnJson->SetStringField(TEXT("was_connected_to"), Input->Expression->GetName());
					DisconnectedArray.Add(MakeShared<FJsonValueObject>(DisconnJson));

					Input->Expression = nullptr;
					Input->OutputIndex = 0;
					DisconnectCount++;
				}
			}
		}
	}
	else
	{
		for (const TObjectPtr<UMaterialExpression>& Expr : Expressions)
		{
			if (!Expr)
			{
				continue;
			}
			for (int32 i = 0; ; ++i)
			{
				FExpressionInput* Input = Expr->GetInput(i);
				if (!Input)
				{
					break;
				}
				if (Input->Expression == TargetExpr)
				{
					// Filter by target downstream expression name if specified
					if (!TargetDownstream.IsEmpty() && Expr->GetName() != TargetDownstream) continue;
					// Filter by output index if specified
					if (TargetOutputIndex >= 0 && Input->OutputIndex != TargetOutputIndex) continue;

					auto DisconnJson = MakeShared<FJsonObject>();
					DisconnJson->SetStringField(TEXT("expression"), Expr->GetName());
					DisconnJson->SetStringField(TEXT("pin"), Expr->GetInputName(i).ToString());
					DisconnJson->SetNumberField(TEXT("output_index"), Input->OutputIndex);
					DisconnectedArray.Add(MakeShared<FJsonValueObject>(DisconnJson));

					Input->Expression = nullptr;
					Input->OutputIndex = 0;
					DisconnectCount++;
				}
			}
		}

		// Also check material output properties (expression -> material pin connections)
		for (const FMaterialOutputEntry& Entry : MaterialOutputEntries)
		{
			FExpressionInput* MatInput = Mat->GetExpressionInputForProperty(Entry.Property);
			if (!MatInput || MatInput->Expression != TargetExpr) continue;

			// Filter by target downstream: match against material property name (e.g. "BaseColor", "EmissiveColor")
			if (!TargetDownstream.IsEmpty() && TargetDownstream != Entry.Name) continue;
			// Filter by output index if specified
			if (TargetOutputIndex >= 0 && MatInput->OutputIndex != TargetOutputIndex) continue;

			auto DisconnJson = MakeShared<FJsonObject>();
			DisconnJson->SetStringField(TEXT("material_property"), Entry.Name);
			DisconnJson->SetNumberField(TEXT("output_index"), MatInput->OutputIndex);
			DisconnectedArray.Add(MakeShared<FJsonValueObject>(DisconnJson));

			MatInput->Expression = nullptr;
			MatInput->OutputIndex = 0;
			DisconnectCount++;
		}
	}

	Mat->PreEditChange(nullptr);
	Mat->PostEditChange();
	GEditor->EndTransaction();

	if (DisconnectedArray.Num() == 0 && !InputName.IsEmpty())
	{
		TArray<FString> AvailablePins;
		for (int32 i = 0; ; ++i)
		{
			FExpressionInput* Input = TargetExpr->GetInput(i);
			if (!Input) break;
			FString PinName = TargetExpr->GetInputName(i).ToString();
			AvailablePins.Add(FString::Printf(TEXT("%s%s"), *PinName,
				Input->Expression ? TEXT(" [connected]") : TEXT(" [disconnected]")));
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No input '%s' found on '%s'. Available pins: %s"),
			*InputName, *ExpressionName, *FString::Join(AvailablePins, TEXT(", "))));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetArrayField(TEXT("disconnected"), DisconnectedArray);
	ResultJson->SetNumberField(TEXT("count"), DisconnectCount);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: begin_transaction
// Params: { "transaction_name": "..." }
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::BeginTransaction(const TSharedPtr<FJsonObject>& Params)
{
	FString TransactionName = Params->GetStringField(TEXT("transaction_name"));
	GEditor->BeginTransaction(FText::FromString(TransactionName));

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("transaction"), TransactionName);
	ResultJson->SetStringField(TEXT("status"), TEXT("begun"));
	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: end_transaction
// Params: {}
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::EndTransaction(const TSharedPtr<FJsonObject>& Params)
{
	GEditor->EndTransaction();

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("status"), TEXT("ended"));
	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: build_material_graph
// Params: { "asset_path": "...", "graph_spec": { nodes, custom_hlsl_nodes, connections, outputs }, "clear_existing": true }
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::BuildMaterialGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	bool bClearExisting = Params->HasField(TEXT("clear_existing")) ? Params->GetBoolField(TEXT("clear_existing")) : true;

	UMaterial* Mat = LoadBaseMaterial(AssetPath);
	if (!Mat)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load base material at '%s'"), *AssetPath));
	}

	// graph_spec can be passed as a nested object or as a JSON string
	TSharedPtr<FJsonObject> Spec;
	if (Params->HasTypedField<EJson::Object>(TEXT("graph_spec")))
	{
		Spec = Params->GetObjectField(TEXT("graph_spec"));
	}
	else if (Params->HasTypedField<EJson::String>(TEXT("graph_spec")))
	{
		FString GraphSpecJson = Params->GetStringField(TEXT("graph_spec"));
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(GraphSpecJson);
		if (!FJsonSerializer::Deserialize(Reader, Spec) || !Spec.IsValid())
		{
			return FMonolithActionResult::Error(TEXT("Failed to parse graph_spec JSON string"));
		}
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("Missing 'graph_spec' parameter"));
	}

	TArray<TSharedPtr<FJsonValue>> ErrorsArray;
	int32 NodesCreated = 0;
	int32 ConnectionsMade = 0;

	GEditor->BeginTransaction(FText::FromString(TEXT("BuildMaterialGraph")));
	Mat->Modify();

	// Bug fix: save material-level properties before clear — DeleteAllMaterialExpressions
	// triggers PostEditChange which can reset BlendMode and other scalar properties
	const EBlendMode SavedBlendMode = Mat->BlendMode;
	const EMaterialShadingModel SavedShadingModel = Mat->GetShadingModels().GetFirstShadingModel();
	const bool bSavedTwoSided = Mat->TwoSided;
	const float SavedOpacityMaskClipValue = Mat->OpacityMaskClipValue;

	if (bClearExisting)
	{
		// Bug fix: DeleteAllMaterialExpressions uses a range-for over TConstArrayView
		// while Remove() mutates the backing TArray — iterator invalidation skips ~half.
		// Copy to a local array first.
		TArray<UMaterialExpression*> ToDelete;
		for (const TObjectPtr<UMaterialExpression>& Expr : Mat->GetExpressions())
		{
			if (Expr)
			{
				ToDelete.Add(Expr);
			}
		}
		for (UMaterialExpression* Expr : ToDelete)
		{
			UMaterialEditingLibrary::DeleteMaterialExpression(Mat, Expr);
		}
	}

	// Restore material-level properties that may have been reset by PostEditChange
	Mat->BlendMode = SavedBlendMode;
	Mat->SetShadingModel(SavedShadingModel);
	Mat->TwoSided = bSavedTwoSided;
	Mat->OpacityMaskClipValue = SavedOpacityMaskClipValue;

	TMap<FString, UMaterialExpression*> IdToExpr;

	// Bug fix: seed remap table with pre-existing expressions so connections
	// can reference nodes that survived a partial clear or were already present
	for (const TObjectPtr<UMaterialExpression>& Expr : Mat->GetExpressions())
	{
		if (Expr)
		{
			IdToExpr.Add(Expr->GetName(), Expr.Get());
		}
	}

	// Phases 1-3 — Standard nodes, Custom HLSL, and connections (shared logic)
	FCreateExpressionFunc CreateFunc = [Mat](UClass* ExprClass, int32 PosX, int32 PosY) -> UMaterialExpression*
	{
		return UMaterialEditingLibrary::CreateMaterialExpression(Mat, ExprClass, PosX, PosY);
	};
	BuildGraphFromSpec(Spec, CreateFunc, IdToExpr, NodesCreated, ConnectionsMade, ErrorsArray);

	if (NodesCreated == 0 && ConnectionsMade == 0 && ErrorsArray.Num() == 0)
	{
		auto WarnJson = MakeShared<FJsonObject>();
		WarnJson->SetStringField(TEXT("warning"),
			TEXT("No nodes created, no connections made — verify graph_spec contains a non-empty 'nodes' array with 'class' fields"));
		ErrorsArray.Add(MakeShared<FJsonValueObject>(WarnJson));
	}

	// Phase 4 — Wire material output properties
	const TArray<TSharedPtr<FJsonValue>>* OutputsArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("outputs"), OutputsArray))
	{
		for (const TSharedPtr<FJsonValue>& OutVal : *OutputsArray)
		{
			const TSharedPtr<FJsonObject>* OutObjPtr = nullptr;
			if (!OutVal || !OutVal->TryGetObject(OutObjPtr) || !OutObjPtr)
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& OutObj = *OutObjPtr;

			// Accept both "from" (export format) and "from_expression" (connect_expressions style)
			FString FromId = OutObj->HasField(TEXT("from")) ? OutObj->GetStringField(TEXT("from"))
			               : OutObj->HasField(TEXT("from_expression")) ? OutObj->GetStringField(TEXT("from_expression")) : TEXT("");
			FString FromPin = OutObj->HasField(TEXT("from_pin")) ? OutObj->GetStringField(TEXT("from_pin")) : TEXT("");
			FString ToProp = OutObj->GetStringField(TEXT("to_property"));

			UMaterialExpression** FromPtr = IdToExpr.Find(FromId);
			if (!FromPtr || !*FromPtr)
			{
				auto ErrJson = MakeShared<FJsonObject>();
				ErrJson->SetStringField(TEXT("output"), ToProp);
				ErrJson->SetStringField(TEXT("error"), FString::Printf(TEXT("Source node '%s' not found"), *FromId));
				ErrorsArray.Add(MakeShared<FJsonValueObject>(ErrJson));
				continue;
			}

			EMaterialProperty MatProp = ParseMaterialProperty(ToProp);
			if (MatProp == MP_MAX)
			{
				auto ErrJson = MakeShared<FJsonObject>();
				ErrJson->SetStringField(TEXT("output"), ToProp);
				ErrJson->SetStringField(TEXT("error"), FString::Printf(TEXT("Unknown material property '%s'"), *ToProp));
				ErrorsArray.Add(MakeShared<FJsonValueObject>(ErrJson));
				continue;
			}

			// Blend mode validation warnings
			{
				EBlendMode BM = Mat->BlendMode;
				if (MatProp == MP_Opacity && (BM == BLEND_Opaque || BM == BLEND_Masked)
					&& !IsSubsurfaceShadingModel(Mat->GetShadingModels())
					&& !Mat->GetShadingModels().HasShadingModel(MSM_SingleLayerWater))
				{
					auto WJ = MakeShared<FJsonObject>();
					WJ->SetStringField(TEXT("output"), ToProp);
					WJ->SetStringField(TEXT("warning"), TEXT("Opacity has no effect on Opaque/Masked materials"));
					ErrorsArray.Add(MakeShared<FJsonValueObject>(WJ));
				}
				if (MatProp == MP_OpacityMask && BM != BLEND_Masked)
				{
					auto WJ = MakeShared<FJsonObject>();
					WJ->SetStringField(TEXT("output"), ToProp);
					WJ->SetStringField(TEXT("warning"), TEXT("OpacityMask only affects Masked blend mode"));
					ErrorsArray.Add(MakeShared<FJsonValueObject>(WJ));
				}
			}

			bool bConnected = UMaterialEditingLibrary::ConnectMaterialProperty(*FromPtr, FromPin, MatProp);
			if (bConnected)
			{
				ConnectionsMade++;
			}
			else
			{
				auto ErrJson = MakeShared<FJsonObject>();
				ErrJson->SetStringField(TEXT("output"), ToProp);
				ErrJson->SetStringField(TEXT("error"), TEXT("ConnectMaterialProperty returned false"));
				ErrorsArray.Add(MakeShared<FJsonValueObject>(ErrJson));
			}
		}
	}

	// Auto-recompile so the material is immediately usable
	UMaterialEditingLibrary::RecompileMaterial(Mat);

	GEditor->EndTransaction();

	auto ResultJson = MakeShared<FJsonObject>();
	// Separate actual errors from warnings (blend mode warnings are not errors)
	int32 ErrorCount = 0, WarningCount = 0;
	for (const TSharedPtr<FJsonValue>& Entry : ErrorsArray)
	{
		if (Entry->AsObject()->HasField(TEXT("warning"))) WarningCount++;
		else ErrorCount++;
	}
	ResultJson->SetBoolField(TEXT("has_errors"), ErrorCount > 0);
	ResultJson->SetBoolField(TEXT("has_warnings"), WarningCount > 0);
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetNumberField(TEXT("nodes_created"), NodesCreated);
	ResultJson->SetNumberField(TEXT("connections_made"), ConnectionsMade);
	ResultJson->SetBoolField(TEXT("recompiled"), true);

	auto IdMapJson = MakeShared<FJsonObject>();
	for (const auto& Pair : IdToExpr)
	{
		if (Pair.Value)
		{
			IdMapJson->SetStringField(Pair.Key, Pair.Value->GetName());
		}
	}
	ResultJson->SetObjectField(TEXT("id_to_name"), IdMapJson);
	ResultJson->SetArrayField(TEXT("errors"), ErrorsArray);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: export_material_graph
// Params: { "asset_path": "...", "include_properties": true, "include_positions": true }
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::ExportMaterialGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	bool bIncludeProperties = true;
	bool bIncludePositions = true;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("include_properties"), bIncludeProperties);
		Params->TryGetBoolField(TEXT("include_positions"), bIncludePositions);
	}

	UMaterial* Mat = LoadBaseMaterial(AssetPath);
	if (!Mat)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load base material at '%s'"), *AssetPath));
	}

	TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = Mat->GetExpressions();

	TMap<UMaterialExpression*, FString> ExprToId;
	for (const TObjectPtr<UMaterialExpression>& Expr : Expressions)
	{
		if (Expr)
		{
			ExprToId.Add(Expr, Expr->GetName());
		}
	}

	TArray<TSharedPtr<FJsonValue>> NodesArray;
	TArray<TSharedPtr<FJsonValue>> CustomHlslArray;

	for (const TObjectPtr<UMaterialExpression>& Expr : Expressions)
	{
		if (!Expr)
		{
			continue;
		}

		const UMaterialExpressionCustom* CustomExpr = Cast<UMaterialExpressionCustom>(Expr);

		if (CustomExpr)
		{
			auto NodeJson = MakeShared<FJsonObject>();
			NodeJson->SetStringField(TEXT("id"), Expr->GetName());

			if (bIncludePositions)
			{
				TArray<TSharedPtr<FJsonValue>> PosArr;
				PosArr.Add(MakeShared<FJsonValueNumber>(Expr->MaterialExpressionEditorX));
				PosArr.Add(MakeShared<FJsonValueNumber>(Expr->MaterialExpressionEditorY));
				NodeJson->SetArrayField(TEXT("pos"), PosArr);
			}

			NodeJson->SetStringField(TEXT("code"), CustomExpr->Code);
			NodeJson->SetStringField(TEXT("description"), CustomExpr->Description);
			NodeJson->SetStringField(TEXT("output_type"), CustomOutputTypeToString(CustomExpr->OutputType));

			TArray<TSharedPtr<FJsonValue>> InputsArr;
			for (const FCustomInput& CustInput : CustomExpr->Inputs)
			{
				auto InputJson = MakeShared<FJsonObject>();
				InputJson->SetStringField(TEXT("name"), CustInput.InputName.ToString());
				InputsArr.Add(MakeShared<FJsonValueObject>(InputJson));
			}
			NodeJson->SetArrayField(TEXT("inputs"), InputsArr);

			TArray<TSharedPtr<FJsonValue>> AddOutputsArr;
			for (const FCustomOutput& AddOut : CustomExpr->AdditionalOutputs)
			{
				auto OutJson = MakeShared<FJsonObject>();
				OutJson->SetStringField(TEXT("name"), AddOut.OutputName.ToString());
				OutJson->SetStringField(TEXT("type"), CustomOutputTypeToString(AddOut.OutputType));
				AddOutputsArr.Add(MakeShared<FJsonValueObject>(OutJson));
			}
			NodeJson->SetArrayField(TEXT("additional_outputs"), AddOutputsArr);

			CustomHlslArray.Add(MakeShared<FJsonValueObject>(NodeJson));
		}
		else
		{
			auto NodeJson = MakeShared<FJsonObject>();
			NodeJson->SetStringField(TEXT("id"), Expr->GetName());

			FString ClassName = Expr->GetClass()->GetName();
			ClassName.RemoveFromStart(TEXT("MaterialExpression"));
			NodeJson->SetStringField(TEXT("class"), ClassName);

			if (bIncludePositions)
			{
				TArray<TSharedPtr<FJsonValue>> PosArr;
				PosArr.Add(MakeShared<FJsonValueNumber>(Expr->MaterialExpressionEditorX));
				PosArr.Add(MakeShared<FJsonValueNumber>(Expr->MaterialExpressionEditorY));
				NodeJson->SetArrayField(TEXT("pos"), PosArr);
			}

			if (bIncludeProperties)
			{
				auto PropsJson = MakeShared<FJsonObject>();
				for (TFieldIterator<FProperty> PropIt(Expr->GetClass()); PropIt; ++PropIt)
				{
					FProperty* Prop = *PropIt;
					if (!Prop || Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient))
					{
						continue;
					}
					if (Prop->GetOwnerClass() == UMaterialExpression::StaticClass())
					{
						continue;
					}
					FString ValueStr;
					const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Expr);
					Prop->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, nullptr, PPF_None);
					if (!ValueStr.IsEmpty())
					{
						PropsJson->SetStringField(Prop->GetName(), ValueStr);
					}
				}
				NodeJson->SetObjectField(TEXT("props"), PropsJson);
			}

			NodesArray.Add(MakeShared<FJsonValueObject>(NodeJson));
		}
	}

	// Build connections
	TArray<TSharedPtr<FJsonValue>> ConnectionsArray;
	for (const TObjectPtr<UMaterialExpression>& Expr : Expressions)
	{
		if (!Expr)
		{
			continue;
		}
		for (int32 i = 0; ; ++i)
		{
			FExpressionInput* ExprInput = Expr->GetInput(i);
			if (!ExprInput)
			{
				break;
			}
			if (!ExprInput->Expression)
			{
				continue;
			}

			auto ConnJson = MakeShared<FJsonObject>();
			FString* FromId = ExprToId.Find(ExprInput->Expression);
			ConnJson->SetStringField(TEXT("from"), FromId ? *FromId : ExprInput->Expression->GetName());

			FString FromPin;
			const TArray<FExpressionOutput>& SourceOutputs = ExprInput->Expression->Outputs;
			if (SourceOutputs.IsValidIndex(ExprInput->OutputIndex))
			{
				FromPin = SourceOutputs[ExprInput->OutputIndex].OutputName.ToString();
			}
			ConnJson->SetStringField(TEXT("from_pin"), FromPin);
			ConnJson->SetStringField(TEXT("to"), Expr->GetName());
			ConnJson->SetStringField(TEXT("to_pin"), Expr->GetInputName(i).ToString());

			ConnectionsArray.Add(MakeShared<FJsonValueObject>(ConnJson));
		}
	}

	// Build outputs
	TArray<TSharedPtr<FJsonValue>> OutputsArray;
	for (EMaterialProperty MatProp : AllMaterialProperties)
	{
		FExpressionInput* PropInput = Mat->GetExpressionInputForProperty(MatProp);
		if (PropInput && PropInput->Expression)
		{
			auto OutJson = MakeShared<FJsonObject>();
			FString* FromId = ExprToId.Find(PropInput->Expression);
			OutJson->SetStringField(TEXT("from"), FromId ? *FromId : PropInput->Expression->GetName());

			FString FromPin;
			const TArray<FExpressionOutput>& SourceOutputs = PropInput->Expression->Outputs;
			if (SourceOutputs.IsValidIndex(PropInput->OutputIndex))
			{
				FromPin = SourceOutputs[PropInput->OutputIndex].OutputName.ToString();
			}
			OutJson->SetStringField(TEXT("from_pin"), FromPin);
			OutJson->SetStringField(TEXT("to_property"), MaterialPropertyToString(MatProp));
			OutputsArray.Add(MakeShared<FJsonValueObject>(OutJson));
		}
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetArrayField(TEXT("nodes"), NodesArray);
	ResultJson->SetArrayField(TEXT("custom_hlsl_nodes"), CustomHlslArray);
	ResultJson->SetArrayField(TEXT("connections"), ConnectionsArray);
	ResultJson->SetArrayField(TEXT("outputs"), OutputsArray);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: import_material_graph
// Params: { "asset_path": "...", "graph_json": "...", "mode": "overwrite"|"merge" }
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::ImportMaterialGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString GraphJson = Params->GetStringField(TEXT("graph_json"));
	FString Mode = Params->HasField(TEXT("mode")) ? Params->GetStringField(TEXT("mode")) : TEXT("overwrite");

	UMaterial* Mat = LoadBaseMaterial(AssetPath);
	if (!Mat)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load base material at '%s'"), *AssetPath));
	}

	if (Mode == TEXT("overwrite"))
	{
		// Build a params object for BuildMaterialGraph with clear_existing=true
		auto BuildParams = MakeShared<FJsonObject>();
		BuildParams->SetStringField(TEXT("asset_path"), AssetPath);
		BuildParams->SetStringField(TEXT("graph_spec"), GraphJson);
		BuildParams->SetBoolField(TEXT("clear_existing"), true);
		return BuildMaterialGraph(BuildParams);
	}
	else if (Mode == TEXT("merge"))
	{
		TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(GraphJson);
		TSharedPtr<FJsonObject> Spec;
		if (!FJsonSerializer::Deserialize(JsonReader, Spec) || !Spec.IsValid())
		{
			return FMonolithActionResult::Error(TEXT("Failed to parse graph_json for merge"));
		}

		auto OffsetNodePositions = [](const TArray<TSharedPtr<FJsonValue>>* ArrayPtr)
		{
			if (!ArrayPtr) return;
			for (const TSharedPtr<FJsonValue>& NodeVal : *ArrayPtr)
			{
				const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
				if (NodeVal && NodeVal->TryGetObject(NodeObjPtr) && NodeObjPtr)
				{
					const TArray<TSharedPtr<FJsonValue>>* PosArray = nullptr;
					if ((*NodeObjPtr)->TryGetArrayField(TEXT("pos"), PosArray) && PosArray->Num() >= 2)
					{
						double OrigX = (*PosArray)[0]->AsNumber();
						double OrigY = (*PosArray)[1]->AsNumber();
						TArray<TSharedPtr<FJsonValue>> NewPos;
						NewPos.Add(MakeShared<FJsonValueNumber>(OrigX + 500.0));
						NewPos.Add(MakeShared<FJsonValueNumber>(OrigY));
						(*NodeObjPtr)->SetArrayField(TEXT("pos"), NewPos);
					}
				}
			}
		};

		const TArray<TSharedPtr<FJsonValue>>* NodesArr = nullptr;
		Spec->TryGetArrayField(TEXT("nodes"), NodesArr);
		OffsetNodePositions(NodesArr);

		const TArray<TSharedPtr<FJsonValue>>* CustomArr = nullptr;
		Spec->TryGetArrayField(TEXT("custom_hlsl_nodes"), CustomArr);
		OffsetNodePositions(CustomArr);

		auto BuildParams = MakeShared<FJsonObject>();
		BuildParams->SetStringField(TEXT("asset_path"), AssetPath);
		BuildParams->SetObjectField(TEXT("graph_spec"), Spec);
		BuildParams->SetBoolField(TEXT("clear_existing"), false);
		return BuildMaterialGraph(BuildParams);
	}
	else
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown import mode '%s'. Use 'overwrite' or 'merge'."), *Mode));
	}
}

// ============================================================================
// Action: validate_material
// Params: { "asset_path": "...", "fix_issues": false }
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::ValidateMaterial(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	bool bFixIssues = Params->HasField(TEXT("fix_issues")) ? Params->GetBoolField(TEXT("fix_issues")) : false;

	UMaterial* Mat = LoadBaseMaterial(AssetPath);
	if (!Mat)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load base material at '%s'"), *AssetPath));
	}

	TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = Mat->GetExpressions();
	TArray<TSharedPtr<FJsonValue>> IssuesArray;
	int32 FixedCount = 0;

	// BFS from material outputs to find reachable expressions
	TSet<UMaterialExpression*> ReachableSet;
	TArray<UMaterialExpression*> BfsQueue;

	for (EMaterialProperty Prop : AllMaterialProperties)
	{
		FExpressionInput* PropInput = Mat->GetExpressionInputForProperty(Prop);
		if (PropInput && PropInput->Expression)
		{
			if (!ReachableSet.Contains(PropInput->Expression))
			{
				ReachableSet.Add(PropInput->Expression);
				BfsQueue.Add(PropInput->Expression);
			}
		}
	}

	// Also seed from UMaterialExpressionCustomOutput subclasses — they are output terminals too
	for (UMaterialExpression* Expr : Expressions)
	{
		if (Expr && Expr->IsA<UMaterialExpressionCustomOutput>())
		{
			if (!ReachableSet.Contains(Expr))
			{
				ReachableSet.Add(Expr);
				BfsQueue.Add(Expr);
			}
		}
	}

	// Seed from UMaterialExpressionMaterialAttributeLayers — implicit output terminals for layer-blend materials
	for (UMaterialExpression* Expr : Expressions)
	{
		if (Expr && Expr->IsA<UMaterialExpressionMaterialAttributeLayers>())
		{
			if (!ReachableSet.Contains(Expr))
			{
				ReachableSet.Add(Expr);
				BfsQueue.Add(Expr);
			}
		}
	}

	while (BfsQueue.Num() > 0)
	{
		UMaterialExpression* Current = BfsQueue.Pop(EAllowShrinking::No);
		for (int32 i = 0; ; ++i)
		{
			FExpressionInput* NodeInput = Current->GetInput(i);
			if (!NodeInput)
			{
				break;
			}
			if (NodeInput->Expression && !ReachableSet.Contains(NodeInput->Expression))
			{
				ReachableSet.Add(NodeInput->Expression);
				BfsQueue.Add(NodeInput->Expression);
			}
		}
	}

	TMap<FString, TArray<UMaterialExpression*>> ParameterNames;
	TArray<UMaterialExpression*> IslandExprs;

	for (const TObjectPtr<UMaterialExpression>& Expr : Expressions)
	{
		if (!Expr)
		{
			continue;
		}

		bool bIsReachable = ReachableSet.Contains(Expr);

		// Check: Disconnected islands (skip comments)
		if (!bIsReachable && !Cast<UMaterialExpressionComment>(Expr))
		{
			auto IssueJson = MakeShared<FJsonObject>();
			IssueJson->SetStringField(TEXT("severity"), TEXT("warning"));
			IssueJson->SetStringField(TEXT("type"), TEXT("island"));
			IssueJson->SetStringField(TEXT("expression"), Expr->GetName());
			IssueJson->SetStringField(TEXT("class"), Expr->GetClass()->GetName());

			bool bFixed = false;
			if (bFixIssues)
			{
				IslandExprs.Add(Expr);
				bFixed = true;
				FixedCount++;
			}
			IssueJson->SetBoolField(TEXT("fixed"), bFixed);
			IssuesArray.Add(MakeShared<FJsonValueObject>(IssueJson));
		}

		// Check: Broken texture refs
		if (const auto* TexBase = Cast<UMaterialExpressionTextureBase>(Expr))
		{
			if (!TexBase->Texture)
			{
				auto IssueJson = MakeShared<FJsonObject>();
				IssueJson->SetStringField(TEXT("severity"), TEXT("error"));
				IssueJson->SetStringField(TEXT("type"), TEXT("broken_texture_ref"));
				IssueJson->SetStringField(TEXT("expression"), Expr->GetName());
				IssueJson->SetBoolField(TEXT("fixed"), false);
				IssuesArray.Add(MakeShared<FJsonValueObject>(IssueJson));
			}
		}

		// Check: Missing material functions
		if (const auto* FuncCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expr))
		{
			if (!FuncCall->MaterialFunction)
			{
				auto IssueJson = MakeShared<FJsonObject>();
				IssueJson->SetStringField(TEXT("severity"), TEXT("error"));
				IssueJson->SetStringField(TEXT("type"), TEXT("missing_material_function"));
				IssueJson->SetStringField(TEXT("expression"), Expr->GetName());
				IssueJson->SetBoolField(TEXT("fixed"), false);
				IssuesArray.Add(MakeShared<FJsonValueObject>(IssueJson));
			}
		}

		// Collect parameter names for duplicate detection
		if (const auto* Param = Cast<UMaterialExpressionParameter>(Expr))
		{
			ParameterNames.FindOrAdd(Param->ParameterName.ToString()).Add(Expr);
		}
		else if (const auto* TexParam = Cast<UMaterialExpressionTextureSampleParameter>(Expr))
		{
			ParameterNames.FindOrAdd(TexParam->ParameterName.ToString()).Add(Expr);
		}

		// Check: Unused parameters
		if (!bIsReachable)
		{
			bool bIsParam = Cast<UMaterialExpressionParameter>(Expr) != nullptr
				|| Cast<UMaterialExpressionTextureSampleParameter>(Expr) != nullptr;
			if (bIsParam)
			{
				auto IssueJson = MakeShared<FJsonObject>();
				IssueJson->SetStringField(TEXT("severity"), TEXT("warning"));
				IssueJson->SetStringField(TEXT("type"), TEXT("unused_parameter"));
				IssueJson->SetStringField(TEXT("expression"), Expr->GetName());
				IssueJson->SetBoolField(TEXT("fixed"), false);
				IssuesArray.Add(MakeShared<FJsonValueObject>(IssueJson));
			}
		}
	}

	// Check: Duplicate parameter names
	for (const auto& Pair : ParameterNames)
	{
		if (Pair.Value.Num() > 1)
		{
			auto IssueJson = MakeShared<FJsonObject>();
			IssueJson->SetStringField(TEXT("severity"), TEXT("warning"));
			IssueJson->SetStringField(TEXT("type"), TEXT("duplicate_parameter_name"));
			IssueJson->SetStringField(TEXT("parameter_name"), Pair.Key);
			IssueJson->SetNumberField(TEXT("count"), Pair.Value.Num());

			TArray<TSharedPtr<FJsonValue>> DupExprs;
			for (UMaterialExpression* DupExpr : Pair.Value)
			{
				DupExprs.Add(MakeShared<FJsonValueString>(DupExpr->GetName()));
			}
			IssueJson->SetArrayField(TEXT("expressions"), DupExprs);
			IssueJson->SetBoolField(TEXT("fixed"), false);
			IssuesArray.Add(MakeShared<FJsonValueObject>(IssueJson));
		}
	}

	// Check: Expression count warning
	if (Expressions.Num() > 200)
	{
		auto IssueJson = MakeShared<FJsonObject>();
		IssueJson->SetStringField(TEXT("severity"), TEXT("info"));
		IssueJson->SetStringField(TEXT("type"), TEXT("high_expression_count"));
		IssueJson->SetNumberField(TEXT("count"), Expressions.Num());
		IssueJson->SetBoolField(TEXT("fixed"), false);
		IssuesArray.Add(MakeShared<FJsonValueObject>(IssueJson));
	}

	// Check: Blend mode / shading model vs connected outputs
	{
		auto AddBlendWarning = [&](const FString& Type, const FString& Message)
		{
			auto IssueJson = MakeShared<FJsonObject>();
			IssueJson->SetStringField(TEXT("severity"), TEXT("warning"));
			IssueJson->SetStringField(TEXT("type"), Type);
			IssueJson->SetStringField(TEXT("message"), Message);
			IssueJson->SetBoolField(TEXT("fixed"), false);
			IssuesArray.Add(MakeShared<FJsonValueObject>(IssueJson));
		};

		EBlendMode BM = Mat->BlendMode;
		FExpressionInput* OpacityInput = Mat->GetExpressionInputForProperty(MP_Opacity);
		FExpressionInput* OpacityMaskInput = Mat->GetExpressionInputForProperty(MP_OpacityMask);
		FExpressionInput* RefractionInput = Mat->GetExpressionInputForProperty(MP_Refraction);
		FExpressionInput* EmissiveInput = Mat->GetExpressionInputForProperty(MP_EmissiveColor);
		FExpressionInput* SubsurfaceInput = Mat->GetExpressionInputForProperty(MP_SubsurfaceColor);

		bool bHasOpacity = OpacityInput && OpacityInput->Expression;
		bool bHasOpacityMask = OpacityMaskInput && OpacityMaskInput->Expression;
		bool bHasRefraction = RefractionInput && RefractionInput->Expression;
		bool bHasEmissive = EmissiveInput && EmissiveInput->Expression;
		bool bHasSubsurface = SubsurfaceInput && SubsurfaceInput->Expression;

		// Masked without OpacityMask
		if (BM == BLEND_Masked && !bHasOpacityMask)
		{
			AddBlendWarning(TEXT("masked_no_opacity_mask"),
				TEXT("Masked blend mode but no OpacityMask input connected — material will be fully opaque or fully transparent"));
		}

		// Translucent/Additive without Opacity
		if ((BM == BLEND_Translucent || BM == BLEND_Additive) && !bHasOpacity)
		{
			AddBlendWarning(TEXT("translucent_no_opacity"),
				TEXT("Translucent/Additive blend mode but no Opacity input connected — defaults to fully opaque"));
		}

		// Opaque with Opacity or Refraction connected (wasted work)
		// NOTE: Subsurface/SingleLayerWater models legitimately use Opacity for scattering even in Opaque mode
		if (BM == BLEND_Opaque)
		{
			if (bHasOpacity
				&& !IsSubsurfaceShadingModel(Mat->GetShadingModels())
				&& !Mat->GetShadingModels().HasShadingModel(MSM_SingleLayerWater))
			{
				AddBlendWarning(TEXT("opaque_unused_opacity"),
					TEXT("Opaque blend mode with Opacity connected — Opacity has no effect, consider removing or changing blend mode"));
			}
			if (bHasRefraction)
			{
				AddBlendWarning(TEXT("opaque_unused_refraction"),
					TEXT("Opaque blend mode with Refraction connected — Refraction has no effect on Opaque materials"));
			}
		}

		// PostProcess domain without EmissiveColor
		if (Mat->MaterialDomain == MD_PostProcess && !bHasEmissive)
		{
			AddBlendWarning(TEXT("postprocess_no_emissive"),
				TEXT("PostProcess domain but no EmissiveColor connected — PostProcess materials output through EmissiveColor"));
		}

		// Subsurface outputs on non-subsurface shading model
		EMaterialShadingModel SM = Mat->GetShadingModels().GetFirstShadingModel();
		if (bHasSubsurface && SM != MSM_Subsurface && SM != MSM_SubsurfaceProfile
			&& SM != MSM_PreintegratedSkin && SM != MSM_TwoSidedFoliage)
		{
			AddBlendWarning(TEXT("subsurface_wrong_model"),
				TEXT("SubsurfaceColor connected but shading model is not Subsurface/SubsurfaceProfile/PreintegratedSkin/TwoSidedFoliage — SubsurfaceColor will be ignored"));
		}
	}

	// Apply fixes — delete island expressions
	if (bFixIssues && IslandExprs.Num() > 0)
	{
		GEditor->BeginTransaction(FText::FromString(TEXT("ValidateMaterial_FixIslands")));
		Mat->Modify();
		for (UMaterialExpression* IslandExpr : IslandExprs)
		{
			UMaterialEditingLibrary::DeleteMaterialExpression(Mat, IslandExpr);
		}
		GEditor->EndTransaction();
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetArrayField(TEXT("issues"), IssuesArray);
	ResultJson->SetNumberField(TEXT("issue_count"), IssuesArray.Num());
	ResultJson->SetNumberField(TEXT("fixed_count"), FixedCount);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: render_preview
// Params: { "asset_path": "...", "resolution": 256, "uv_tiling": 1.0, "preview_mesh": "sphere" }
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::RenderPreview(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	int32 Resolution = Params->HasField(TEXT("resolution")) ? static_cast<int32>(Params->GetNumberField(TEXT("resolution"))) : 256;
	if (Resolution <= 0)
	{
		Resolution = 256;
	}

	// Read new params
	double UVTiling = 1.0;
	Params->TryGetNumberField(TEXT("uv_tiling"), UVTiling);
	FString PreviewMesh = TEXT("sphere");
	Params->TryGetStringField(TEXT("preview_mesh"), PreviewMesh);

	// Custom preview pipeline: route through editor capture_scene_preview when
	// uv_tiling or preview_mesh differ from defaults (avoids MonolithEditor dependency)
	bool bUseCustomPipeline = !FMath::IsNearlyEqual(UVTiling, 1.0) || PreviewMesh != TEXT("sphere");

	if (bUseCustomPipeline)
	{
		auto CaptureParams = MakeShared<FJsonObject>();
		CaptureParams->SetStringField(TEXT("asset_path"), AssetPath);
		CaptureParams->SetStringField(TEXT("asset_type"), TEXT("material"));
		CaptureParams->SetStringField(TEXT("preview_mesh"), PreviewMesh);
		CaptureParams->SetNumberField(TEXT("uv_tiling"), UVTiling);

		// Resolution as array [W, H] for capture_scene_preview
		TArray<TSharedPtr<FJsonValue>> ResArr;
		ResArr.Add(MakeShared<FJsonValueNumber>((double)Resolution));
		ResArr.Add(MakeShared<FJsonValueNumber>((double)Resolution));
		CaptureParams->SetArrayField(TEXT("resolution"), ResArr);

		// Pass through background_color if provided
		if (Params->HasField(TEXT("background_color")))
		{
			CaptureParams->SetField(TEXT("background_color"), Params->TryGetField(TEXT("background_color")));
		}

		FMonolithActionResult CaptureResult = FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("editor"), TEXT("capture_scene_preview"), CaptureParams);

		if (!CaptureResult.bSuccess)
		{
			return CaptureResult;
		}

		// Augment the result with material-specific fields
		if (CaptureResult.Result.IsValid())
		{
			CaptureResult.Result->SetStringField(TEXT("asset_path"), AssetPath);
			CaptureResult.Result->SetNumberField(TEXT("uv_tiling"), UVTiling);
			CaptureResult.Result->SetStringField(TEXT("preview_mesh"), PreviewMesh);
			CaptureResult.Result->SetStringField(TEXT("pipeline"), TEXT("custom_capture"));
		}

		return CaptureResult;
	}

	// Fast path: default 1x sphere via ThumbnailTools
	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	if (!LoadedAsset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load asset at '%s'"), *AssetPath));
	}

	FObjectThumbnail Thumbnail;
	ThumbnailTools::RenderThumbnail(LoadedAsset, Resolution, Resolution,
		ThumbnailTools::EThumbnailTextureFlushMode::NeverFlush, nullptr, &Thumbnail);

	if (Thumbnail.GetImageWidth() == 0 || Thumbnail.GetImageHeight() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Thumbnail rendering produced an empty image"));
	}

	TArray<uint8>& ThumbData = Thumbnail.AccessImageData();
	int32 Width = Thumbnail.GetImageWidth();
	int32 Height = Thumbnail.GetImageHeight();

	TArray64<uint8> PngData;
	FImageView ImageView((void*)ThumbData.GetData(), Width, Height, ERawImageFormat::BGRA8);
	FImageUtils::CompressImage(PngData, TEXT(".png"), ImageView);

	if (PngData.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Failed to compress thumbnail to PNG"));
	}

	FString MaterialName = FPaths::GetBaseFilename(AssetPath);
	FString PreviewDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Monolith"), TEXT("previews"));
	IFileManager::Get().MakeDirectory(*PreviewDir, true);
	FString FilePath = FPaths::Combine(PreviewDir, FString::Printf(TEXT("%s_%d.png"), *MaterialName, Resolution));

	if (!FFileHelper::SaveArrayToFile(PngData, *FilePath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to save PNG to '%s'"), *FilePath));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetStringField(TEXT("file_path"), FilePath);
	ResultJson->SetNumberField(TEXT("width"), Width);
	ResultJson->SetNumberField(TEXT("height"), Height);
	ResultJson->SetNumberField(TEXT("uv_tiling"), UVTiling);
	ResultJson->SetStringField(TEXT("preview_mesh"), PreviewMesh);
	ResultJson->SetStringField(TEXT("pipeline"), TEXT("thumbnail"));

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: get_thumbnail
// Params: { "asset_path": "...", "resolution": 256, "save_to_file": false }
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::GetThumbnail(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	int32 Resolution = Params->HasField(TEXT("resolution")) ? static_cast<int32>(Params->GetNumberField(TEXT("resolution"))) : 256;
	if (Resolution <= 0)
	{
		Resolution = 256;
	}

	bool bSaveToFile = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("save_to_file"), bSaveToFile);
	}

	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	if (!LoadedAsset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load asset at '%s'"), *AssetPath));
	}

	FObjectThumbnail Thumbnail;
	ThumbnailTools::RenderThumbnail(LoadedAsset, Resolution, Resolution,
		ThumbnailTools::EThumbnailTextureFlushMode::NeverFlush, nullptr, &Thumbnail);

	if (Thumbnail.GetImageWidth() == 0 || Thumbnail.GetImageHeight() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Thumbnail rendering produced an empty image"));
	}

	TArray<uint8>& ThumbData = Thumbnail.AccessImageData();
	int32 Width = Thumbnail.GetImageWidth();
	int32 Height = Thumbnail.GetImageHeight();

	TArray64<uint8> PngData;
	FImageView ImageView((void*)ThumbData.GetData(), Width, Height, ERawImageFormat::BGRA8);
	FImageUtils::CompressImage(PngData, TEXT(".png"), ImageView);

	if (PngData.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Failed to compress thumbnail to PNG"));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetNumberField(TEXT("width"), Width);
	ResultJson->SetNumberField(TEXT("height"), Height);

	if (bSaveToFile)
	{
		FString AssetName = FPaths::GetBaseFilename(AssetPath);
		FString SaveDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Monolith"), TEXT("previews"));
		IFileManager::Get().MakeDirectory(*SaveDir, true);
		FString FullPath = FPaths::Combine(SaveDir, FString::Printf(TEXT("%s_%d.png"), *AssetName, Resolution));

		if (!FFileHelper::SaveArrayToFile(PngData, *FullPath))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to save thumbnail to '%s'"), *FullPath));
		}

		ResultJson->SetStringField(TEXT("file_path"), FullPath);
	}
	else
	{
		FString Base64String = FBase64::Encode(PngData.GetData(), static_cast<uint32>(PngData.Num()));
		ResultJson->SetStringField(TEXT("format"), TEXT("png"));
		ResultJson->SetStringField(TEXT("encoding"), TEXT("base64"));
		ResultJson->SetStringField(TEXT("data"), Base64String);
	}

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: create_custom_hlsl_node
// Params: { "asset_path": "...", "code": "...", "description": "...", "output_type": "Float4",
//           "inputs": [...], "additional_outputs": [...], "pos_x": 0, "pos_y": 0 }
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::CreateCustomHLSLNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString Code = Params->GetStringField(TEXT("code"));
	FString Description = Params->HasField(TEXT("description")) ? Params->GetStringField(TEXT("description")) : TEXT("");
	FString OutputType = Params->HasField(TEXT("output_type")) ? Params->GetStringField(TEXT("output_type")) : TEXT("");
	int32 PosX = Params->HasField(TEXT("pos_x")) ? static_cast<int32>(Params->GetNumberField(TEXT("pos_x"))) : 0;
	int32 PosY = Params->HasField(TEXT("pos_y")) ? static_cast<int32>(Params->GetNumberField(TEXT("pos_y"))) : 0;

	UMaterial* Mat = LoadBaseMaterial(AssetPath);
	if (!Mat)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load base material at '%s'"), *AssetPath));
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("CreateCustomHLSLNode")));
	Mat->Modify();

	UMaterialExpression* BaseExpr = UMaterialEditingLibrary::CreateMaterialExpression(
		Mat, UMaterialExpressionCustom::StaticClass(), PosX, PosY);

	UMaterialExpressionCustom* CustomExpr = Cast<UMaterialExpressionCustom>(BaseExpr);
	if (!CustomExpr)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(TEXT("Failed to create Custom HLSL expression"));
	}

	CustomExpr->Code = Code;
	CustomExpr->Description = Description;

	if (!OutputType.IsEmpty())
	{
		CustomExpr->OutputType = ParseCustomOutputType(OutputType);
	}

	// Set inputs from JSON array
	const TArray<TSharedPtr<FJsonValue>>* InputsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("inputs"), InputsArray))
	{
		CustomExpr->Inputs.Empty();
		for (const TSharedPtr<FJsonValue>& InputVal : *InputsArray)
		{
			const TSharedPtr<FJsonObject>* InputObjPtr = nullptr;
			if (InputVal && InputVal->TryGetObject(InputObjPtr) && InputObjPtr)
			{
				FCustomInput NewInput;
				NewInput.InputName = *(*InputObjPtr)->GetStringField(TEXT("name"));
				CustomExpr->Inputs.Add(NewInput);
			}
		}
	}

	// Set additional outputs from JSON array
	const TArray<TSharedPtr<FJsonValue>>* AddOutputsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("additional_outputs"), AddOutputsArray))
	{
		CustomExpr->AdditionalOutputs.Empty();
		for (const TSharedPtr<FJsonValue>& OutVal : *AddOutputsArray)
		{
			const TSharedPtr<FJsonObject>* OutObjPtr = nullptr;
			if (OutVal && OutVal->TryGetObject(OutObjPtr) && OutObjPtr)
			{
				FCustomOutput NewOutput;
				NewOutput.OutputName = *(*OutObjPtr)->GetStringField(TEXT("name"));
				if ((*OutObjPtr)->HasField(TEXT("type")))
				{
					NewOutput.OutputType = ParseCustomOutputType((*OutObjPtr)->GetStringField(TEXT("type")));
				}
				CustomExpr->AdditionalOutputs.Add(NewOutput);
			}
		}
	}

	CustomExpr->RebuildOutputs();
	GEditor->EndTransaction();

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetStringField(TEXT("expression_name"), CustomExpr->GetName());
	ResultJson->SetStringField(TEXT("description"), CustomExpr->Description);
	ResultJson->SetStringField(TEXT("output_type"), CustomOutputTypeToString(CustomExpr->OutputType));
	ResultJson->SetNumberField(TEXT("input_count"), CustomExpr->Inputs.Num());
	ResultJson->SetNumberField(TEXT("additional_output_count"), CustomExpr->AdditionalOutputs.Num());
	ResultJson->SetNumberField(TEXT("pos_x"), PosX);
	ResultJson->SetNumberField(TEXT("pos_y"), PosY);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: get_layer_info
// Params: { "asset_path": "..." }
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::GetLayerInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	if (!LoadedAsset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load asset at '%s'"), *AssetPath));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);

	UMaterialFunctionMaterialLayer* Layer = Cast<UMaterialFunctionMaterialLayer>(LoadedAsset);
	UMaterialFunctionMaterialLayerBlend* LayerBlend = Cast<UMaterialFunctionMaterialLayerBlend>(LoadedAsset);
	UMaterialFunction* MatFunc = Cast<UMaterialFunction>(LoadedAsset);

	if (Layer)
	{
		ResultJson->SetStringField(TEXT("type"), TEXT("MaterialLayer"));
		MatFunc = Layer;
	}
	else if (LayerBlend)
	{
		ResultJson->SetStringField(TEXT("type"), TEXT("MaterialLayerBlend"));
		MatFunc = LayerBlend;
	}
	else if (MatFunc)
	{
		ResultJson->SetStringField(TEXT("type"), TEXT("MaterialFunction"));
	}
	else
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset '%s' is not a MaterialFunction, MaterialLayer, or MaterialLayerBlend"), *AssetPath));
	}

	ResultJson->SetStringField(TEXT("description"), MatFunc->Description);

	TArray<TSharedPtr<FJsonValue>> FuncExpressionsArray;
	TArray<TSharedPtr<FJsonValue>> FuncInputsArray;
	TArray<TSharedPtr<FJsonValue>> FuncOutputsArray;

	TConstArrayView<TObjectPtr<UMaterialExpression>> FuncExprs = MatFunc->GetExpressions();

	for (const TObjectPtr<UMaterialExpression>& Expr : FuncExprs)
	{
		if (!Expr)
		{
			continue;
		}

		if (const auto* FuncInput = Cast<UMaterialExpressionFunctionInput>(Expr))
		{
			auto InputJson = MakeShared<FJsonObject>();
			InputJson->SetStringField(TEXT("name"), FuncInput->InputName.ToString());
			InputJson->SetStringField(TEXT("expression_name"), FuncInput->GetName());
			InputJson->SetNumberField(TEXT("sort_priority"), FuncInput->SortPriority);
			FuncInputsArray.Add(MakeShared<FJsonValueObject>(InputJson));
		}

		if (const auto* FuncOutput = Cast<UMaterialExpressionFunctionOutput>(Expr))
		{
			auto OutputJson = MakeShared<FJsonObject>();
			OutputJson->SetStringField(TEXT("name"), FuncOutput->OutputName.ToString());
			OutputJson->SetStringField(TEXT("expression_name"), FuncOutput->GetName());
			OutputJson->SetNumberField(TEXT("sort_priority"), FuncOutput->SortPriority);
			FuncOutputsArray.Add(MakeShared<FJsonValueObject>(OutputJson));
		}

		auto ExprJson = MakeShared<FJsonObject>();
		ExprJson->SetStringField(TEXT("name"), Expr->GetName());
		ExprJson->SetStringField(TEXT("class"), Expr->GetClass()->GetName());
		ExprJson->SetNumberField(TEXT("pos_x"), Expr->MaterialExpressionEditorX);
		ExprJson->SetNumberField(TEXT("pos_y"), Expr->MaterialExpressionEditorY);
		FuncExpressionsArray.Add(MakeShared<FJsonValueObject>(ExprJson));
	}

	ResultJson->SetArrayField(TEXT("inputs"), FuncInputsArray);
	ResultJson->SetArrayField(TEXT("outputs"), FuncOutputsArray);
	ResultJson->SetArrayField(TEXT("expressions"), FuncExpressionsArray);
	ResultJson->SetNumberField(TEXT("expression_count"), FuncExpressionsArray.Num());

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Wave 2 — Helpers
// ============================================================================

// Generic enum parser using StaticEnum — handles bare names ("Opaque"),
// prefixed ("BLEND_Opaque"), and fully qualified ("EBlendMode::Opaque") forms.
template<typename TEnum>
static bool ParseEnum(const FString& Str, TEnum& OutValue, FString& OutError)
{
	const UEnum* Enum = StaticEnum<TEnum>();

	// Pass 1: try the raw input (handles prefixed and fully qualified forms)
	int64 Value = Enum->GetValueByNameString(Str);

	// Pass 2: if raw failed, try prepending common enum prefixes for short names
	if (Value == INDEX_NONE)
	{
		// Extract the prefix from the first enum value (e.g., "BLEND_" from "BLEND_Opaque")
		FString FirstName = Enum->GetNameStringByIndex(0);
		int32 UnderscoreIdx;
		if (FirstName.FindChar('_', UnderscoreIdx))
		{
			FString Prefix = FirstName.Left(UnderscoreIdx + 1); // e.g., "BLEND_"
			Value = Enum->GetValueByNameString(Prefix + Str);
		}
	}

	if (Value == INDEX_NONE)
	{
		TArray<FString> ValidNames;
		for (int32 i = 0; i < Enum->NumEnums() - 1; i++)
		{
			FString Name = Enum->GetNameStringByIndex(i);
			// Filter out sentinel values (_MAX, _NUM, FromMaterialExpression)
			if (Name.EndsWith(TEXT("_MAX")) || Name.EndsWith(TEXT("_NUM")) || Name.Contains(TEXT("FromMaterial")))
			{
				continue;
			}
			ValidNames.Add(Name);
		}
		OutError = FString::Printf(TEXT("Invalid value '%s'. Valid values: %s"), *Str, *FString::Join(ValidNames, TEXT(", ")));
		return false;
	}
	OutValue = static_cast<TEnum>(Value);
	return true;
}

// ============================================================================
// Action: create_material
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::CreateMaterial(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	// Parse optional properties
	FString BlendModeStr = Params->HasField(TEXT("blend_mode")) ? Params->GetStringField(TEXT("blend_mode")) : TEXT("Opaque");
	FString ShadingModelStr = Params->HasField(TEXT("shading_model")) ? Params->GetStringField(TEXT("shading_model")) : TEXT("DefaultLit");
	FString DomainStr = Params->HasField(TEXT("material_domain")) ? Params->GetStringField(TEXT("material_domain")) : TEXT("Surface");
	bool bTwoSided = Params->HasField(TEXT("two_sided")) ? Params->GetBoolField(TEXT("two_sided")) : false;

	// Extract package path and asset name from the asset path
	FString PackagePath, AssetName;
	int32 LastSlash;
	if (AssetPath.FindLastChar('/', LastSlash))
	{
		PackagePath = AssetPath.Left(LastSlash);
		AssetName = AssetPath.Mid(LastSlash + 1);
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("Invalid asset path — must contain at least one '/' (e.g. /Game/Materials/M_Name)"));
	}

	if (AssetName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Asset name is empty"));
	}

	// Check if asset already exists
	UObject* Existing = UEditorAssetLibrary::LoadAsset(AssetPath);
	if (Existing)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));
	}

	// Create package and material
	UPackage* Pkg = CreatePackage(*AssetPath);
	if (!Pkg)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at '%s'"), *AssetPath));
	}

	UMaterial* NewMat = NewObject<UMaterial>(Pkg, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	if (!NewMat)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create UMaterial object"));
	}

	// Set material properties
	FString EnumError;
	EMaterialDomain Domain;
	if (!ParseEnum<EMaterialDomain>(DomainStr, Domain, EnumError))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("material_domain: %s"), *EnumError));
	}
	EBlendMode BlendMode;
	if (!ParseEnum<EBlendMode>(BlendModeStr, BlendMode, EnumError))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("blend_mode: %s"), *EnumError));
	}
	EMaterialShadingModel ShadingModel;
	if (!ParseEnum<EMaterialShadingModel>(ShadingModelStr, ShadingModel, EnumError))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("shading_model: %s"), *EnumError));
	}
	NewMat->MaterialDomain = Domain;
	NewMat->BlendMode = BlendMode;
	NewMat->SetShadingModel(ShadingModel);
	NewMat->TwoSided = bTwoSided;

	// Register with asset registry and mark dirty
	FAssetRegistryModule::AssetCreated(NewMat);
	Pkg->MarkPackageDirty();

	// Trigger initial compile
	NewMat->PreEditChange(nullptr);
	NewMat->PostEditChange();

	// Save to disk so LoadAsset can find it later
	FString PackageFilename = FPackageName::LongPackageNameToFilename(AssetPath, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	UPackage::SavePackage(Pkg, NewMat, *PackageFilename, SaveArgs);

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), NewMat->GetPathName());
	ResultJson->SetStringField(TEXT("asset_name"), AssetName);
	ResultJson->SetStringField(TEXT("blend_mode"), BlendModeStr);
	ResultJson->SetStringField(TEXT("shading_model"), ShadingModelStr);
	ResultJson->SetStringField(TEXT("material_domain"), DomainStr);
	ResultJson->SetBoolField(TEXT("two_sided"), bTwoSided);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: create_material_instance
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::CreateMaterialInstance(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString ParentPath = Params->GetStringField(TEXT("parent_material"));

	// Load parent material
	UObject* ParentObj = UEditorAssetLibrary::LoadAsset(ParentPath);
	UMaterialInterface* ParentMat = ParentObj ? Cast<UMaterialInterface>(ParentObj) : nullptr;
	if (!ParentMat)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load parent material at '%s'"), *ParentPath));
	}

	// Check if asset already exists
	UObject* Existing = UEditorAssetLibrary::LoadAsset(AssetPath);
	if (Existing)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));
	}

	// Extract asset name
	FString AssetName;
	int32 LastSlash;
	if (!AssetPath.FindLastChar('/', LastSlash) || LastSlash == AssetPath.Len() - 1)
	{
		return FMonolithActionResult::Error(TEXT("Invalid asset path"));
	}
	AssetName = AssetPath.Mid(LastSlash + 1);

	// Create package and MIC
	UPackage* Pkg = CreatePackage(*AssetPath);
	if (!Pkg)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create package"));
	}

	UMaterialInstanceConstant* MIC = NewObject<UMaterialInstanceConstant>(Pkg, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	if (!MIC)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create UMaterialInstanceConstant"));
	}

	MIC->SetParentEditorOnly(ParentMat);

	// Apply scalar parameter overrides
	int32 ScalarCount = 0;
	const TSharedPtr<FJsonObject>* ScalarParams = nullptr;
	if (Params->TryGetObjectField(TEXT("scalar_parameters"), ScalarParams))
	{
		for (const auto& Pair : (*ScalarParams)->Values)
		{
			float Value = static_cast<float>(Pair.Value->AsNumber());
			MIC->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(*Pair.Key), Value);
			ScalarCount++;
		}
	}

	// Apply vector parameter overrides
	int32 VectorCount = 0;
	const TSharedPtr<FJsonObject>* VectorParams = nullptr;
	if (Params->TryGetObjectField(TEXT("vector_parameters"), VectorParams))
	{
		for (const auto& Pair : (*VectorParams)->Values)
		{
			const TSharedPtr<FJsonObject>* ColorObj = nullptr;
			if (Pair.Value->TryGetObject(ColorObj))
			{
				FLinearColor Color;
				Color.R = (*ColorObj)->HasField(TEXT("R")) ? static_cast<float>((*ColorObj)->GetNumberField(TEXT("R"))) : 0.f;
				Color.G = (*ColorObj)->HasField(TEXT("G")) ? static_cast<float>((*ColorObj)->GetNumberField(TEXT("G"))) : 0.f;
				Color.B = (*ColorObj)->HasField(TEXT("B")) ? static_cast<float>((*ColorObj)->GetNumberField(TEXT("B"))) : 0.f;
				Color.A = (*ColorObj)->HasField(TEXT("A")) ? static_cast<float>((*ColorObj)->GetNumberField(TEXT("A"))) : 1.f;
				MIC->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(*Pair.Key), Color);
				VectorCount++;
			}
		}
	}

	// Apply texture parameter overrides
	int32 TextureCount = 0;
	const TSharedPtr<FJsonObject>* TextureParams = nullptr;
	if (Params->TryGetObjectField(TEXT("texture_parameters"), TextureParams))
	{
		for (const auto& Pair : (*TextureParams)->Values)
		{
			FString TexPath = Pair.Value->AsString();
			UTexture* Tex = Cast<UTexture>(UEditorAssetLibrary::LoadAsset(TexPath));
			if (Tex)
			{
				MIC->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(*Pair.Key), Tex);
				TextureCount++;
			}
		}
	}

	// Apply static switch parameter overrides
	int32 SwitchCount = 0;
	const TSharedPtr<FJsonObject>* SwitchParams = nullptr;
	if (Params->TryGetObjectField(TEXT("static_switch_parameters"), SwitchParams))
	{
		for (const auto& Pair : (*SwitchParams)->Values)
		{
			bool bValue = Pair.Value->AsBool();
			MIC->SetStaticSwitchParameterValueEditorOnly(FMaterialParameterInfo(*Pair.Key), bValue);
			SwitchCount++;
		}
	}

	// If static switches were set, update the static permutation
	if (SwitchCount > 0)
	{
		UMaterialEditingLibrary::UpdateMaterialInstance(MIC);
	}

	FAssetRegistryModule::AssetCreated(MIC);
	Pkg->MarkPackageDirty();

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), MIC->GetPathName());
	ResultJson->SetStringField(TEXT("asset_name"), AssetName);
	ResultJson->SetStringField(TEXT("parent_material"), ParentMat->GetPathName());
	ResultJson->SetNumberField(TEXT("scalar_overrides"), ScalarCount);
	ResultJson->SetNumberField(TEXT("vector_overrides"), VectorCount);
	ResultJson->SetNumberField(TEXT("texture_overrides"), TextureCount);
	ResultJson->SetNumberField(TEXT("static_switch_overrides"), SwitchCount);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: set_material_property
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::SetMaterialProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UMaterial* Mat = LoadBaseMaterial(AssetPath);
	if (!Mat)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load base material at '%s'"), *AssetPath));
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("SetMaterialProperty")));
	Mat->Modify();

	TArray<TSharedPtr<FJsonValue>> ChangedArray;
	int32 ChangeCount = 0;

	auto RecordChange = [&](const FString& PropName, const FString& Value)
	{
		auto ChangeJson = MakeShared<FJsonObject>();
		ChangeJson->SetStringField(TEXT("property"), PropName);
		ChangeJson->SetStringField(TEXT("value"), Value);
		ChangedArray.Add(MakeShared<FJsonValueObject>(ChangeJson));
		ChangeCount++;
	};

	if (Params->HasField(TEXT("blend_mode")))
	{
		FString Val = Params->GetStringField(TEXT("blend_mode"));
		FString EnumError;
		EBlendMode ParsedMode;
		if (!ParseEnum<EBlendMode>(Val, ParsedMode, EnumError))
		{
			GEditor->EndTransaction();
			return FMonolithActionResult::Error(FString::Printf(TEXT("blend_mode: %s"), *EnumError));
		}
		Mat->BlendMode = ParsedMode;
		RecordChange(TEXT("blend_mode"), Val);
	}
	if (Params->HasField(TEXT("shading_model")))
	{
		FString Val = Params->GetStringField(TEXT("shading_model"));
		FString EnumError;
		EMaterialShadingModel ParsedModel;
		if (!ParseEnum<EMaterialShadingModel>(Val, ParsedModel, EnumError))
		{
			GEditor->EndTransaction();
			return FMonolithActionResult::Error(FString::Printf(TEXT("shading_model: %s"), *EnumError));
		}
		Mat->SetShadingModel(ParsedModel);
		RecordChange(TEXT("shading_model"), Val);
	}
	if (Params->HasField(TEXT("material_domain")))
	{
		FString Val = Params->GetStringField(TEXT("material_domain"));
		FString EnumError;
		EMaterialDomain ParsedDomain;
		if (!ParseEnum<EMaterialDomain>(Val, ParsedDomain, EnumError))
		{
			GEditor->EndTransaction();
			return FMonolithActionResult::Error(FString::Printf(TEXT("material_domain: %s"), *EnumError));
		}
		Mat->MaterialDomain = ParsedDomain;
		RecordChange(TEXT("material_domain"), Val);
	}
	if (Params->HasField(TEXT("two_sided")))
	{
		bool Val = Params->GetBoolField(TEXT("two_sided"));
		Mat->TwoSided = Val;
		RecordChange(TEXT("two_sided"), Val ? TEXT("true") : TEXT("false"));
	}
	if (Params->HasField(TEXT("opacity_mask_clip_value")))
	{
		float Val = static_cast<float>(Params->GetNumberField(TEXT("opacity_mask_clip_value")));
		Mat->OpacityMaskClipValue = Val;
		RecordChange(TEXT("opacity_mask_clip_value"), FString::SanitizeFloat(Val));
	}
	if (Params->HasField(TEXT("dithered_lod_transition")))
	{
		bool Val = Params->GetBoolField(TEXT("dithered_lod_transition"));
		Mat->DitheredLODTransition = Val;
		RecordChange(TEXT("dithered_lod_transition"), Val ? TEXT("true") : TEXT("false"));
	}
	if (Params->HasField(TEXT("used_with_skeletal_mesh")))
	{
		bool Val = Params->GetBoolField(TEXT("used_with_skeletal_mesh"));
		if (Val)
		{
			bool bRecompile = false;
			UMaterialEditingLibrary::SetMaterialUsage(Mat, MATUSAGE_SkeletalMesh, bRecompile);
		}
		RecordChange(TEXT("used_with_skeletal_mesh"), Val ? TEXT("true") : TEXT("false"));
	}
	if (Params->HasField(TEXT("used_with_particle_sprites")))
	{
		bool Val = Params->GetBoolField(TEXT("used_with_particle_sprites"));
		if (Val)
		{
			bool bRecompile = false;
			UMaterialEditingLibrary::SetMaterialUsage(Mat, MATUSAGE_ParticleSprites, bRecompile);
		}
		RecordChange(TEXT("used_with_particle_sprites"), Val ? TEXT("true") : TEXT("false"));
	}
	if (Params->HasField(TEXT("used_with_niagara_sprites")))
	{
		bool Val = Params->GetBoolField(TEXT("used_with_niagara_sprites"));
		if (Val)
		{
			bool bRecompile = false;
			UMaterialEditingLibrary::SetMaterialUsage(Mat, MATUSAGE_NiagaraSprites, bRecompile);
		}
		RecordChange(TEXT("used_with_niagara_sprites"), Val ? TEXT("true") : TEXT("false"));
	}
	if (Params->HasField(TEXT("used_with_niagara_meshes")))
	{
		bool Val = Params->GetBoolField(TEXT("used_with_niagara_meshes"));
		if (Val)
		{
			bool bRecompile = false;
			UMaterialEditingLibrary::SetMaterialUsage(Mat, MATUSAGE_NiagaraMeshParticles, bRecompile);
		}
		RecordChange(TEXT("used_with_niagara_meshes"), Val ? TEXT("true") : TEXT("false"));
	}
	if (Params->HasField(TEXT("used_with_niagara_ribbons")))
	{
		bool Val = Params->GetBoolField(TEXT("used_with_niagara_ribbons"));
		if (Val)
		{
			bool bRecompile = false;
			UMaterialEditingLibrary::SetMaterialUsage(Mat, MATUSAGE_NiagaraRibbons, bRecompile);
		}
		RecordChange(TEXT("used_with_niagara_ribbons"), Val ? TEXT("true") : TEXT("false"));
	}
	if (Params->HasField(TEXT("used_with_morph_targets")))
	{
		bool Val = Params->GetBoolField(TEXT("used_with_morph_targets"));
		if (Val)
		{
			bool bRecompile = false;
			UMaterialEditingLibrary::SetMaterialUsage(Mat, MATUSAGE_MorphTargets, bRecompile);
		}
		RecordChange(TEXT("used_with_morph_targets"), Val ? TEXT("true") : TEXT("false"));
	}
	if (Params->HasField(TEXT("used_with_instanced_static_meshes")))
	{
		bool Val = Params->GetBoolField(TEXT("used_with_instanced_static_meshes"));
		if (Val)
		{
			bool bRecompile = false;
			UMaterialEditingLibrary::SetMaterialUsage(Mat, MATUSAGE_InstancedStaticMeshes, bRecompile);
		}
		RecordChange(TEXT("used_with_instanced_static_meshes"), Val ? TEXT("true") : TEXT("false"));
	}
	if (Params->HasField(TEXT("used_with_static_lighting")))
	{
		bool Val = Params->GetBoolField(TEXT("used_with_static_lighting"));
		if (Val)
		{
			bool bRecompile = false;
			UMaterialEditingLibrary::SetMaterialUsage(Mat, MATUSAGE_StaticLighting, bRecompile);
		}
		RecordChange(TEXT("used_with_static_lighting"), Val ? TEXT("true") : TEXT("false"));
	}
	if (Params->HasField(TEXT("fully_rough")))
	{
		bool Val = Params->GetBoolField(TEXT("fully_rough"));
		Mat->bFullyRough = Val;
		RecordChange(TEXT("fully_rough"), Val ? TEXT("true") : TEXT("false"));
	}
	if (Params->HasField(TEXT("cast_shadow_as_masked")))
	{
		bool Val = Params->GetBoolField(TEXT("cast_shadow_as_masked"));
		Mat->bCastDynamicShadowAsMasked = Val;
		RecordChange(TEXT("cast_shadow_as_masked"), Val ? TEXT("true") : TEXT("false"));
	}
	// Note: bOutputVelocityOnBasePass removed in UE 5.7 — velocity output is now automatic

	Mat->PreEditChange(nullptr);
	Mat->PostEditChange();
	GEditor->EndTransaction();

	// Bug fix: flush property changes to disk so subsequent LoadAsset calls
	// (e.g. get_compilation_stats) don't read stale on-disk values after GC
	UEditorAssetLibrary::SaveAsset(AssetPath, false);

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetNumberField(TEXT("changes"), ChangeCount);
	ResultJson->SetArrayField(TEXT("changed"), ChangedArray);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: delete_expression
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::DeleteExpression(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString ExprName = Params->GetStringField(TEXT("expression_name"));

	UMaterial* Mat = LoadBaseMaterial(AssetPath);
	if (!Mat)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load base material at '%s'"), *AssetPath));
	}

	// Find the expression
	UMaterialExpression* TargetExpr = nullptr;
	TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = Mat->GetExpressions();
	for (const TObjectPtr<UMaterialExpression>& Expr : Expressions)
	{
		if (Expr && Expr->GetName() == ExprName)
		{
			TargetExpr = Expr;
			break;
		}
	}

	if (!TargetExpr)
	{
		TArray<FString> AvailableNames;
		for (const TObjectPtr<UMaterialExpression>& Expr : Expressions)
		{
			if (Expr) AvailableNames.Add(Expr->GetName());
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Expression '%s' not found. Available: %s"),
			*ExprName, *FString::Join(AvailableNames, TEXT(", "))));
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("DeleteExpression")));
	Mat->Modify();

	FString ClassName = TargetExpr->GetClass()->GetName();
	UMaterialEditingLibrary::DeleteMaterialExpression(Mat, TargetExpr);

	Mat->PreEditChange(nullptr);
	Mat->PostEditChange();
	GEditor->EndTransaction();

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetStringField(TEXT("deleted"), ExprName);
	ResultJson->SetStringField(TEXT("class"), ClassName);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: get_material_parameters
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::GetMaterialParameters(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	UMaterialInterface* MatInterface = LoadedAsset ? Cast<UMaterialInterface>(LoadedAsset) : nullptr;
	if (!MatInterface)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load material at '%s'"), *AssetPath));
	}

	TArray<TSharedPtr<FJsonValue>> ScalarArray, VectorArray, TextureArray, SwitchArray;

	// Scalar parameters
	TArray<FMaterialParameterInfo> ScalarInfos;
	TArray<FGuid> ScalarGuids;
	MatInterface->GetAllScalarParameterInfo(ScalarInfos, ScalarGuids);
	for (int32 i = 0; i < ScalarInfos.Num(); ++i)
	{
		float Value = 0.f;
		MatInterface->GetScalarParameterValue(ScalarInfos[i], Value);

		auto PJson = MakeShared<FJsonObject>();
		PJson->SetStringField(TEXT("name"), ScalarInfos[i].Name.ToString());
		PJson->SetNumberField(TEXT("value"), Value);
		PJson->SetStringField(TEXT("type"), TEXT("scalar"));
		ScalarArray.Add(MakeShared<FJsonValueObject>(PJson));
	}

	// Vector parameters
	TArray<FMaterialParameterInfo> VectorInfos;
	TArray<FGuid> VectorGuids;
	MatInterface->GetAllVectorParameterInfo(VectorInfos, VectorGuids);
	for (int32 i = 0; i < VectorInfos.Num(); ++i)
	{
		FLinearColor Value;
		MatInterface->GetVectorParameterValue(VectorInfos[i], Value);

		auto PJson = MakeShared<FJsonObject>();
		PJson->SetStringField(TEXT("name"), VectorInfos[i].Name.ToString());
		auto ColorJson = MakeShared<FJsonObject>();
		ColorJson->SetNumberField(TEXT("R"), Value.R);
		ColorJson->SetNumberField(TEXT("G"), Value.G);
		ColorJson->SetNumberField(TEXT("B"), Value.B);
		ColorJson->SetNumberField(TEXT("A"), Value.A);
		PJson->SetObjectField(TEXT("value"), ColorJson);
		PJson->SetStringField(TEXT("type"), TEXT("vector"));
		VectorArray.Add(MakeShared<FJsonValueObject>(PJson));
	}

	// Texture parameters
	TArray<FMaterialParameterInfo> TextureInfos;
	TArray<FGuid> TextureGuids;
	MatInterface->GetAllTextureParameterInfo(TextureInfos, TextureGuids);
	for (int32 i = 0; i < TextureInfos.Num(); ++i)
	{
		UTexture* Tex = nullptr;
		MatInterface->GetTextureParameterValue(TextureInfos[i], Tex);

		auto PJson = MakeShared<FJsonObject>();
		PJson->SetStringField(TEXT("name"), TextureInfos[i].Name.ToString());
		PJson->SetStringField(TEXT("value"), Tex ? Tex->GetPathName() : TEXT("None"));
		PJson->SetStringField(TEXT("type"), TEXT("texture"));
		TextureArray.Add(MakeShared<FJsonValueObject>(PJson));
	}

	// Static switch parameters
	TArray<FMaterialParameterInfo> SwitchInfos;
	TArray<FGuid> SwitchGuids;
	MatInterface->GetAllStaticSwitchParameterInfo(SwitchInfos, SwitchGuids);
	for (int32 i = 0; i < SwitchInfos.Num(); ++i)
	{
		bool Value = false;
		FGuid OutGuid;
		MatInterface->GetStaticSwitchParameterValue(SwitchInfos[i], Value, OutGuid);

		auto PJson = MakeShared<FJsonObject>();
		PJson->SetStringField(TEXT("name"), SwitchInfos[i].Name.ToString());
		PJson->SetBoolField(TEXT("value"), Value);
		PJson->SetStringField(TEXT("type"), TEXT("static_switch"));
		SwitchArray.Add(MakeShared<FJsonValueObject>(PJson));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetArrayField(TEXT("scalar_parameters"), ScalarArray);
	ResultJson->SetArrayField(TEXT("vector_parameters"), VectorArray);
	ResultJson->SetArrayField(TEXT("texture_parameters"), TextureArray);
	ResultJson->SetArrayField(TEXT("static_switch_parameters"), SwitchArray);
	ResultJson->SetNumberField(TEXT("total_parameters"),
		ScalarArray.Num() + VectorArray.Num() + TextureArray.Num() + SwitchArray.Num());

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: set_instance_parameter
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::SetInstanceParameter(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString ParamName = Params->GetStringField(TEXT("parameter_name"));

	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	UMaterialInstanceConstant* MIC = LoadedAsset ? Cast<UMaterialInstanceConstant>(LoadedAsset) : nullptr;
	if (!MIC)
	{
		if (Cast<UMaterial>(LoadedAsset))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("'%s' is a Material, not a Material Instance. Use 'set_expression_property' to modify expression defaults on base materials, or create a Material Instance with 'create_material_instance'."),
				*AssetPath));
		}
		if (Cast<UMaterialFunction>(LoadedAsset))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("'%s' is a Material Function, not a Material Instance. Use 'set_expression_property' to modify expression defaults."),
				*AssetPath));
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to load material instance at '%s' (loaded as %s)"),
			*AssetPath, LoadedAsset ? *LoadedAsset->GetClass()->GetName() : TEXT("null")));
	}

	MIC->Modify();
	FString SetType;
	FString SetValue;

	FMaterialParameterInfo ParamInfo(*ParamName);

	if (Params->HasField(TEXT("scalar_value")))
	{
		float Val = static_cast<float>(Params->GetNumberField(TEXT("scalar_value")));
		MIC->SetScalarParameterValueEditorOnly(ParamInfo, Val);
		SetType = TEXT("scalar");
		SetValue = FString::SanitizeFloat(Val);
	}
	else if (Params->HasField(TEXT("vector_value")))
	{
		const TSharedPtr<FJsonObject>* ColorObj = nullptr;
		if (Params->TryGetObjectField(TEXT("vector_value"), ColorObj))
		{
			FLinearColor Color;
			Color.R = (*ColorObj)->HasField(TEXT("R")) ? static_cast<float>((*ColorObj)->GetNumberField(TEXT("R"))) : 0.f;
			Color.G = (*ColorObj)->HasField(TEXT("G")) ? static_cast<float>((*ColorObj)->GetNumberField(TEXT("G"))) : 0.f;
			Color.B = (*ColorObj)->HasField(TEXT("B")) ? static_cast<float>((*ColorObj)->GetNumberField(TEXT("B"))) : 0.f;
			Color.A = (*ColorObj)->HasField(TEXT("A")) ? static_cast<float>((*ColorObj)->GetNumberField(TEXT("A"))) : 1.f;
			MIC->SetVectorParameterValueEditorOnly(ParamInfo, Color);
			SetType = TEXT("vector");
			SetValue = FString::Printf(TEXT("(%.3f, %.3f, %.3f, %.3f)"), Color.R, Color.G, Color.B, Color.A);
		}
	}
	else if (Params->HasField(TEXT("texture_value")))
	{
		FString TexPath = Params->GetStringField(TEXT("texture_value"));
		UTexture* Tex = Cast<UTexture>(UEditorAssetLibrary::LoadAsset(TexPath));
		if (!Tex)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load texture at '%s'"), *TexPath));
		}
		MIC->SetTextureParameterValueEditorOnly(ParamInfo, Tex);
		SetType = TEXT("texture");
		SetValue = TexPath;
	}
	else if (Params->HasField(TEXT("switch_value")))
	{
		bool Val = Params->GetBoolField(TEXT("switch_value"));
		MIC->SetStaticSwitchParameterValueEditorOnly(ParamInfo, Val);
		SetType = TEXT("static_switch");
		SetValue = Val ? TEXT("true") : TEXT("false");
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("Must provide one of: scalar_value, vector_value, texture_value, switch_value"));
	}

	// Static switches need UpdateMaterialInstance to compile the permutation and persist correctly.
	// For all other param types MarkPackageDirty is sufficient.
	if (SetType == TEXT("static_switch"))
	{
		UMaterialEditingLibrary::UpdateMaterialInstance(MIC);
	}
	else
	{
		MIC->MarkPackageDirty();
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetStringField(TEXT("parameter_name"), ParamName);
	ResultJson->SetStringField(TEXT("type"), SetType);
	ResultJson->SetStringField(TEXT("value"), SetValue);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: recompile_material
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::RecompileMaterial(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	UMaterialInterface* MatInterface = LoadedAsset ? Cast<UMaterialInterface>(LoadedAsset) : nullptr;
	if (!MatInterface)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load material at '%s'"), *AssetPath));
	}

	// For base materials, trigger full recompile
	UMaterial* BaseMat = MatInterface->GetMaterial();
	if (BaseMat)
	{
		UMaterialEditingLibrary::RecompileMaterial(BaseMat);
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetStringField(TEXT("status"), TEXT("recompiled"));

	// Optional inline compilation stats
	bool bIncludeStats = false;
	if (Params->HasField(TEXT("include_stats")))
	{
		bIncludeStats = Params->GetBoolField(TEXT("include_stats"));
	}

	if (bIncludeStats && BaseMat)
	{
		// GetStatistics internally calls FinishCompilation(), blocking until shaders are done
		FMaterialStatistics Stats = UMaterialEditingLibrary::GetStatistics(MatInterface);
		ResultJson->SetNumberField(TEXT("num_pixel_shader_instructions"), Stats.NumPixelShaderInstructions);
		ResultJson->SetNumberField(TEXT("num_vertex_shader_instructions"), Stats.NumVertexShaderInstructions);

		const EShaderPlatform ShaderPlatform = GShaderPlatformForFeatureLevel[GMaxRHIFeatureLevel];
		FMaterialResource* MatResource = BaseMat->GetMaterialResource(ShaderPlatform);
		bool bIsCompiled = false;
		if (MatResource)
		{
			bIsCompiled = MatResource->IsGameThreadShaderMapComplete();

			const TArray<FString>& Errors = MatResource->GetCompileErrors();
			if (Errors.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> ErrorsArray;
				for (const FString& Err : Errors)
				{
					ErrorsArray.Add(MakeShared<FJsonValueString>(Err));
				}
				ResultJson->SetArrayField(TEXT("compile_errors"), ErrorsArray);
			}
		}

		// Override is_compiled if we got valid instruction counts — shader map may not be loaded in memory
		// but stats are cached from a previous compilation
		if (!bIsCompiled && (Stats.NumVertexShaderInstructions > 0 || Stats.NumPixelShaderInstructions > 0))
		{
			bIsCompiled = true;
		}
		ResultJson->SetBoolField(TEXT("is_compiled"), bIsCompiled);
	}

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: duplicate_material
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::DuplicateMaterial(const TSharedPtr<FJsonObject>& Params)
{
	FString SourcePath = Params->GetStringField(TEXT("source_path"));
	FString DestPath = Params->GetStringField(TEXT("dest_path"));

	// Check source exists
	UObject* SourceObj = UEditorAssetLibrary::LoadAsset(SourcePath);
	if (!SourceObj)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Source material not found at '%s'"), *SourcePath));
	}

	// Check dest doesn't exist
	if (UEditorAssetLibrary::LoadAsset(DestPath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at '%s'"), *DestPath));
	}

	UObject* DuplicatedObj = UEditorAssetLibrary::DuplicateAsset(SourcePath, DestPath);
	if (!DuplicatedObj)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to duplicate '%s' to '%s'"), *SourcePath, *DestPath));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("source_path"), SourcePath);
	ResultJson->SetStringField(TEXT("dest_path"), DestPath);
	ResultJson->SetStringField(TEXT("asset_class"), SourceObj->GetClass()->GetName());

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: get_compilation_stats
// Params: { "asset_path": "..." }
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::GetCompilationStats(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	UMaterialInterface* MatInterface = LoadedAsset ? Cast<UMaterialInterface>(LoadedAsset) : nullptr;
	if (!MatInterface)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load material at '%s'"), *AssetPath));
	}

	UMaterial* BaseMat = MatInterface->GetMaterial();
	if (!BaseMat)
	{
		return FMonolithActionResult::Error(TEXT("Could not resolve base material"));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);

	// Get material resource for the current shader platform
	const EShaderPlatform ShaderPlatform = GShaderPlatformForFeatureLevel[GMaxRHIFeatureLevel];
	FMaterialResource* MatResource = BaseMat->GetMaterialResource(ShaderPlatform);
	if (MatResource)
	{
		bool bIsCompiled = MatResource->IsGameThreadShaderMapComplete();
		ResultJson->SetBoolField(TEXT("is_compiled"), bIsCompiled);

#if WITH_EDITOR
		// Sampler count
		int32 SamplerCount = MatResource->GetSamplerUsage();
		ResultJson->SetNumberField(TEXT("num_samplers"), SamplerCount);

		// Estimated texture samples (VS + PS)
		uint32 VSSamples = 0, PSSamples = 0;
		MatResource->GetEstimatedNumTextureSamples(VSSamples, PSSamples);
		ResultJson->SetNumberField(TEXT("estimated_vs_texture_samples"), static_cast<double>(VSSamples));
		ResultJson->SetNumberField(TEXT("estimated_ps_texture_samples"), static_cast<double>(PSSamples));

		// User interpolator usage
		uint32 NumUsedUVScalars = 0, NumUsedCustomInterpolatorScalars = 0;
		MatResource->GetUserInterpolatorUsage(NumUsedUVScalars, NumUsedCustomInterpolatorScalars);
		ResultJson->SetNumberField(TEXT("used_uv_scalars"), static_cast<double>(NumUsedUVScalars));
		ResultJson->SetNumberField(TEXT("used_custom_interpolator_scalars"), static_cast<double>(NumUsedCustomInterpolatorScalars));
#endif

		// Compile errors
		const TArray<FString>& Errors = MatResource->GetCompileErrors();
		if (Errors.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> ErrorsArray;
			for (const FString& Err : Errors)
			{
				ErrorsArray.Add(MakeShared<FJsonValueString>(Err));
			}
			ResultJson->SetArrayField(TEXT("compile_errors"), ErrorsArray);
		}
	}
	else
	{
		ResultJson->SetBoolField(TEXT("is_compiled"), false);
		ResultJson->SetStringField(TEXT("note"), TEXT("Material resource not available - try recompile_material first"));
	}

	// Instruction counts via UMaterialEditingLibrary::GetStatistics (uses FMaterialStatsUtils internally)
	FMaterialStatistics Stats = UMaterialEditingLibrary::GetStatistics(MatInterface);
	ResultJson->SetNumberField(TEXT("num_vertex_shader_instructions"), Stats.NumVertexShaderInstructions);
	ResultJson->SetNumberField(TEXT("num_pixel_shader_instructions"), Stats.NumPixelShaderInstructions);

	// Override is_compiled if we got valid instruction counts — shader map may not be loaded in memory
	// but stats are cached from a previous compilation. Without this, is_compiled=false is misleading.
	if (!ResultJson->GetBoolField(TEXT("is_compiled")) && (Stats.NumVertexShaderInstructions > 0 || Stats.NumPixelShaderInstructions > 0))
	{
		ResultJson->SetBoolField(TEXT("is_compiled"), true);
	}

	// Material properties (always available from the base material)
	ResultJson->SetStringField(TEXT("blend_mode"),
		BaseMat->BlendMode == BLEND_Opaque ? TEXT("Opaque") :
		BaseMat->BlendMode == BLEND_Masked ? TEXT("Masked") :
		BaseMat->BlendMode == BLEND_Translucent ? TEXT("Translucent") :
		BaseMat->BlendMode == BLEND_Additive ? TEXT("Additive") :
		BaseMat->BlendMode == BLEND_AlphaComposite ? TEXT("AlphaComposite") :
		BaseMat->BlendMode == BLEND_Modulate ? TEXT("Modulate") : TEXT("Other"));
	ResultJson->SetBoolField(TEXT("two_sided"), BaseMat->TwoSided);
	ResultJson->SetNumberField(TEXT("expression_count"), BaseMat->GetExpressions().Num());

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: set_expression_property
// Params: { "asset_path": "...", "expression_name": "...", "property_name": "...", "value": "..." }
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::SetExpressionProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString ExprName = Params->GetStringField(TEXT("expression_name"));
	FString PropName = Params->GetStringField(TEXT("property_name"));

	// Extract value as string regardless of JSON type — GetStringField returns ""
	// for JSON numbers, which then gets Atof'd to 0.0 (Bug #6).
	FString ValueStr;
	{
		const TSharedPtr<FJsonValue>* ValueField = Params->Values.Find(TEXT("value"));
		if (ValueField && ValueField->IsValid())
		{
			switch ((*ValueField)->Type)
			{
				case EJson::Number:
					ValueStr = FString::SanitizeFloat((*ValueField)->AsNumber());
					break;
				case EJson::Boolean:
					ValueStr = (*ValueField)->AsBool() ? TEXT("true") : TEXT("false");
					break;
				case EJson::String:
					ValueStr = (*ValueField)->AsString();
					break;
				default:
					break;
			}
		}
	}

	// Try UMaterial first, then fall back to UMaterialFunction
	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	UMaterial* Mat = Cast<UMaterial>(LoadedAsset);
	UMaterialFunction* MatFunc = Mat ? nullptr : Cast<UMaterialFunction>(LoadedAsset);

	if (!Mat && !MatFunc)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to load material or material function at '%s'"), *AssetPath));
	}

	TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions =
		Mat ? Mat->GetExpressions() : MatFunc->GetExpressions();

	// Find expression
	UMaterialExpression* TargetExpr = nullptr;
	for (const TObjectPtr<UMaterialExpression>& Expr : Expressions)
	{
		if (Expr && Expr->GetName() == ExprName)
		{
			TargetExpr = Expr;
			break;
		}
	}

	if (!TargetExpr)
	{
		TArray<FString> AvailableNames;
		for (const TObjectPtr<UMaterialExpression>& E : Expressions)
		{
			if (E) AvailableNames.Add(E->GetName());
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Expression '%s' not found. Available: %s"),
			*ExprName, *FString::Join(AvailableNames, TEXT(", "))));
	}

	FProperty* Prop = TargetExpr->GetClass()->FindPropertyByName(*PropName);
	if (!Prop)
	{
		TArray<FString> AvailableProps;
		for (TFieldIterator<FProperty> PropIt(TargetExpr->GetClass()); PropIt; ++PropIt)
		{
			if (*PropIt && !(*PropIt)->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient))
			{
				AvailableProps.Add((*PropIt)->GetName());
			}
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Property '%s' not found on expression '%s' (%s). Available: %s"),
			*PropName, *ExprName, *TargetExpr->GetClass()->GetName(),
			*FString::Join(AvailableProps, TEXT(", "))));
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("SetExpressionProperty")));
	TargetExpr->Modify();

	// Handle numeric types directly, everything else via ImportText
	bool bSuccess = false;

	if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
	{
		float Val = FCString::Atof(*ValueStr);
		FloatProp->SetPropertyValue_InContainer(TargetExpr, Val);
		bSuccess = true;
	}
	else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
	{
		double Val = FCString::Atod(*ValueStr);
		DoubleProp->SetPropertyValue_InContainer(TargetExpr, Val);
		bSuccess = true;
	}
	else if (FIntProperty* IntProp = CastField<FIntProperty>(Prop))
	{
		int32 Val = FCString::Atoi(*ValueStr);
		IntProp->SetPropertyValue_InContainer(TargetExpr, Val);
		bSuccess = true;
	}
	else if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
	{
		bool Val = ValueStr.Equals(TEXT("true"), ESearchCase::IgnoreCase) || ValueStr == TEXT("1");
		BoolProp->SetPropertyValue_InContainer(TargetExpr, Val);
		bSuccess = true;
	}
	else if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
	{
		NameProp->SetPropertyValue_InContainer(TargetExpr, FName(*ValueStr));
		bSuccess = true;
	}
	else if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
	{
		StrProp->SetPropertyValue_InContainer(TargetExpr, ValueStr);
		bSuccess = true;
	}
	else
	{
		// Generic ImportText for structs, enums, object references, etc.
		void* PropAddr = Prop->ContainerPtrToValuePtr<void>(TargetExpr);
		bSuccess = Prop->ImportText_Direct(*ValueStr, PropAddr, TargetExpr, PPF_None) != nullptr;
	}

	if (bSuccess)
	{
		// Fire PostEditChangeProperty on the expression first (matches editor behavior).
		// This triggers AutoSetSampleType() for texture expressions.
		FPropertyChangedEvent ExprChangeEvent(Prop);
		TargetExpr->PostEditChangeProperty(ExprChangeEvent);

		// Pass the actual property so PostEditChangePropertyInternal calls
		// MaterialGraph->RebuildGraph() and the editor display updates correctly.
		FPropertyChangedEvent ChangeEvent(Prop);
		if (Mat)
		{
			Mat->PreEditChange(Prop);
			Mat->PostEditChangeProperty(ChangeEvent);
		}
		else if (MatFunc)
		{
			MatFunc->PreEditChange(Prop);
			MatFunc->PostEditChangeProperty(ChangeEvent);
		}
	}

	GEditor->EndTransaction();

	if (!bSuccess)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to set property '%s' to '%s' on expression '%s'"), *PropName, *ValueStr, *ExprName));
	}

	// VT compatibility check — warn on texture/sampler mismatch
	TArray<FString> Warnings;
	if (PropName.Equals(TEXT("Texture"), ESearchCase::IgnoreCase))
	{
		if (UMaterialExpressionTextureBase* TexExpr = Cast<UMaterialExpressionTextureBase>(TargetExpr))
		{
			UTexture* AssignedTex = TexExpr->Texture;
			if (AssignedTex)
			{
				bool bSamplerIsVT = IsVirtualSamplerType(TexExpr->SamplerType);
				bool bTextureIsVT = AssignedTex->VirtualTextureStreaming;

				if (bTextureIsVT && !bSamplerIsVT)
				{
					Warnings.Add(FString::Printf(
						TEXT("Texture '%s' has VirtualTextureStreaming=true but sampler type is non-VT. This will cause compilation errors. Disable VirtualTextureStreaming on the texture or change sampler type."),
						*AssignedTex->GetName()));
				}
				else if (!bTextureIsVT && bSamplerIsVT)
				{
					Warnings.Add(FString::Printf(
						TEXT("Texture '%s' has VirtualTextureStreaming=false but sampler type is VT. This will cause compilation errors."),
						*AssignedTex->GetName()));
				}
			}
		}
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetStringField(TEXT("expression_name"), ExprName);
	ResultJson->SetStringField(TEXT("property_name"), PropName);
	ResultJson->SetStringField(TEXT("value"), ValueStr);

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarningArray;
		for (const FString& W : Warnings)
		{
			WarningArray.Add(MakeShared<FJsonValueString>(W));
		}
		ResultJson->SetArrayField(TEXT("warnings"), WarningArray);
	}

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: connect_expressions
// Params: { "asset_path", "from_expression", "from_output"?, "to_expression"?, "to_input"?, "to_property"? }
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::ConnectExpressions(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString FromExprName = Params->GetStringField(TEXT("from_expression"));
	FString FromOutput = Params->HasField(TEXT("from_output")) ? Params->GetStringField(TEXT("from_output"))
	                   : Params->HasField(TEXT("from_pin")) ? Params->GetStringField(TEXT("from_pin")) : TEXT("");
	FString ToExprName = Params->HasField(TEXT("to_expression")) ? Params->GetStringField(TEXT("to_expression")) : TEXT("");
	FString ToInput = Params->HasField(TEXT("to_input")) ? Params->GetStringField(TEXT("to_input"))
	                : Params->HasField(TEXT("to_pin")) ? Params->GetStringField(TEXT("to_pin")) : TEXT("");
	ToInput = NormalizeInputPinName(ToInput);
	FString ToProperty = Params->HasField(TEXT("to_property")) ? Params->GetStringField(TEXT("to_property")) : TEXT("");

	if (ToExprName.IsEmpty() && ToProperty.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Must provide either to_expression or to_property"));
	}

	UMaterial* Mat = LoadBaseMaterial(AssetPath);
	if (!Mat)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load base material at '%s'"), *AssetPath));
	}

	// Find source expression (and optionally target expression)
	UMaterialExpression* FromExpr = nullptr;
	UMaterialExpression* ToExpr = nullptr;
	for (const TObjectPtr<UMaterialExpression>& Expr : Mat->GetExpressions())
	{
		if (Expr)
		{
			if (Expr->GetName() == FromExprName) FromExpr = Expr;
			if (!ToExprName.IsEmpty() && Expr->GetName() == ToExprName) ToExpr = Expr;
		}
	}

	if (!FromExpr)
	{
		TArray<FString> AvailableNames;
		for (const TObjectPtr<UMaterialExpression>& Expr : Mat->GetExpressions())
		{
			if (Expr) AvailableNames.Add(Expr->GetName());
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Source expression '%s' not found. Available: %s"),
			*FromExprName, *FString::Join(AvailableNames, TEXT(", "))));
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("ConnectExpressions")));
	Mat->Modify();

	bool bConnected = false;
	FString ConnectionDesc;

	TArray<FString> Warnings;

	if (!ToProperty.IsEmpty())
	{
		// Blend mode validation: warn about irrelevant material output connections
		EMaterialProperty MatProp = ParseMaterialProperty(ToProperty);
		if (MatProp != MP_MAX)
		{
			EBlendMode BM = Mat->BlendMode;
			if (MatProp == MP_Opacity && (BM == BLEND_Opaque || BM == BLEND_Masked)
				&& !IsSubsurfaceShadingModel(Mat->GetShadingModels())
				&& !Mat->GetShadingModels().HasShadingModel(MSM_SingleLayerWater))
				Warnings.Add(TEXT("Opacity has no effect on Opaque/Masked materials. Set blend mode to Translucent/Additive first."));
			if (MatProp == MP_OpacityMask && BM != BLEND_Masked)
				Warnings.Add(TEXT("OpacityMask only affects Masked blend mode."));
			if (MatProp == MP_Refraction && BM == BLEND_Opaque)
				Warnings.Add(TEXT("Refraction has no effect on Opaque materials."));
		}

		// Connect to material output property
		bConnected = UMaterialEditingLibrary::ConnectMaterialProperty(FromExpr, FromOutput, MatProp);
		ConnectionDesc = FString::Printf(TEXT("%s -> %s"), *FromExprName, *ToProperty);
	}
	else if (ToExpr)
	{
		// Connect expression to expression
		bConnected = UMaterialEditingLibrary::ConnectMaterialExpressions(FromExpr, FromOutput, ToExpr, ToInput);
		ConnectionDesc = FString::Printf(TEXT("%s -> %s"), *FromExprName, *ToExprName);
	}
	else
	{
		GEditor->EndTransaction();
		TArray<FString> AvailableNames;
		for (const TObjectPtr<UMaterialExpression>& Expr : Mat->GetExpressions())
		{
			if (Expr) AvailableNames.Add(Expr->GetName());
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Target expression '%s' not found. Available: %s"),
			*ToExprName, *FString::Join(AvailableNames, TEXT(", "))));
	}

	Mat->PreEditChange(nullptr);
	Mat->PostEditChange();
	GEditor->EndTransaction();

	if (!bConnected)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Connection failed: %s"), *ConnectionDesc));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetStringField(TEXT("connection"), ConnectionDesc);
	ResultJson->SetBoolField(TEXT("connected"), true);
	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarnArr;
		for (const FString& W : Warnings) WarnArr.Add(MakeShared<FJsonValueString>(W));
		ResultJson->SetArrayField(TEXT("warnings"), WarnArr);
	}

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Wave 3 — Free Wins
// ============================================================================

// ============================================================================
// Action: auto_layout
// Params: { "asset_path": "..." }
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::AutoLayout(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	if (!LoadedAsset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load asset at '%s'"), *AssetPath));
	}

	FString Formatter = TEXT("auto");
	if (Params->HasField(TEXT("formatter")))
	{
		Formatter = Params->GetStringField(TEXT("formatter"));
	}

	// Validate formatter
	if (Formatter != TEXT("auto") && Formatter != TEXT("blueprint_assist") && Formatter != TEXT("monolith"))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown formatter '%s'. Valid: 'auto', 'blueprint_assist', 'monolith'"), *Formatter));
	}

	// --- BA formatter path (UMaterial only — MaterialFunctions have no MaterialGraph) ---
	if (Formatter == TEXT("auto") || Formatter == TEXT("blueprint_assist"))
	{
		bool bExplicitBA = (Formatter == TEXT("blueprint_assist"));

		UMaterial* Mat = Cast<UMaterial>(LoadedAsset);
		UEdGraph* MaterialGraph = (Mat && Mat->MaterialGraph) ? Mat->MaterialGraph : nullptr;

		bool bBAAvailable = MaterialGraph
			&& IMonolithGraphFormatter::IsAvailable()
			&& IMonolithGraphFormatter::Get().SupportsGraph(MaterialGraph);

		if (bBAAvailable)
		{
			int32 NodesFormatted = 0;
			FString ErrorMessage;
			if (IMonolithGraphFormatter::Get().FormatGraph(MaterialGraph, NodesFormatted, ErrorMessage))
			{
				auto Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("asset_path"), AssetPath);
				Result->SetStringField(TEXT("type"), TEXT("Material"));
				Result->SetStringField(TEXT("formatter_used"), TEXT("blueprint_assist"));
				Result->SetNumberField(TEXT("nodes_formatted"), NodesFormatted);
				Result->SetBoolField(TEXT("positions_changed"), true);

				FMonolithFormatterInfo Info = IMonolithGraphFormatter::Get().GetFormatterInfo(MaterialGraph);
				Result->SetStringField(TEXT("formatter_type"), Info.FormatterType);
				Result->SetStringField(TEXT("graph_class"), Info.GraphClassName);
				return FMonolithActionResult::Success(Result);
			}

			if (bExplicitBA)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Blueprint Assist formatter failed: %s"), *ErrorMessage));
			}
			// auto mode: fall through to built-in
		}
		else if (bExplicitBA)
		{
			if (!MaterialGraph)
			{
				return FMonolithActionResult::Error(
					TEXT("Blueprint Assist formatting is only supported for UMaterial assets (not MaterialFunctions). "
						 "Use formatter='monolith' for MaterialFunction layout."));
			}
			return FMonolithActionResult::Error(
				TEXT("Blueprint Assist formatter is not available. "
					 "Install Blueprint Assist and restart the editor, or ensure the material is open in an editor tab."));
		}
	}

	// --- Built-in UMaterialEditingLibrary path ---

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);

	// Try as UMaterialFunction first (covers MaterialLayer, MaterialLayerBlend too)
	UMaterialFunction* MatFunc = Cast<UMaterialFunction>(LoadedAsset);
	if (MatFunc)
	{
		int32 ExprCount = MatFunc->GetExpressions().Num();
		UMaterialEditingLibrary::LayoutMaterialFunctionExpressions(MatFunc);
		ResultJson->SetStringField(TEXT("type"), TEXT("MaterialFunction"));
		ResultJson->SetNumberField(TEXT("expression_count"), ExprCount);
		ResultJson->SetBoolField(TEXT("positions_changed"), true);
		ResultJson->SetStringField(TEXT("formatter_used"), TEXT("monolith"));
		return FMonolithActionResult::Success(ResultJson);
	}

	// Try as UMaterial
	UMaterial* Mat = Cast<UMaterial>(LoadedAsset);
	if (Mat)
	{
		int32 ExprCount = Mat->GetExpressions().Num();
		UMaterialEditingLibrary::LayoutMaterialExpressions(Mat);
		ResultJson->SetStringField(TEXT("type"), TEXT("Material"));
		ResultJson->SetNumberField(TEXT("expression_count"), ExprCount);
		ResultJson->SetBoolField(TEXT("positions_changed"), true);
		ResultJson->SetStringField(TEXT("formatter_used"), TEXT("monolith"));
		return FMonolithActionResult::Success(ResultJson);
	}

	return FMonolithActionResult::Error(FString::Printf(TEXT("Asset '%s' is not a Material or MaterialFunction"), *AssetPath));
}

// ============================================================================
// Action: duplicate_expression
// Params: { "asset_path", "expression_name", "offset_x"?, "offset_y"? }
// Note: Output connections are NOT duplicated (input connections are preserved) by UMaterialEditingLibrary::DuplicateMaterialExpression
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::DuplicateExpression(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString ExprName = Params->GetStringField(TEXT("expression_name"));
	int32 OffsetX = Params->HasField(TEXT("offset_x")) ? static_cast<int32>(Params->GetNumberField(TEXT("offset_x"))) : 50;
	int32 OffsetY = Params->HasField(TEXT("offset_y")) ? static_cast<int32>(Params->GetNumberField(TEXT("offset_y"))) : 50;

	UMaterial* Mat = LoadBaseMaterial(AssetPath);
	if (!Mat)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load base material at '%s'"), *AssetPath));
	}

	// Find source expression
	UMaterialExpression* SourceExpr = nullptr;
	for (const TObjectPtr<UMaterialExpression>& Expr : Mat->GetExpressions())
	{
		if (Expr && Expr->GetName() == ExprName)
		{
			SourceExpr = Expr;
			break;
		}
	}

	if (!SourceExpr)
	{
		TArray<FString> AvailableNames;
		for (const TObjectPtr<UMaterialExpression>& Expr : Mat->GetExpressions())
		{
			if (Expr) AvailableNames.Add(Expr->GetName());
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Expression '%s' not found. Available: %s"),
			*ExprName, *FString::Join(AvailableNames, TEXT(", "))));
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("DuplicateExpression")));
	Mat->Modify();

	UMaterialExpression* NewExpr = UMaterialEditingLibrary::DuplicateMaterialExpression(Mat, nullptr, SourceExpr);
	if (!NewExpr)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to duplicate expression '%s'"), *ExprName));
	}

	// Apply position offset
	NewExpr->MaterialExpressionEditorX = SourceExpr->MaterialExpressionEditorX + OffsetX;
	NewExpr->MaterialExpressionEditorY = SourceExpr->MaterialExpressionEditorY + OffsetY;

	Mat->PreEditChange(nullptr);
	Mat->PostEditChange();
	GEditor->EndTransaction();

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetStringField(TEXT("source_name"), ExprName);
	ResultJson->SetStringField(TEXT("new_name"), NewExpr->GetName());
	ResultJson->SetStringField(TEXT("class"), NewExpr->GetClass()->GetName());
	ResultJson->SetNumberField(TEXT("pos_x"), NewExpr->MaterialExpressionEditorX);
	ResultJson->SetNumberField(TEXT("pos_y"), NewExpr->MaterialExpressionEditorY);
	ResultJson->SetStringField(TEXT("note"), TEXT("Output connections are NOT duplicated (input connections are preserved) — only the expression node itself is copied"));

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: list_expression_classes
// Params: { "filter"?, "category"? }
// Cached in a static TMap — first call creates temp instances (~1-2s), subsequent calls instant.
// ============================================================================

namespace
{
	struct FExpressionClassInfo
	{
		FString ClassName;
		FString DisplayName;
		FString MenuCategories;
		bool bIsParameter;
		int32 InputCount;
		int32 OutputCount;
	};

	static TArray<FExpressionClassInfo> GCachedExpressionClasses;
	static bool GExpressionClassesCached = false;

	void EnsureExpressionClassCache()
	{
		if (GExpressionClassesCached)
		{
			return;
		}

		TArray<UClass*> DerivedClasses;
		GetDerivedClasses(UMaterialExpression::StaticClass(), DerivedClasses, true);

		for (UClass* Class : DerivedClasses)
		{
			if (!Class || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_Hidden))
			{
				continue;
			}

			// Create temporary instance to read pin info
			UMaterialExpression* TempExpr = NewObject<UMaterialExpression>(GetTransientPackage(), Class, NAME_None, RF_Transient);
			if (!TempExpr)
			{
				continue;
			}

			FExpressionClassInfo Info;
			Info.ClassName = Class->GetName();
			Info.DisplayName = Class->GetDisplayNameText().ToString();
			Info.bIsParameter = Class->IsChildOf(UMaterialExpressionParameter::StaticClass())
				|| Class->IsChildOf(UMaterialExpressionTextureSampleParameter::StaticClass());

			// Read keywords from the temp instance
			Info.MenuCategories = TempExpr->GetKeywords().ToString();

			// Count inputs
			Info.InputCount = 0;
			for (int32 i = 0; ; ++i)
			{
				if (!TempExpr->GetInput(i))
				{
					break;
				}
				Info.InputCount++;
			}

			// Count outputs
			Info.OutputCount = TempExpr->Outputs.Num();

			GCachedExpressionClasses.Add(MoveTemp(Info));

			TempExpr->MarkAsGarbage();
		}

		// Sort alphabetically
		GCachedExpressionClasses.Sort([](const FExpressionClassInfo& A, const FExpressionClassInfo& B)
		{
			return A.ClassName < B.ClassName;
		});

		GExpressionClassesCached = true;
	}
}

FMonolithActionResult FMonolithMaterialActions::ListExpressionClasses(const TSharedPtr<FJsonObject>& Params)
{
	FString Filter = Params->HasField(TEXT("filter")) ? Params->GetStringField(TEXT("filter")) : TEXT("");
	FString Category = Params->HasField(TEXT("category")) ? Params->GetStringField(TEXT("category")) : TEXT("");

	EnsureExpressionClassCache();

	TArray<TSharedPtr<FJsonValue>> ClassesArray;
	for (const FExpressionClassInfo& Info : GCachedExpressionClasses)
	{
		// Apply filter
		if (!Filter.IsEmpty() && !Info.ClassName.Contains(Filter, ESearchCase::IgnoreCase)
			&& !Info.DisplayName.Contains(Filter, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (!Category.IsEmpty() && !Info.MenuCategories.Contains(Category, ESearchCase::IgnoreCase))
		{
			continue;
		}

		auto ClassJson = MakeShared<FJsonObject>();
		ClassJson->SetStringField(TEXT("class_name"), Info.ClassName);
		ClassJson->SetStringField(TEXT("display_name"), Info.DisplayName);
		ClassJson->SetBoolField(TEXT("is_parameter"), Info.bIsParameter);
		ClassJson->SetNumberField(TEXT("input_count"), Info.InputCount);
		ClassJson->SetNumberField(TEXT("output_count"), Info.OutputCount);
		if (!Info.MenuCategories.IsEmpty())
		{
			ClassJson->SetStringField(TEXT("keywords"), Info.MenuCategories);
		}
		ClassesArray.Add(MakeShared<FJsonValueObject>(ClassJson));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetNumberField(TEXT("total_classes"), GCachedExpressionClasses.Num());
	ResultJson->SetNumberField(TEXT("filtered_count"), ClassesArray.Num());
	ResultJson->SetArrayField(TEXT("classes"), ClassesArray);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: get_expression_connections
// Params: { "asset_path", "expression_name" }
// Returns inputs (what feeds into this node) and output consumers (what this node feeds)
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::GetExpressionConnections(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString ExprName = Params->GetStringField(TEXT("expression_name"));

	UMaterial* Mat = LoadBaseMaterial(AssetPath);
	if (!Mat)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load base material at '%s'"), *AssetPath));
	}

	// Find target expression
	UMaterialExpression* TargetExpr = nullptr;
	TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = Mat->GetExpressions();
	for (const TObjectPtr<UMaterialExpression>& Expr : Expressions)
	{
		if (Expr && Expr->GetName() == ExprName)
		{
			TargetExpr = Expr;
			break;
		}
	}

	if (!TargetExpr)
	{
		TArray<FString> AvailableNames;
		for (const TObjectPtr<UMaterialExpression>& Expr : Expressions)
		{
			if (Expr) AvailableNames.Add(Expr->GetName());
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Expression '%s' not found. Available: %s"),
			*ExprName, *FString::Join(AvailableNames, TEXT(", "))));
	}

	// --- Inputs: what feeds into this expression ---
	TArray<TSharedPtr<FJsonValue>> InputsArray;
	for (int32 i = 0; ; ++i)
	{
		FExpressionInput* Input = TargetExpr->GetInput(i);
		if (!Input)
		{
			break;
		}
		auto InputJson = MakeShared<FJsonObject>();
		InputJson->SetNumberField(TEXT("index"), i);
		InputJson->SetStringField(TEXT("name"), TargetExpr->GetInputName(i).ToString());
		InputJson->SetBoolField(TEXT("connected"), Input->Expression != nullptr);
		if (Input->Expression)
		{
			InputJson->SetStringField(TEXT("connected_to"), Input->Expression->GetName());
			InputJson->SetNumberField(TEXT("output_index"), Input->OutputIndex);
			// Include output name if available
			const TArray<FExpressionOutput>& SourceOutputs = Input->Expression->Outputs;
			if (SourceOutputs.IsValidIndex(Input->OutputIndex))
			{
				InputJson->SetStringField(TEXT("output_name"), SourceOutputs[Input->OutputIndex].OutputName.ToString());
			}
		}
		InputsArray.Add(MakeShared<FJsonValueObject>(InputJson));
	}

	// --- Outputs: what consumes this expression ---
	// Build per-output-index consumer lists
	TMap<int32, TArray<TSharedPtr<FJsonValue>>> OutputConsumers;
	// Initialize with all known output indices
	for (int32 i = 0; i < TargetExpr->Outputs.Num(); ++i)
	{
		OutputConsumers.FindOrAdd(i);
	}

	// Scan all expressions for inputs referencing TargetExpr
	for (const TObjectPtr<UMaterialExpression>& Expr : Expressions)
	{
		if (!Expr)
		{
			continue;
		}
		for (int32 i = 0; ; ++i)
		{
			FExpressionInput* Input = Expr->GetInput(i);
			if (!Input)
			{
				break;
			}
			if (Input->Expression == TargetExpr)
			{
				auto ConsumerJson = MakeShared<FJsonObject>();
				ConsumerJson->SetStringField(TEXT("expression"), Expr->GetName());
				ConsumerJson->SetStringField(TEXT("input_name"), Expr->GetInputName(i).ToString());
				ConsumerJson->SetNumberField(TEXT("input_index"), i);
				OutputConsumers.FindOrAdd(Input->OutputIndex).Add(MakeShared<FJsonValueObject>(ConsumerJson));
			}
		}
	}

	// Also scan material output slots
	for (const FMaterialOutputEntry& Entry : MaterialOutputEntries)
	{
		FExpressionInput* Input = Mat->GetExpressionInputForProperty(Entry.Property);
		if (Input && Input->Expression == TargetExpr)
		{
			auto ConsumerJson = MakeShared<FJsonObject>();
			ConsumerJson->SetStringField(TEXT("material_output"), Entry.Name);
			OutputConsumers.FindOrAdd(Input->OutputIndex).Add(MakeShared<FJsonValueObject>(ConsumerJson));
		}
	}

	// Build outputs array
	TArray<TSharedPtr<FJsonValue>> OutputsArray;
	for (int32 i = 0; i < TargetExpr->Outputs.Num(); ++i)
	{
		auto OutputJson = MakeShared<FJsonObject>();
		OutputJson->SetNumberField(TEXT("index"), i);
		OutputJson->SetStringField(TEXT("name"), TargetExpr->Outputs[i].OutputName.ToString());
		TArray<TSharedPtr<FJsonValue>>* Consumers = OutputConsumers.Find(i);
		OutputJson->SetArrayField(TEXT("consumers"), Consumers ? *Consumers : TArray<TSharedPtr<FJsonValue>>());
		OutputJson->SetNumberField(TEXT("consumer_count"), Consumers ? Consumers->Num() : 0);
		OutputsArray.Add(MakeShared<FJsonValueObject>(OutputJson));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetStringField(TEXT("expression_name"), ExprName);
	ResultJson->SetStringField(TEXT("class"), TargetExpr->GetClass()->GetName());
	ResultJson->SetArrayField(TEXT("inputs"), InputsArray);
	ResultJson->SetArrayField(TEXT("outputs"), OutputsArray);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: move_expression
// Params: { "asset_path", "expression_name"?, "pos_x"?, "pos_y"?, "relative"?, "expressions"? }
// Single move: expression_name + pos_x + pos_y
// Batch move: expressions = [{name, x, y}]
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::MoveExpression(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UMaterial* Mat = LoadBaseMaterial(AssetPath);
	if (!Mat)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load base material at '%s'"), *AssetPath));
	}

	// Build a list of move operations
	struct FMoveOp
	{
		FString Name;
		int32 X;
		int32 Y;
	};
	TArray<FMoveOp> MoveOps;
	bool bRelative = Params->HasField(TEXT("relative")) ? Params->GetBoolField(TEXT("relative")) : false;

	const TArray<TSharedPtr<FJsonValue>>* ExpressionsArr = nullptr;
	// Handle Claude Code JSON string serialization quirk — array may arrive as string
	TSharedPtr<FJsonValue> ExpressionsField = Params->TryGetField(TEXT("expressions"));
	TArray<TSharedPtr<FJsonValue>> ParsedArray;
	if (ExpressionsField.IsValid())
	{
		if (ExpressionsField->Type == EJson::Array)
		{
			ExpressionsArr = &ExpressionsField->AsArray();
		}
		else if (ExpressionsField->Type == EJson::String)
		{
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ExpressionsField->AsString());
			TSharedPtr<FJsonValue> Parsed;
			if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid() && Parsed->Type == EJson::Array)
			{
				ParsedArray = Parsed->AsArray();
				ExpressionsArr = &ParsedArray;
			}
		}
	}
	if (ExpressionsArr && ExpressionsArr->Num() > 0)
	{
		for (const TSharedPtr<FJsonValue>& Val : *ExpressionsArr)
		{
			const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
			if (Val && Val->TryGetObject(ObjPtr) && ObjPtr)
			{
				FMoveOp Op;
				Op.Name = (*ObjPtr)->GetStringField(TEXT("name"));
				// Accept both "x"/"y" and "pos_x"/"pos_y"
				Op.X = static_cast<int32>((*ObjPtr)->HasField(TEXT("x")) ? (*ObjPtr)->GetNumberField(TEXT("x")) : (*ObjPtr)->GetNumberField(TEXT("pos_x")));
				Op.Y = static_cast<int32>((*ObjPtr)->HasField(TEXT("y")) ? (*ObjPtr)->GetNumberField(TEXT("y")) : (*ObjPtr)->GetNumberField(TEXT("pos_y")));
				MoveOps.Add(MoveTemp(Op));
			}
		}
	}
	else if (Params->HasField(TEXT("expression_name")))
	{
		FMoveOp Op;
		Op.Name = Params->GetStringField(TEXT("expression_name"));
		Op.X = Params->HasField(TEXT("pos_x")) ? static_cast<int32>(Params->GetNumberField(TEXT("pos_x"))) : 0;
		Op.Y = Params->HasField(TEXT("pos_y")) ? static_cast<int32>(Params->GetNumberField(TEXT("pos_y"))) : 0;
		MoveOps.Add(MoveTemp(Op));
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("Must provide either 'expression_name' or 'expressions' array"));
	}

	if (MoveOps.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("No move operations specified"));
	}

	// Build name -> expression lookup
	TMap<FString, UMaterialExpression*> NameToExpr;
	for (const TObjectPtr<UMaterialExpression>& Expr : Mat->GetExpressions())
	{
		if (Expr)
		{
			NameToExpr.Add(Expr->GetName(), Expr);
		}
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("MoveExpression")));
	Mat->Modify();

	TArray<TSharedPtr<FJsonValue>> MovedArray;
	TArray<FString> NotFound;

	for (const FMoveOp& Op : MoveOps)
	{
		UMaterialExpression** FoundPtr = NameToExpr.Find(Op.Name);
		if (!FoundPtr || !*FoundPtr)
		{
			NotFound.Add(Op.Name);
			continue;
		}

		UMaterialExpression* Expr = *FoundPtr;
		if (bRelative)
		{
			Expr->MaterialExpressionEditorX += Op.X;
			Expr->MaterialExpressionEditorY += Op.Y;
		}
		else
		{
			Expr->MaterialExpressionEditorX = Op.X;
			Expr->MaterialExpressionEditorY = Op.Y;
		}

		auto MovedJson = MakeShared<FJsonObject>();
		MovedJson->SetStringField(TEXT("name"), Op.Name);
		MovedJson->SetNumberField(TEXT("x"), Expr->MaterialExpressionEditorX);
		MovedJson->SetNumberField(TEXT("y"), Expr->MaterialExpressionEditorY);
		MovedArray.Add(MakeShared<FJsonValueObject>(MovedJson));
	}

	Mat->PreEditChange(nullptr);
	Mat->PostEditChange();
	GEditor->EndTransaction();

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetNumberField(TEXT("moved_count"), MovedArray.Num());
	ResultJson->SetArrayField(TEXT("moved"), MovedArray);
	if (NotFound.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> NotFoundArr;
		for (const FString& Name : NotFound) NotFoundArr.Add(MakeShared<FJsonValueString>(Name));
		ResultJson->SetArrayField(TEXT("not_found"), NotFoundArr);
	}

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: get_material_properties
// Params: { "asset_path": "..." }
// Works on both UMaterial and UMaterialInstance (loaded as UMaterialInterface)
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::GetMaterialProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	UMaterialInterface* MatInterface = LoadedAsset ? Cast<UMaterialInterface>(LoadedAsset) : nullptr;
	if (!MatInterface)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load material at '%s'"), *AssetPath));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetStringField(TEXT("asset_class"), MatInterface->GetClass()->GetName());

	// Get blend mode
	EBlendMode BlendMode = MatInterface->GetBlendMode();
	const UEnum* BlendEnum = StaticEnum<EBlendMode>();
	ResultJson->SetStringField(TEXT("blend_mode"), BlendEnum->GetNameStringByIndex(static_cast<int32>(BlendMode)));

	// Get shading model(s)
	FMaterialShadingModelField ShadingModels = MatInterface->GetShadingModels();
	EMaterialShadingModel FirstModel = ShadingModels.GetFirstShadingModel();
	const UEnum* ShadingEnum = StaticEnum<EMaterialShadingModel>();
	ResultJson->SetStringField(TEXT("shading_model"), ShadingEnum->GetNameStringByIndex(static_cast<int32>(FirstModel)));

	// If multiple shading models, list them all
	if (ShadingModels.CountShadingModels() > 1)
	{
		TArray<TSharedPtr<FJsonValue>> ModelsArr;
		for (int32 i = 0; i < static_cast<int32>(MSM_NUM); ++i)
		{
			if (ShadingModels.HasShadingModel(static_cast<EMaterialShadingModel>(i)))
			{
				ModelsArr.Add(MakeShared<FJsonValueString>(ShadingEnum->GetNameStringByIndex(i)));
			}
		}
		ResultJson->SetArrayField(TEXT("shading_models"), ModelsArr);
	}

	// Material domain — only available on base UMaterial
	UMaterial* BaseMat = MatInterface->GetMaterial();
	if (BaseMat)
	{
		const UEnum* DomainEnum = StaticEnum<EMaterialDomain>();
		ResultJson->SetStringField(TEXT("material_domain"), DomainEnum->GetNameStringByIndex(static_cast<int32>(BaseMat->MaterialDomain)));
		ResultJson->SetBoolField(TEXT("two_sided"), BaseMat->TwoSided != 0);
		ResultJson->SetNumberField(TEXT("opacity_mask_clip_value"), BaseMat->OpacityMaskClipValue);
		ResultJson->SetBoolField(TEXT("dithered_lod_transition"), BaseMat->DitheredLODTransition != 0);
		ResultJson->SetNumberField(TEXT("expression_count"), BaseMat->GetExpressions().Num());
		// BUG #1: optimization flags missing from read path
		ResultJson->SetBoolField(TEXT("fully_rough"), BaseMat->bFullyRough != 0);
		ResultJson->SetBoolField(TEXT("cast_shadow_as_masked"), BaseMat->bCastDynamicShadowAsMasked != 0);
	}

	// Is it translucent?
	ResultJson->SetBoolField(TEXT("is_translucent"), IsTranslucentBlendMode(BlendMode));
	ResultJson->SetBoolField(TEXT("is_masked"), BlendMode == BLEND_Masked);

	// Usage flags
	auto UsageJson = MakeShared<FJsonObject>();
	if (BaseMat)
	{
		UsageJson->SetBoolField(TEXT("skeletal_mesh"), BaseMat->GetUsageByFlag(MATUSAGE_SkeletalMesh));
		UsageJson->SetBoolField(TEXT("particle_sprites"), BaseMat->GetUsageByFlag(MATUSAGE_ParticleSprites));
		UsageJson->SetBoolField(TEXT("beam_trails"), BaseMat->GetUsageByFlag(MATUSAGE_BeamTrails));
		UsageJson->SetBoolField(TEXT("mesh_particles"), BaseMat->GetUsageByFlag(MATUSAGE_MeshParticles));
		UsageJson->SetBoolField(TEXT("static_lighting"), BaseMat->GetUsageByFlag(MATUSAGE_StaticLighting));
		UsageJson->SetBoolField(TEXT("morph_targets"), BaseMat->GetUsageByFlag(MATUSAGE_MorphTargets));
		UsageJson->SetBoolField(TEXT("spline_mesh"), BaseMat->GetUsageByFlag(MATUSAGE_SplineMesh));
		UsageJson->SetBoolField(TEXT("instanced_static_meshes"), BaseMat->GetUsageByFlag(MATUSAGE_InstancedStaticMeshes));
		UsageJson->SetBoolField(TEXT("niagara_sprites"), BaseMat->GetUsageByFlag(MATUSAGE_NiagaraSprites));
		UsageJson->SetBoolField(TEXT("niagara_meshes"), BaseMat->GetUsageByFlag(MATUSAGE_NiagaraMeshParticles));
		UsageJson->SetBoolField(TEXT("niagara_ribbons"), BaseMat->GetUsageByFlag(MATUSAGE_NiagaraRibbons));
		UsageJson->SetBoolField(TEXT("virtual_heightfield_mesh"), BaseMat->GetUsageByFlag(MATUSAGE_VirtualHeightfieldMesh));
	}
	ResultJson->SetObjectField(TEXT("usage_flags"), UsageJson);

	// Check if this is a material instance — report parent
	UMaterialInstance* MatInst = Cast<UMaterialInstance>(MatInterface);
	if (MatInst && MatInst->Parent)
	{
		ResultJson->SetStringField(TEXT("parent_material"), MatInst->Parent->GetPathName());
		ResultJson->SetBoolField(TEXT("is_instance"), true);
	}
	else
	{
		ResultJson->SetBoolField(TEXT("is_instance"), false);
	}

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: get_instance_parameters
// Reads all parameter overrides from a MIC with override detection vs parent
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::GetInstanceParameters(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	UMaterialInstanceConstant* MIC = LoadedAsset ? Cast<UMaterialInstanceConstant>(LoadedAsset) : nullptr;
	if (!MIC)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load material instance at '%s'"), *AssetPath));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);

	// Parent info
	if (MIC->Parent)
	{
		ResultJson->SetStringField(TEXT("parent"), MIC->Parent->GetPathName());
	}

	// Scalar parameters
	TArray<TSharedPtr<FJsonValue>> ScalarArr;
	for (const auto& Param : MIC->ScalarParameterValues)
	{
		auto ParamJson = MakeShared<FJsonObject>();
		ParamJson->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
		ParamJson->SetNumberField(TEXT("value"), Param.ParameterValue);
		ParamJson->SetBoolField(TEXT("is_overridden"), true); // present in array = overridden
		ScalarArr.Add(MakeShared<FJsonValueObject>(ParamJson));
	}
	ResultJson->SetArrayField(TEXT("scalar"), ScalarArr);

	// Vector parameters
	TArray<TSharedPtr<FJsonValue>> VectorArr;
	for (const auto& Param : MIC->VectorParameterValues)
	{
		auto ParamJson = MakeShared<FJsonObject>();
		ParamJson->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
		auto ColorJson = MakeShared<FJsonObject>();
		ColorJson->SetNumberField(TEXT("R"), Param.ParameterValue.R);
		ColorJson->SetNumberField(TEXT("G"), Param.ParameterValue.G);
		ColorJson->SetNumberField(TEXT("B"), Param.ParameterValue.B);
		ColorJson->SetNumberField(TEXT("A"), Param.ParameterValue.A);
		ParamJson->SetObjectField(TEXT("value"), ColorJson);
		ParamJson->SetBoolField(TEXT("is_overridden"), true);
		VectorArr.Add(MakeShared<FJsonValueObject>(ParamJson));
	}
	ResultJson->SetArrayField(TEXT("vector"), VectorArr);

	// Texture parameters
	TArray<TSharedPtr<FJsonValue>> TextureArr;
	for (const auto& Param : MIC->TextureParameterValues)
	{
		auto ParamJson = MakeShared<FJsonObject>();
		ParamJson->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
		ParamJson->SetStringField(TEXT("value"), Param.ParameterValue ? Param.ParameterValue->GetPathName() : TEXT("None"));
		ParamJson->SetBoolField(TEXT("is_overridden"), true);
		TextureArr.Add(MakeShared<FJsonValueObject>(ParamJson));
	}
	ResultJson->SetArrayField(TEXT("texture"), TextureArr);

	// Static switch parameters — use GetAllStaticSwitchParameterInfo (proven API)
	TArray<TSharedPtr<FJsonValue>> SwitchArr;
	{
		TArray<FMaterialParameterInfo> SwitchInfos;
		TArray<FGuid> SwitchGuids;
		MIC->GetAllStaticSwitchParameterInfo(SwitchInfos, SwitchGuids);

		// Also get parent values to detect overrides
		for (int32 i = 0; i < SwitchInfos.Num(); ++i)
		{
			bool Value = false;
			FGuid OutGuid;
			MIC->GetStaticSwitchParameterValue(SwitchInfos[i], Value, OutGuid);

			// Check if overridden by inspecting the bOverride flag via public GetStaticParameters() API.
			// Value comparison against parent is wrong: a switch explicitly set to the same value as the
			// parent would incorrectly show is_overridden=false.
			bool bIsOverridden = false;
			{
				FStaticParameterSet StaticParams = MIC->GetStaticParameters();
				for (const FStaticSwitchParameter& SP : StaticParams.StaticSwitchParameters)
				{
					if (SP.ParameterInfo == SwitchInfos[i])
					{
						bIsOverridden = SP.bOverride;
						break;
					}
				}
			}

			auto ParamJson = MakeShared<FJsonObject>();
			ParamJson->SetStringField(TEXT("name"), SwitchInfos[i].Name.ToString());
			ParamJson->SetBoolField(TEXT("value"), Value);
			ParamJson->SetBoolField(TEXT("is_overridden"), bIsOverridden);
			SwitchArr.Add(MakeShared<FJsonValueObject>(ParamJson));
		}
	}
	ResultJson->SetArrayField(TEXT("static_switch"), SwitchArr);

	// MINOR #1: only count switch params that are actually overridden (SwitchArr contains all parent params)
	int32 OverriddenSwitchCount = 0;
	for (const TSharedPtr<FJsonValue>& SwitchVal : SwitchArr)
	{
		bool bOverridden = false;
		if (SwitchVal.IsValid())
		{
			SwitchVal->AsObject()->TryGetBoolField(TEXT("is_overridden"), bOverridden);
		}
		if (bOverridden) OverriddenSwitchCount++;
	}
	ResultJson->SetNumberField(TEXT("total_overrides"),
		ScalarArr.Num() + VectorArr.Num() + TextureArr.Num() + OverriddenSwitchCount);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: set_instance_parameters (batch)
// Sets multiple parameter overrides in one call with a single update
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::SetInstanceParameters(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	UMaterialInstanceConstant* MIC = LoadedAsset ? Cast<UMaterialInstanceConstant>(LoadedAsset) : nullptr;
	if (!MIC)
	{
		if (Cast<UMaterial>(LoadedAsset))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("'%s' is a Material, not a Material Instance. Use 'set_expression_property' to modify expression defaults on base materials, or create a Material Instance with 'create_material_instance'."),
				*AssetPath));
		}
		if (Cast<UMaterialFunction>(LoadedAsset))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("'%s' is a Material Function, not a Material Instance. Use 'set_expression_property' to modify expression defaults."),
				*AssetPath));
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to load material instance at '%s' (loaded as %s)"),
			*AssetPath, LoadedAsset ? *LoadedAsset->GetClass()->GetName() : TEXT("null")));
	}

	const TArray<TSharedPtr<FJsonValue>>* ParamArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("parameters"), ParamArray) || !ParamArray)
	{
		return FMonolithActionResult::Error(TEXT("Missing required 'parameters' array"));
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("SetInstanceParameters")));
	MIC->Modify();

	int32 SetCount = 0;
	bool bHasStaticSwitches = false;
	TArray<TSharedPtr<FJsonValue>> ErrorsArr;
	TArray<TSharedPtr<FJsonValue>> ResultsArr;  // MINOR #3: per-param results

	for (const auto& ParamVal : *ParamArray)
	{
		const TSharedPtr<FJsonObject>* ParamObj = nullptr;
		if (!ParamVal->TryGetObject(ParamObj) || !ParamObj)
		{
			auto ErrJson = MakeShared<FJsonObject>();
			ErrJson->SetStringField(TEXT("error"), TEXT("Array element is not a JSON object"));
			ErrorsArr.Add(MakeShared<FJsonValueObject>(ErrJson));
			continue;
		}

		FString Name = (*ParamObj)->GetStringField(TEXT("name"));
		FString Type = (*ParamObj)->GetStringField(TEXT("type"));
		FMaterialParameterInfo ParamInfo(*Name);

		if (Name.IsEmpty() || Type.IsEmpty())
		{
			auto ErrJson = MakeShared<FJsonObject>();
			ErrJson->SetStringField(TEXT("error"), TEXT("Parameter missing 'name' or 'type'"));
			ErrorsArr.Add(MakeShared<FJsonValueObject>(ErrJson));
			continue;
		}

		bool bParamSuccess = false;
		FString ParamError;

		if (Type.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
		{
			double NumVal = 0.0;
			if ((*ParamObj)->TryGetNumberField(TEXT("value"), NumVal))
			{
				MIC->SetScalarParameterValueEditorOnly(ParamInfo, static_cast<float>(NumVal));
				SetCount++;
				bParamSuccess = true;
			}
			else
			{
				ParamError = FString::Printf(TEXT("Scalar param '%s': missing or invalid 'value'"), *Name);
				auto ErrJson = MakeShared<FJsonObject>();
				ErrJson->SetStringField(TEXT("error"), ParamError);
				ErrorsArr.Add(MakeShared<FJsonValueObject>(ErrJson));
			}
		}
		else if (Type.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
		{
			const TSharedPtr<FJsonObject>* ColorObj = nullptr;
			if ((*ParamObj)->TryGetObjectField(TEXT("value"), ColorObj))
			{
				FLinearColor Color;
				Color.R = (*ColorObj)->HasField(TEXT("R")) ? static_cast<float>((*ColorObj)->GetNumberField(TEXT("R"))) : 0.f;
				Color.G = (*ColorObj)->HasField(TEXT("G")) ? static_cast<float>((*ColorObj)->GetNumberField(TEXT("G"))) : 0.f;
				Color.B = (*ColorObj)->HasField(TEXT("B")) ? static_cast<float>((*ColorObj)->GetNumberField(TEXT("B"))) : 0.f;
				Color.A = (*ColorObj)->HasField(TEXT("A")) ? static_cast<float>((*ColorObj)->GetNumberField(TEXT("A"))) : 1.f;
				MIC->SetVectorParameterValueEditorOnly(ParamInfo, Color);
				SetCount++;
				bParamSuccess = true;
			}
			else
			{
				ParamError = FString::Printf(TEXT("Vector param '%s': missing or invalid 'value' object"), *Name);
				auto ErrJson = MakeShared<FJsonObject>();
				ErrJson->SetStringField(TEXT("error"), ParamError);
				ErrorsArr.Add(MakeShared<FJsonValueObject>(ErrJson));
			}
		}
		else if (Type.Equals(TEXT("texture"), ESearchCase::IgnoreCase))
		{
			FString TexPath = (*ParamObj)->GetStringField(TEXT("value"));
			UTexture* Tex = Cast<UTexture>(UEditorAssetLibrary::LoadAsset(TexPath));
			if (Tex)
			{
				MIC->SetTextureParameterValueEditorOnly(ParamInfo, Tex);
				SetCount++;
				bParamSuccess = true;
			}
			else
			{
				ParamError = FString::Printf(TEXT("Texture param '%s': failed to load texture at '%s'"), *Name, *TexPath);
				auto ErrJson = MakeShared<FJsonObject>();
				ErrJson->SetStringField(TEXT("error"), ParamError);
				ErrorsArr.Add(MakeShared<FJsonValueObject>(ErrJson));
			}
		}
		else if (Type.Equals(TEXT("switch"), ESearchCase::IgnoreCase))
		{
			bool bValue = false;
			if ((*ParamObj)->TryGetBoolField(TEXT("value"), bValue))
			{
				MIC->SetStaticSwitchParameterValueEditorOnly(ParamInfo, bValue);
				bHasStaticSwitches = true;
				SetCount++;
				bParamSuccess = true;
			}
			else
			{
				ParamError = FString::Printf(TEXT("Switch param '%s': missing or invalid 'value' bool"), *Name);
				auto ErrJson = MakeShared<FJsonObject>();
				ErrJson->SetStringField(TEXT("error"), ParamError);
				ErrorsArr.Add(MakeShared<FJsonValueObject>(ErrJson));
			}
		}
		else
		{
			ParamError = FString::Printf(TEXT("Unknown type '%s' for param '%s'. Valid: scalar, vector, texture, switch"), *Type, *Name);
			auto ErrJson = MakeShared<FJsonObject>();
			ErrJson->SetStringField(TEXT("error"), ParamError);
			ErrorsArr.Add(MakeShared<FJsonValueObject>(ErrJson));
		}

		// MINOR #3: record per-param result
		auto ResJson = MakeShared<FJsonObject>();
		ResJson->SetStringField(TEXT("name"), Name);
		ResJson->SetStringField(TEXT("type"), Type);
		ResJson->SetBoolField(TEXT("success"), bParamSuccess);
		if (!bParamSuccess && !ParamError.IsEmpty())
		{
			ResJson->SetStringField(TEXT("error"), ParamError);
		}
		ResultsArr.Add(MakeShared<FJsonValueObject>(ResJson));
	}

	// Single update call at the end — handles static permutation recompile if needed
	if (SetCount > 0)
	{
		UMaterialEditingLibrary::UpdateMaterialInstance(MIC);
	}

	MIC->MarkPackageDirty();
	GEditor->EndTransaction();

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetNumberField(TEXT("set_count"), SetCount);
	ResultJson->SetArrayField(TEXT("results"), ResultsArr);  // MINOR #3
	if (ErrorsArr.Num() > 0)
	{
		ResultJson->SetArrayField(TEXT("errors"), ErrorsArr);
	}

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: set_instance_parent
// Reparent a material instance, reporting lost/kept parameters
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::SetInstanceParent(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString NewParentPath = Params->GetStringField(TEXT("new_parent"));

	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	UMaterialInstanceConstant* MIC = LoadedAsset ? Cast<UMaterialInstanceConstant>(LoadedAsset) : nullptr;
	if (!MIC)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load material instance at '%s'"), *AssetPath));
	}

	UObject* NewParentObj = UEditorAssetLibrary::LoadAsset(NewParentPath);
	UMaterialInterface* NewParent = NewParentObj ? Cast<UMaterialInterface>(NewParentObj) : nullptr;
	if (!NewParent)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load new parent material at '%s'"), *NewParentPath));
	}

	// Snapshot current override names before reparenting
	TSet<FString> OldParamNames;
	for (const auto& P : MIC->ScalarParameterValues) OldParamNames.Add(P.ParameterInfo.Name.ToString());
	for (const auto& P : MIC->VectorParameterValues) OldParamNames.Add(P.ParameterInfo.Name.ToString());
	for (const auto& P : MIC->TextureParameterValues) OldParamNames.Add(P.ParameterInfo.Name.ToString());
	// Also snapshot static switch names
	{
		TArray<FMaterialParameterInfo> SwitchInfos;
		TArray<FGuid> SwitchGuids;
		MIC->GetAllStaticSwitchParameterInfo(SwitchInfos, SwitchGuids);
		for (const auto& Info : SwitchInfos) OldParamNames.Add(Info.Name.ToString());
	}

	FString OldParentPath = MIC->Parent ? MIC->Parent->GetPathName() : TEXT("None");

	GEditor->BeginTransaction(FText::FromString(TEXT("SetInstanceParent")));
	MIC->Modify();
	MIC->SetParentEditorOnly(NewParent, true);
	UMaterialEditingLibrary::UpdateMaterialInstance(MIC);
	MIC->MarkPackageDirty();
	GEditor->EndTransaction();

	// After reparenting, check which parameters from the new parent exist
	// to determine what was kept vs lost
	TArray<TSharedPtr<FJsonValue>> KeptArr, LostArr;

	// Gather all parameter names the new parent exposes
	TSet<FString> NewParentParamNames;
	{
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Guids;
		NewParent->GetAllScalarParameterInfo(Infos, Guids);
		for (const auto& Info : Infos) NewParentParamNames.Add(Info.Name.ToString());
		Infos.Reset(); Guids.Reset();
		NewParent->GetAllVectorParameterInfo(Infos, Guids);
		for (const auto& Info : Infos) NewParentParamNames.Add(Info.Name.ToString());
		Infos.Reset(); Guids.Reset();
		NewParent->GetAllTextureParameterInfo(Infos, Guids);
		for (const auto& Info : Infos) NewParentParamNames.Add(Info.Name.ToString());
		Infos.Reset(); Guids.Reset();
		NewParent->GetAllStaticSwitchParameterInfo(Infos, Guids);
		for (const auto& Info : Infos) NewParentParamNames.Add(Info.Name.ToString());
	}

	for (const FString& Name : OldParamNames)
	{
		if (NewParentParamNames.Contains(Name))
		{
			KeptArr.Add(MakeShared<FJsonValueString>(Name));
		}
		else
		{
			LostArr.Add(MakeShared<FJsonValueString>(Name));
		}
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetStringField(TEXT("old_parent"), OldParentPath);
	ResultJson->SetStringField(TEXT("new_parent"), NewParent->GetPathName());
	ResultJson->SetArrayField(TEXT("kept_parameters"), KeptArr);
	ResultJson->SetArrayField(TEXT("lost_parameters"), LostArr);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: clear_instance_parameter
// Remove a single override (or all overrides) from a material instance
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::ClearInstanceParameter(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	UMaterialInstanceConstant* MIC = LoadedAsset ? Cast<UMaterialInstanceConstant>(LoadedAsset) : nullptr;
	if (!MIC)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load material instance at '%s'"), *AssetPath));
	}

	FString ParamName = Params->HasField(TEXT("parameter_name")) ? Params->GetStringField(TEXT("parameter_name")) : TEXT("");
	FString ParamType = Params->HasField(TEXT("parameter_type")) ? Params->GetStringField(TEXT("parameter_type")) : TEXT("all");

	GEditor->BeginTransaction(FText::FromString(TEXT("ClearInstanceParameter")));
	MIC->Modify();

	int32 ClearedCount = 0;
	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);

	if (ParamName.IsEmpty())
	{
		// Clear ALL overrides — count all types before clearing (MINOR #2: include static switches)
		int32 SwitchCountBefore = 0;
		{
			FStaticParameterSet StaticParams;
			MIC->GetStaticParameterValues(StaticParams);
			for (const auto& SP : StaticParams.StaticSwitchParameters)
			{
				if (SP.bOverride) SwitchCountBefore++;
			}
		}
		int32 TotalBefore = MIC->ScalarParameterValues.Num() + MIC->VectorParameterValues.Num()
			+ MIC->TextureParameterValues.Num() + SwitchCountBefore;
		UMaterialEditingLibrary::ClearAllMaterialInstanceParameters(MIC);

		// ClearAllMaterialInstanceParameters only clears dynamic params (scalar/vector/texture).
		// Static switches must be cleared manually.
		if (SwitchCountBefore > 0)
		{
			FStaticParameterSet StaticParams;
			MIC->GetStaticParameterValues(StaticParams);
			bool bModifiedStatic = false;
			for (auto& SP : StaticParams.StaticSwitchParameters)
			{
				if (SP.bOverride)
				{
					SP.bOverride = false;
					bModifiedStatic = true;
				}
			}
			if (bModifiedStatic)
			{
				MIC->UpdateStaticPermutation(StaticParams);
			}
		}

		ClearedCount = TotalBefore;
		ResultJson->SetStringField(TEXT("cleared"), TEXT("all"));
	}
	else
	{
		// Clear a specific parameter by name and type
		bool bClearScalar = ParamType.Equals(TEXT("scalar"), ESearchCase::IgnoreCase) || ParamType.Equals(TEXT("all"), ESearchCase::IgnoreCase);
		bool bClearVector = ParamType.Equals(TEXT("vector"), ESearchCase::IgnoreCase) || ParamType.Equals(TEXT("all"), ESearchCase::IgnoreCase);
		bool bClearTexture = ParamType.Equals(TEXT("texture"), ESearchCase::IgnoreCase) || ParamType.Equals(TEXT("all"), ESearchCase::IgnoreCase);
		bool bClearSwitch = ParamType.Equals(TEXT("switch"), ESearchCase::IgnoreCase) || ParamType.Equals(TEXT("all"), ESearchCase::IgnoreCase);

		if (bClearScalar)
		{
			ClearedCount += MIC->ScalarParameterValues.RemoveAll([&](const FScalarParameterValue& P)
			{
				return P.ParameterInfo.Name.ToString() == ParamName;
			});
		}
		if (bClearVector)
		{
			ClearedCount += MIC->VectorParameterValues.RemoveAll([&](const FVectorParameterValue& P)
			{
				return P.ParameterInfo.Name.ToString() == ParamName;
			});
		}
		if (bClearTexture)
		{
			ClearedCount += MIC->TextureParameterValues.RemoveAll([&](const FTextureParameterValue& P)
			{
				return P.ParameterInfo.Name.ToString() == ParamName;
			});
		}
		if (bClearSwitch)
		{
			// Static switches: get params, unset the override flag, then update permutation
			FStaticParameterSet StaticParams;
			MIC->GetStaticParameterValues(StaticParams);
			bool bModifiedStatic = false;
			for (auto& SP : StaticParams.StaticSwitchParameters)
			{
				if (SP.ParameterInfo.Name.ToString() == ParamName && SP.bOverride)
				{
					SP.bOverride = false;
					bModifiedStatic = true;
					ClearedCount++;
				}
			}
			if (bModifiedStatic)
			{
				MIC->UpdateStaticPermutation(StaticParams);
			}
		}

		ResultJson->SetStringField(TEXT("cleared"), ParamName);
		ResultJson->SetStringField(TEXT("type_filter"), ParamType);
	}

	MIC->MarkPackageDirty();
	GEditor->EndTransaction();

	ResultJson->SetNumberField(TEXT("cleared_count"), ClearedCount);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: save_material
// Save a material asset to disk
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::SaveMaterial(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	bool bOnlyIfDirty = Params->HasField(TEXT("only_if_dirty")) ? Params->GetBoolField(TEXT("only_if_dirty")) : true;

	// Verify asset exists
	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	if (!LoadedAsset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load asset at '%s'"), *AssetPath));
	}

	bool bWasDirty = LoadedAsset->GetPackage()->IsDirty();
	bool bSaved = UEditorAssetLibrary::SaveAsset(AssetPath, bOnlyIfDirty);

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetBoolField(TEXT("saved"), bSaved);
	ResultJson->SetBoolField(TEXT("was_dirty"), bWasDirty);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Wave 5 — Graph Editing Power
// ============================================================================

// ============================================================================
// Action: update_custom_hlsl_node
// Params: { "asset_path", "expression_name", "code"?, "description"?,
//           "output_type"?, "inputs"?, "additional_outputs"?,
//           "include_file_paths"?, "additional_defines"? }
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::UpdateCustomHlslNode(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString ExprName = Params->GetStringField(TEXT("expression_name"));

	UMaterial* Mat = LoadBaseMaterial(AssetPath);
	if (!Mat)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load base material at '%s'"), *AssetPath));
	}

	// Find the expression
	UMaterialExpressionCustom* CustomExpr = nullptr;
	for (const TObjectPtr<UMaterialExpression>& Expr : Mat->GetExpressions())
	{
		if (Expr && Expr->GetName() == ExprName)
		{
			CustomExpr = Cast<UMaterialExpressionCustom>(Expr);
			if (!CustomExpr)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Expression '%s' is a %s, not a UMaterialExpressionCustom"),
					*ExprName, *Expr->GetClass()->GetName()));
			}
			break;
		}
	}

	if (!CustomExpr)
	{
		// List available expressions for better error message
		TArray<FString> AvailableNames;
		for (const TObjectPtr<UMaterialExpression>& Expr : Mat->GetExpressions())
		{
			if (Expr) AvailableNames.Add(Expr->GetName());
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Expression '%s' not found. Available: %s"),
			*ExprName, *FString::Join(AvailableNames, TEXT(", "))));
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("UpdateCustomHlslNode")));
	Mat->Modify();
	CustomExpr->Modify();

	TArray<FString> UpdatedFields;

	if (Params->HasField(TEXT("code")))
	{
		CustomExpr->Code = Params->GetStringField(TEXT("code"));
		UpdatedFields.Add(TEXT("code"));
	}

	if (Params->HasField(TEXT("description")))
	{
		CustomExpr->Description = Params->GetStringField(TEXT("description"));
		UpdatedFields.Add(TEXT("description"));
	}

	if (Params->HasField(TEXT("output_type")))
	{
		CustomExpr->OutputType = ParseCustomOutputType(Params->GetStringField(TEXT("output_type")));
		UpdatedFields.Add(TEXT("output_type"));
	}

	const TArray<TSharedPtr<FJsonValue>>* InputsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("inputs"), InputsArray))
	{
		CustomExpr->Inputs.Empty();
		for (const TSharedPtr<FJsonValue>& InputVal : *InputsArray)
		{
			const TSharedPtr<FJsonObject>* InputObjPtr = nullptr;
			if (InputVal && InputVal->TryGetObject(InputObjPtr) && InputObjPtr)
			{
				FCustomInput NewInput;
				NewInput.InputName = *(*InputObjPtr)->GetStringField(TEXT("name"));
				CustomExpr->Inputs.Add(NewInput);
			}
		}
		UpdatedFields.Add(TEXT("inputs"));
	}

	const TArray<TSharedPtr<FJsonValue>>* AddOutputsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("additional_outputs"), AddOutputsArray))
	{
		CustomExpr->AdditionalOutputs.Empty();
		for (const TSharedPtr<FJsonValue>& OutVal : *AddOutputsArray)
		{
			const TSharedPtr<FJsonObject>* OutObjPtr = nullptr;
			if (OutVal && OutVal->TryGetObject(OutObjPtr) && OutObjPtr)
			{
				FCustomOutput NewOutput;
				NewOutput.OutputName = *(*OutObjPtr)->GetStringField(TEXT("name"));
				if ((*OutObjPtr)->HasField(TEXT("type")))
				{
					NewOutput.OutputType = ParseCustomOutputType((*OutObjPtr)->GetStringField(TEXT("type")));
				}
				CustomExpr->AdditionalOutputs.Add(NewOutput);
			}
		}
		UpdatedFields.Add(TEXT("additional_outputs"));
	}

	const TArray<TSharedPtr<FJsonValue>>* IncludePathsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("include_file_paths"), IncludePathsArray))
	{
		CustomExpr->IncludeFilePaths.Empty();
		for (const TSharedPtr<FJsonValue>& PathVal : *IncludePathsArray)
		{
			FString Path;
			if (PathVal && PathVal->TryGetString(Path))
			{
				CustomExpr->IncludeFilePaths.Add(Path);
			}
		}
		UpdatedFields.Add(TEXT("include_file_paths"));
	}

	const TArray<TSharedPtr<FJsonValue>>* DefinesArray = nullptr;
	if (Params->TryGetArrayField(TEXT("additional_defines"), DefinesArray))
	{
		CustomExpr->AdditionalDefines.Empty();
		for (const TSharedPtr<FJsonValue>& DefVal : *DefinesArray)
		{
			const TSharedPtr<FJsonObject>* DefObjPtr = nullptr;
			if (DefVal && DefVal->TryGetObject(DefObjPtr) && DefObjPtr)
			{
				FCustomDefine NewDefine;
				NewDefine.DefineName = (*DefObjPtr)->GetStringField(TEXT("name"));
				NewDefine.DefineValue = (*DefObjPtr)->HasField(TEXT("value"))
					? (*DefObjPtr)->GetStringField(TEXT("value")) : TEXT("");
				CustomExpr->AdditionalDefines.Add(NewDefine);
			}
		}
		UpdatedFields.Add(TEXT("additional_defines"));
	}

	// Rebuild outputs after any structural change
	CustomExpr->RebuildOutputs();

	Mat->PreEditChange(nullptr);
	Mat->PostEditChange();
	GEditor->EndTransaction();

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetStringField(TEXT("expression_name"), CustomExpr->GetName());

	TArray<TSharedPtr<FJsonValue>> UpdatedArr;
	for (const FString& F : UpdatedFields) UpdatedArr.Add(MakeShared<FJsonValueString>(F));
	ResultJson->SetArrayField(TEXT("updated_fields"), UpdatedArr);
	ResultJson->SetNumberField(TEXT("input_count"), CustomExpr->Inputs.Num());
	ResultJson->SetNumberField(TEXT("output_count"), 1 + CustomExpr->AdditionalOutputs.Num());

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: replace_expression
// Params: { "asset_path", "expression_name", "new_class",
//           "new_properties"?, "preserve_connections"? }
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::ReplaceExpression(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString ExprName = Params->GetStringField(TEXT("expression_name"));
	FString NewClassName = Params->GetStringField(TEXT("new_class"));
	bool bPreserveConnections = Params->HasField(TEXT("preserve_connections"))
		? Params->GetBoolField(TEXT("preserve_connections")) : true;

	UMaterial* Mat = LoadBaseMaterial(AssetPath);
	if (!Mat)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load base material at '%s'"), *AssetPath));
	}

	// Find old expression
	UMaterialExpression* OldExpr = nullptr;
	TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = Mat->GetExpressions();
	for (const TObjectPtr<UMaterialExpression>& Expr : Expressions)
	{
		if (Expr && Expr->GetName() == ExprName)
		{
			OldExpr = Expr;
			break;
		}
	}

	if (!OldExpr)
	{
		TArray<FString> AvailableNames;
		for (const TObjectPtr<UMaterialExpression>& Expr : Expressions)
		{
			if (Expr) AvailableNames.Add(Expr->GetName());
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Expression '%s' not found. Available: %s"),
			*ExprName, *FString::Join(AvailableNames, TEXT(", "))));
	}

	// Resolve new class
	FString FullClassName = NewClassName;
	if (!FullClassName.StartsWith(TEXT("MaterialExpression")))
	{
		FullClassName = TEXT("MaterialExpression") + FullClassName;
	}
	UClass* NewClass = FindFirstObject<UClass>(*FullClassName, EFindFirstObjectOptions::NativeFirst);
	if (!NewClass)
	{
		// Try with U prefix
		NewClass = FindFirstObject<UClass>(*(TEXT("U") + FullClassName), EFindFirstObjectOptions::NativeFirst);
	}
	if (!NewClass || !NewClass->IsChildOf(UMaterialExpression::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Invalid expression class '%s'. Use list_expression_classes to see valid options."), *NewClassName));
	}

	// Record old expression state before deleting
	FString OldClassName = OldExpr->GetClass()->GetName();
	int32 OldPosX = OldExpr->MaterialExpressionEditorX;
	int32 OldPosY = OldExpr->MaterialExpressionEditorY;

	// --- Record INPUT connections on the old expression (what feeds into it) ---
	struct FRecordedInput
	{
		FString InputName;
		int32 InputIndex;
		UMaterialExpression* SourceExpr;
		int32 SourceOutputIndex;
	};
	TArray<FRecordedInput> OldInputs;
	for (int32 i = 0; ; ++i)
	{
		FExpressionInput* Input = OldExpr->GetInput(i);
		if (!Input) break;
		if (Input->Expression)
		{
			FRecordedInput Rec;
			Rec.InputName = OldExpr->GetInputName(i).ToString();
			Rec.InputIndex = i;
			Rec.SourceExpr = Input->Expression;
			Rec.SourceOutputIndex = Input->OutputIndex;
			OldInputs.Add(Rec);
		}
	}

	// --- Record OUTPUT consumers (what references the old expression) ---
	struct FRecordedOutput
	{
		FString ConsumerInputName;
		int32 ConsumerInputIndex;
		int32 OldOutputIndex;
		FString OldOutputName;
		UMaterialExpression* ConsumerExpr; // nullptr for material output
		EMaterialProperty MatProperty;     // MP_MAX if not material output
	};
	TArray<FRecordedOutput> OldOutputConsumers;

	// Build output name map from old expression
	TMap<int32, FString> OldOutputNames;
	for (int32 i = 0; i < OldExpr->Outputs.Num(); ++i)
	{
		OldOutputNames.Add(i, OldExpr->Outputs[i].OutputName.ToString());
	}

	// Scan all expressions for inputs referencing OldExpr
	for (const TObjectPtr<UMaterialExpression>& Expr : Expressions)
	{
		if (!Expr || Expr == OldExpr) continue;
		for (int32 i = 0; ; ++i)
		{
			FExpressionInput* Input = Expr->GetInput(i);
			if (!Input) break;
			if (Input->Expression == OldExpr)
			{
				FRecordedOutput Rec;
				Rec.ConsumerInputName = Expr->GetInputName(i).ToString();
				Rec.ConsumerInputIndex = i;
				Rec.OldOutputIndex = Input->OutputIndex;
				Rec.OldOutputName = OldOutputNames.Contains(Input->OutputIndex)
					? OldOutputNames[Input->OutputIndex] : TEXT("");
				Rec.ConsumerExpr = Expr;
				Rec.MatProperty = MP_MAX;
				OldOutputConsumers.Add(Rec);
			}
		}
	}

	// Scan material output slots
	for (const FMaterialOutputEntry& Entry : MaterialOutputEntries)
	{
		FExpressionInput* Input = Mat->GetExpressionInputForProperty(Entry.Property);
		if (Input && Input->Expression == OldExpr)
		{
			FRecordedOutput Rec;
			Rec.ConsumerInputName = Entry.Name;
			Rec.ConsumerInputIndex = -1;
			Rec.OldOutputIndex = Input->OutputIndex;
			Rec.OldOutputName = OldOutputNames.Contains(Input->OutputIndex)
				? OldOutputNames[Input->OutputIndex] : TEXT("");
			Rec.ConsumerExpr = nullptr;
			Rec.MatProperty = Entry.Property;
			OldOutputConsumers.Add(Rec);
		}
	}

	// --- Perform the replacement in a single undo transaction ---
	GEditor->BeginTransaction(FText::FromString(TEXT("ReplaceExpression")));
	Mat->Modify();

	// Delete old expression
	UMaterialEditingLibrary::DeleteMaterialExpression(Mat, OldExpr);
	OldExpr = nullptr; // dangling after delete

	// Create new expression
	UMaterialExpression* NewExpr = UMaterialEditingLibrary::CreateMaterialExpression(
		Mat, NewClass, OldPosX, OldPosY);

	if (!NewExpr)
	{
		GEditor->EndTransaction();
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to create replacement expression of class '%s'"), *NewClassName));
	}

	// Set properties via reflection if provided
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("new_properties"), PropsObj) && PropsObj)
	{
		for (const auto& Pair : (*PropsObj)->Values)
		{
			FProperty* Prop = NewExpr->GetClass()->FindPropertyByName(*Pair.Key);
			if (Prop)
			{
				FString ValueStr;
				if (Pair.Value->TryGetString(ValueStr))
				{
					void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(NewExpr);
					Prop->ImportText_Direct(*ValueStr, ValuePtr, NewExpr, PPF_None);
				}
			}
		}
	}

	// --- Reconnect ---
	TArray<TSharedPtr<FJsonValue>> ReconnectedArr;
	TArray<TSharedPtr<FJsonValue>> FailedArr;
	TArray<TSharedPtr<FJsonValue>> WarningsArr;

	if (bPreserveConnections)
	{
		// Build new expression input name map
		TMap<FString, int32> NewInputNameToIndex;
		for (int32 i = 0; ; ++i)
		{
			FExpressionInput* Input = NewExpr->GetInput(i);
			if (!Input) break;
			NewInputNameToIndex.Add(NewExpr->GetInputName(i).ToString(), i);
		}

		// Count new inputs for index-based fallback bounds
		int32 NewInputCount = NewInputNameToIndex.Num();

		// Build new expression output name map
		TMap<FString, int32> NewOutputNameToIndex;
		for (int32 i = 0; i < NewExpr->Outputs.Num(); ++i)
		{
			FString OutName = NewExpr->Outputs[i].OutputName.ToString();
			if (!OutName.IsEmpty())
			{
				NewOutputNameToIndex.Add(OutName, i);
			}
		}

		// Reconnect inputs (what fed into old expr -> feed into new expr)
		for (const FRecordedInput& Rec : OldInputs)
		{
			bool bReconnected = false;
			bool bUsedFallback = false;

			// Try name match first
			int32* MatchedIdx = NewInputNameToIndex.Find(Rec.InputName);
			if (MatchedIdx)
			{
				FString SourceOutputName;
				if (Rec.SourceExpr->Outputs.IsValidIndex(Rec.SourceOutputIndex))
				{
					SourceOutputName = Rec.SourceExpr->Outputs[Rec.SourceOutputIndex].OutputName.ToString();
				}
				bReconnected = UMaterialEditingLibrary::ConnectMaterialExpressions(
					Rec.SourceExpr, SourceOutputName, NewExpr, Rec.InputName);
			}

			// Index fallback
			if (!bReconnected && Rec.InputIndex < NewInputCount)
			{
				FString SourceOutputName;
				if (Rec.SourceExpr->Outputs.IsValidIndex(Rec.SourceOutputIndex))
				{
					SourceOutputName = Rec.SourceExpr->Outputs[Rec.SourceOutputIndex].OutputName.ToString();
				}
				// Get the new input name at the same index
				FString NewInputName;
				for (const auto& Pair : NewInputNameToIndex)
				{
					if (Pair.Value == Rec.InputIndex)
					{
						NewInputName = Pair.Key;
						break;
					}
				}
				bReconnected = UMaterialEditingLibrary::ConnectMaterialExpressions(
					Rec.SourceExpr, SourceOutputName, NewExpr, NewInputName);
				if (bReconnected) bUsedFallback = true;
			}

			if (bReconnected)
			{
				auto RecJson = MakeShared<FJsonObject>();
				RecJson->SetStringField(TEXT("type"), TEXT("input"));
				RecJson->SetStringField(TEXT("old_input"), Rec.InputName);
				RecJson->SetStringField(TEXT("source"), Rec.SourceExpr->GetName());
				RecJson->SetBoolField(TEXT("name_match"), !bUsedFallback);
				ReconnectedArr.Add(MakeShared<FJsonValueObject>(RecJson));

				if (bUsedFallback)
				{
					WarningsArr.Add(MakeShared<FJsonValueString>(FString::Printf(
						TEXT("Input '%s' reconnected via index fallback (index %d) — new expression has no pin named '%s'"),
						*Rec.InputName, Rec.InputIndex, *Rec.InputName)));
				}
			}
			else
			{
				auto FailJson = MakeShared<FJsonObject>();
				FailJson->SetStringField(TEXT("type"), TEXT("input"));
				FailJson->SetStringField(TEXT("old_input"), Rec.InputName);
				FailJson->SetStringField(TEXT("source"), Rec.SourceExpr->GetName());
				FailedArr.Add(MakeShared<FJsonValueObject>(FailJson));
			}
		}

		// Reconnect outputs (consumers of old expr -> now consume new expr)
		for (const FRecordedOutput& Rec : OldOutputConsumers)
		{
			bool bReconnected = false;
			bool bUsedFallback = false;

			// Determine which output index on the new expression to use
			int32 NewOutputIndex = -1;

			// Try name match on output
			if (!Rec.OldOutputName.IsEmpty())
			{
				int32* MatchedIdx = NewOutputNameToIndex.Find(Rec.OldOutputName);
				if (MatchedIdx)
				{
					NewOutputIndex = *MatchedIdx;
				}
			}

			// Index fallback for output
			if (NewOutputIndex < 0 && Rec.OldOutputIndex < NewExpr->Outputs.Num())
			{
				NewOutputIndex = Rec.OldOutputIndex;
				if (!Rec.OldOutputName.IsEmpty()) bUsedFallback = true;
			}

			if (NewOutputIndex < 0)
			{
				NewOutputIndex = 0; // default to first output
				bUsedFallback = true;
			}

			FString NewOutputName;
			if (NewExpr->Outputs.IsValidIndex(NewOutputIndex))
			{
				NewOutputName = NewExpr->Outputs[NewOutputIndex].OutputName.ToString();
			}

			if (Rec.ConsumerExpr)
			{
				// Expression-to-expression connection
				bReconnected = UMaterialEditingLibrary::ConnectMaterialExpressions(
					NewExpr, NewOutputName, Rec.ConsumerExpr, Rec.ConsumerInputName);
			}
			else
			{
				// Material output connection
				bReconnected = UMaterialEditingLibrary::ConnectMaterialProperty(
					NewExpr, NewOutputName, Rec.MatProperty);
			}

			if (bReconnected)
			{
				auto RecJson = MakeShared<FJsonObject>();
				RecJson->SetStringField(TEXT("type"), TEXT("output"));
				RecJson->SetStringField(TEXT("consumer"), Rec.ConsumerExpr
					? Rec.ConsumerExpr->GetName() : Rec.ConsumerInputName);
				RecJson->SetBoolField(TEXT("name_match"), !bUsedFallback);
				ReconnectedArr.Add(MakeShared<FJsonValueObject>(RecJson));

				if (bUsedFallback)
				{
					WarningsArr.Add(MakeShared<FJsonValueString>(FString::Printf(
						TEXT("Output to '%s' reconnected via index fallback (output index %d)"),
						*Rec.ConsumerInputName, NewOutputIndex)));
				}
			}
			else
			{
				auto FailJson = MakeShared<FJsonObject>();
				FailJson->SetStringField(TEXT("type"), TEXT("output"));
				FailJson->SetStringField(TEXT("consumer"), Rec.ConsumerExpr
					? Rec.ConsumerExpr->GetName() : Rec.ConsumerInputName);
				FailedArr.Add(MakeShared<FJsonValueObject>(FailJson));
			}
		}
	}

	Mat->PreEditChange(nullptr);
	Mat->PostEditChange();
	GEditor->EndTransaction();

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetStringField(TEXT("old_class"), OldClassName);
	ResultJson->SetStringField(TEXT("new_class"), NewExpr->GetClass()->GetName());
	ResultJson->SetStringField(TEXT("new_name"), NewExpr->GetName());
	ResultJson->SetArrayField(TEXT("reconnected"), ReconnectedArr);
	ResultJson->SetArrayField(TEXT("failed_reconnections"), FailedArr);
	ResultJson->SetArrayField(TEXT("warnings"), WarningsArr);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: get_expression_pin_info
// Params: { "class_name": "MaterialExpressionMultiply" }
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::GetExpressionPinInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString ClassName = Params->GetStringField(TEXT("class_name"));

	// Resolve class name with shorthand support
	FString FullClassName = ClassName;
	if (!FullClassName.StartsWith(TEXT("MaterialExpression")))
	{
		FullClassName = TEXT("MaterialExpression") + FullClassName;
	}
	UClass* ExprClass = FindFirstObject<UClass>(*FullClassName, EFindFirstObjectOptions::NativeFirst);
	if (!ExprClass)
	{
		ExprClass = FindFirstObject<UClass>(*(TEXT("U") + FullClassName), EFindFirstObjectOptions::NativeFirst);
	}
	if (!ExprClass || !ExprClass->IsChildOf(UMaterialExpression::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Expression class '%s' not found. Use list_expression_classes to see valid options."), *ClassName));
	}

	// Create temporary instance to read pin info
	UMaterialExpression* TempExpr = NewObject<UMaterialExpression>(GetTransientPackage(), ExprClass, NAME_None, RF_Transient);
	if (!TempExpr)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to create temp instance of '%s'"), *ClassName));
	}

	// Read inputs
	TArray<TSharedPtr<FJsonValue>> InputsArray;
	for (int32 i = 0; ; ++i)
	{
		FExpressionInput* Input = TempExpr->GetInput(i);
		if (!Input) break;

		auto InputJson = MakeShared<FJsonObject>();
		InputJson->SetNumberField(TEXT("index"), i);
		FName RawName = TempExpr->GetInputName(i);
		FName ShortName = UMaterialGraphNode::GetShortenPinName(RawName);
		InputJson->SetStringField(TEXT("name"), ShortName.IsNone() ? TEXT("") : ShortName.ToString());
		InputsArray.Add(MakeShared<FJsonValueObject>(InputJson));
	}

	// Read outputs
	TArray<TSharedPtr<FJsonValue>> OutputsArray;
	for (int32 i = 0; i < TempExpr->Outputs.Num(); ++i)
	{
		auto OutputJson = MakeShared<FJsonObject>();
		OutputJson->SetNumberField(TEXT("index"), i);
		OutputJson->SetStringField(TEXT("name"), TempExpr->Outputs[i].OutputName.ToString());
		OutputsArray.Add(MakeShared<FJsonValueObject>(OutputJson));
	}

	TempExpr->MarkAsGarbage();

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("class"), ExprClass->GetName());
	ResultJson->SetStringField(TEXT("display_name"), ExprClass->GetDisplayNameText().ToString());
	ResultJson->SetArrayField(TEXT("inputs"), InputsArray);
	ResultJson->SetNumberField(TEXT("input_count"), InputsArray.Num());
	ResultJson->SetArrayField(TEXT("outputs"), OutputsArray);
	ResultJson->SetNumberField(TEXT("output_count"), OutputsArray.Num());

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: rename_expression
// Params: { "asset_path", "expression_name", "new_desc" }
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::RenameExpression(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString ExprName = Params->GetStringField(TEXT("expression_name"));
	FString NewDesc = Params->GetStringField(TEXT("new_desc"));

	UMaterial* Mat = LoadBaseMaterial(AssetPath);
	if (!Mat)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load base material at '%s'"), *AssetPath));
	}

	UMaterialExpression* TargetExpr = nullptr;
	for (const TObjectPtr<UMaterialExpression>& Expr : Mat->GetExpressions())
	{
		if (Expr && Expr->GetName() == ExprName)
		{
			TargetExpr = Expr;
			break;
		}
	}

	if (!TargetExpr)
	{
		TArray<FString> AvailableNames;
		for (const TObjectPtr<UMaterialExpression>& Expr : Mat->GetExpressions())
		{
			if (Expr) AvailableNames.Add(Expr->GetName());
		}
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Expression '%s' not found. Available: %s"),
			*ExprName, *FString::Join(AvailableNames, TEXT(", "))));
	}

	FString OldDesc = TargetExpr->Desc;

	GEditor->BeginTransaction(FText::FromString(TEXT("RenameExpression")));
	TargetExpr->Modify();
	TargetExpr->Desc = NewDesc;
	Mat->PreEditChange(nullptr);
	Mat->PostEditChange();
	GEditor->EndTransaction();

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetStringField(TEXT("expression_name"), ExprName);
	ResultJson->SetStringField(TEXT("old_desc"), OldDesc);
	ResultJson->SetStringField(TEXT("new_desc"), NewDesc);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: list_material_instances
// Params: { "parent_path", "recursive"? }
// Uses IAssetRegistry::GetReferencers to find assets that reference the parent.
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::ListMaterialInstances(const TSharedPtr<FJsonObject>& Params)
{
	FString ParentPath = Params->GetStringField(TEXT("parent_path"));
	bool bRecursive = Params->HasField(TEXT("recursive")) ? Params->GetBoolField(TEXT("recursive")) : true;

	// Verify parent exists
	UObject* ParentAsset = UEditorAssetLibrary::LoadAsset(ParentPath);
	if (!ParentAsset || !Cast<UMaterialInterface>(ParentAsset))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to load material interface at '%s'"), *ParentPath));
	}

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	struct FInstanceInfo
	{
		FString Path;
		FString Name;
		int32 Depth;
	};
	TArray<FInstanceInfo> AllInstances;

	// BFS to find instances
	struct FQueueEntry
	{
		FName PackageName;
		int32 Depth;
	};

	TArray<FQueueEntry> Queue;
	TSet<FName> Visited;

	// Extract package name from asset path
	FString PackageName = ParentPath;
	Queue.Add({ FName(*PackageName), 1 });
	Visited.Add(FName(*PackageName));

	while (Queue.Num() > 0)
	{
		FQueueEntry Current = Queue[0];
		Queue.RemoveAt(0);

		TArray<FAssetIdentifier> Referencers;
		AR.GetReferencers(FAssetIdentifier(Current.PackageName), Referencers);

		for (const FAssetIdentifier& Ref : Referencers)
		{
			FName RefPackage = Ref.PackageName;
			if (RefPackage.IsNone() || Visited.Contains(RefPackage))
			{
				continue;
			}

			// Check if this referencer is a MaterialInstanceConstant
			TArray<FAssetData> AssetsInPackage;
			AR.GetAssetsByPackageName(RefPackage, AssetsInPackage, true);

			for (const FAssetData& AssetData : AssetsInPackage)
			{
				// Check if it's a MIC or subclass (MaterialInstanceConstant is the primary persistent type)
				FTopLevelAssetPath ClassPath = AssetData.AssetClassPath;
				FString ClassStr = ClassPath.GetAssetName().ToString();
				if (ClassStr == TEXT("MaterialInstanceConstant"))
				{
					FInstanceInfo Info;
					Info.Path = AssetData.GetObjectPathString();
					// Clean up the path: remove the .AssetName suffix if present
					if (Info.Path.Contains(TEXT(".")))
					{
						Info.Path = AssetData.PackageName.ToString();
					}
					Info.Name = AssetData.AssetName.ToString();
					Info.Depth = Current.Depth;
					AllInstances.Add(Info);

					// Queue for recursive traversal
					if (bRecursive && !Visited.Contains(RefPackage))
					{
						Queue.Add({ RefPackage, Current.Depth + 1 });
					}
				}
			}

			Visited.Add(RefPackage);
		}
	}

	// Sort by depth then name
	AllInstances.Sort([](const FInstanceInfo& A, const FInstanceInfo& B)
	{
		if (A.Depth != B.Depth) return A.Depth < B.Depth;
		return A.Name < B.Name;
	});

	TArray<TSharedPtr<FJsonValue>> InstancesArray;
	for (const FInstanceInfo& Info : AllInstances)
	{
		auto InstJson = MakeShared<FJsonObject>();
		InstJson->SetStringField(TEXT("path"), Info.Path);
		InstJson->SetStringField(TEXT("name"), Info.Name);
		InstJson->SetNumberField(TEXT("depth"), Info.Depth);
		InstancesArray.Add(MakeShared<FJsonValueObject>(InstJson));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("parent"), ParentPath);
	ResultJson->SetNumberField(TEXT("instance_count"), InstancesArray.Num());
	ResultJson->SetArrayField(TEXT("instances"), InstancesArray);
	ResultJson->SetBoolField(TEXT("recursive"), bRecursive);
	// UX #1: clarify that unsaved assets won't appear
	ResultJson->SetStringField(TEXT("note"), TEXT("Only includes assets saved to disk and registered with the Asset Registry"));

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Shared Helper: BuildGraphFromSpec
// Handles node creation (standard + Custom HLSL) and connection wiring.
// Used by both BuildMaterialGraph and BuildFunctionGraph.
// ============================================================================

void FMonolithMaterialActions::BuildGraphFromSpec(
	const TSharedPtr<FJsonObject>& Spec,
	const FCreateExpressionFunc& CreateExpressionFunc,
	TMap<FString, UMaterialExpression*>& IdToExpr,
	int32& OutNodesCreated,
	int32& OutConnectionsMade,
	TArray<TSharedPtr<FJsonValue>>& OutErrors)
{
	OutNodesCreated = 0;
	OutConnectionsMade = 0;

	// Phase 1 — Standard nodes
	const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("nodes"), NodesArray))
	{
		for (const TSharedPtr<FJsonValue>& NodeVal : *NodesArray)
		{
			const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
			if (!NodeVal || !NodeVal->TryGetObject(NodeObjPtr) || !NodeObjPtr)
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& NodeObj = *NodeObjPtr;

			FString Id = NodeObj->GetStringField(TEXT("id"));
			// BUG #3: also read optional user-provided name alias for connection resolution
			FString UserName;
			NodeObj->TryGetStringField(TEXT("name"), UserName);
			FString ShortClass = NodeObj->GetStringField(TEXT("class"));
			if (ShortClass.IsEmpty())
			{
				ShortClass = NodeObj->GetStringField(TEXT("type"));
			}
			if (ShortClass.IsEmpty())
			{
				auto ErrJson = MakeShared<FJsonObject>();
				ErrJson->SetStringField(TEXT("node_id"), Id);
				ErrJson->SetStringField(TEXT("error"), TEXT("Node spec missing required 'class' field"));
				OutErrors.Add(MakeShared<FJsonValueObject>(ErrJson));
				continue;
			}

			FString FullClassName = ShortClass;
			if (!ShortClass.StartsWith(TEXT("MaterialExpression")))
			{
				FullClassName = FString::Printf(TEXT("MaterialExpression%s"), *ShortClass);
			}

			// Try multiple lookup strategies for the expression class
			UClass* ExprClass = FindFirstObject<UClass>(*FullClassName, EFindFirstObjectOptions::NativeFirst);
			if (!ExprClass)
			{
				FString UClassName = FString::Printf(TEXT("U%s"), *FullClassName);
				ExprClass = FindFirstObject<UClass>(*UClassName, EFindFirstObjectOptions::NativeFirst);
			}
			if (!ExprClass)
			{
				auto ErrJson = MakeShared<FJsonObject>();
				ErrJson->SetStringField(TEXT("node_id"), Id);
				ErrJson->SetStringField(TEXT("error"), FString::Printf(TEXT("Class '%s' not found"), *FullClassName));
				OutErrors.Add(MakeShared<FJsonValueObject>(ErrJson));
				continue;
			}

			int32 PosX = 0, PosY = 0;
			const TArray<TSharedPtr<FJsonValue>>* PosArray = nullptr;
			if (NodeObj->TryGetArrayField(TEXT("pos"), PosArray) && PosArray->Num() >= 2)
			{
				PosX = static_cast<int32>((*PosArray)[0]->AsNumber());
				PosY = static_cast<int32>((*PosArray)[1]->AsNumber());
			}
			else
			{
				// Also accept individual pos_x / pos_y fields (graph_spec uses this format)
				double TmpX = 0.0, TmpY = 0.0;
				NodeObj->TryGetNumberField(TEXT("pos_x"), TmpX);
				NodeObj->TryGetNumberField(TEXT("pos_y"), TmpY);
				PosX = static_cast<int32>(TmpX);
				PosY = static_cast<int32>(TmpY);
			}

			UMaterialExpression* NewExpr = CreateExpressionFunc(ExprClass, PosX, PosY);
			if (!NewExpr)
			{
				auto ErrJson = MakeShared<FJsonObject>();
				ErrJson->SetStringField(TEXT("node_id"), Id);
				ErrJson->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to create expression of class '%s'"), *FullClassName));
				OutErrors.Add(MakeShared<FJsonValueObject>(ErrJson));
				continue;
			}

			// Set properties — accept both "props" and "properties" as key names
			bool bSamplerTypeExplicitlySet = false;
			const TSharedPtr<FJsonObject>* PropsObjPtr = nullptr;
			if (!NodeObj->TryGetObjectField(TEXT("props"), PropsObjPtr))
			{
				NodeObj->TryGetObjectField(TEXT("properties"), PropsObjPtr);
			}
			if (PropsObjPtr)
			{
				const TSharedPtr<FJsonObject>& PropsObj = *PropsObjPtr;
				for (const auto& Pair : PropsObj->Values)
				{
					if (Pair.Key == TEXT("SamplerType"))
					{
						bSamplerTypeExplicitlySet = true;
					}
					FProperty* Prop = NewExpr->GetClass()->FindPropertyByName(*Pair.Key);
					if (!Prop)
					{
						auto ErrJson = MakeShared<FJsonObject>();
						ErrJson->SetStringField(TEXT("node_id"), Id);
						ErrJson->SetStringField(TEXT("warning"), FString::Printf(TEXT("Property '%s' not found on '%s'"), *Pair.Key, *FullClassName));
						OutErrors.Add(MakeShared<FJsonValueObject>(ErrJson));
						continue;
					}

					// Derive a string representation regardless of JSON value type
					FString ValueStr;
					switch (Pair.Value->Type)
					{
						case EJson::Number:  ValueStr = FString::SanitizeFloat(Pair.Value->AsNumber()); break;
						case EJson::Boolean: ValueStr = Pair.Value->AsBool() ? TEXT("true") : TEXT("false"); break;
						default:             ValueStr = Pair.Value->AsString(); break;
					}

					void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(NewExpr);

					if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
					{
						FloatProp->SetPropertyValue(ValuePtr, static_cast<float>(Pair.Value->AsNumber()));
					}
					else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
					{
						DoubleProp->SetPropertyValue(ValuePtr, Pair.Value->AsNumber());
					}
					else if (FIntProperty* IntProp = CastField<FIntProperty>(Prop))
					{
						IntProp->SetPropertyValue(ValuePtr, static_cast<int32>(Pair.Value->AsNumber()));
					}
					else if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
					{
						bool bVal = ValueStr.Equals(TEXT("true"), ESearchCase::IgnoreCase) || ValueStr == TEXT("1");
						BoolProp->SetPropertyValue(ValuePtr, bVal);
					}
					else if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
					{
						// ValueStr is a plain asset path like "/Game/Textures/T_Foo" — load and assign directly.
						// StaticLoadObject works with either bare paths or full class-prefix reference notation.
						UObject* LoadedObj = StaticLoadObject(ObjProp->PropertyClass, nullptr, *ValueStr);
						if (LoadedObj)
						{
							ObjProp->SetObjectPropertyValue(ValuePtr, LoadedObj);
						}
						else
						{
							auto ErrJson = MakeShared<FJsonObject>();
							ErrJson->SetStringField(TEXT("node_id"), Id);
							ErrJson->SetStringField(TEXT("warning"), FString::Printf(
								TEXT("Could not load asset '%s' for property '%s' on '%s'"),
								*ValueStr, *Pair.Key, *FullClassName));
							OutErrors.Add(MakeShared<FJsonValueObject>(ErrJson));
						}
					}
					else if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
					{
						// Covers TEnumAsByte<EFoo> — try enum name lookup first, fall back to integer
						if (ByteProp->Enum)
						{
							int64 EnumVal = ByteProp->Enum->GetValueByNameString(ValueStr);
							if (EnumVal == INDEX_NONE)
							{
								EnumVal = FCString::Atoi(*ValueStr);
							}
							ByteProp->SetPropertyValue(ValuePtr, static_cast<uint8>(EnumVal));
						}
						else
						{
							ByteProp->SetPropertyValue(ValuePtr, static_cast<uint8>(FCString::Atoi(*ValueStr)));
						}
					}
					else if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
					{
						// Scoped enum (UENUM class) — try enum name lookup first, fall back to integer
						UEnum* Enum = EnumProp->GetEnum();
						FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
						if (Enum && UnderlyingProp)
						{
							int64 EnumVal = Enum->GetValueByNameString(ValueStr);
							if (EnumVal == INDEX_NONE)
							{
								EnumVal = FCString::Atoi64(*ValueStr);
							}
							UnderlyingProp->SetIntPropertyValue(ValuePtr, EnumVal);
						}
						else
						{
							Prop->ImportText_Direct(*ValueStr, ValuePtr, NewExpr, PPF_None);
						}
					}
					else
					{
						Prop->ImportText_Direct(*ValueStr, ValuePtr, NewExpr, PPF_None);
					}
				}
			}

			// Auto-detect SamplerType from texture if user didn't set it explicitly
			if (!bSamplerTypeExplicitlySet)
			{
				if (UMaterialExpressionTextureBase* TextureExpr = Cast<UMaterialExpressionTextureBase>(NewExpr))
				{
					TextureExpr->AutoSetSampleType();
				}
			}

			// Bug fix: if Id is empty (node used "name" instead of "id"), fall back to UserName or the
			// engine-assigned expression name so we don't register a blank-key entry in IdToExpr.
			// A blank key causes any to_property connection (whose ToId resolves to "") to accidentally
			// match this expression and create a spurious self-connection.
			FString LookupId = !Id.IsEmpty() ? Id : (!UserName.IsEmpty() ? UserName : NewExpr->GetName());
			IdToExpr.Add(LookupId, NewExpr);
			// Also register the name alias if it differs, so connections can reference by either id or name
			if (!UserName.IsEmpty() && UserName != LookupId)
			{
				IdToExpr.Add(UserName, NewExpr);
			}
			OutNodesCreated++;
		}
	}
	else
	{
		if (Spec->HasField(TEXT("nodes")))
		{
			auto ErrJson = MakeShared<FJsonObject>();
			ErrJson->SetStringField(TEXT("error"), TEXT("'nodes' field is not a JSON array"));
			OutErrors.Add(MakeShared<FJsonValueObject>(ErrJson));
		}
	}

	// Phase 2 — Custom HLSL nodes
	const TArray<TSharedPtr<FJsonValue>>* CustomArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("custom_hlsl_nodes"), CustomArray))
	{
		for (const TSharedPtr<FJsonValue>& CustomVal : *CustomArray)
		{
			const TSharedPtr<FJsonObject>* CustomObjPtr = nullptr;
			if (!CustomVal || !CustomVal->TryGetObject(CustomObjPtr) || !CustomObjPtr)
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& CustomObj = *CustomObjPtr;

			FString Id = CustomObj->GetStringField(TEXT("id"));
			// BUG #3: also read optional user-provided name alias
			FString CustomUserName;
			CustomObj->TryGetStringField(TEXT("name"), CustomUserName);

			int32 PosX = 0, PosY = 0;
			const TArray<TSharedPtr<FJsonValue>>* PosArray = nullptr;
			if (CustomObj->TryGetArrayField(TEXT("pos"), PosArray) && PosArray->Num() >= 2)
			{
				PosX = static_cast<int32>((*PosArray)[0]->AsNumber());
				PosY = static_cast<int32>((*PosArray)[1]->AsNumber());
			}

			UMaterialExpression* BaseExpr = CreateExpressionFunc(UMaterialExpressionCustom::StaticClass(), PosX, PosY);
			UMaterialExpressionCustom* CustomExpr = Cast<UMaterialExpressionCustom>(BaseExpr);
			if (!CustomExpr)
			{
				auto ErrJson = MakeShared<FJsonObject>();
				ErrJson->SetStringField(TEXT("node_id"), Id);
				ErrJson->SetStringField(TEXT("error"), TEXT("Failed to create Custom HLSL expression"));
				OutErrors.Add(MakeShared<FJsonValueObject>(ErrJson));
				continue;
			}

			CustomExpr->Code = CustomObj->GetStringField(TEXT("code"));
			// Unescape double-encoded newlines from JSON string serialization
			CustomExpr->Code.ReplaceInline(TEXT("\\n"), TEXT("\n"), ESearchCase::CaseSensitive);
			CustomExpr->Code.ReplaceInline(TEXT("\\t"), TEXT("\t"), ESearchCase::CaseSensitive);
			if (CustomObj->HasField(TEXT("description")))
			{
				CustomExpr->Description = CustomObj->GetStringField(TEXT("description"));
			}
			if (CustomObj->HasField(TEXT("output_type")))
			{
				CustomExpr->OutputType = ParseCustomOutputType(CustomObj->GetStringField(TEXT("output_type")));
			}

			const TArray<TSharedPtr<FJsonValue>>* InputsArray = nullptr;
			if (CustomObj->TryGetArrayField(TEXT("inputs"), InputsArray))
			{
				CustomExpr->Inputs.Empty();
				for (const TSharedPtr<FJsonValue>& InputVal : *InputsArray)
				{
					const TSharedPtr<FJsonObject>* InputObjPtr = nullptr;
					if (InputVal && InputVal->TryGetObject(InputObjPtr) && InputObjPtr)
					{
						FCustomInput NewInput;
						NewInput.InputName = *(*InputObjPtr)->GetStringField(TEXT("name"));
						CustomExpr->Inputs.Add(NewInput);
					}
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* AddOutputsArray = nullptr;
			if (CustomObj->TryGetArrayField(TEXT("additional_outputs"), AddOutputsArray))
			{
				CustomExpr->AdditionalOutputs.Empty();
				for (const TSharedPtr<FJsonValue>& OutVal : *AddOutputsArray)
				{
					const TSharedPtr<FJsonObject>* OutObjPtr = nullptr;
					if (OutVal && OutVal->TryGetObject(OutObjPtr) && OutObjPtr)
					{
						FCustomOutput NewOutput;
						NewOutput.OutputName = *(*OutObjPtr)->GetStringField(TEXT("name"));
						if ((*OutObjPtr)->HasField(TEXT("type")))
						{
							NewOutput.OutputType = ParseCustomOutputType((*OutObjPtr)->GetStringField(TEXT("type")));
						}
						CustomExpr->AdditionalOutputs.Add(NewOutput);
					}
				}
			}

			CustomExpr->RebuildOutputs();
			// Mirror Phase 1 empty-key guard: never register a blank key in IdToExpr
			FString CustomLookupId = !Id.IsEmpty() ? Id : (!CustomUserName.IsEmpty() ? CustomUserName : CustomExpr->GetName());
			IdToExpr.Add(CustomLookupId, CustomExpr);
			// Register name alias if it differs from lookup id
			if (!CustomUserName.IsEmpty() && CustomUserName != CustomLookupId)
			{
				IdToExpr.Add(CustomUserName, CustomExpr);
			}
			OutNodesCreated++;
		}
	}

	// Phase 3 — Wire connections between expressions
	const TArray<TSharedPtr<FJsonValue>>* ConnsArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("connections"), ConnsArray))
	{
		for (const TSharedPtr<FJsonValue>& ConnVal : *ConnsArray)
		{
			const TSharedPtr<FJsonObject>* ConnObjPtr = nullptr;
			if (!ConnVal || !ConnVal->TryGetObject(ConnObjPtr) || !ConnObjPtr)
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& ConnObj = *ConnObjPtr;

			// Accept both "from"/"to" (export format) and "from_expression"/"to_expression" (connect_expressions style)
			FString FromId = ConnObj->HasField(TEXT("from")) ? ConnObj->GetStringField(TEXT("from"))
			               : ConnObj->HasField(TEXT("from_expression")) ? ConnObj->GetStringField(TEXT("from_expression")) : TEXT("");
			FString ToId   = ConnObj->HasField(TEXT("to")) ? ConnObj->GetStringField(TEXT("to"))
			               : ConnObj->HasField(TEXT("to_expression")) ? ConnObj->GetStringField(TEXT("to_expression")) : TEXT("");
			// Accept both "from_pin"/"to_pin" (export format) and "from_output"/"to_input" (connect_expressions style)
			FString FromPin = ConnObj->HasField(TEXT("from_pin")) ? ConnObj->GetStringField(TEXT("from_pin"))
			                : ConnObj->HasField(TEXT("from_output")) ? ConnObj->GetStringField(TEXT("from_output")) : TEXT("");
			FString ToPin   = ConnObj->HasField(TEXT("to_pin")) ? ConnObj->GetStringField(TEXT("to_pin"))
			                : ConnObj->HasField(TEXT("to_input")) ? ConnObj->GetStringField(TEXT("to_input")) : TEXT("");
			ToPin = NormalizeInputPinName(ToPin);

			UMaterialExpression** FromPtr = IdToExpr.Find(FromId);
			UMaterialExpression** ToPtr = IdToExpr.Find(ToId);

			// Check if this is a material-output connection (to_property) rather than
			// expression-to-expression. These must use ConnectMaterialProperty instead.
			FString ToPropName;
			if (ConnObj->TryGetStringField(TEXT("to_property"), ToPropName))
			{
				if (!FromPtr || !*FromPtr)
				{
					auto ErrJson = MakeShared<FJsonObject>();
					ErrJson->SetStringField(TEXT("connection"), FString::Printf(TEXT("%s -> [property:%s]"), *FromId, *ToPropName));
					ErrJson->SetStringField(TEXT("error"), FString::Printf(TEXT("Source node '%s' not found"), *FromId));
					OutErrors.Add(MakeShared<FJsonValueObject>(ErrJson));
					continue;
				}
				EMaterialProperty MatProp = ParseMaterialProperty(ToPropName);
				if (MatProp == MP_MAX)
				{
					auto ErrJson = MakeShared<FJsonObject>();
					ErrJson->SetStringField(TEXT("connection"), FString::Printf(TEXT("%s -> [property:%s]"), *FromId, *ToPropName));
					ErrJson->SetStringField(TEXT("error"), FString::Printf(TEXT("Unknown material property '%s'"), *ToPropName));
					OutErrors.Add(MakeShared<FJsonValueObject>(ErrJson));
					continue;
				}
				bool bConnected = UMaterialEditingLibrary::ConnectMaterialProperty(*FromPtr, FromPin, MatProp);
				if (bConnected)
				{
					OutConnectionsMade++;
				}
				else
				{
					auto ErrJson = MakeShared<FJsonObject>();
					ErrJson->SetStringField(TEXT("connection"), FString::Printf(TEXT("%s.%s -> [property:%s]"), *FromId, *FromPin, *ToPropName));
					ErrJson->SetStringField(TEXT("error"), TEXT("ConnectMaterialProperty returned false"));
					OutErrors.Add(MakeShared<FJsonValueObject>(ErrJson));
				}
				continue; // Skip expression-to-expression logic below
			}

			if (!FromPtr || !*FromPtr)
			{
				auto ErrJson = MakeShared<FJsonObject>();
				ErrJson->SetStringField(TEXT("connection"), FString::Printf(TEXT("%s -> %s"), *FromId, *ToId));
				ErrJson->SetStringField(TEXT("error"), FString::Printf(TEXT("Source node '%s' not found"), *FromId));
				OutErrors.Add(MakeShared<FJsonValueObject>(ErrJson));
				continue;
			}
			if (!ToPtr || !*ToPtr)
			{
				auto ErrJson = MakeShared<FJsonObject>();
				ErrJson->SetStringField(TEXT("connection"), FString::Printf(TEXT("%s -> %s"), *FromId, *ToId));
				ErrJson->SetStringField(TEXT("error"), FString::Printf(TEXT("Target node '%s' not found"), *ToId));
				OutErrors.Add(MakeShared<FJsonValueObject>(ErrJson));
				continue;
			}

			bool bConnected = UMaterialEditingLibrary::ConnectMaterialExpressions(*FromPtr, FromPin, *ToPtr, ToPin);
			if (bConnected)
			{
				OutConnectionsMade++;
			}
			else
			{
				auto ErrJson = MakeShared<FJsonObject>();
				ErrJson->SetStringField(TEXT("connection"), FString::Printf(TEXT("%s.%s -> %s.%s"), *FromId, *FromPin, *ToId, *ToPin));
				ErrJson->SetStringField(TEXT("error"), TEXT("ConnectMaterialExpressions returned false"));
				OutErrors.Add(MakeShared<FJsonValueObject>(ErrJson));
			}
		}
	}
}

// ============================================================================
// Action: create_material_function
// Params: { "asset_path", "description"?, "expose_to_library"?, "library_categories"?, "type"? }
// Creates a new UMaterialFunction, UMaterialFunctionMaterialLayer, or
// UMaterialFunctionMaterialLayerBlend asset depending on the "type" param.
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::CreateMaterialFunction(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	// Extract package path and asset name
	FString PackagePath, AssetName;
	int32 LastSlash;
	if (AssetPath.FindLastChar('/', LastSlash))
	{
		PackagePath = AssetPath.Left(LastSlash);
		AssetName = AssetPath.Mid(LastSlash + 1);
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("Invalid asset path — must contain at least one '/' (e.g. /Game/Materials/Functions/MF_MyFunc)"));
	}

	if (AssetName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Asset name is empty"));
	}

	// Check if asset already exists
	UObject* Existing = UEditorAssetLibrary::LoadAsset(AssetPath);
	if (Existing)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));
	}

	// Resolve function class from type param
	FString FuncType = TEXT("MaterialFunction");
	Params->TryGetStringField(TEXT("type"), FuncType);

	UClass* FuncClass = UMaterialFunction::StaticClass();
	if (FuncType == TEXT("MaterialLayer"))
	{
		FuncClass = UMaterialFunctionMaterialLayer::StaticClass();
	}
	else if (FuncType == TEXT("MaterialLayerBlend"))
	{
		FuncClass = UMaterialFunctionMaterialLayerBlend::StaticClass();
	}
	else if (FuncType != TEXT("MaterialFunction"))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown function type '%s' — valid values: MaterialFunction, MaterialLayer, MaterialLayerBlend"), *FuncType));
	}

	// Create package and material function
	UPackage* Pkg = CreatePackage(*AssetPath);
	if (!Pkg)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at '%s'"), *AssetPath));
	}

	UMaterialFunction* NewFunc = Cast<UMaterialFunction>(NewObject<UObject>(Pkg, FuncClass, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional));
	if (!NewFunc)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create UMaterialFunction object"));
	}

	// Set optional properties
	if (Params->HasField(TEXT("description")))
	{
		NewFunc->Description = Params->GetStringField(TEXT("description"));
	}

	bool bExposeToLibrary = Params->HasField(TEXT("expose_to_library"))
		? Params->GetBoolField(TEXT("expose_to_library"))
		: true;
	NewFunc->bExposeToLibrary = bExposeToLibrary;

	// LibraryCategories was renamed to LibraryCategoriesText (TArray<FString> → TArray<FText>) in UE 5.x
	const TArray<TSharedPtr<FJsonValue>>* CategoriesArray = nullptr;
	if (Params->TryGetArrayField(TEXT("library_categories"), CategoriesArray))
	{
		NewFunc->LibraryCategoriesText.Empty();
		for (const TSharedPtr<FJsonValue>& CatVal : *CategoriesArray)
		{
			if (CatVal)
			{
				NewFunc->LibraryCategoriesText.Add(FText::FromString(CatVal->AsString()));
			}
		}
	}

	// Register with asset registry
	FAssetRegistryModule::AssetCreated(NewFunc);
	Pkg->MarkPackageDirty();

	// Save to disk
	FString PackageFilename = FPackageName::LongPackageNameToFilename(AssetPath, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	UPackage::SavePackage(Pkg, NewFunc, *PackageFilename, SaveArgs);

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), NewFunc->GetPathName());
	ResultJson->SetStringField(TEXT("asset_name"), AssetName);
	ResultJson->SetStringField(TEXT("type"), FuncType);
	ResultJson->SetStringField(TEXT("description"), NewFunc->Description);
	ResultJson->SetBoolField(TEXT("expose_to_library"), bExposeToLibrary);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: build_function_graph
// Params: { "asset_path", "graph_spec": { inputs, outputs, nodes, custom_hlsl_nodes, connections }, "clear_existing"? }
// Builds a material function graph from JSON spec. Same schema as build_material_graph
// but with function inputs[] and outputs[] definitions.
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::BuildFunctionGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	bool bClearExisting = Params->HasField(TEXT("clear_existing")) ? Params->GetBoolField(TEXT("clear_existing")) : false;

	// Load the material function
	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	if (!LoadedAsset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load asset at '%s'"), *AssetPath));
	}
	UMaterialFunction* Func = Cast<UMaterialFunction>(LoadedAsset);
	if (!Func)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset '%s' is not a UMaterialFunction"), *AssetPath));
	}

	// Parse graph_spec (object or JSON string)
	TSharedPtr<FJsonObject> Spec;
	if (Params->HasTypedField<EJson::Object>(TEXT("graph_spec")))
	{
		Spec = Params->GetObjectField(TEXT("graph_spec"));
	}
	else if (Params->HasTypedField<EJson::String>(TEXT("graph_spec")))
	{
		FString GraphSpecJson = Params->GetStringField(TEXT("graph_spec"));
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(GraphSpecJson);
		if (!FJsonSerializer::Deserialize(Reader, Spec) || !Spec.IsValid())
		{
			return FMonolithActionResult::Error(TEXT("Failed to parse graph_spec JSON string"));
		}
	}
	else
	{
		return FMonolithActionResult::Error(TEXT("Missing 'graph_spec' parameter"));
	}

	TArray<TSharedPtr<FJsonValue>> ErrorsArray;

	GEditor->BeginTransaction(FText::FromString(TEXT("BuildFunctionGraph")));
	Func->Modify();

	if (bClearExisting)
	{
		// Copy to local array to avoid iterator invalidation during deletion
		TArray<UMaterialExpression*> ToDelete;
		for (const TObjectPtr<UMaterialExpression>& Expr : Func->GetExpressions())
		{
			if (Expr)
			{
				ToDelete.Add(Expr);
			}
		}
		for (UMaterialExpression* Expr : ToDelete)
		{
			UMaterialEditingLibrary::DeleteMaterialExpression(nullptr, Expr);
		}
	}

	TMap<FString, UMaterialExpression*> IdToExpr;

	// Seed remap table with pre-existing expressions
	for (const TObjectPtr<UMaterialExpression>& Expr : Func->GetExpressions())
	{
		if (Expr)
		{
			IdToExpr.Add(Expr->GetName(), Expr.Get());
		}
	}

	// Create expression lambda that uses CreateMaterialExpressionInFunction
	FCreateExpressionFunc CreateFunc = [Func](UClass* ExprClass, int32 PosX, int32 PosY) -> UMaterialExpression*
	{
		return UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Func, ExprClass, PosX, PosY);
	};

	int32 NodesCreated = 0;
	int32 ConnectionsMade = 0;
	int32 InputCount = 0;
	int32 OutputCount = 0;

	// Phase 0a — Create FunctionInput nodes from spec
	const TArray<TSharedPtr<FJsonValue>>* FuncInputsArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("inputs"), FuncInputsArray))
	{
		for (const TSharedPtr<FJsonValue>& InputVal : *FuncInputsArray)
		{
			const TSharedPtr<FJsonObject>* InputObjPtr = nullptr;
			if (!InputVal || !InputVal->TryGetObject(InputObjPtr) || !InputObjPtr)
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& InputObj = *InputObjPtr;

			FString InputName = InputObj->GetStringField(TEXT("name"));
			FString InputId = InputObj->HasField(TEXT("id")) ? InputObj->GetStringField(TEXT("id")) : InputName;

			// Position: spread inputs vertically, allow override
			int32 PosX = -400;
			int32 PosY = InputCount * 200;
			const TArray<TSharedPtr<FJsonValue>>* PosArray = nullptr;
			if (InputObj->TryGetArrayField(TEXT("pos"), PosArray) && PosArray->Num() >= 2)
			{
				PosX = static_cast<int32>((*PosArray)[0]->AsNumber());
				PosY = static_cast<int32>((*PosArray)[1]->AsNumber());
			}

			UMaterialExpression* BaseExpr = UMaterialEditingLibrary::CreateMaterialExpressionInFunction(
				Func, UMaterialExpressionFunctionInput::StaticClass(), PosX, PosY);
			UMaterialExpressionFunctionInput* FuncInput = Cast<UMaterialExpressionFunctionInput>(BaseExpr);
			if (!FuncInput)
			{
				auto ErrJson = MakeShared<FJsonObject>();
				ErrJson->SetStringField(TEXT("input"), InputName);
				ErrJson->SetStringField(TEXT("error"), TEXT("Failed to create FunctionInput expression"));
				ErrorsArray.Add(MakeShared<FJsonValueObject>(ErrJson));
				continue;
			}

			FuncInput->InputName = *InputName;

			// Parse input type via StaticEnum
			if (InputObj->HasField(TEXT("type")))
			{
				FString TypeStr = InputObj->GetStringField(TEXT("type"));

				// UX #2: normalize type shorthands before enum lookup
				if (TypeStr.Equals(TEXT("float"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
					TypeStr = TEXT("FunctionInput_Scalar");
				else if (TypeStr.Equals(TEXT("float2"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("vector2"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("vec2"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("Vector2"), ESearchCase::IgnoreCase))
					TypeStr = TEXT("FunctionInput_Vector2");
				else if (TypeStr.Equals(TEXT("float3"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("vector3"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("vec3"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("Vector3"), ESearchCase::IgnoreCase))
					TypeStr = TEXT("FunctionInput_Vector3");
				else if (TypeStr.Equals(TEXT("float4"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("vector4"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("vec4"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("Vector4"), ESearchCase::IgnoreCase))
					TypeStr = TEXT("FunctionInput_Vector4");
				else if (TypeStr.Equals(TEXT("bool"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("staticbool"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("StaticBool"), ESearchCase::IgnoreCase))
					TypeStr = TEXT("FunctionInput_StaticBool");
				else if (TypeStr.Equals(TEXT("texture2d"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("Texture2D"), ESearchCase::IgnoreCase))
					TypeStr = TEXT("FunctionInput_Texture2D");
				else if (TypeStr.Equals(TEXT("texturecube"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("TextureCube"), ESearchCase::IgnoreCase))
					TypeStr = TEXT("FunctionInput_TextureCube");

				FString EnumError;
				EFunctionInputType InputType;
				if (ParseEnum<EFunctionInputType>(TypeStr, InputType, EnumError))
				{
					FuncInput->InputType = InputType;
				}
				else
				{
					// BUG #2: explicitly set Scalar so behavior matches the warning
					FuncInput->InputType = FunctionInput_Scalar;
					auto ErrJson = MakeShared<FJsonObject>();
					ErrJson->SetStringField(TEXT("input"), InputName);
					ErrJson->SetStringField(TEXT("warning"), FString::Printf(TEXT("input type: %s — defaulting to Scalar"), *EnumError));
					ErrorsArray.Add(MakeShared<FJsonValueObject>(ErrJson));
				}
			}

			if (InputObj->HasField(TEXT("sort_priority")))
			{
				FuncInput->SortPriority = static_cast<int32>(InputObj->GetNumberField(TEXT("sort_priority")));
			}
			else
			{
				FuncInput->SortPriority = InputCount;
			}

			if (InputObj->HasField(TEXT("description")))
			{
				FuncInput->Description = InputObj->GetStringField(TEXT("description"));
			}

			if (InputObj->HasField(TEXT("preview_value")))
			{
				FuncInput->bUsePreviewValueAsDefault = true;
				// Preview value is connected via the Preview input pin, not set directly as a number.
				// The user should provide a preview_value node in the nodes array and wire it.
			}

			IdToExpr.Add(InputId, FuncInput);
			InputCount++;
			NodesCreated++;
		}
	}

	// Phase 0b — Create FunctionOutput nodes from spec
	const TArray<TSharedPtr<FJsonValue>>* FuncOutputsArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("outputs"), FuncOutputsArray))
	{
		for (const TSharedPtr<FJsonValue>& OutputVal : *FuncOutputsArray)
		{
			const TSharedPtr<FJsonObject>* OutputObjPtr = nullptr;
			if (!OutputVal || !OutputVal->TryGetObject(OutputObjPtr) || !OutputObjPtr)
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& OutputObj = *OutputObjPtr;

			FString OutputName = OutputObj->GetStringField(TEXT("name"));
			FString OutputId = OutputObj->HasField(TEXT("id")) ? OutputObj->GetStringField(TEXT("id")) : OutputName;

			// Position: spread outputs vertically on the right, allow override
			int32 PosX = 400;
			int32 PosY = OutputCount * 200;
			const TArray<TSharedPtr<FJsonValue>>* PosArray = nullptr;
			if (OutputObj->TryGetArrayField(TEXT("pos"), PosArray) && PosArray->Num() >= 2)
			{
				PosX = static_cast<int32>((*PosArray)[0]->AsNumber());
				PosY = static_cast<int32>((*PosArray)[1]->AsNumber());
			}

			UMaterialExpression* BaseExpr = UMaterialEditingLibrary::CreateMaterialExpressionInFunction(
				Func, UMaterialExpressionFunctionOutput::StaticClass(), PosX, PosY);
			UMaterialExpressionFunctionOutput* FuncOutput = Cast<UMaterialExpressionFunctionOutput>(BaseExpr);
			if (!FuncOutput)
			{
				auto ErrJson = MakeShared<FJsonObject>();
				ErrJson->SetStringField(TEXT("output"), OutputName);
				ErrJson->SetStringField(TEXT("error"), TEXT("Failed to create FunctionOutput expression"));
				ErrorsArray.Add(MakeShared<FJsonValueObject>(ErrJson));
				continue;
			}

			FuncOutput->OutputName = *OutputName;

			if (OutputObj->HasField(TEXT("sort_priority")))
			{
				FuncOutput->SortPriority = static_cast<int32>(OutputObj->GetNumberField(TEXT("sort_priority")));
			}
			else
			{
				FuncOutput->SortPriority = OutputCount;
			}

			if (OutputObj->HasField(TEXT("description")))
			{
				FuncOutput->Description = OutputObj->GetStringField(TEXT("description"));
			}

			IdToExpr.Add(OutputId, FuncOutput);
			OutputCount++;
			NodesCreated++;
		}
	}

	// Phases 1-3 — Standard nodes, Custom HLSL, and connections (shared logic)
	int32 SharedNodesCreated = 0, SharedConnectionsMade = 0;
	BuildGraphFromSpec(Spec, CreateFunc, IdToExpr, SharedNodesCreated, SharedConnectionsMade, ErrorsArray);
	NodesCreated += SharedNodesCreated;
	ConnectionsMade += SharedConnectionsMade;

	// Update the function to propagate changes to dependent materials
	UMaterialEditingLibrary::UpdateMaterialFunction(Func, nullptr);

	GEditor->EndTransaction();

	auto ResultJson = MakeShared<FJsonObject>();
	// Separate errors from warnings
	int32 ErrorCount = 0, WarningCount = 0;
	for (const TSharedPtr<FJsonValue>& Entry : ErrorsArray)
	{
		if (Entry->AsObject()->HasField(TEXT("warning"))) WarningCount++;
		else ErrorCount++;
	}
	ResultJson->SetBoolField(TEXT("has_errors"), ErrorCount > 0);
	ResultJson->SetBoolField(TEXT("has_warnings"), WarningCount > 0);
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetNumberField(TEXT("nodes_created"), NodesCreated);
	ResultJson->SetNumberField(TEXT("connections_made"), ConnectionsMade);
	ResultJson->SetNumberField(TEXT("input_count"), InputCount);
	ResultJson->SetNumberField(TEXT("output_count"), OutputCount);

	auto IdMapJson = MakeShared<FJsonObject>();
	for (const auto& Pair : IdToExpr)
	{
		if (Pair.Value)
		{
			IdMapJson->SetStringField(Pair.Key, Pair.Value->GetName());
		}
	}
	ResultJson->SetObjectField(TEXT("id_to_name"), IdMapJson);
	ResultJson->SetArrayField(TEXT("errors"), ErrorsArray);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: get_function_info
// Params: { "asset_path" }
// Returns detailed info about a material function — inputs, outputs, description,
// categories, and full expression list.
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::GetFunctionInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	if (!LoadedAsset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load asset at '%s'"), *AssetPath));
	}

	// UMaterialFunction is the base class for layers and layer blends too
	UMaterialFunction* MatFunc = Cast<UMaterialFunction>(LoadedAsset);
	if (!MatFunc)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset '%s' is not a MaterialFunction"), *AssetPath));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetStringField(TEXT("description"), MatFunc->Description);
	ResultJson->SetBoolField(TEXT("expose_to_library"), MatFunc->bExposeToLibrary);

	// Determine specific type
	if (Cast<UMaterialFunctionMaterialLayer>(LoadedAsset))
	{
		ResultJson->SetStringField(TEXT("type"), TEXT("MaterialLayer"));
	}
	else if (Cast<UMaterialFunctionMaterialLayerBlend>(LoadedAsset))
	{
		ResultJson->SetStringField(TEXT("type"), TEXT("MaterialLayerBlend"));
	}
	else
	{
		ResultJson->SetStringField(TEXT("type"), TEXT("MaterialFunction"));
	}

	// Library categories (LibraryCategoriesText — renamed from LibraryCategories)
	TArray<TSharedPtr<FJsonValue>> CatsJson;
	for (const FText& Cat : MatFunc->LibraryCategoriesText)
	{
		CatsJson.Add(MakeShared<FJsonValueString>(Cat.ToString()));
	}
	ResultJson->SetArrayField(TEXT("library_categories"), CatsJson);

	// Walk expressions to collect inputs, outputs, and expression list
	TArray<TSharedPtr<FJsonValue>> InputsJson;
	TArray<TSharedPtr<FJsonValue>> OutputsJson;
	TArray<TSharedPtr<FJsonValue>> ExpressionsJson;

	const UEnum* InputTypeEnum = StaticEnum<EFunctionInputType>();

	TConstArrayView<TObjectPtr<UMaterialExpression>> FuncExprs = MatFunc->GetExpressions();
	for (const TObjectPtr<UMaterialExpression>& Expr : FuncExprs)
	{
		if (!Expr)
		{
			continue;
		}

		if (const auto* FuncInput = Cast<UMaterialExpressionFunctionInput>(Expr))
		{
			auto InputJson = MakeShared<FJsonObject>();
			InputJson->SetStringField(TEXT("name"), FuncInput->InputName.ToString());
			InputJson->SetStringField(TEXT("expression_name"), FuncInput->GetName());
			InputJson->SetNumberField(TEXT("sort_priority"), FuncInput->SortPriority);

			// Map InputType enum to readable string
			if (InputTypeEnum)
			{
				FString TypeStr = InputTypeEnum->GetNameStringByIndex(static_cast<int32>(FuncInput->InputType));
				TypeStr.RemoveFromStart(TEXT("FunctionInput_"));
				InputJson->SetStringField(TEXT("type"), TypeStr);
			}
			InputJson->SetStringField(TEXT("description"), FuncInput->Description);
			InputJson->SetBoolField(TEXT("use_preview_value_as_default"), FuncInput->bUsePreviewValueAsDefault);

			InputsJson.Add(MakeShared<FJsonValueObject>(InputJson));
		}

		if (const auto* FuncOutput = Cast<UMaterialExpressionFunctionOutput>(Expr))
		{
			auto OutputJson = MakeShared<FJsonObject>();
			OutputJson->SetStringField(TEXT("name"), FuncOutput->OutputName.ToString());
			OutputJson->SetStringField(TEXT("expression_name"), FuncOutput->GetName());
			OutputJson->SetNumberField(TEXT("sort_priority"), FuncOutput->SortPriority);
			OutputJson->SetStringField(TEXT("description"), FuncOutput->Description);

			OutputsJson.Add(MakeShared<FJsonValueObject>(OutputJson));
		}

		auto ExprJson = MakeShared<FJsonObject>();
		ExprJson->SetStringField(TEXT("name"), Expr->GetName());
		ExprJson->SetStringField(TEXT("class"), Expr->GetClass()->GetName());
		ExprJson->SetNumberField(TEXT("pos_x"), Expr->MaterialExpressionEditorX);
		ExprJson->SetNumberField(TEXT("pos_y"), Expr->MaterialExpressionEditorY);
		ExpressionsJson.Add(MakeShared<FJsonValueObject>(ExprJson));
	}

	// Sort inputs and outputs by sort priority
	InputsJson.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		return A->AsObject()->GetNumberField(TEXT("sort_priority")) < B->AsObject()->GetNumberField(TEXT("sort_priority"));
	});
	OutputsJson.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		return A->AsObject()->GetNumberField(TEXT("sort_priority")) < B->AsObject()->GetNumberField(TEXT("sort_priority"));
	});

	ResultJson->SetArrayField(TEXT("inputs"), InputsJson);
	ResultJson->SetArrayField(TEXT("outputs"), OutputsJson);
	ResultJson->SetArrayField(TEXT("expressions"), ExpressionsJson);
	ResultJson->SetNumberField(TEXT("expression_count"), ExpressionsJson.Num());
	ResultJson->SetNumberField(TEXT("input_count"), InputsJson.Num());
	ResultJson->SetNumberField(TEXT("output_count"), OutputsJson.Num());

	return FMonolithActionResult::Success(ResultJson);
}

// ----------------------------------------------------------------------------
// export_function_graph — Full material function graph export (GitHub #7)
// ----------------------------------------------------------------------------

FMonolithActionResult FMonolithMaterialActions::ExportFunctionGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	bool bIncludeProperties = true;
	bool bIncludePositions = true;
	Params->TryGetBoolField(TEXT("include_properties"), bIncludeProperties);
	Params->TryGetBoolField(TEXT("include_positions"), bIncludePositions);

	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	if (!LoadedAsset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load asset at '%s'"), *AssetPath));
	}

	UMaterialFunction* MatFunc = Cast<UMaterialFunction>(LoadedAsset);
	if (!MatFunc)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset '%s' is not a MaterialFunction"), *AssetPath));
	}

	TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = MatFunc->GetExpressions();

	// Build expression ID map
	TMap<UMaterialExpression*, FString> ExprToId;
	for (const TObjectPtr<UMaterialExpression>& Expr : Expressions)
	{
		if (Expr)
		{
			ExprToId.Add(Expr, Expr->GetName());
		}
	}

	// --- Metadata ---
	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetStringField(TEXT("description"), MatFunc->Description);
	ResultJson->SetStringField(TEXT("user_exposed_caption"), MatFunc->UserExposedCaption);
	ResultJson->SetBoolField(TEXT("expose_to_library"), MatFunc->bExposeToLibrary);

	// Determine specific type
	if (Cast<UMaterialFunctionMaterialLayer>(LoadedAsset))
	{
		ResultJson->SetStringField(TEXT("type"), TEXT("MaterialLayer"));
	}
	else if (Cast<UMaterialFunctionMaterialLayerBlend>(LoadedAsset))
	{
		ResultJson->SetStringField(TEXT("type"), TEXT("MaterialLayerBlend"));
	}
	else
	{
		ResultJson->SetStringField(TEXT("type"), TEXT("MaterialFunction"));
	}

	// Library categories
	TArray<TSharedPtr<FJsonValue>> CatsJson;
	for (const FText& Cat : MatFunc->LibraryCategoriesText)
	{
		CatsJson.Add(MakeShared<FJsonValueString>(Cat.ToString()));
	}
	ResultJson->SetArrayField(TEXT("library_categories"), CatsJson);

	// --- Separate inputs/outputs from regular nodes ---
	const UEnum* InputTypeEnum = StaticEnum<EFunctionInputType>();

	TArray<TSharedPtr<FJsonValue>> InputsJson;
	TArray<TSharedPtr<FJsonValue>> OutputsJson;
	TArray<TSharedPtr<FJsonValue>> NodesArray;
	TArray<TSharedPtr<FJsonValue>> CustomHlslArray;

	for (const TObjectPtr<UMaterialExpression>& Expr : Expressions)
	{
		if (!Expr)
		{
			continue;
		}

		// --- Function Inputs ---
		if (const auto* FuncInput = Cast<UMaterialExpressionFunctionInput>(Expr))
		{
			auto InputJson = MakeShared<FJsonObject>();
			InputJson->SetStringField(TEXT("id"), Expr->GetName());
			InputJson->SetStringField(TEXT("name"), FuncInput->InputName.ToString());
			InputJson->SetNumberField(TEXT("sort_priority"), FuncInput->SortPriority);
			InputJson->SetStringField(TEXT("description"), FuncInput->Description);
			InputJson->SetBoolField(TEXT("use_preview_value_as_default"), FuncInput->bUsePreviewValueAsDefault);

			if (InputTypeEnum)
			{
				FString TypeStr = InputTypeEnum->GetNameStringByIndex(static_cast<int32>(FuncInput->InputType));
				TypeStr.RemoveFromStart(TEXT("FunctionInput_"));
				InputJson->SetStringField(TEXT("type"), TypeStr);
			}

			// Preview value as 4-element array
			TArray<TSharedPtr<FJsonValue>> PreviewArr;
			PreviewArr.Add(MakeShared<FJsonValueNumber>(FuncInput->PreviewValue.X));
			PreviewArr.Add(MakeShared<FJsonValueNumber>(FuncInput->PreviewValue.Y));
			PreviewArr.Add(MakeShared<FJsonValueNumber>(FuncInput->PreviewValue.Z));
			PreviewArr.Add(MakeShared<FJsonValueNumber>(FuncInput->PreviewValue.W));
			InputJson->SetArrayField(TEXT("preview_value"), PreviewArr);

			// Blend input relevance
			InputJson->SetNumberField(TEXT("blend_input_relevance"), static_cast<int32>(FuncInput->BlendInputRelevance));

			if (bIncludePositions)
			{
				TArray<TSharedPtr<FJsonValue>> PosArr;
				PosArr.Add(MakeShared<FJsonValueNumber>(Expr->MaterialExpressionEditorX));
				PosArr.Add(MakeShared<FJsonValueNumber>(Expr->MaterialExpressionEditorY));
				InputJson->SetArrayField(TEXT("pos"), PosArr);
			}

			InputsJson.Add(MakeShared<FJsonValueObject>(InputJson));
			continue;
		}

		// --- Function Outputs ---
		if (const auto* FuncOutput = Cast<UMaterialExpressionFunctionOutput>(Expr))
		{
			auto OutputJson = MakeShared<FJsonObject>();
			OutputJson->SetStringField(TEXT("id"), Expr->GetName());
			OutputJson->SetStringField(TEXT("name"), FuncOutput->OutputName.ToString());
			OutputJson->SetNumberField(TEXT("sort_priority"), FuncOutput->SortPriority);
			OutputJson->SetStringField(TEXT("description"), FuncOutput->Description);

			if (bIncludePositions)
			{
				TArray<TSharedPtr<FJsonValue>> PosArr;
				PosArr.Add(MakeShared<FJsonValueNumber>(Expr->MaterialExpressionEditorX));
				PosArr.Add(MakeShared<FJsonValueNumber>(Expr->MaterialExpressionEditorY));
				OutputJson->SetArrayField(TEXT("pos"), PosArr);
			}

			OutputsJson.Add(MakeShared<FJsonValueObject>(OutputJson));
			continue;
		}

		// --- Custom HLSL nodes ---
		const UMaterialExpressionCustom* CustomExpr = Cast<UMaterialExpressionCustom>(Expr);
		if (CustomExpr)
		{
			auto NodeJson = MakeShared<FJsonObject>();
			NodeJson->SetStringField(TEXT("id"), Expr->GetName());

			if (bIncludePositions)
			{
				TArray<TSharedPtr<FJsonValue>> PosArr;
				PosArr.Add(MakeShared<FJsonValueNumber>(Expr->MaterialExpressionEditorX));
				PosArr.Add(MakeShared<FJsonValueNumber>(Expr->MaterialExpressionEditorY));
				NodeJson->SetArrayField(TEXT("pos"), PosArr);
			}

			NodeJson->SetStringField(TEXT("code"), CustomExpr->Code);
			NodeJson->SetStringField(TEXT("description"), CustomExpr->Description);
			NodeJson->SetStringField(TEXT("output_type"), CustomOutputTypeToString(CustomExpr->OutputType));

			TArray<TSharedPtr<FJsonValue>> CustInputsArr;
			for (const FCustomInput& CustInput : CustomExpr->Inputs)
			{
				auto CustInputJson = MakeShared<FJsonObject>();
				CustInputJson->SetStringField(TEXT("name"), CustInput.InputName.ToString());
				CustInputsArr.Add(MakeShared<FJsonValueObject>(CustInputJson));
			}
			NodeJson->SetArrayField(TEXT("inputs"), CustInputsArr);

			TArray<TSharedPtr<FJsonValue>> AddOutputsArr;
			for (const FCustomOutput& AddOut : CustomExpr->AdditionalOutputs)
			{
				auto OutJson = MakeShared<FJsonObject>();
				OutJson->SetStringField(TEXT("name"), AddOut.OutputName.ToString());
				OutJson->SetStringField(TEXT("type"), CustomOutputTypeToString(AddOut.OutputType));
				AddOutputsArr.Add(MakeShared<FJsonValueObject>(OutJson));
			}
			NodeJson->SetArrayField(TEXT("additional_outputs"), AddOutputsArr);

			CustomHlslArray.Add(MakeShared<FJsonValueObject>(NodeJson));
		}
		else
		{
			// --- Regular expression nodes ---
			auto NodeJson = MakeShared<FJsonObject>();
			NodeJson->SetStringField(TEXT("id"), Expr->GetName());

			FString ClassName = Expr->GetClass()->GetName();
			ClassName.RemoveFromStart(TEXT("MaterialExpression"));
			NodeJson->SetStringField(TEXT("class"), ClassName);

			if (bIncludePositions)
			{
				TArray<TSharedPtr<FJsonValue>> PosArr;
				PosArr.Add(MakeShared<FJsonValueNumber>(Expr->MaterialExpressionEditorX));
				PosArr.Add(MakeShared<FJsonValueNumber>(Expr->MaterialExpressionEditorY));
				NodeJson->SetArrayField(TEXT("pos"), PosArr);
			}

			// Parameter enrichment
			if (auto* Param = Cast<UMaterialExpressionParameter>(Expr))
			{
				NodeJson->SetStringField(TEXT("parameter_name"), Param->ParameterName.ToString());
				NodeJson->SetStringField(TEXT("group"), Param->Group.ToString());
				NodeJson->SetNumberField(TEXT("sort_priority"), Param->SortPriority);
			}

			// Static switch (must check before StaticBool since switch inherits from it)
			if (auto* SwitchParam = Cast<UMaterialExpressionStaticSwitchParameter>(Expr))
			{
				NodeJson->SetBoolField(TEXT("default_value"), SwitchParam->DefaultValue != 0);
				NodeJson->SetBoolField(TEXT("dynamic_branch"), SwitchParam->DynamicBranch != 0);
			}
			else if (auto* StaticBoolParam = Cast<UMaterialExpressionStaticBoolParameter>(Expr))
			{
				NodeJson->SetBoolField(TEXT("default_value"), StaticBoolParam->DefaultValue != 0);
				NodeJson->SetBoolField(TEXT("dynamic_branch"), StaticBoolParam->DynamicBranch != 0);
			}

			// Comment text
			if (const auto* Comment = Cast<UMaterialExpressionComment>(Expr))
			{
				NodeJson->SetStringField(TEXT("text"), Comment->Text);
			}

			// Function call reference
			if (const auto* FuncCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expr))
			{
				if (FuncCall->MaterialFunction)
				{
					NodeJson->SetStringField(TEXT("function"), FuncCall->MaterialFunction->GetPathName());
				}
			}

			// Full property reflection
			if (bIncludeProperties)
			{
				auto PropsJson = MakeShared<FJsonObject>();
				for (TFieldIterator<FProperty> PropIt(Expr->GetClass()); PropIt; ++PropIt)
				{
					FProperty* Prop = *PropIt;
					if (!Prop || Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient))
					{
						continue;
					}
					if (Prop->GetOwnerClass() == UMaterialExpression::StaticClass())
					{
						continue;
					}
					FString ValueStr;
					const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Expr);
					Prop->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, nullptr, PPF_None);
					if (!ValueStr.IsEmpty())
					{
						PropsJson->SetStringField(Prop->GetName(), ValueStr);
					}
				}
				NodeJson->SetObjectField(TEXT("props"), PropsJson);
			}

			NodesArray.Add(MakeShared<FJsonValueObject>(NodeJson));
		}
	}

	// Sort inputs and outputs by sort priority
	InputsJson.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		return A->AsObject()->GetNumberField(TEXT("sort_priority")) < B->AsObject()->GetNumberField(TEXT("sort_priority"));
	});
	OutputsJson.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		return A->AsObject()->GetNumberField(TEXT("sort_priority")) < B->AsObject()->GetNumberField(TEXT("sort_priority"));
	});

	// --- Build connections (traverse ALL expressions including Input/Output) ---
	TArray<TSharedPtr<FJsonValue>> ConnectionsArray;
	for (const TObjectPtr<UMaterialExpression>& Expr : Expressions)
	{
		if (!Expr)
		{
			continue;
		}
		for (int32 i = 0; ; ++i)
		{
			FExpressionInput* ExprInput = Expr->GetInput(i);
			if (!ExprInput)
			{
				break;
			}
			if (!ExprInput->Expression)
			{
				continue;
			}

			auto ConnJson = MakeShared<FJsonObject>();
			FString* FromId = ExprToId.Find(ExprInput->Expression);
			ConnJson->SetStringField(TEXT("from"), FromId ? *FromId : ExprInput->Expression->GetName());

			FString FromPin;
			const TArray<FExpressionOutput>& SourceOutputs = ExprInput->Expression->Outputs;
			if (SourceOutputs.IsValidIndex(ExprInput->OutputIndex))
			{
				FromPin = SourceOutputs[ExprInput->OutputIndex].OutputName.ToString();
			}
			ConnJson->SetStringField(TEXT("from_pin"), FromPin);
			ConnJson->SetStringField(TEXT("to"), Expr->GetName());
			ConnJson->SetStringField(TEXT("to_pin"), Expr->GetInputName(i).ToString());

			ConnectionsArray.Add(MakeShared<FJsonValueObject>(ConnJson));
		}
	}

	// --- Assemble result ---
	ResultJson->SetArrayField(TEXT("inputs"), InputsJson);
	ResultJson->SetArrayField(TEXT("outputs"), OutputsJson);
	ResultJson->SetArrayField(TEXT("nodes"), NodesArray);
	ResultJson->SetArrayField(TEXT("custom_hlsl_nodes"), CustomHlslArray);
	ResultJson->SetArrayField(TEXT("connections"), ConnectionsArray);
	ResultJson->SetNumberField(TEXT("expression_count"), Expressions.Num());

	return FMonolithActionResult::Success(ResultJson);
}

// ----------------------------------------------------------------------------
// set_function_metadata — Update description, library exposure, and categories
// ----------------------------------------------------------------------------

FMonolithActionResult FMonolithMaterialActions::SetFunctionMetadata(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	if (!LoadedAsset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load asset at '%s'"), *AssetPath));
	}

	UMaterialFunction* MatFunc = Cast<UMaterialFunction>(LoadedAsset);
	if (!MatFunc)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset '%s' is not a MaterialFunction"), *AssetPath));
	}

	MatFunc->Modify();

	TArray<FString> UpdatedFields;

	FString DescriptionValue;
	if (Params->TryGetStringField(TEXT("description"), DescriptionValue))
	{
		MatFunc->Description = DescriptionValue;
		UpdatedFields.Add(TEXT("description"));
	}

	bool bExposeValue;
	if (Params->TryGetBoolField(TEXT("expose_to_library"), bExposeValue))
	{
		MatFunc->bExposeToLibrary = bExposeValue;
		UpdatedFields.Add(TEXT("expose_to_library"));
	}

	const TArray<TSharedPtr<FJsonValue>>* CategoriesArray;
	if (Params->TryGetArrayField(TEXT("library_categories"), CategoriesArray))
	{
		MatFunc->LibraryCategoriesText.Empty();
		for (const TSharedPtr<FJsonValue>& CatVal : *CategoriesArray)
		{
			FString CatStr;
			if (CatVal.IsValid() && CatVal->TryGetString(CatStr))
			{
				MatFunc->LibraryCategoriesText.Add(FText::FromString(CatStr));
			}
		}
		UpdatedFields.Add(TEXT("library_categories"));
	}

	MatFunc->MarkPackageDirty();

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	AR.AssetTagsFinalized(*MatFunc);

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetBoolField(TEXT("modified"), true);

	TArray<TSharedPtr<FJsonValue>> UpdatedFieldsJson;
	for (const FString& Field : UpdatedFields)
	{
		UpdatedFieldsJson.Add(MakeShared<FJsonValueString>(Field));
	}
	ResultJson->SetArrayField(TEXT("updated_fields"), UpdatedFieldsJson);

	return FMonolithActionResult::Success(ResultJson);
}

// ----------------------------------------------------------------------------
// update_material_function — Recompile function and cascade to all references
// ----------------------------------------------------------------------------

FMonolithActionResult FMonolithMaterialActions::UpdateMaterialFunction(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	if (!LoadedAsset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load asset at '%s'"), *AssetPath));
	}

	UMaterialFunctionInterface* MatFuncInterface = Cast<UMaterialFunctionInterface>(LoadedAsset);
	if (!MatFuncInterface)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset '%s' is not a MaterialFunction or MaterialFunctionInstance"), *AssetPath));
	}

	// UpdateMaterialFunction handles ForceRecompileForRendering, MarkPackageDirty,
	// iterating in-memory UMaterialFunctionInstance objects, and rebuilding editors/viewports.
	UMaterialEditingLibrary::UpdateMaterialFunction(MatFuncInterface, nullptr);

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetBoolField(TEXT("updated"), true);
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);

	return FMonolithActionResult::Success(ResultJson);
}

// ----------------------------------------------------------------------------
// delete_function_expression — Delete one or more expressions from a MaterialFunction
// ----------------------------------------------------------------------------

FMonolithActionResult FMonolithMaterialActions::DeleteFunctionExpression(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString ExpressionNameParam = Params->GetStringField(TEXT("expression_name"));

	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	if (!LoadedAsset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load asset at '%s'"), *AssetPath));
	}

	// Reject function instances — DeleteMaterialExpressionInFunction requires UMaterialFunction*, not UMaterialFunctionInterface*
	if (Cast<UMaterialFunctionInstance>(LoadedAsset))
	{
		return FMonolithActionResult::Error(TEXT("Cannot delete expressions from a function instance — modify the base function instead"));
	}

	UMaterialFunction* MatFunc = Cast<UMaterialFunction>(LoadedAsset);
	if (!MatFunc)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset '%s' is not a MaterialFunction"), *AssetPath));
	}

	// Parse comma-separated expression names, trim whitespace
	TArray<FString> NamesToDelete;
	ExpressionNameParam.ParseIntoArray(NamesToDelete, TEXT(","));
	for (FString& Name : NamesToDelete)
	{
		Name = Name.TrimStartAndEnd();
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("DeleteFunctionExpression")));
	MatFunc->Modify();

	// Build name→expression map for O(1) lookups instead of O(N) per name
	TMap<FString, UMaterialExpression*> NameToExpr;
	for (UMaterialExpression* Expr : MatFunc->GetExpressions())
	{
		if (Expr)
		{
			NameToExpr.Add(Expr->GetName(), Expr);
		}
	}

	int32 DeletedCount = 0;
	TArray<FString> NotFound;

	for (const FString& Name : NamesToDelete)
	{
		UMaterialExpression** FoundPtr = NameToExpr.Find(Name);
		if (!FoundPtr || !(*FoundPtr))
		{
			NotFound.Add(Name);
			continue;
		}

		// Breaks all connections automatically
		UMaterialEditingLibrary::DeleteMaterialExpressionInFunction(MatFunc, *FoundPtr);
		NameToExpr.Remove(Name);
		++DeletedCount;
	}

	GEditor->EndTransaction();
	MatFunc->MarkPackageDirty();

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetNumberField(TEXT("deleted"), DeletedCount);

	TArray<TSharedPtr<FJsonValue>> NotFoundJson;
	for (const FString& Name : NotFound)
	{
		NotFoundJson.Add(MakeShared<FJsonValueString>(Name));
	}
	ResultJson->SetArrayField(TEXT("not_found"), NotFoundJson);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Wave 7 — Batch & Advanced
// ============================================================================

// Helper: Parse an array field that may arrive as a JSON string (Claude Code quirk)
static bool ParseJsonArrayField(const TSharedPtr<FJsonObject>& Params, const FString& FieldName,
	TArray<TSharedPtr<FJsonValue>>& OutArray, FString& OutError)
{
	TSharedPtr<FJsonValue> Field = Params->TryGetField(FieldName);
	if (!Field.IsValid())
	{
		OutError = FString::Printf(TEXT("Missing required field '%s'"), *FieldName);
		return false;
	}

	if (Field->Type == EJson::Array)
	{
		OutArray = Field->AsArray();
		return true;
	}

	// Claude Code JSON string serialization quirk — array may arrive as string
	if (Field->Type == EJson::String)
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Field->AsString());
		TSharedPtr<FJsonValue> Parsed;
		if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid() && Parsed->Type == EJson::Array)
		{
			OutArray = Parsed->AsArray();
			return true;
		}
	}

	OutError = FString::Printf(TEXT("'%s' must be a JSON array"), *FieldName);
	return false;
}

// Helper: Extract string array from parsed JSON values
static TArray<FString> JsonArrayToStringArray(const TArray<TSharedPtr<FJsonValue>>& JsonArray)
{
	TArray<FString> Result;
	Result.Reserve(JsonArray.Num());
	for (const TSharedPtr<FJsonValue>& Val : JsonArray)
	{
		if (Val.IsValid())
		{
			Result.Add(Val->AsString());
		}
	}
	return Result;
}

// ============================================================================
// Action: batch_set_material_property
// Params: { "asset_paths": [...], plus all set_material_property fields }
// Applies the same property changes to multiple materials in a single transaction.
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::BatchSetMaterialProperty(const TSharedPtr<FJsonObject>& Params)
{
	// Parse asset_paths array
	TArray<TSharedPtr<FJsonValue>> PathsJsonArray;
	FString ParseError;
	if (!ParseJsonArrayField(Params, TEXT("asset_paths"), PathsJsonArray, ParseError))
	{
		return FMonolithActionResult::Error(ParseError);
	}

	TArray<FString> AssetPaths = JsonArrayToStringArray(PathsJsonArray);
	if (AssetPaths.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("asset_paths array is empty"));
	}

	// Build a params object without asset_paths — just the properties
	// We reuse SetMaterialProperty by forwarding per-asset, but we wrap in a single transaction
	GEditor->BeginTransaction(FText::FromString(TEXT("BatchSetMaterialProperty")));

	TArray<TSharedPtr<FJsonValue>> ResultsArray;

	for (const FString& AssetPath : AssetPaths)
	{
		auto PerAssetResult = MakeShared<FJsonObject>();
		PerAssetResult->SetStringField(TEXT("asset_path"), AssetPath);

		UMaterial* Mat = LoadBaseMaterial(AssetPath);
		if (!Mat)
		{
			PerAssetResult->SetBoolField(TEXT("success"), false);
			PerAssetResult->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to load base material at '%s'"), *AssetPath));
			ResultsArray.Add(MakeShared<FJsonValueObject>(PerAssetResult));
			continue;
		}

		Mat->Modify();

		TArray<TSharedPtr<FJsonValue>> ChangedArray;
		int32 ChangeCount = 0;
		TArray<FString> Errors;

		auto RecordChange = [&](const FString& PropName, const FString& Value)
		{
			auto ChangeJson = MakeShared<FJsonObject>();
			ChangeJson->SetStringField(TEXT("property"), PropName);
			ChangeJson->SetStringField(TEXT("value"), Value);
			ChangedArray.Add(MakeShared<FJsonValueObject>(ChangeJson));
			ChangeCount++;
		};

		// Blend mode
		if (Params->HasField(TEXT("blend_mode")))
		{
			FString Val = Params->GetStringField(TEXT("blend_mode"));
			FString EnumError;
			EBlendMode ParsedMode;
			if (ParseEnum<EBlendMode>(Val, ParsedMode, EnumError))
			{
				Mat->BlendMode = ParsedMode;
				RecordChange(TEXT("blend_mode"), Val);
			}
			else
			{
				Errors.Add(FString::Printf(TEXT("blend_mode: %s"), *EnumError));
			}
		}

		// Shading model
		if (Params->HasField(TEXT("shading_model")))
		{
			FString Val = Params->GetStringField(TEXT("shading_model"));
			FString EnumError;
			EMaterialShadingModel ParsedModel;
			if (ParseEnum<EMaterialShadingModel>(Val, ParsedModel, EnumError))
			{
				Mat->SetShadingModel(ParsedModel);
				RecordChange(TEXT("shading_model"), Val);
			}
			else
			{
				Errors.Add(FString::Printf(TEXT("shading_model: %s"), *EnumError));
			}
		}

		// Material domain
		if (Params->HasField(TEXT("material_domain")))
		{
			FString Val = Params->GetStringField(TEXT("material_domain"));
			FString EnumError;
			EMaterialDomain ParsedDomain;
			if (ParseEnum<EMaterialDomain>(Val, ParsedDomain, EnumError))
			{
				Mat->MaterialDomain = ParsedDomain;
				RecordChange(TEXT("material_domain"), Val);
			}
			else
			{
				Errors.Add(FString::Printf(TEXT("material_domain: %s"), *EnumError));
			}
		}

		// Boolean properties
		auto ApplyBoolProp = [&](const TCHAR* ParamName, auto Setter)
		{
			if (Params->HasField(ParamName))
			{
				bool Val = Params->GetBoolField(ParamName);
				Setter(Val);
				RecordChange(ParamName, Val ? TEXT("true") : TEXT("false"));
			}
		};

		ApplyBoolProp(TEXT("two_sided"), [&](bool V) { Mat->TwoSided = V; });
		ApplyBoolProp(TEXT("dithered_lod_transition"), [&](bool V) { Mat->DitheredLODTransition = V; });
		ApplyBoolProp(TEXT("fully_rough"), [&](bool V) { Mat->bFullyRough = V; });
		ApplyBoolProp(TEXT("cast_shadow_as_masked"), [&](bool V) { Mat->bCastDynamicShadowAsMasked = V; });

		if (Params->HasField(TEXT("opacity_mask_clip_value")))
		{
			float Val = static_cast<float>(Params->GetNumberField(TEXT("opacity_mask_clip_value")));
			Mat->OpacityMaskClipValue = Val;
			RecordChange(TEXT("opacity_mask_clip_value"), FString::SanitizeFloat(Val));
		}

		// Usage flags
		auto ApplyUsage = [&](const TCHAR* ParamName, EMaterialUsage Usage)
		{
			if (Params->HasField(ParamName))
			{
				bool Val = Params->GetBoolField(ParamName);
				if (Val)
				{
					bool bRecompile = false;
					UMaterialEditingLibrary::SetMaterialUsage(Mat, Usage, bRecompile);
				}
				RecordChange(ParamName, Val ? TEXT("true") : TEXT("false"));
			}
		};

		ApplyUsage(TEXT("used_with_skeletal_mesh"), MATUSAGE_SkeletalMesh);
		ApplyUsage(TEXT("used_with_particle_sprites"), MATUSAGE_ParticleSprites);
		ApplyUsage(TEXT("used_with_niagara_sprites"), MATUSAGE_NiagaraSprites);
		ApplyUsage(TEXT("used_with_niagara_meshes"), MATUSAGE_NiagaraMeshParticles);
		ApplyUsage(TEXT("used_with_niagara_ribbons"), MATUSAGE_NiagaraRibbons);
		ApplyUsage(TEXT("used_with_morph_targets"), MATUSAGE_MorphTargets);
		ApplyUsage(TEXT("used_with_instanced_static_meshes"), MATUSAGE_InstancedStaticMeshes);
		ApplyUsage(TEXT("used_with_static_lighting"), MATUSAGE_StaticLighting);

		Mat->PreEditChange(nullptr);
		Mat->PostEditChange();

		// Save to disk so subsequent reads get fresh data
		UEditorAssetLibrary::SaveAsset(AssetPath, false);

		PerAssetResult->SetBoolField(TEXT("success"), Errors.Num() == 0);
		PerAssetResult->SetNumberField(TEXT("changes"), ChangeCount);
		PerAssetResult->SetArrayField(TEXT("changed"), ChangedArray);
		if (Errors.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> ErrorsJson;
			for (const FString& Err : Errors)
			{
				ErrorsJson.Add(MakeShared<FJsonValueString>(Err));
			}
			PerAssetResult->SetArrayField(TEXT("errors"), ErrorsJson);
		}

		ResultsArray.Add(MakeShared<FJsonValueObject>(PerAssetResult));
	}

	GEditor->EndTransaction();

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetNumberField(TEXT("material_count"), AssetPaths.Num());
	ResultJson->SetArrayField(TEXT("results"), ResultsArray);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: batch_recompile
// Params: { "asset_paths": [...] }
// Recompile multiple materials and return per-material instruction counts.
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::BatchRecompile(const TSharedPtr<FJsonObject>& Params)
{
	// Parse asset_paths array
	TArray<TSharedPtr<FJsonValue>> PathsJsonArray;
	FString ParseError;
	if (!ParseJsonArrayField(Params, TEXT("asset_paths"), PathsJsonArray, ParseError))
	{
		return FMonolithActionResult::Error(ParseError);
	}

	TArray<FString> AssetPaths = JsonArrayToStringArray(PathsJsonArray);
	if (AssetPaths.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("asset_paths array is empty"));
	}

	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	int32 SuccessCount = 0;

	for (const FString& AssetPath : AssetPaths)
	{
		auto PerAssetResult = MakeShared<FJsonObject>();
		PerAssetResult->SetStringField(TEXT("asset_path"), AssetPath);

		UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
		UMaterialInterface* MatInterface = LoadedAsset ? Cast<UMaterialInterface>(LoadedAsset) : nullptr;
		if (!MatInterface)
		{
			PerAssetResult->SetBoolField(TEXT("success"), false);
			PerAssetResult->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to load material at '%s'"), *AssetPath));
			ResultsArray.Add(MakeShared<FJsonValueObject>(PerAssetResult));
			continue;
		}

		UMaterial* BaseMat = MatInterface->GetMaterial();
		if (!BaseMat)
		{
			PerAssetResult->SetBoolField(TEXT("success"), false);
			PerAssetResult->SetStringField(TEXT("error"), TEXT("Could not resolve base material"));
			ResultsArray.Add(MakeShared<FJsonValueObject>(PerAssetResult));
			continue;
		}

		UMaterialEditingLibrary::RecompileMaterial(BaseMat);

		// Retrieve instruction counts after recompile
		FMaterialStatistics Stats = UMaterialEditingLibrary::GetStatistics(MatInterface);
		PerAssetResult->SetBoolField(TEXT("success"), true);
		PerAssetResult->SetNumberField(TEXT("vs_instructions"), Stats.NumVertexShaderInstructions);
		PerAssetResult->SetNumberField(TEXT("ps_instructions"), Stats.NumPixelShaderInstructions);

		SuccessCount++;
		ResultsArray.Add(MakeShared<FJsonValueObject>(PerAssetResult));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetNumberField(TEXT("material_count"), AssetPaths.Num());
	ResultJson->SetNumberField(TEXT("success_count"), SuccessCount);
	ResultJson->SetArrayField(TEXT("results"), ResultsArray);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: import_texture
// Params: { "source_file", "dest_path", "dest_name?", "compression?",
//           "srgb?", "lod_group?", "max_size?", "replace_existing?" }
// Import a texture from disk using UAssetImportTask + IAssetTools.
// ============================================================================

// Helper: Parse TextureCompressionSettings from string
static bool ParseTextureCompression(const FString& Str, TextureCompressionSettings& OutSetting)
{
	// Common shorthand mappings
	static const TMap<FString, TextureCompressionSettings> Mappings = {
		{ TEXT("default"),       TC_Default },
		{ TEXT("dxt5"),          TC_Default },
		{ TEXT("dxt1"),          TC_Default },
		{ TEXT("normalmap"),     TC_Normalmap },
		{ TEXT("grayscale"),     TC_Grayscale },
		{ TEXT("alpha"),         TC_Alpha },
		{ TEXT("masks"),         TC_Masks },
		{ TEXT("hdr"),           TC_HDR },
		{ TEXT("bc7"),           TC_BC7 },
		{ TEXT("halfhdr"),       TC_HalfFloat },
		{ TEXT("halffloat"),     TC_HalfFloat },
		{ TEXT("displacementmap"), TC_Displacementmap },
		{ TEXT("vectordisplacementmap"), TC_VectorDisplacementmap },
	};

	const TextureCompressionSettings* Found = Mappings.Find(Str.ToLower());
	if (Found)
	{
		OutSetting = *Found;
		return true;
	}

	// Try StaticEnum as fallback for full enum names like "TC_Default"
	const UEnum* Enum = StaticEnum<TextureCompressionSettings>();
	if (Enum)
	{
		int64 Value = Enum->GetValueByNameString(Str);
		if (Value != INDEX_NONE)
		{
			OutSetting = static_cast<TextureCompressionSettings>(Value);
			return true;
		}
	}

	return false;
}

// Helper: Parse TextureLODGroup from string
static bool ParseTextureLODGroup(const FString& Str, TextureGroup& OutGroup)
{
	// Common shorthand mappings
	static const TMap<FString, TextureGroup> Mappings = {
		{ TEXT("world"),                TEXTUREGROUP_World },
		{ TEXT("worldnormalmap"),        TEXTUREGROUP_WorldNormalMap },
		{ TEXT("worldspecular"),         TEXTUREGROUP_WorldSpecular },
		{ TEXT("character"),             TEXTUREGROUP_Character },
		{ TEXT("characternormalmap"),    TEXTUREGROUP_CharacterNormalMap },
		{ TEXT("characterspecular"),     TEXTUREGROUP_CharacterSpecular },
		{ TEXT("weapon"),               TEXTUREGROUP_Weapon },
		{ TEXT("weaponnormalmap"),       TEXTUREGROUP_WeaponNormalMap },
		{ TEXT("weaponspecular"),        TEXTUREGROUP_WeaponSpecular },
		{ TEXT("vehicle"),              TEXTUREGROUP_Vehicle },
		{ TEXT("vehiclenormalmap"),      TEXTUREGROUP_VehicleNormalMap },
		{ TEXT("vehiclespecular"),       TEXTUREGROUP_VehicleSpecular },
		{ TEXT("effects"),              TEXTUREGROUP_Effects },
		{ TEXT("ui"),                   TEXTUREGROUP_UI },
		{ TEXT("skybox"),               TEXTUREGROUP_Skybox },
	};

	const TextureGroup* Found = Mappings.Find(Str.ToLower());
	if (Found)
	{
		OutGroup = *Found;
		return true;
	}

	// Try StaticEnum for full enum names
	const UEnum* Enum = StaticEnum<TextureGroup>();
	if (Enum)
	{
		int64 Value = Enum->GetValueByNameString(Str);
		if (Value != INDEX_NONE)
		{
			OutGroup = static_cast<TextureGroup>(Value);
			return true;
		}
	}

	return false;
}

// ============================================================================
// Shared texture import helper — used by import_texture and create_pbr_material_from_disk
// ============================================================================

struct FTextureImportResult
{
	bool bSuccess = false;
	FString AssetPath;
	FString ErrorMessage;
	UTexture2D* Texture = nullptr;
	int32 ResX = 0;
	int32 ResY = 0;
};

static FTextureImportResult ImportTextureInternal(
	const FString& SourceFile,
	const FString& DestPath,
	const FString& DestName,
	TextureCompressionSettings Compression,
	bool bSRGB,
	TextureGroup LODGroup,
	int32 MaxSize,
	bool bReplaceExisting)
{
	FTextureImportResult Result;

	// Validate source file exists on disk
	if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*SourceFile))
	{
		Result.ErrorMessage = FString::Printf(TEXT("Source file not found: '%s'"), *SourceFile);
		return Result;
	}

	// Split dest_path into directory and asset name
	FString DestDirectory = FPaths::GetPath(DestPath);
	FString FinalDestName = DestName.IsEmpty() ? FPaths::GetBaseFilename(DestPath) : DestName;

	// Check if asset already exists
	FString FinalAssetPath = DestDirectory / FinalDestName;
	if (!bReplaceExisting && UEditorAssetLibrary::DoesAssetExist(FinalAssetPath))
	{
		Result.ErrorMessage = FString::Printf(TEXT("Asset already exists at '%s'. Set replace_existing: true to overwrite."), *FinalAssetPath);
		return Result;
	}

	// Create and configure import task
	UAssetImportTask* ImportTask = NewObject<UAssetImportTask>();
	ImportTask->Filename = SourceFile;
	ImportTask->DestinationPath = DestDirectory;
	ImportTask->DestinationName = FinalDestName;
	ImportTask->bAutomated = true;
	ImportTask->bReplaceExisting = bReplaceExisting;
	ImportTask->bSave = true;

	// Run import
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	TArray<UAssetImportTask*> Tasks;
	Tasks.Add(ImportTask);
	AssetTools.ImportAssetTasks(Tasks);

	// Verify the import succeeded by loading the asset
	UObject* ImportedObj = UEditorAssetLibrary::LoadAsset(FinalAssetPath);
	UTexture2D* Texture = ImportedObj ? Cast<UTexture2D>(ImportedObj) : nullptr;

	if (!Texture)
	{
		// Try without explicit name (some importers use the source filename)
		FString FallbackName = FPaths::GetBaseFilename(SourceFile);
		FString FallbackPath = DestDirectory / FallbackName;
		ImportedObj = UEditorAssetLibrary::LoadAsset(FallbackPath);
		Texture = ImportedObj ? Cast<UTexture2D>(ImportedObj) : nullptr;
		if (Texture)
		{
			FinalAssetPath = FallbackPath;
		}
	}

	if (!Texture)
	{
		Result.ErrorMessage = FString::Printf(
			TEXT("Import appeared to succeed but texture not found at '%s'. Check that the source file is a valid image format."),
			*FinalAssetPath);
		return Result;
	}

	// Apply post-import settings — known UAssetImportTask quirk: some settings
	// don't persist through the import pipeline, so we re-apply after loading.
	bool bNeedsResave = false;

	if (Texture->CompressionSettings != Compression)
	{
		Texture->CompressionSettings = Compression;
		bNeedsResave = true;
	}

	if (Texture->SRGB != bSRGB)
	{
		Texture->SRGB = bSRGB;
		bNeedsResave = true;
	}

	if (Texture->LODGroup != LODGroup)
	{
		Texture->LODGroup = LODGroup;
		bNeedsResave = true;
	}

	if (MaxSize > 0 && Texture->MaxTextureSize != MaxSize)
	{
		Texture->MaxTextureSize = MaxSize;
		bNeedsResave = true;
	}

	if (bNeedsResave)
	{
		Texture->Modify();
		Texture->PostEditChange();
		UEditorAssetLibrary::SaveAsset(FinalAssetPath, false);
	}

	Result.bSuccess = true;
	Result.AssetPath = FinalAssetPath;
	Result.Texture = Texture;
	Result.ResX = Texture->GetSizeX();
	Result.ResY = Texture->GetSizeY();
	return Result;
}

// ============================================================================
// Action: import_texture (refactored to use ImportTextureInternal)
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::ImportTexture(const TSharedPtr<FJsonObject>& Params)
{
	FString SourceFile = Params->GetStringField(TEXT("source_file"));
	FString DestPath = Params->GetStringField(TEXT("dest_path"));
	FString DestName = Params->HasField(TEXT("dest_name"))
		? Params->GetStringField(TEXT("dest_name"))
		: FString();
	bool bReplaceExisting = Params->HasField(TEXT("replace_existing")) ? Params->GetBoolField(TEXT("replace_existing")) : false;

	// Parse optional settings
	TextureCompressionSettings Compression = TC_Default;
	if (Params->HasField(TEXT("compression")))
	{
		FString CompressionStr = Params->GetStringField(TEXT("compression"));
		if (!ParseTextureCompression(CompressionStr, Compression))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Invalid compression setting: '%s'. Valid: Default, Normalmap, NormalmapBC5, NormalmapLA, Grayscale, Alpha, Masks, HDR, BC7, HalfFloat"),
				*CompressionStr));
		}
	}

	bool bSRGB = Params->HasField(TEXT("srgb")) ? Params->GetBoolField(TEXT("srgb")) : true;

	TextureGroup LODGroup = TEXTUREGROUP_World;
	if (Params->HasField(TEXT("lod_group")))
	{
		FString LODGroupStr = Params->GetStringField(TEXT("lod_group"));
		if (!ParseTextureLODGroup(LODGroupStr, LODGroup))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Invalid lod_group: '%s'. Valid: World, WorldNormalMap, WorldSpecular, Character, CharacterNormalMap, Weapon, UI, etc."),
				*LODGroupStr));
		}
	}

	int32 MaxSize = 0;
	if (Params->HasField(TEXT("max_size")))
	{
		MaxSize = static_cast<int32>(Params->GetNumberField(TEXT("max_size")));
	}

	FTextureImportResult ImportResult = ImportTextureInternal(
		SourceFile, DestPath, DestName, Compression, bSRGB, LODGroup, MaxSize, bReplaceExisting);

	if (!ImportResult.bSuccess)
	{
		return FMonolithActionResult::Error(ImportResult.ErrorMessage);
	}

	// Build result JSON (preserving original response format)
	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), ImportResult.AssetPath);
	ResultJson->SetNumberField(TEXT("resolution_x"), ImportResult.ResX);
	ResultJson->SetNumberField(TEXT("resolution_y"), ImportResult.ResY);

	const UEnum* CompressionEnum = StaticEnum<TextureCompressionSettings>();
	if (CompressionEnum)
	{
		FString CompStr = CompressionEnum->GetNameStringByIndex(static_cast<int32>(ImportResult.Texture->CompressionSettings));
		ResultJson->SetStringField(TEXT("compression_settings"), CompStr);
	}

	ResultJson->SetBoolField(TEXT("srgb"), ImportResult.Texture->SRGB);

	const UEnum* LODGroupEnum = StaticEnum<TextureGroup>();
	if (LODGroupEnum)
	{
		FString LODStr = LODGroupEnum->GetNameStringByIndex(static_cast<int32>(ImportResult.Texture->LODGroup));
		ResultJson->SetStringField(TEXT("lod_group"), LODStr);
	}

	if (ImportResult.Texture->MaxTextureSize > 0)
	{
		ResultJson->SetNumberField(TEXT("max_size"), ImportResult.Texture->MaxTextureSize);
	}

	int64 ResourceSize = ImportResult.Texture->GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal);
	ResultJson->SetNumberField(TEXT("estimated_size_kb"), static_cast<double>(ResourceSize) / 1024.0);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: create_pbr_material_from_disk
// Import PBR textures, create material, build graph, compile — one action.
// ============================================================================

// PBR map settings table
struct FPBRMapSettings
{
	TextureCompressionSettings Compression;
	bool bSRGB;
	TextureGroup LODGroup;
	FString NameSuffix;
	EMaterialProperty MaterialProperty;
	FString OutputPin; // "RGB", "R", or "A"
};

static const TMap<FString, FPBRMapSettings>& GetPBRMapSettingsTable()
{
	static const TMap<FString, FPBRMapSettings> Table = {
		{ TEXT("basecolor"), { TC_Default,   true,  TEXTUREGROUP_World,          TEXT("_D"),  MP_BaseColor,        TEXT("RGB") } },
		{ TEXT("albedo"),    { TC_Default,   true,  TEXTUREGROUP_World,          TEXT("_D"),  MP_BaseColor,        TEXT("RGB") } },
		{ TEXT("normal"),    { TC_Normalmap, false, TEXTUREGROUP_WorldNormalMap, TEXT("_N"),  MP_Normal,           TEXT("RGB") } },
		{ TEXT("roughness"), { TC_Masks,     false, TEXTUREGROUP_WorldSpecular,  TEXT("_R"),  MP_Roughness,        TEXT("R")   } },
		{ TEXT("metallic"),  { TC_Masks,     false, TEXTUREGROUP_WorldSpecular,  TEXT("_M"),  MP_Metallic,         TEXT("R")   } },
		{ TEXT("metalness"), { TC_Masks,     false, TEXTUREGROUP_WorldSpecular,  TEXT("_M"),  MP_Metallic,         TEXT("R")   } },
		{ TEXT("ao"),        { TC_Masks,     false, TEXTUREGROUP_World,          TEXT("_AO"), MP_AmbientOcclusion, TEXT("R")   } },
		{ TEXT("height"),    { TC_Masks,     false, TEXTUREGROUP_World,          TEXT("_H"),  MP_WorldPositionOffset, TEXT("R") } },
		{ TEXT("emissive"),  { TC_Default,   true,  TEXTUREGROUP_World,          TEXT("_E"),  MP_EmissiveColor,    TEXT("RGB") } },
		{ TEXT("opacity"),   { TC_Masks,     false, TEXTUREGROUP_World,          TEXT("_O"),  MP_Opacity,          TEXT("R")   } },
	};
	return Table;
}

FMonolithActionResult FMonolithMaterialActions::CreatePbrMaterialFromDisk(const TSharedPtr<FJsonObject>& Params)
{
	// ---- Parse required params ----
	if (!Params->HasField(TEXT("material_path")))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: material_path"));
	}
	if (!Params->HasField(TEXT("texture_folder")))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: texture_folder"));
	}
	if (!Params->HasField(TEXT("maps")))
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: maps"));
	}

	FString MaterialPath = Params->GetStringField(TEXT("material_path"));
	FString TextureFolder = Params->GetStringField(TEXT("texture_folder"));
	const TSharedPtr<FJsonObject>& MapsObj = Params->GetObjectField(TEXT("maps"));

	if (!MapsObj.IsValid() || MapsObj->Values.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("'maps' must be a non-empty object mapping PBR type to disk file path"));
	}

	// ---- Parse optional params ----
	FString BlendModeStr = Params->HasField(TEXT("blend_mode")) ? Params->GetStringField(TEXT("blend_mode")) : TEXT("Opaque");
	FString ShadingModelStr = Params->HasField(TEXT("shading_model")) ? Params->GetStringField(TEXT("shading_model")) : TEXT("DefaultLit");
	FString DomainStr = Params->HasField(TEXT("material_domain")) ? Params->GetStringField(TEXT("material_domain")) : TEXT("Surface");
	bool bTwoSided = Params->HasField(TEXT("two_sided")) ? Params->GetBoolField(TEXT("two_sided")) : false;
	int32 MaxTextureSize = Params->HasField(TEXT("max_texture_size")) ? static_cast<int32>(Params->GetNumberField(TEXT("max_texture_size"))) : 2048;
	bool bOpacityFromAlpha = Params->HasField(TEXT("opacity_from_alpha")) ? Params->GetBoolField(TEXT("opacity_from_alpha")) : false;
	bool bReplaceExisting = Params->HasField(TEXT("replace_existing")) ? Params->GetBoolField(TEXT("replace_existing")) : false;

	// Validate material_path format
	FString MaterialPackagePath, MaterialAssetName;
	{
		int32 LastSlash;
		if (!MaterialPath.FindLastChar('/', LastSlash) || LastSlash == MaterialPath.Len() - 1)
		{
			return FMonolithActionResult::Error(TEXT("Invalid material_path — must contain at least one '/' and an asset name (e.g. /Game/Materials/M_MyMat)"));
		}
		MaterialPackagePath = MaterialPath.Left(LastSlash);
		MaterialAssetName = MaterialPath.Mid(LastSlash + 1);
	}
	if (MaterialAssetName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("material_path has empty asset name"));
	}

	// Derive base name for textures: strip "M_" or "MI_" prefix if present
	FString TextureBaseName = MaterialAssetName;
	if (TextureBaseName.StartsWith(TEXT("M_")))
	{
		TextureBaseName = TextureBaseName.Mid(2);
	}
	else if (TextureBaseName.StartsWith(TEXT("MI_")))
	{
		TextureBaseName = TextureBaseName.Mid(3);
	}

	// Ensure texture folder has no trailing slash
	if (TextureFolder.EndsWith(TEXT("/")))
	{
		TextureFolder = TextureFolder.LeftChop(1);
	}

	const TMap<FString, FPBRMapSettings>& SettingsTable = GetPBRMapSettingsTable();

	// ========================================================================
	// Phase 1 — Import textures
	// ========================================================================

	struct FImportedTexture
	{
		FString MapType;
		FString AssetPath;
		UTexture2D* Texture;
		FPBRMapSettings Settings;
	};

	TArray<FImportedTexture> ImportedTextures;
	TArray<TSharedPtr<FJsonValue>> TextureErrors;

	for (const auto& MapEntry : MapsObj->Values)
	{
		FString MapType = MapEntry.Key.ToLower();
		FString DiskPath = MapEntry.Value->AsString();

		if (DiskPath.IsEmpty())
		{
			auto ErrJson = MakeShared<FJsonObject>();
			ErrJson->SetStringField(TEXT("map"), MapType);
			ErrJson->SetStringField(TEXT("error"), TEXT("Empty disk path"));
			TextureErrors.Add(MakeShared<FJsonValueObject>(ErrJson));
			continue;
		}

		const FPBRMapSettings* MapSettings = SettingsTable.Find(MapType);
		if (!MapSettings)
		{
			auto ErrJson = MakeShared<FJsonObject>();
			ErrJson->SetStringField(TEXT("map"), MapType);
			ErrJson->SetStringField(TEXT("error"), FString::Printf(TEXT("Unknown PBR map type '%s'. Valid: basecolor, albedo, normal, roughness, metallic, metalness, ao, height, emissive, opacity"), *MapType));
			TextureErrors.Add(MakeShared<FJsonValueObject>(ErrJson));
			continue;
		}

		// Build texture asset name: T_<BaseName><Suffix>
		FString TexAssetName = FString::Printf(TEXT("T_%s%s"), *TextureBaseName, *MapSettings->NameSuffix);
		FString TexDestPath = TextureFolder / TexAssetName;

		FTextureImportResult ImportResult = ImportTextureInternal(
			DiskPath, TexDestPath, FString(), MapSettings->Compression, MapSettings->bSRGB,
			MapSettings->LODGroup, MaxTextureSize, bReplaceExisting);

		if (ImportResult.bSuccess)
		{
			FImportedTexture Imported;
			Imported.MapType = MapType;
			Imported.AssetPath = ImportResult.AssetPath;
			Imported.Texture = ImportResult.Texture;
			Imported.Settings = *MapSettings;
			ImportedTextures.Add(MoveTemp(Imported));
		}
		else
		{
			auto ErrJson = MakeShared<FJsonObject>();
			ErrJson->SetStringField(TEXT("map"), MapType);
			ErrJson->SetStringField(TEXT("error"), ImportResult.ErrorMessage);
			TextureErrors.Add(MakeShared<FJsonValueObject>(ErrJson));
		}
	}

	if (ImportedTextures.Num() == 0)
	{
		FString CombinedErrors;
		for (const auto& Err : TextureErrors)
		{
			if (!CombinedErrors.IsEmpty()) CombinedErrors += TEXT("; ");
			CombinedErrors += Err->AsObject()->GetStringField(TEXT("error"));
		}
		return FMonolithActionResult::Error(FString::Printf(TEXT("No textures were imported. Errors: %s"), *CombinedErrors));
	}

	// ========================================================================
	// Phase 2 — Create material
	// ========================================================================

	// Handle replace_existing for the material
	if (bReplaceExisting)
	{
		UObject* ExistingMat = UEditorAssetLibrary::LoadAsset(MaterialPath);
		if (ExistingMat)
		{
			UEditorAssetLibrary::DeleteAsset(MaterialPath);
		}
	}
	else
	{
		UObject* ExistingMat = UEditorAssetLibrary::LoadAsset(MaterialPath);
		if (ExistingMat)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Material already exists at '%s'. Set replace_existing: true to overwrite."), *MaterialPath));
		}
	}

	// Parse enums
	FString EnumError;
	EMaterialDomain Domain;
	if (!ParseEnum<EMaterialDomain>(DomainStr, Domain, EnumError))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("material_domain: %s"), *EnumError));
	}
	EBlendMode BlendMode;
	if (!ParseEnum<EBlendMode>(BlendModeStr, BlendMode, EnumError))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("blend_mode: %s"), *EnumError));
	}
	EMaterialShadingModel ShadingModel;
	if (!ParseEnum<EMaterialShadingModel>(ShadingModelStr, ShadingModel, EnumError))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("shading_model: %s"), *EnumError));
	}

	// Create package and material
	UPackage* Pkg = CreatePackage(*MaterialPath);
	if (!Pkg)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to create package at '%s'"), *MaterialPath));
	}

	UMaterial* NewMat = NewObject<UMaterial>(Pkg, FName(*MaterialAssetName), RF_Public | RF_Standalone | RF_Transactional);
	if (!NewMat)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create UMaterial object"));
	}

	// Set material properties BEFORE creating expressions
	NewMat->MaterialDomain = Domain;
	NewMat->BlendMode = BlendMode;
	NewMat->SetShadingModel(ShadingModel);
	NewMat->TwoSided = bTwoSided;

	FAssetRegistryModule::AssetCreated(NewMat);
	Pkg->MarkPackageDirty();

	// ========================================================================
	// Phase 3 — Build graph directly
	// ========================================================================

	GEditor->BeginTransaction(NSLOCTEXT("Monolith", "CreatePBR", "Create PBR Material From Disk"));

	int32 NodesCreated = 0;
	int32 ConnectionsMade = 0;
	int32 YPos = -200; // Start position for first node
	const int32 XPos = -400;
	const int32 YSpacing = 280;

	UMaterialExpression* BaseColorNode = nullptr;

	for (const FImportedTexture& Imported : ImportedTextures)
	{
		UMaterialExpression* TexSample = UMaterialEditingLibrary::CreateMaterialExpression(
			NewMat, UMaterialExpressionTextureSample::StaticClass(), XPos, YPos);

		if (!TexSample)
		{
			UE_LOG(LogTemp, Warning, TEXT("CreatePbrMaterialFromDisk: Failed to create TextureSample for '%s'"), *Imported.MapType);
			YPos += YSpacing;
			continue;
		}

		// Set texture — cast to UMaterialExpressionTextureBase which owns the Texture + SamplerType properties
		UMaterialExpressionTextureBase* TexBase = Cast<UMaterialExpressionTextureBase>(TexSample);
		if (TexBase)
		{
			TexBase->Texture = Imported.Texture;

			// Set SamplerType AFTER Texture (order matters for validation)
			if (Imported.Settings.Compression == TC_Normalmap)
			{
				TexBase->SamplerType = SAMPLERTYPE_Normal;
			}
			else if (Imported.Settings.Compression == TC_Masks)
			{
				TexBase->SamplerType = SAMPLERTYPE_Masks;
			}
		}

		// Connect to the appropriate material property pin
		bool bConnected = UMaterialEditingLibrary::ConnectMaterialProperty(
			TexSample, Imported.Settings.OutputPin, Imported.Settings.MaterialProperty);

		if (bConnected)
		{
			ConnectionsMade++;
		}

		NodesCreated++;

		// Track basecolor node for opacity_from_alpha
		if (Imported.MapType == TEXT("basecolor") || Imported.MapType == TEXT("albedo"))
		{
			BaseColorNode = TexSample;
		}

		YPos += YSpacing;
	}

	// Wire basecolor alpha to Opacity if requested (useful for decals)
	if (bOpacityFromAlpha && BaseColorNode)
	{
		bool bAlphaConnected = UMaterialEditingLibrary::ConnectMaterialProperty(
			BaseColorNode, TEXT("A"), MP_Opacity);
		if (bAlphaConnected)
		{
			ConnectionsMade++;
		}
	}

	GEditor->EndTransaction();

	// ========================================================================
	// Phase 4 — Compile and save
	// ========================================================================

	NewMat->PreEditChange(nullptr);
	NewMat->PostEditChange();
	UMaterialEditingLibrary::RecompileMaterial(NewMat);

	// Save to disk
	FString PackageFilename = FPackageName::LongPackageNameToFilename(MaterialPath, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	UPackage::SavePackage(Pkg, NewMat, *PackageFilename, SaveArgs);

	// Get compilation stats
	FMaterialStatistics Stats = UMaterialEditingLibrary::GetStatistics(NewMat);

	// ========================================================================
	// Build result JSON
	// ========================================================================

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("material_path"), MaterialPath);

	// Textures imported
	auto TexImportedObj = MakeShared<FJsonObject>();
	for (const FImportedTexture& Imported : ImportedTextures)
	{
		TexImportedObj->SetStringField(Imported.MapType, Imported.AssetPath);
	}
	ResultJson->SetObjectField(TEXT("textures_imported"), TexImportedObj);
	ResultJson->SetNumberField(TEXT("textures_imported_count"), ImportedTextures.Num());
	ResultJson->SetNumberField(TEXT("nodes_created"), NodesCreated);
	ResultJson->SetNumberField(TEXT("connections_made"), ConnectionsMade);

	// Compile stats
	auto StatsJson = MakeShared<FJsonObject>();
	StatsJson->SetNumberField(TEXT("vs_instructions"), Stats.NumVertexShaderInstructions);
	StatsJson->SetNumberField(TEXT("ps_instructions"), Stats.NumPixelShaderInstructions);

	// Sampler count from material resource
	const EShaderPlatform ShaderPlatform = GShaderPlatformForFeatureLevel[GMaxRHIFeatureLevel];
	FMaterialResource* MatResource = NewMat->GetMaterialResource(ShaderPlatform);
	if (MatResource)
	{
		StatsJson->SetNumberField(TEXT("num_samplers"), MatResource->GetSamplerUsage());
	}
	ResultJson->SetObjectField(TEXT("compile_stats"), StatsJson);

	// Texture errors (only if any)
	if (TextureErrors.Num() > 0)
	{
		ResultJson->SetArrayField(TEXT("texture_errors"), TextureErrors);
	}

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: create_function_instance
// Creates a UMaterialFunctionInstance (or layer/blend subclass) with a parent
// function and optional parameter overrides.
// ============================================================================
FMonolithActionResult FMonolithMaterialActions::CreateFunctionInstance(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString ParentPath = Params->GetStringField(TEXT("parent"));

	// Check if asset already exists
	UObject* Existing = UEditorAssetLibrary::LoadAsset(AssetPath);
	if (Existing)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));
	}

	// Extract package path and asset name
	FString AssetName;
	int32 LastSlash;
	if (!AssetPath.FindLastChar('/', LastSlash) || LastSlash == AssetPath.Len() - 1)
	{
		return FMonolithActionResult::Error(TEXT("Invalid asset path"));
	}
	AssetName = AssetPath.Mid(LastSlash + 1);

	// Load parent as UMaterialFunctionInterface
	UObject* ParentObj = UEditorAssetLibrary::LoadAsset(ParentPath);
	UMaterialFunctionInterface* ParentFunc = ParentObj ? Cast<UMaterialFunctionInterface>(ParentObj) : nullptr;
	if (!ParentFunc)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load parent function at '%s'"), *ParentPath));
	}

	// Create package
	UPackage* Pkg = CreatePackage(*AssetPath);
	if (!Pkg)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create package"));
	}

	// Determine correct instance class based on parent's usage
	EMaterialFunctionUsage Usage = ParentFunc->GetMaterialFunctionUsage();
	FString TypeName;
	UMaterialFunctionInstance* MFI = nullptr;

	switch (Usage)
	{
	case EMaterialFunctionUsage::MaterialLayer:
		MFI = NewObject<UMaterialFunctionMaterialLayerInstance>(Pkg, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
		TypeName = TEXT("MaterialFunctionMaterialLayerInstance");
		break;
	case EMaterialFunctionUsage::MaterialLayerBlend:
		MFI = NewObject<UMaterialFunctionMaterialLayerBlendInstance>(Pkg, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
		TypeName = TEXT("MaterialFunctionMaterialLayerBlendInstance");
		break;
	default:
		MFI = NewObject<UMaterialFunctionInstance>(Pkg, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
		TypeName = TEXT("MaterialFunctionInstance");
		break;
	}

	if (!MFI)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create MaterialFunctionInstance object"));
	}

	// Set parent — this sets Parent, caches Base, syncs usage
	MFI->SetParent(ParentFunc);

	// Build parameter lookup from base function expressions.
	// UpdateParameterSet() only syncs names on EXISTING entries — it does NOT populate
	// empty arrays. We must manually create entries with the correct ExpressionGUIDs.
	UMaterialFunction* BaseFunc = MFI->GetBaseFunction();
	TMap<FName, FGuid> ScalarParamGUIDs;
	TMap<FName, FGuid> VectorParamGUIDs;
	TMap<FName, FGuid> TextureParamGUIDs;
	TMap<FName, FGuid> SwitchParamGUIDs;

	if (BaseFunc)
	{
		for (UMaterialExpression* Expr : BaseFunc->GetExpressions())
		{
			if (auto* SP = Cast<UMaterialExpressionScalarParameter>(Expr))
			{
				ScalarParamGUIDs.Add(SP->ParameterName, SP->ExpressionGUID);
			}
			else if (auto* VP = Cast<UMaterialExpressionVectorParameter>(Expr))
			{
				VectorParamGUIDs.Add(VP->ParameterName, VP->ExpressionGUID);
			}
			else if (auto* TP = Cast<UMaterialExpressionTextureSampleParameter>(Expr))
			{
				TextureParamGUIDs.Add(TP->ParameterName, TP->ExpressionGUID);
			}
			else if (auto* SWP = Cast<UMaterialExpressionStaticSwitchParameter>(Expr))
			{
				SwitchParamGUIDs.Add(SWP->ParameterName, SWP->ExpressionGUID);
			}
			else if (auto* SBP = Cast<UMaterialExpressionStaticBoolParameter>(Expr))
			{
				// StaticBoolParameter without switch — still a switch override in MFI
				if (!Cast<UMaterialExpressionStaticSwitchParameter>(Expr))
				{
					SwitchParamGUIDs.Add(SBP->ParameterName, SBP->ExpressionGUID);
				}
			}
		}
	}

	// Track override counts and errors
	int32 ScalarCount = 0;
	int32 VectorCount = 0;
	int32 TextureCount = 0;
	int32 SwitchCount = 0;
	TArray<TSharedPtr<FJsonValue>> Errors;

	// Apply scalar overrides — create entries with GUIDs from base function
	const TSharedPtr<FJsonObject>* ScalarOverrides = nullptr;
	if (Params->TryGetObjectField(TEXT("scalar_overrides"), ScalarOverrides))
	{
		for (const auto& Pair : (*ScalarOverrides)->Values)
		{
			FName ParamName(*Pair.Key);
			FGuid* FoundGUID = ScalarParamGUIDs.Find(ParamName);
			if (!FoundGUID)
			{
				Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("Scalar param '%s' not found in parent"), *Pair.Key)));
				continue;
			}
			FScalarParameterValue NewEntry;
			NewEntry.ParameterInfo = FMaterialParameterInfo(ParamName);
			NewEntry.ExpressionGUID = *FoundGUID;
			NewEntry.ParameterValue = static_cast<float>(Pair.Value->AsNumber());
			MFI->ScalarParameterValues.Add(NewEntry);
			ScalarCount++;
		}
	}

	// Apply vector overrides
	const TSharedPtr<FJsonObject>* VectorOverrides = nullptr;
	if (Params->TryGetObjectField(TEXT("vector_overrides"), VectorOverrides))
	{
		for (const auto& Pair : (*VectorOverrides)->Values)
		{
			FName ParamName(*Pair.Key);
			const TSharedPtr<FJsonObject>* ColorObj = nullptr;
			if (!Pair.Value->TryGetObject(ColorObj))
			{
				Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("Vector param '%s' value must be an object with r,g,b,a"), *Pair.Key)));
				continue;
			}
			FGuid* FoundGUID = VectorParamGUIDs.Find(ParamName);
			if (!FoundGUID)
			{
				Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("Vector param '%s' not found in parent"), *Pair.Key)));
				continue;
			}
			FLinearColor Color;
			Color.R = (*ColorObj)->HasField(TEXT("r")) ? static_cast<float>((*ColorObj)->GetNumberField(TEXT("r"))) : 0.f;
			Color.G = (*ColorObj)->HasField(TEXT("g")) ? static_cast<float>((*ColorObj)->GetNumberField(TEXT("g"))) : 0.f;
			Color.B = (*ColorObj)->HasField(TEXT("b")) ? static_cast<float>((*ColorObj)->GetNumberField(TEXT("b"))) : 0.f;
			Color.A = (*ColorObj)->HasField(TEXT("a")) ? static_cast<float>((*ColorObj)->GetNumberField(TEXT("a"))) : 1.f;
			FVectorParameterValue NewEntry;
			NewEntry.ParameterInfo = FMaterialParameterInfo(ParamName);
			NewEntry.ExpressionGUID = *FoundGUID;
			NewEntry.ParameterValue = Color;
			MFI->VectorParameterValues.Add(NewEntry);
			VectorCount++;
		}
	}

	// Apply texture overrides
	const TSharedPtr<FJsonObject>* TextureOverrides = nullptr;
	if (Params->TryGetObjectField(TEXT("texture_overrides"), TextureOverrides))
	{
		for (const auto& Pair : (*TextureOverrides)->Values)
		{
			FName ParamName(*Pair.Key);
			FString TexPath = Pair.Value->AsString();
			UTexture* Tex = Cast<UTexture>(UEditorAssetLibrary::LoadAsset(TexPath));
			if (!Tex)
			{
				Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("Failed to load texture '%s' for param '%s'"), *TexPath, *Pair.Key)));
				continue;
			}
			FGuid* FoundGUID = TextureParamGUIDs.Find(ParamName);
			if (!FoundGUID)
			{
				Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("Texture param '%s' not found in parent"), *Pair.Key)));
				continue;
			}
			FTextureParameterValue NewEntry;
			NewEntry.ParameterInfo = FMaterialParameterInfo(ParamName);
			NewEntry.ExpressionGUID = *FoundGUID;
			NewEntry.ParameterValue = Tex;
			MFI->TextureParameterValues.Add(NewEntry);
			TextureCount++;
		}
	}

	// Apply static switch overrides
	const TSharedPtr<FJsonObject>* SwitchOverrides = nullptr;
	if (Params->TryGetObjectField(TEXT("static_switch_overrides"), SwitchOverrides))
	{
		for (const auto& Pair : (*SwitchOverrides)->Values)
		{
			FName ParamName(*Pair.Key);
			bool bValue = Pair.Value->AsBool();
			FGuid* FoundGUID = SwitchParamGUIDs.Find(ParamName);
			if (!FoundGUID)
			{
				Errors.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("Static switch param '%s' not found in parent"), *Pair.Key)));
				continue;
			}
			FStaticSwitchParameter NewEntry;
			NewEntry.ParameterInfo = FMaterialParameterInfo(ParamName);
			NewEntry.ExpressionGUID = *FoundGUID;
			NewEntry.Value = bValue;
			NewEntry.bOverride = true;
			MFI->StaticSwitchParameterValues.Add(NewEntry);
			SwitchCount++;
		}
	}

	// Sync names after manual population
	MFI->UpdateParameterSet();

	// Register with asset registry and mark dirty
	FAssetRegistryModule::AssetCreated(MFI);
	Pkg->MarkPackageDirty();

	// Build result JSON
	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), MFI->GetPathName());
	ResultJson->SetStringField(TEXT("parent"), ParentFunc->GetPathName());
	ResultJson->SetStringField(TEXT("type"), TypeName);

	auto OverridesJson = MakeShared<FJsonObject>();
	OverridesJson->SetNumberField(TEXT("scalar"), ScalarCount);
	OverridesJson->SetNumberField(TEXT("vector"), VectorCount);
	OverridesJson->SetNumberField(TEXT("texture"), TextureCount);
	OverridesJson->SetNumberField(TEXT("static_switch"), SwitchCount);
	ResultJson->SetObjectField(TEXT("overrides_applied"), OverridesJson);

	if (Errors.Num() > 0)
	{
		ResultJson->SetArrayField(TEXT("errors"), Errors);
	}

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: set_function_instance_parameter
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::SetFunctionInstanceParameter(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString ParamName = Params->GetStringField(TEXT("parameter_name"));

	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	UMaterialFunctionInstance* MFI = LoadedAsset ? Cast<UMaterialFunctionInstance>(LoadedAsset) : nullptr;
	if (!MFI)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load material function instance at '%s'"), *AssetPath));
	}

	// Build parameter GUID lookup from base function expressions
	// UpdateParameterSet() only syncs names on existing entries — doesn't populate empty arrays
	UMaterialFunction* BaseFunc = MFI->GetBaseFunction();
	TMap<FName, FGuid> ScalarGUIDs, VectorGUIDs, TextureGUIDs, SwitchGUIDs;
	if (BaseFunc)
	{
		for (UMaterialExpression* Expr : BaseFunc->GetExpressions())
		{
			if (auto* SP = Cast<UMaterialExpressionScalarParameter>(Expr))
				ScalarGUIDs.Add(SP->ParameterName, SP->ExpressionGUID);
			else if (auto* VP = Cast<UMaterialExpressionVectorParameter>(Expr))
				VectorGUIDs.Add(VP->ParameterName, VP->ExpressionGUID);
			else if (auto* TP = Cast<UMaterialExpressionTextureSampleParameter>(Expr))
				TextureGUIDs.Add(TP->ParameterName, TP->ExpressionGUID);
			else if (auto* SWP = Cast<UMaterialExpressionStaticSwitchParameter>(Expr))
				SwitchGUIDs.Add(SWP->ParameterName, SWP->ExpressionGUID);
			else if (auto* SBP = Cast<UMaterialExpressionStaticBoolParameter>(Expr))
				SwitchGUIDs.Add(SBP->ParameterName, SBP->ExpressionGUID);
		}
	}

	// Determine which param type to set (mutually exclusive)
	int32 TypeCount = 0;
	if (Params->HasField(TEXT("scalar_value"))) TypeCount++;
	if (Params->HasField(TEXT("vector_value"))) TypeCount++;
	if (Params->HasField(TEXT("texture_value"))) TypeCount++;
	if (Params->HasField(TEXT("switch_value"))) TypeCount++;

	if (TypeCount == 0)
	{
		return FMonolithActionResult::Error(TEXT("Must provide one of: scalar_value, vector_value, texture_value, switch_value"));
	}
	if (TypeCount > 1)
	{
		return FMonolithActionResult::Error(TEXT("Only one of scalar_value, vector_value, texture_value, switch_value may be specified"));
	}

	MFI->Modify();
	FString SetType;
	FString SetValue;
	FName ParamFName(*ParamName);

	if (Params->HasField(TEXT("scalar_value")))
	{
		float Val = static_cast<float>(Params->GetNumberField(TEXT("scalar_value")));
		// Try to find existing entry first, then create if not found
		bool bFound = false;
		for (FScalarParameterValue& Entry : MFI->ScalarParameterValues)
		{
			if (Entry.ParameterInfo.Name == ParamFName)
			{
				Entry.ParameterValue = Val;
				bFound = true;
				break;
			}
		}
		if (!bFound)
		{
			FGuid* FoundGUID = ScalarGUIDs.Find(ParamFName);
			if (!FoundGUID)
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("Scalar parameter '%s' not found in base function"), *ParamName));
			}
			FScalarParameterValue NewEntry;
			NewEntry.ParameterInfo = FMaterialParameterInfo(ParamFName);
			NewEntry.ExpressionGUID = *FoundGUID;
			NewEntry.ParameterValue = Val;
			MFI->ScalarParameterValues.Add(NewEntry);
		}
		SetType = TEXT("scalar");
		SetValue = FString::SanitizeFloat(Val);
	}
	else if (Params->HasField(TEXT("vector_value")))
	{
		const TSharedPtr<FJsonObject>* ColorObj = nullptr;
		if (!Params->TryGetObjectField(TEXT("vector_value"), ColorObj))
		{
			return FMonolithActionResult::Error(TEXT("vector_value must be a JSON object with r, g, b, a fields"));
		}

		FLinearColor Color;
		Color.R = (*ColorObj)->HasField(TEXT("r")) ? static_cast<float>((*ColorObj)->GetNumberField(TEXT("r"))) : 0.f;
		Color.G = (*ColorObj)->HasField(TEXT("g")) ? static_cast<float>((*ColorObj)->GetNumberField(TEXT("g"))) : 0.f;
		Color.B = (*ColorObj)->HasField(TEXT("b")) ? static_cast<float>((*ColorObj)->GetNumberField(TEXT("b"))) : 0.f;
		Color.A = (*ColorObj)->HasField(TEXT("a")) ? static_cast<float>((*ColorObj)->GetNumberField(TEXT("a"))) : 1.f;

		bool bFound = false;
		for (FVectorParameterValue& Entry : MFI->VectorParameterValues)
		{
			if (Entry.ParameterInfo.Name == ParamFName)
			{
				Entry.ParameterValue = Color;
				bFound = true;
				break;
			}
		}
		if (!bFound)
		{
			FGuid* FoundGUID = VectorGUIDs.Find(ParamFName);
			if (!FoundGUID)
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("Vector parameter '%s' not found in base function"), *ParamName));
			}
			FVectorParameterValue NewEntry;
			NewEntry.ParameterInfo = FMaterialParameterInfo(ParamFName);
			NewEntry.ExpressionGUID = *FoundGUID;
			NewEntry.ParameterValue = Color;
			MFI->VectorParameterValues.Add(NewEntry);
		}
		SetType = TEXT("vector");
		SetValue = FString::Printf(TEXT("(%.3f, %.3f, %.3f, %.3f)"), Color.R, Color.G, Color.B, Color.A);
	}
	else if (Params->HasField(TEXT("texture_value")))
	{
		FString TexPath = Params->GetStringField(TEXT("texture_value"));
		UTexture* Tex = Cast<UTexture>(UEditorAssetLibrary::LoadAsset(TexPath));
		if (!Tex)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load texture at '%s'"), *TexPath));
		}

		bool bFound = false;
		for (FTextureParameterValue& Entry : MFI->TextureParameterValues)
		{
			if (Entry.ParameterInfo.Name == ParamFName)
			{
				Entry.ParameterValue = Tex;
				bFound = true;
				break;
			}
		}
		if (!bFound)
		{
			FGuid* FoundGUID = TextureGUIDs.Find(ParamFName);
			if (!FoundGUID)
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("Texture parameter '%s' not found in base function"), *ParamName));
			}
			FTextureParameterValue NewEntry;
			NewEntry.ParameterInfo = FMaterialParameterInfo(ParamFName);
			NewEntry.ExpressionGUID = *FoundGUID;
			NewEntry.ParameterValue = Tex;
			MFI->TextureParameterValues.Add(NewEntry);
		}
		SetType = TEXT("texture");
		SetValue = TexPath;
	}
	else if (Params->HasField(TEXT("switch_value")))
	{
		bool Val = Params->GetBoolField(TEXT("switch_value"));

		bool bFound = false;
		for (FStaticSwitchParameter& Entry : MFI->StaticSwitchParameterValues)
		{
			if (Entry.ParameterInfo.Name == ParamFName)
			{
				Entry.bOverride = true;
				Entry.Value = Val;
				bFound = true;
				break;
			}
		}
		if (!bFound)
		{
			FGuid* FoundGUID = SwitchGUIDs.Find(ParamFName);
			if (!FoundGUID)
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("Static switch parameter '%s' not found in base function"), *ParamName));
			}
			FStaticSwitchParameter NewEntry;
			NewEntry.ParameterInfo = FMaterialParameterInfo(ParamFName);
			NewEntry.ExpressionGUID = *FoundGUID;
			NewEntry.Value = Val;
			NewEntry.bOverride = true;
			MFI->StaticSwitchParameterValues.Add(NewEntry);
		}
		SetType = TEXT("static_switch");
		SetValue = Val ? TEXT("true") : TEXT("false");
	}

	// Cascade recompile to all materials using this function instance
	UMaterialEditingLibrary::UpdateMaterialFunction(MFI, nullptr);
	MFI->MarkPackageDirty();

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetStringField(TEXT("parameter_name"), ParamName);
	ResultJson->SetStringField(TEXT("type"), SetType);
	ResultJson->SetStringField(TEXT("value"), SetValue);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: get_function_instance_info
// Reads parent chain, parameter overrides, inputs/outputs from a MFI
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::GetFunctionInstanceInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	if (!LoadedAsset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load asset at '%s'"), *AssetPath));
	}

	UMaterialFunctionInstance* MFI = Cast<UMaterialFunctionInstance>(LoadedAsset);
	if (!MFI)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset '%s' is not a MaterialFunctionInstance (type: %s)"), *AssetPath, *LoadedAsset->GetClass()->GetName()));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);

	// --- Parent chain ---
	if (MFI->Parent)
	{
		ResultJson->SetStringField(TEXT("parent"), MFI->Parent->GetPathName());
	}
	else
	{
		ResultJson->SetField(TEXT("parent"), MakeShared<FJsonValueNull>());
	}

	UMaterialFunction* BaseFunc = MFI->GetBaseFunction();
	if (BaseFunc)
	{
		ResultJson->SetStringField(TEXT("base"), BaseFunc->GetPathName());
	}
	else
	{
		ResultJson->SetField(TEXT("base"), MakeShared<FJsonValueNull>());
	}

	// --- Function usage type ---
	EMaterialFunctionUsage Usage = MFI->GetMaterialFunctionUsage();
	const UEnum* UsageEnum = StaticEnum<EMaterialFunctionUsage>();
	if (UsageEnum)
	{
		FString UsageStr = UsageEnum->GetNameStringByIndex(static_cast<int32>(Usage));
		ResultJson->SetStringField(TEXT("type"), UsageStr);
	}

	// --- Scalar parameter overrides ---
	TArray<TSharedPtr<FJsonValue>> ScalarArr;
	for (const auto& Param : MFI->ScalarParameterValues)
	{
		auto PJson = MakeShared<FJsonObject>();
		PJson->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
		PJson->SetNumberField(TEXT("value"), Param.ParameterValue);
		ScalarArr.Add(MakeShared<FJsonValueObject>(PJson));
	}
	ResultJson->SetArrayField(TEXT("scalar_overrides"), ScalarArr);

	// --- Vector parameter overrides ---
	TArray<TSharedPtr<FJsonValue>> VectorArr;
	for (const auto& Param : MFI->VectorParameterValues)
	{
		auto PJson = MakeShared<FJsonObject>();
		PJson->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
		auto ValJson = MakeShared<FJsonObject>();
		ValJson->SetNumberField(TEXT("r"), Param.ParameterValue.R);
		ValJson->SetNumberField(TEXT("g"), Param.ParameterValue.G);
		ValJson->SetNumberField(TEXT("b"), Param.ParameterValue.B);
		ValJson->SetNumberField(TEXT("a"), Param.ParameterValue.A);
		PJson->SetObjectField(TEXT("value"), ValJson);
		VectorArr.Add(MakeShared<FJsonValueObject>(PJson));
	}
	ResultJson->SetArrayField(TEXT("vector_overrides"), VectorArr);

	// --- DoubleVector parameter overrides ---
	TArray<TSharedPtr<FJsonValue>> DoubleVecArr;
	for (const auto& Param : MFI->DoubleVectorParameterValues)
	{
		auto PJson = MakeShared<FJsonObject>();
		PJson->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
		auto ValJson = MakeShared<FJsonObject>();
		ValJson->SetNumberField(TEXT("r"), Param.ParameterValue.X);
		ValJson->SetNumberField(TEXT("g"), Param.ParameterValue.Y);
		ValJson->SetNumberField(TEXT("b"), Param.ParameterValue.Z);
		ValJson->SetNumberField(TEXT("a"), Param.ParameterValue.W);
		PJson->SetObjectField(TEXT("value"), ValJson);
		DoubleVecArr.Add(MakeShared<FJsonValueObject>(PJson));
	}
	ResultJson->SetArrayField(TEXT("double_vector_overrides"), DoubleVecArr);

	// --- Texture parameter overrides ---
	TArray<TSharedPtr<FJsonValue>> TextureArr;
	for (const auto& Param : MFI->TextureParameterValues)
	{
		auto PJson = MakeShared<FJsonObject>();
		PJson->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
		PJson->SetStringField(TEXT("value"), Param.ParameterValue ? Param.ParameterValue->GetPathName() : TEXT("None"));
		TextureArr.Add(MakeShared<FJsonValueObject>(PJson));
	}
	ResultJson->SetArrayField(TEXT("texture_overrides"), TextureArr);

	// --- Texture collection parameter overrides ---
	TArray<TSharedPtr<FJsonValue>> TexCollArr;
	for (const auto& Param : MFI->TextureCollectionParameterValues)
	{
		auto PJson = MakeShared<FJsonObject>();
		PJson->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
		PJson->SetStringField(TEXT("value"), Param.ParameterValue ? Param.ParameterValue->GetPathName() : TEXT("None"));
		TexCollArr.Add(MakeShared<FJsonValueObject>(PJson));
	}
	ResultJson->SetArrayField(TEXT("texture_collection_overrides"), TexCollArr);

	// --- Parameter collection parameter overrides ---
	TArray<TSharedPtr<FJsonValue>> ParamCollArr;
	for (const auto& Param : MFI->ParameterCollectionParameterValues)
	{
		auto PJson = MakeShared<FJsonObject>();
		PJson->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
		PJson->SetStringField(TEXT("value"), Param.ParameterValue ? Param.ParameterValue->GetPathName() : TEXT("None"));
		ParamCollArr.Add(MakeShared<FJsonValueObject>(PJson));
	}
	ResultJson->SetArrayField(TEXT("parameter_collection_overrides"), ParamCollArr);

	// --- Font parameter overrides ---
	TArray<TSharedPtr<FJsonValue>> FontArr;
	for (const auto& Param : MFI->FontParameterValues)
	{
		auto PJson = MakeShared<FJsonObject>();
		PJson->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
		PJson->SetStringField(TEXT("font"), Param.FontValue ? Param.FontValue->GetPathName() : TEXT("None"));
		PJson->SetNumberField(TEXT("page"), Param.FontPage);
		FontArr.Add(MakeShared<FJsonValueObject>(PJson));
	}
	ResultJson->SetArrayField(TEXT("font_overrides"), FontArr);

	// --- Runtime virtual texture parameter overrides ---
	TArray<TSharedPtr<FJsonValue>> RVTArr;
	for (const auto& Param : MFI->RuntimeVirtualTextureParameterValues)
	{
		auto PJson = MakeShared<FJsonObject>();
		PJson->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
		PJson->SetStringField(TEXT("value"), Param.ParameterValue ? Param.ParameterValue->GetPathName() : TEXT("None"));
		RVTArr.Add(MakeShared<FJsonValueObject>(PJson));
	}
	ResultJson->SetArrayField(TEXT("runtime_virtual_texture_overrides"), RVTArr);

	// --- Sparse volume texture parameter overrides ---
	TArray<TSharedPtr<FJsonValue>> SVTArr;
	for (const auto& Param : MFI->SparseVolumeTextureParameterValues)
	{
		auto PJson = MakeShared<FJsonObject>();
		PJson->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
		PJson->SetStringField(TEXT("value"), Param.ParameterValue ? Param.ParameterValue->GetPathName() : TEXT("None"));
		SVTArr.Add(MakeShared<FJsonValueObject>(PJson));
	}
	ResultJson->SetArrayField(TEXT("sparse_volume_texture_overrides"), SVTArr);

	// --- Static switch parameter overrides ---
	TArray<TSharedPtr<FJsonValue>> SwitchArr;
	for (const auto& Param : MFI->StaticSwitchParameterValues)
	{
		auto PJson = MakeShared<FJsonObject>();
		PJson->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
		PJson->SetBoolField(TEXT("value"), Param.Value);
		PJson->SetBoolField(TEXT("is_overridden"), Param.bOverride);
		SwitchArr.Add(MakeShared<FJsonValueObject>(PJson));
	}
	ResultJson->SetArrayField(TEXT("static_switch_overrides"), SwitchArr);

	// --- Static component mask parameter overrides ---
	TArray<TSharedPtr<FJsonValue>> MaskArr;
	for (const auto& Param : MFI->StaticComponentMaskParameterValues)
	{
		auto PJson = MakeShared<FJsonObject>();
		PJson->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
		PJson->SetBoolField(TEXT("r"), Param.R);
		PJson->SetBoolField(TEXT("g"), Param.G);
		PJson->SetBoolField(TEXT("b"), Param.B);
		PJson->SetBoolField(TEXT("a"), Param.A);
		PJson->SetBoolField(TEXT("is_overridden"), Param.bOverride);
		MaskArr.Add(MakeShared<FJsonValueObject>(PJson));
	}
	ResultJson->SetArrayField(TEXT("static_component_mask_overrides"), MaskArr);

	// --- Inputs and outputs via GetInputsAndOutputs ---
	TArray<FFunctionExpressionInput> FuncInputs;
	TArray<FFunctionExpressionOutput> FuncOutputs;
	MFI->GetInputsAndOutputs(FuncInputs, FuncOutputs);

	const UEnum* InputTypeEnum = StaticEnum<EFunctionInputType>();

	TArray<TSharedPtr<FJsonValue>> InputsJson;
	for (const FFunctionExpressionInput& FuncIn : FuncInputs)
	{
		if (!FuncIn.ExpressionInput)
		{
			continue;
		}
		auto InputJson = MakeShared<FJsonObject>();
		InputJson->SetStringField(TEXT("name"), FuncIn.ExpressionInput->InputName.ToString());
		InputJson->SetStringField(TEXT("expression_name"), FuncIn.ExpressionInput->GetName());
		InputJson->SetNumberField(TEXT("sort_priority"), FuncIn.ExpressionInput->SortPriority);
		if (InputTypeEnum)
		{
			FString TypeStr = InputTypeEnum->GetNameStringByIndex(static_cast<int32>(FuncIn.ExpressionInput->InputType));
			TypeStr.RemoveFromStart(TEXT("FunctionInput_"));
			InputJson->SetStringField(TEXT("type"), TypeStr);
		}
		InputJson->SetStringField(TEXT("description"), FuncIn.ExpressionInput->Description);
		InputJson->SetBoolField(TEXT("use_preview_value_as_default"), FuncIn.ExpressionInput->bUsePreviewValueAsDefault);
		InputsJson.Add(MakeShared<FJsonValueObject>(InputJson));
	}

	InputsJson.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		return A->AsObject()->GetNumberField(TEXT("sort_priority")) < B->AsObject()->GetNumberField(TEXT("sort_priority"));
	});

	TArray<TSharedPtr<FJsonValue>> OutputsJson;
	for (const FFunctionExpressionOutput& FuncOut : FuncOutputs)
	{
		if (!FuncOut.ExpressionOutput)
		{
			continue;
		}
		auto OutputJson = MakeShared<FJsonObject>();
		OutputJson->SetStringField(TEXT("name"), FuncOut.ExpressionOutput->OutputName.ToString());
		OutputJson->SetStringField(TEXT("expression_name"), FuncOut.ExpressionOutput->GetName());
		OutputJson->SetNumberField(TEXT("sort_priority"), FuncOut.ExpressionOutput->SortPriority);
		OutputJson->SetStringField(TEXT("description"), FuncOut.ExpressionOutput->Description);
		OutputsJson.Add(MakeShared<FJsonValueObject>(OutputJson));
	}

	OutputsJson.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		return A->AsObject()->GetNumberField(TEXT("sort_priority")) < B->AsObject()->GetNumberField(TEXT("sort_priority"));
	});

	ResultJson->SetArrayField(TEXT("inputs"), InputsJson);
	ResultJson->SetArrayField(TEXT("outputs"), OutputsJson);
	ResultJson->SetNumberField(TEXT("input_count"), InputsJson.Num());
	ResultJson->SetNumberField(TEXT("output_count"), OutputsJson.Num());

	// Total override count
	int32 TotalOverrides = ScalarArr.Num() + VectorArr.Num() + DoubleVecArr.Num()
		+ TextureArr.Num() + TexCollArr.Num() + ParamCollArr.Num()
		+ FontArr.Num() + RVTArr.Num() + SVTArr.Num()
		+ SwitchArr.Num() + MaskArr.Num();
	ResultJson->SetNumberField(TEXT("total_overrides"), TotalOverrides);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Wave 10 — Function utilities
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::LayoutFunctionExpressions(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	if (!LoadedAsset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load asset at '%s'"), *AssetPath));
	}

	// Reject function instances — layout only works on base functions
	if (Cast<UMaterialFunctionInstance>(LoadedAsset))
	{
		return FMonolithActionResult::Error(TEXT("Cannot layout a function instance — layout the base function instead"));
	}

	UMaterialFunction* MatFunc = Cast<UMaterialFunction>(LoadedAsset);
	if (!MatFunc)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset '%s' is not a UMaterialFunction (type: %s)"), *AssetPath, *LoadedAsset->GetClass()->GetName()));
	}

	UMaterialEditingLibrary::LayoutMaterialFunctionExpressions(MatFunc);
	MatFunc->MarkPackageDirty();

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetBoolField(TEXT("arranged"), true);

	return FMonolithActionResult::Success(ResultJson);
}

FMonolithActionResult FMonolithMaterialActions::RenameFunctionParameterGroup(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString OldGroup  = Params->GetStringField(TEXT("old_group"));
	FString NewGroup  = Params->GetStringField(TEXT("new_group"));

	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	if (!LoadedAsset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load asset at '%s'"), *AssetPath));
	}

	UMaterialFunctionInterface* MatFuncInterface = Cast<UMaterialFunctionInterface>(LoadedAsset);
	if (!MatFuncInterface)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset '%s' is not a MaterialFunctionInterface (type: %s)"), *AssetPath, *LoadedAsset->GetClass()->GetName()));
	}

	bool bRenamed = UMaterialEditingLibrary::RenameMaterialFunctionParameterGroup(MatFuncInterface, FName(*OldGroup), FName(*NewGroup));

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetBoolField(TEXT("renamed"), bRenamed);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Wave 11 — Material expansion
// ============================================================================

// ============================================================================
// Action: clear_graph
// Params: { "asset_path": "...", "preserve_parameters": false }
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::ClearGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	bool bPreserveParams = false;
	Params->TryGetBoolField(TEXT("preserve_parameters"), bPreserveParams);

	UMaterial* Mat = LoadBaseMaterial(AssetPath);
	if (!Mat)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load base material at '%s'"), *AssetPath));
	}

	// Save material-level properties — PostEditChange can reset them
	const EBlendMode SavedBlendMode = Mat->BlendMode;
	const EMaterialShadingModel SavedShadingModel = Mat->GetShadingModels().GetFirstShadingModel();
	const bool bSavedTwoSided = Mat->TwoSided;
	const float SavedOpacityMaskClipValue = Mat->OpacityMaskClipValue;

	GEditor->BeginTransaction(FText::FromString(TEXT("ClearMaterialGraph")));
	Mat->Modify();

	// Collect expressions to delete (copy first to avoid iterator invalidation)
	TArray<UMaterialExpression*> ToDelete;
	TArray<TSharedPtr<FJsonValue>> PreservedArray;
	for (const TObjectPtr<UMaterialExpression>& Expr : Mat->GetExpressions())
	{
		if (!Expr) continue;

		if (bPreserveParams && Expr->bIsParameterExpression)
		{
			auto PJ = MakeShared<FJsonObject>();
			PJ->SetStringField(TEXT("name"), Expr->GetName());
			PJ->SetStringField(TEXT("class"), Expr->GetClass()->GetName());
			PreservedArray.Add(MakeShared<FJsonValueObject>(PJ));
			continue;
		}
		ToDelete.Add(Expr);
	}

	int32 DeletedCount = 0;
	for (UMaterialExpression* Expr : ToDelete)
	{
		UMaterialEditingLibrary::DeleteMaterialExpression(Mat, Expr);
		DeletedCount++;
	}

	// Restore material-level properties
	Mat->BlendMode = SavedBlendMode;
	Mat->SetShadingModel(SavedShadingModel);
	Mat->TwoSided = bSavedTwoSided;
	Mat->OpacityMaskClipValue = SavedOpacityMaskClipValue;

	Mat->PreEditChange(nullptr);
	Mat->PostEditChange();
	GEditor->EndTransaction();

	int32 RemainingCount = Mat->GetExpressions().Num();

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetNumberField(TEXT("deleted_count"), DeletedCount);
	ResultJson->SetNumberField(TEXT("remaining_count"), RemainingCount);
	if (PreservedArray.Num() > 0)
	{
		ResultJson->SetArrayField(TEXT("preserved_parameters"), PreservedArray);
	}

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: delete_expressions
// Params: { "asset_path": "...", "expression_names": ["Name1", "Name2"] }
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::DeleteExpressions(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	const TArray<TSharedPtr<FJsonValue>>* NamesArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("expression_names"), NamesArray) || !NamesArray)
	{
		return FMonolithActionResult::Error(TEXT("Missing required 'expression_names' array"));
	}

	TSet<FString> RequestedNames;
	for (const TSharedPtr<FJsonValue>& Val : *NamesArray)
	{
		FString Name = Val->AsString();
		if (!Name.IsEmpty())
		{
			RequestedNames.Add(Name);
		}
	}

	if (RequestedNames.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("expression_names array is empty"));
	}

	UMaterial* Mat = LoadBaseMaterial(AssetPath);
	if (!Mat)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load base material at '%s'"), *AssetPath));
	}

	// Collect matching expressions into local array first (avoid iterator invalidation)
	TArray<UMaterialExpression*> ToDelete;
	TArray<TSharedPtr<FJsonValue>> DeletedArray;
	TSet<FString> FoundNames;
	for (const TObjectPtr<UMaterialExpression>& Expr : Mat->GetExpressions())
	{
		if (Expr && RequestedNames.Contains(Expr->GetName()))
		{
			ToDelete.Add(Expr);
			FoundNames.Add(Expr->GetName());

			auto DJ = MakeShared<FJsonObject>();
			DJ->SetStringField(TEXT("name"), Expr->GetName());
			DJ->SetStringField(TEXT("class"), Expr->GetClass()->GetName());
			DeletedArray.Add(MakeShared<FJsonValueObject>(DJ));
		}
	}

	// Find names that weren't matched
	TArray<TSharedPtr<FJsonValue>> NotFoundArray;
	for (const FString& Requested : RequestedNames)
	{
		if (!FoundNames.Contains(Requested))
		{
			NotFoundArray.Add(MakeShared<FJsonValueString>(Requested));
		}
	}

	GEditor->BeginTransaction(FText::FromString(TEXT("DeleteExpressions")));
	Mat->Modify();

	for (UMaterialExpression* Expr : ToDelete)
	{
		UMaterialEditingLibrary::DeleteMaterialExpression(Mat, Expr);
	}

	Mat->PreEditChange(nullptr);
	Mat->PostEditChange();
	GEditor->EndTransaction();

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetArrayField(TEXT("deleted"), DeletedArray);
	ResultJson->SetNumberField(TEXT("deleted_count"), DeletedArray.Num());
	ResultJson->SetNumberField(TEXT("remaining_count"), Mat->GetExpressions().Num());
	if (NotFoundArray.Num() > 0)
	{
		ResultJson->SetArrayField(TEXT("not_found"), NotFoundArray);
	}

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Helper: Populate texture metadata JSON
// ============================================================================

static void PopulateTextureMetadata(UTexture* Tex, const TSharedPtr<FJsonObject>& OutJson)
{
	OutJson->SetBoolField(TEXT("srgb"), Tex->SRGB != 0);
	OutJson->SetStringField(TEXT("compression_settings"), UEnum::GetValueAsString(Tex->CompressionSettings));
	OutJson->SetStringField(TEXT("filter"), UEnum::GetValueAsString(Tex->Filter));
	OutJson->SetStringField(TEXT("lod_group"), UEnum::GetValueAsString(Tex->LODGroup));
	OutJson->SetStringField(TEXT("address_x"), UEnum::GetValueAsString(Tex->GetTextureAddressX()));
	OutJson->SetStringField(TEXT("address_y"), UEnum::GetValueAsString(Tex->GetTextureAddressY()));
#if WITH_EDITORONLY_DATA
	OutJson->SetBoolField(TEXT("compression_no_alpha"), Tex->CompressionNoAlpha != 0);
	OutJson->SetBoolField(TEXT("virtual_texture_streaming"), Tex->VirtualTextureStreaming != 0);
#endif

	// Recommended sampler type for material usage
	EMaterialSamplerType SamplerType = UMaterialExpressionTextureBase::GetSamplerTypeForTexture(Tex);
	UEnum* SamplerEnum = StaticEnum<EMaterialSamplerType>();
	if (SamplerEnum)
	{
		OutJson->SetStringField(TEXT("recommended_sampler_type"), SamplerEnum->GetNameStringByValue(static_cast<int64>(SamplerType)));
	}

	// Texture2D-specific properties
	if (UTexture2D* Tex2D = Cast<UTexture2D>(Tex))
	{
		OutJson->SetNumberField(TEXT("width"), Tex2D->GetSizeX());
		OutJson->SetNumberField(TEXT("height"), Tex2D->GetSizeY());
		OutJson->SetNumberField(TEXT("mip_count"), Tex2D->GetNumMips());
		OutJson->SetBoolField(TEXT("has_alpha"), Tex2D->HasAlphaChannel());
		OutJson->SetStringField(TEXT("pixel_format"), GPixelFormats[Tex2D->GetPixelFormat()].Name);
	}
	else if (UTextureCube* TexCube = Cast<UTextureCube>(Tex))
	{
		OutJson->SetNumberField(TEXT("width"), TexCube->GetSizeX());
		OutJson->SetNumberField(TEXT("height"), TexCube->GetSizeX()); // cube faces are square
		OutJson->SetStringField(TEXT("texture_type"), TEXT("TextureCube"));
	}
	else if (UTextureRenderTarget2D* RT = Cast<UTextureRenderTarget2D>(Tex))
	{
		OutJson->SetNumberField(TEXT("width"), RT->SizeX);
		OutJson->SetNumberField(TEXT("height"), RT->SizeY);
		OutJson->SetStringField(TEXT("texture_type"), TEXT("RenderTarget2D"));
	}
}

// ============================================================================
// Action: get_texture_properties
// Params: { "asset_path": "..." }
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::GetTextureProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	if (!LoadedAsset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load asset at '%s'"), *AssetPath));
	}

	UTexture* Tex = Cast<UTexture>(LoadedAsset);
	if (!Tex)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset '%s' is not a UTexture (type: %s)"), *AssetPath, *LoadedAsset->GetClass()->GetName()));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetStringField(TEXT("class"), Tex->GetClass()->GetName());
	PopulateTextureMetadata(Tex, ResultJson);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: preview_texture
// Params: { "asset_path": "...", "resolution": 256, "output_path": "..." }
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::PreviewTexture(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	int32 Resolution = 256;
	{
		double Tmp = 0;
		if (Params->TryGetNumberField(TEXT("resolution"), Tmp) && Tmp > 0) Resolution = static_cast<int32>(Tmp);
	}
	FString OutputPath;
	Params->TryGetStringField(TEXT("output_path"), OutputPath);

	UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
	if (!LoadedAsset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load asset at '%s'"), *AssetPath));
	}

	UTexture* Tex = Cast<UTexture>(LoadedAsset);
	if (!Tex)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset '%s' is not a UTexture (type: %s)"), *AssetPath, *LoadedAsset->GetClass()->GetName()));
	}

	// Render thumbnail
	FObjectThumbnail Thumbnail;
	ThumbnailTools::RenderThumbnail(LoadedAsset, Resolution, Resolution,
		ThumbnailTools::EThumbnailTextureFlushMode::NeverFlush, nullptr, &Thumbnail);

	if (Thumbnail.GetImageWidth() == 0 || Thumbnail.GetImageHeight() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Thumbnail rendering produced an empty image"));
	}

	TArray<uint8>& ThumbData = Thumbnail.AccessImageData();
	int32 Width = Thumbnail.GetImageWidth();
	int32 Height = Thumbnail.GetImageHeight();

	TArray64<uint8> PngData;
	FImageView ImageView((void*)ThumbData.GetData(), Width, Height, ERawImageFormat::BGRA8);
	FImageUtils::CompressImage(PngData, TEXT(".png"), ImageView);

	if (PngData.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Failed to compress thumbnail to PNG"));
	}

	// Save to file
	if (OutputPath.IsEmpty())
	{
		FString AssetName = FPaths::GetBaseFilename(AssetPath);
		FString SaveDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Monolith"), TEXT("previews"));
		IFileManager::Get().MakeDirectory(*SaveDir, true);
		OutputPath = FPaths::Combine(SaveDir, FString::Printf(TEXT("%s_%d.png"), *AssetName, Resolution));
	}

	if (!FFileHelper::SaveArrayToFile(PngData, *OutputPath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to save PNG to '%s'"), *OutputPath));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetStringField(TEXT("file_path"), OutputPath);
	ResultJson->SetNumberField(TEXT("preview_width"), Width);
	ResultJson->SetNumberField(TEXT("preview_height"), Height);
	ResultJson->SetStringField(TEXT("class"), Tex->GetClass()->GetName());
	PopulateTextureMetadata(Tex, ResultJson);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: preview_textures (contact sheet)
// Params: { "asset_paths": [...], "per_texture_size": 128, "output_path": "..." }
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::PreviewTextures(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* PathsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("asset_paths"), PathsArray) || !PathsArray || PathsArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty 'asset_paths' array"));
	}

	int32 TileSize = 128;
	{
		double Tmp = 0;
		if (Params->TryGetNumberField(TEXT("per_texture_size"), Tmp) && Tmp > 0) TileSize = static_cast<int32>(Tmp);
	}
	FString OutputPath;
	Params->TryGetStringField(TEXT("output_path"), OutputPath);

	int32 Count = PathsArray->Num();
	int32 Cols = FMath::CeilToInt(FMath::Sqrt(static_cast<float>(Count)));
	int32 Rows = FMath::CeilToInt(static_cast<float>(Count) / static_cast<float>(Cols));
	int32 SheetWidth = Cols * TileSize;
	int32 SheetHeight = Rows * TileSize;

	// Allocate contact sheet buffer (BGRA8)
	TArray<uint8> SheetData;
	SheetData.SetNumZeroed(SheetWidth * SheetHeight * 4);

	TArray<TSharedPtr<FJsonValue>> TextureInfoArray;
	int32 SuccessCount = 0;

	for (int32 i = 0; i < Count; ++i)
	{
		FString AssetPath = (*PathsArray)[i]->AsString();
		int32 Col = i % Cols;
		int32 Row = i / Cols;

		auto TexInfo = MakeShared<FJsonObject>();
		TexInfo->SetStringField(TEXT("asset_path"), AssetPath);
		TexInfo->SetNumberField(TEXT("index"), i);

		UObject* LoadedAsset = UEditorAssetLibrary::LoadAsset(AssetPath);
		UTexture* Tex = LoadedAsset ? Cast<UTexture>(LoadedAsset) : nullptr;
		if (!Tex)
		{
			TexInfo->SetStringField(TEXT("error"), TEXT("Failed to load as UTexture"));
			TextureInfoArray.Add(MakeShared<FJsonValueObject>(TexInfo));
			continue;
		}

		// Render thumbnail
		FObjectThumbnail Thumbnail;
		ThumbnailTools::RenderThumbnail(LoadedAsset, TileSize, TileSize,
			ThumbnailTools::EThumbnailTextureFlushMode::NeverFlush, nullptr, &Thumbnail);

		if (Thumbnail.GetImageWidth() > 0 && Thumbnail.GetImageHeight() > 0)
		{
			TArray<uint8>& ThumbData = Thumbnail.AccessImageData();
			int32 ThumbW = Thumbnail.GetImageWidth();
			int32 ThumbH = Thumbnail.GetImageHeight();

			// Blit thumbnail into contact sheet at (Col*TileSize, Row*TileSize)
			int32 CopyW = FMath::Min(ThumbW, TileSize);
			int32 CopyH = FMath::Min(ThumbH, TileSize);
			for (int32 Y = 0; Y < CopyH; ++Y)
			{
				int32 SrcOffset = Y * ThumbW * 4;
				int32 DstOffset = ((Row * TileSize + Y) * SheetWidth + Col * TileSize) * 4;
				FMemory::Memcpy(&SheetData[DstOffset], &ThumbData[SrcOffset], CopyW * 4);
			}
			SuccessCount++;
		}

		PopulateTextureMetadata(Tex, TexInfo);
		TextureInfoArray.Add(MakeShared<FJsonValueObject>(TexInfo));
	}

	// Compress contact sheet to PNG
	TArray64<uint8> PngData;
	FImageView SheetView((void*)SheetData.GetData(), SheetWidth, SheetHeight, ERawImageFormat::BGRA8);
	FImageUtils::CompressImage(PngData, TEXT(".png"), SheetView);

	if (PngData.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Failed to compress contact sheet to PNG"));
	}

	// Save
	if (OutputPath.IsEmpty())
	{
		FString SaveDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Monolith"), TEXT("previews"));
		IFileManager::Get().MakeDirectory(*SaveDir, true);
		OutputPath = FPaths::Combine(SaveDir, FString::Printf(TEXT("contact_sheet_%dx%d_%d.png"), Cols, Rows, Count));
	}

	if (!FFileHelper::SaveArrayToFile(PngData, *OutputPath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to save contact sheet to '%s'"), *OutputPath));
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("file_path"), OutputPath);
	ResultJson->SetNumberField(TEXT("texture_count"), Count);
	ResultJson->SetNumberField(TEXT("successful_renders"), SuccessCount);
	ResultJson->SetNumberField(TEXT("sheet_width"), SheetWidth);
	ResultJson->SetNumberField(TEXT("sheet_height"), SheetHeight);
	ResultJson->SetNumberField(TEXT("columns"), Cols);
	ResultJson->SetNumberField(TEXT("rows"), Rows);
	ResultJson->SetNumberField(TEXT("tile_size"), TileSize);
	ResultJson->SetArrayField(TEXT("textures"), TextureInfoArray);

	return FMonolithActionResult::Success(ResultJson);
}

// ============================================================================
// Action: check_tiling_quality
// Params: { "asset_path": "..." }
// ============================================================================

FMonolithActionResult FMonolithMaterialActions::CheckTilingQuality(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath = Params->GetStringField(TEXT("asset_path"));

	UMaterial* Mat = LoadBaseMaterial(AssetPath);
	if (!Mat)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load base material at '%s'"), *AssetPath));
	}

	TArray<TSharedPtr<FJsonValue>> IssuesArray;
	bool bHasAntiTiling = false;
	bool bHasMacroVariation = false;

	// Gather all expressions by type for analysis
	TArray<UMaterialExpressionTextureSample*> TextureSamples;
	bool bHasWorldPosition = false;
	bool bHasNoise = false;
	bool bHasCustomHLSL = false;

	for (const TObjectPtr<UMaterialExpression>& Expr : Mat->GetExpressions())
	{
		if (!Expr) continue;

		if (UMaterialExpressionTextureSample* TexSample = Cast<UMaterialExpressionTextureSample>(Expr))
		{
			TextureSamples.Add(TexSample);
		}
		if (Expr->IsA(UMaterialExpressionCustom::StaticClass()))
		{
			bHasCustomHLSL = true;
			// Check if HLSL code contains noise-related keywords
			UMaterialExpressionCustom* CustomExpr = Cast<UMaterialExpressionCustom>(Expr);
			if (CustomExpr)
			{
				FString Code = CustomExpr->Code.ToLower();
				if (Code.Contains(TEXT("noise")) || Code.Contains(TEXT("hash"))
					|| Code.Contains(TEXT("random")) || Code.Contains(TEXT("fbm"))
					|| Code.Contains(TEXT("voronoi")) || Code.Contains(TEXT("perlin")))
				{
					bHasAntiTiling = true;
				}
			}
		}

		FString ClassName = Expr->GetClass()->GetName();
		if (ClassName.Contains(TEXT("WorldPosition")))
		{
			bHasWorldPosition = true;
			bHasAntiTiling = true; // World-space UVs break tiling naturally
		}
		if (ClassName.Contains(TEXT("Noise")))
		{
			bHasNoise = true;
			bHasAntiTiling = true;
		}
		if (ClassName.Contains(TEXT("MaterialFunctionCall")))
		{
			// Check if function name suggests anti-tiling
			UMaterialExpressionMaterialFunctionCall* FuncCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expr);
			if (FuncCall && FuncCall->MaterialFunction)
			{
				FString FuncName = FuncCall->MaterialFunction->GetName().ToLower();
				if (FuncName.Contains(TEXT("antitile")) || FuncName.Contains(TEXT("anti_tile"))
					|| FuncName.Contains(TEXT("macro")) || FuncName.Contains(TEXT("variation"))
					|| FuncName.Contains(TEXT("triplanar"))
					|| FuncName.Contains(TEXT("worldaligned")) || FuncName.Contains(TEXT("world_aligned")))
				{
					bHasAntiTiling = true;
					if (FuncName.Contains(TEXT("macro")))
					{
						bHasMacroVariation = true;
					}
				}
			}
		}
	}

	// Analyze each texture sample's UV input
	for (UMaterialExpressionTextureSample* TexSample : TextureSamples)
	{
		// Check UV input (Coordinates)
		FExpressionInput& CoordInput = TexSample->Coordinates;
		bool bDirectTexCoord = false;
		FString UVSource = TEXT("default (TexCoord0)");

		if (!CoordInput.Expression)
		{
			// No UV input at all — using default TexCoord0
			bDirectTexCoord = true;
		}
		else
		{
			FString InputClassName = CoordInput.Expression->GetClass()->GetName();
			if (InputClassName.Contains(TEXT("TextureCoordinate")))
			{
				bDirectTexCoord = true;
				UVSource = TEXT("TextureCoordinate (direct)");
			}
		}

		if (bDirectTexCoord)
		{
			auto IssueJson = MakeShared<FJsonObject>();
			IssueJson->SetStringField(TEXT("expression"), TexSample->GetName());
			IssueJson->SetStringField(TEXT("uv_source"), UVSource);
			IssueJson->SetStringField(TEXT("suggestion"),
				TEXT("Direct UV tiling — visible repetition likely at distance. Consider adding noise offset, world-position blend, or anti-tiling function."));

			// Get texture name if available
			if (TexSample->Texture)
			{
				IssueJson->SetStringField(TEXT("texture"), TexSample->Texture->GetPathName());
			}
			IssuesArray.Add(MakeShared<FJsonValueObject>(IssueJson));
		}
	}

	// Check for macro variation (large-scale noise overlaid on BaseColor or Roughness)
	FExpressionInput* BaseColorInput = Mat->GetExpressionInputForProperty(MP_BaseColor);
	FExpressionInput* RoughnessInput = Mat->GetExpressionInputForProperty(MP_Roughness);

	auto CheckForMacroVariation = [&](FExpressionInput* Input, const FString& PropName)
	{
		if (!Input || !Input->Expression) return;

		// BFS upstream walk — look for noise indicators up to MaxDepth hops
		TSet<UMaterialExpression*> Visited;
		TArray<UMaterialExpression*> Stack;
		Stack.Push(Input->Expression);
		constexpr int32 MaxDepth = 4;

		for (int32 Depth = 0; Depth < MaxDepth && Stack.Num() > 0; ++Depth)
		{
			TArray<UMaterialExpression*> NextLevel;
			for (UMaterialExpression* Expr : Stack)
			{
				if (!Expr || Visited.Contains(Expr)) continue;
				Visited.Add(Expr);

				FString ClassName = Expr->GetClass()->GetName();

				// Direct noise/worldposition/custom expression
				if (ClassName.Contains(TEXT("Noise")) || ClassName.Contains(TEXT("WorldPosition"))
					|| ClassName.Contains(TEXT("Custom")))
				{
					bHasMacroVariation = true;
					return;
				}

				// TextureSample sampling a noise texture (by texture name heuristic)
				if (UMaterialExpressionTextureSample* TS = Cast<UMaterialExpressionTextureSample>(Expr))
				{
					if (TS->Texture)
					{
						FString TexName = TS->Texture->GetName().ToLower();
						if (TexName.Contains(TEXT("noise")) || TexName.Contains(TEXT("curl"))
							|| TexName.Contains(TEXT("perlin")) || TexName.Contains(TEXT("simplex"))
							|| TexName.Contains(TEXT("fbm")) || TexName.Contains(TEXT("voronoi"))
							|| TexName.Contains(TEXT("grunge")))
						{
							bHasMacroVariation = true;
							return;
						}
					}
				}

				// MaterialFunctionCall with macro/variation/noise in name
				if (UMaterialExpressionMaterialFunctionCall* FC = Cast<UMaterialExpressionMaterialFunctionCall>(Expr))
				{
					if (FC->MaterialFunction)
					{
						FString FuncName = FC->MaterialFunction->GetName().ToLower();
						if (FuncName.Contains(TEXT("noise")) || FuncName.Contains(TEXT("macro"))
							|| FuncName.Contains(TEXT("variation")))
						{
							bHasMacroVariation = true;
							return;
						}
					}
				}

				// Enqueue upstream inputs for next depth level
				for (FExpressionInputIterator It(Expr); It; ++It)
				{
					if (It.Input && It.Input->Expression)
					{
						NextLevel.Add(It.Input->Expression);
					}
				}
			}
			Stack = MoveTemp(NextLevel);
		}
	};

	CheckForMacroVariation(BaseColorInput, TEXT("BaseColor"));
	CheckForMacroVariation(RoughnessInput, TEXT("Roughness"));

	// Suggest world-aligned UVs for environment materials that don't already use them
	{
		FString MatName = Mat->GetName().ToLower();
		bool bIsEnvironmentMaterial = MatName.Contains(TEXT("floor")) || MatName.Contains(TEXT("wall"))
			|| MatName.Contains(TEXT("ground")) || MatName.Contains(TEXT("terrain"))
			|| MatName.Contains(TEXT("rock")) || MatName.Contains(TEXT("ceiling"))
			|| MatName.Contains(TEXT("concrete")) || MatName.Contains(TEXT("brick"));

		if (bIsEnvironmentMaterial && !bHasWorldPosition)
		{
			// Also check if MF_WorldAlignedUV is already in use via function calls
			bool bHasWorldAlignedFunc = false;
			for (const TObjectPtr<UMaterialExpression>& Expr : Mat->GetExpressions())
			{
				if (!Expr) continue;
				if (UMaterialExpressionMaterialFunctionCall* FuncCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expr))
				{
					if (FuncCall->MaterialFunction)
					{
						FString FuncName = FuncCall->MaterialFunction->GetName().ToLower();
						if (FuncName.Contains(TEXT("worldaligned")) || FuncName.Contains(TEXT("world_aligned")))
						{
							bHasWorldAlignedFunc = true;
							break;
						}
					}
				}
			}

			if (!bHasWorldAlignedFunc)
			{
				auto SuggestionJson = MakeShared<FJsonObject>();
				SuggestionJson->SetStringField(TEXT("type"), TEXT("world_aligned_uv"));
				SuggestionJson->SetStringField(TEXT("suggestion"),
					TEXT("Environment material may benefit from world-aligned UVs (MF_WorldAlignedUV) for consistent texel density across differently-sized meshes"));
				IssuesArray.Add(MakeShared<FJsonValueObject>(SuggestionJson));
			}
		}
	}

	auto ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), AssetPath);
	ResultJson->SetNumberField(TEXT("texture_sample_count"), TextureSamples.Num());
	ResultJson->SetNumberField(TEXT("tiling_issue_count"), IssuesArray.Num());
	ResultJson->SetBoolField(TEXT("has_anti_tiling"), bHasAntiTiling);
	ResultJson->SetBoolField(TEXT("has_macro_variation"), bHasMacroVariation);
	ResultJson->SetBoolField(TEXT("has_world_position_uvs"), bHasWorldPosition);
	ResultJson->SetBoolField(TEXT("has_noise_nodes"), bHasNoise);
	ResultJson->SetBoolField(TEXT("has_custom_hlsl"), bHasCustomHLSL);
	ResultJson->SetArrayField(TEXT("tiling_issues"), IssuesArray);

	if (!bHasAntiTiling && TextureSamples.Num() > 0)
	{
		ResultJson->SetStringField(TEXT("recommendation"),
			TEXT("No anti-tiling detected. Consider adding MF_AntiTile_IqOffset, world-position noise offset, or macro variation overlay to reduce visible repetition."));
	}

	return FMonolithActionResult::Success(ResultJson);
}
