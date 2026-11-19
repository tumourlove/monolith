// Unit coverage for the two text parsers find_unused and remove rely on: what
// counts as a tag named in C++, and what counts as a tag used in config.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "MonolithGameplayTagsInternal.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGameplayTagsCppLiteralTest,
	"Monolith.GameplayTags.Evidence.CppLiterals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGameplayTagsCppLiteralTest::RunTest(const FString& Parameters)
{
	using namespace MonolithGameplayTags;

	const FString Source = TEXT(
		"UE_DEFINE_GAMEPLAY_TAG(TAG_Defined, \"Ability.Defined\");\n"
		"const FGameplayTag Requested = FGameplayTag::RequestGameplayTag(TEXT(\"Ability.Requested\"));\n"
		"// \"Ability.InLineComment\" is documentation, not a use\n"
		"/* \"Ability.InBlockComment\"\n   spans lines */\n"
		"const TCHAR Quote = '\"'; const FString After = TEXT(\"Ability.AfterCharLiteral\");\n"
		"UE_LOG(LogMonolith, Log, TEXT(\"Loaded %d tags\"), Count);\n"
		"FString Escaped = TEXT(\"Ability.Escaped\\n\");\n"
		"int32 Input = 0; FString Path = TEXT(\".Leading\");\n");

	TMap<FString, FString> Literals;
	ExtractTagLiterals(Source, TEXT("Test.cpp"), Literals);

	TestTrue(TEXT("tag in a define macro"), Literals.Contains(TEXT("Ability.Defined")));
	TestTrue(TEXT("tag passed to RequestGameplayTag"), Literals.Contains(TEXT("Ability.Requested")));
	TestTrue(TEXT("string after a quote character literal"), Literals.Contains(TEXT("Ability.AfterCharLiteral")));
	TestFalse(TEXT("line comment"), Literals.Contains(TEXT("Ability.InLineComment")));
	TestFalse(TEXT("block comment"), Literals.Contains(TEXT("Ability.InBlockComment")));
	TestFalse(TEXT("format string with spaces"), Literals.Contains(TEXT("Loaded %d tags")));
	TestFalse(TEXT("string with an escape sequence"), Literals.Contains(TEXT("Ability.Escaped")));
	TestFalse(TEXT("bare identifier"), Literals.Contains(TEXT("Input")));
	TestFalse(TEXT("leading period"), Literals.Contains(TEXT(".Leading")));
	TestEqual(TEXT("first file is recorded"), Literals.FindRef(TEXT("Ability.Defined")), FString(TEXT("Test.cpp")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGameplayTagsConfigUseTest,
	"Monolith.GameplayTags.Evidence.ConfigUses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGameplayTagsConfigUseTest::RunTest(const FString& Parameters)
{
	using namespace MonolithGameplayTags;

	const FString Config = TEXT(
		"[/Script/CommonUI.CommonUISettings]\n"
		"+PlatformTraits=(TagName=\"Platform.Trait.Mouse\")\n"
		"[/Script/Game.SurfaceSettings]\n"
		"SurfaceMap=((Surface1, (TagName=\"Surface.Glass\")),(Surface2, (TagName=\"Surface.Metal\")))\n"
		"[/Script/GameplayTags.GameplayTagsList]\n"
		"+GameplayTagList=(Tag=\"Declared.Only\",DevComment=\"\")\n"
		"+GameplayTagRedirects=(OldTagName=\"Old.Name\",NewTagName=\"New.Name\")\n");

	TMap<FString, FString> Uses;
	ExtractConfigTagUses(Config, TEXT("DefaultGame.ini"), Uses);

	TestTrue(TEXT("platform trait"), Uses.Contains(TEXT("Platform.Trait.Mouse")));
	TestTrue(TEXT("tag inside a struct map, first entry"), Uses.Contains(TEXT("Surface.Glass")));
	TestTrue(TEXT("tag inside a struct map, second entry"), Uses.Contains(TEXT("Surface.Metal")));
	TestFalse(TEXT("a declaration is not a use"), Uses.Contains(TEXT("Declared.Only")));
	TestFalse(TEXT("a redirect source is not a use"), Uses.Contains(TEXT("Old.Name")));
	TestFalse(TEXT("a redirect target is not a use"), Uses.Contains(TEXT("New.Name")));
	TestEqual(TEXT("exactly the three uses"), Uses.Num(), 3);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
