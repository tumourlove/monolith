// Automation coverage for the write half of the `gameplay_tags` namespace. Every test
// declares its tags in throwaway ini sources under Saved/AutomationTests, below a
// root tag unique to the run, and removes them again, so the project's own tag
// configuration is never touched. Actions are invoked through the tool registry,
// the same path an MCP call takes.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameplayTagsEditorModule.h"
#include "GameplayTagsManager.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "MonolithToolRegistry.h"

namespace MonolithGameplayTagsWriteTestDetail
{
	const TCHAR* const PrimarySource = TEXT("MonolithTagsWriteTest.ini");
	const TCHAR* const SecondarySource = TEXT("MonolithTagsWriteTestB.ini");

	/** Two empty ini sources and a unique root tag, torn down in the destructor. */
	struct FTagSandbox
	{
		FString Dir;
		FString Root;

		FTagSandbox()
		{
			Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AutomationTests") / TEXT("MonolithGameplayTags"));
			IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
			for (const TCHAR* Source : { PrimarySource, SecondarySource })
			{
				FFileHelper::SaveStringToFile(FString(), *(Dir / Source));
			}

			// The search path makes the manager register each ini in the folder as
			// a tag source whose config file lives there, so writes land in Saved.
			UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
			Manager.AddTagIniSearchPath(Dir);
			Manager.EditorRefreshGameplayTagTree();

			Root = FString::Printf(TEXT("MonolithTagsTest%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
		}

		~FTagSandbox()
		{
			UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
			IGameplayTagsEditorModule& TagsEditor = IGameplayTagsEditorModule::Get();

			// Deepest first, so each delete sees its children already gone.
			FGameplayTagContainer All;
			Manager.RequestAllGameplayTags(All, /*OnlyIncludeDictionaryTags=*/true);
			TArray<FString> Ours;
			for (const FGameplayTag& Tag : All)
			{
				const FString Name = Tag.ToString();
				if (Name == Root || Name.StartsWith(Root + TEXT(".")))
				{
					Ours.Add(Name);
				}
			}
			Ours.Sort([](const FString& A, const FString& B) { return A.Len() > B.Len(); });
			for (const FString& Name : Ours)
			{
				const TSharedPtr<FGameplayTagNode> Node = Manager.FindTagNode(FName(*Name));
				if (Node.IsValid() && Node->IsExplicitTag())
				{
					TagsEditor.DeleteTagFromINI(Node);
				}
			}

			Manager.RemoveTagIniSearchPath(Dir);
			for (const TCHAR* Source : { PrimarySource, SecondarySource })
			{
				IFileManager::Get().Delete(*(Dir / Source), /*RequireExists=*/false, /*EvenReadOnly=*/true);
			}
			Manager.EditorRefreshGameplayTagTree();
		}

		FString Tag(const TCHAR* Suffix) const
		{
			return Root + TEXT(".") + Suffix;
		}
	};

	FMonolithActionResult Run(const TCHAR* Action, const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("gameplay_tags"), Action, Params);
	}

	TSharedPtr<FJsonObject> TagsParams(const TArray<FString>& Tags)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Tag : Tags)
		{
			Values.Add(MakeShared<FJsonValueString>(Tag));
		}
		Params->SetArrayField(TEXT("tags"), Values);
		return Params;
	}

	/** The status field of the row for one tag in a batch response, or empty. */
	FString RowStatus(const FMonolithActionResult& Result, const FString& Tag)
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (Result.Result.IsValid() && Result.Result->TryGetArrayField(TEXT("tags"), Rows))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Rows)
			{
				const TSharedPtr<FJsonObject> Row = Value->AsObject();
				if (Row.IsValid() && Row->GetStringField(TEXT("tag")) == Tag)
				{
					return Row->GetStringField(TEXT("status"));
				}
			}
		}
		return FString();
	}

	bool IsDeclared(const FString& Tag)
	{
		const TSharedPtr<FGameplayTagNode> Node = UGameplayTagsManager::Get().FindTagNode(FName(*Tag));
		return Node.IsValid() && Node->IsExplicitTag();
	}

	/**
	 * True when the tag tree lists this exact name. FindTagNode follows redirects,
	 * so after a rename it would find the new tag under the old name.
	 */
	bool IsListed(const FString& Tag)
	{
		FGameplayTagContainer All;
		UGameplayTagsManager::Get().RequestAllGameplayTags(All, /*OnlyIncludeDictionaryTags=*/false);
		for (const FGameplayTag& Candidate : All)
		{
			if (Candidate.ToString() == Tag)
			{
				return true;
			}
		}
		return false;
	}

	bool IsDeclaredIn(const FString& Tag, const TCHAR* Source)
	{
		const TSharedPtr<FGameplayTagNode> Node = UGameplayTagsManager::Get().FindTagNode(FName(*Tag));
		return Node.IsValid() && Node->GetAllSourceNames().Contains(FName(Source));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGameplayTagsAddTest,
	"Monolith.GameplayTags.Write.Add",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGameplayTagsAddTest::RunTest(const FString& Parameters)
{
	using namespace MonolithGameplayTagsWriteTestDetail;
	FTagSandbox Sandbox;

	const FString Leaf = Sandbox.Tag(TEXT("Group.Leaf"));
	// Projects choose their own invalid characters (this one allows any), but a
	// trailing period is refused by the engine whatever the settings say.
	const FString Invalid = Sandbox.Tag(TEXT("Trailing."));

	TSharedPtr<FJsonObject> Params = TagsParams({ Leaf, Invalid, Leaf });
	Params->SetStringField(TEXT("source"), PrimarySource);
	Params->SetStringField(TEXT("comment"), TEXT("written by an automation test"));
	const FMonolithActionResult First = Run(TEXT("add"), Params);
	if (!TestTrue(TEXT("add succeeds as a call"), First.bSuccess))
	{
		AddError(First.ErrorMessage);
		return false;
	}
	TestEqual(TEXT("valid tag is added"), RowStatus(First, Leaf), FString(TEXT("added")));
	TestEqual(TEXT("invalid name is rejected with a reason"), RowStatus(First, Invalid), FString(TEXT("invalid")));
	TestTrue(TEXT("tag is declared in the requested source"), IsDeclaredIn(Leaf, PrimarySource));
	TestFalse(TEXT("parent stays implicit"), IsDeclared(Sandbox.Tag(TEXT("Group"))));

	const FMonolithActionResult Second = Run(TEXT("add"), Params);
	TestEqual(TEXT("adding again reports exists"), RowStatus(Second, Leaf), FString(TEXT("exists")));

	TSharedPtr<FJsonObject> UnknownSource = TagsParams({ Sandbox.Tag(TEXT("Other")) });
	UnknownSource->SetStringField(TEXT("source"), TEXT("NoSuchMonolithSource.ini"));
	TestFalse(TEXT("unknown source is refused"), Run(TEXT("add"), UnknownSource).bSuccess);
	TestFalse(TEXT("nothing was declared for the refused call"), IsDeclared(Sandbox.Tag(TEXT("Other"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGameplayTagsUpdateTest,
	"Monolith.GameplayTags.Write.UpdateComment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGameplayTagsUpdateTest::RunTest(const FString& Parameters)
{
	using namespace MonolithGameplayTagsWriteTestDetail;
	FTagSandbox Sandbox;

	const FString Tag = Sandbox.Tag(TEXT("Commented"));
	TSharedPtr<FJsonObject> AddParams = TagsParams({ Tag });
	AddParams->SetStringField(TEXT("source"), PrimarySource);
	AddParams->SetStringField(TEXT("comment"), TEXT("before"));
	TestEqual(TEXT("setup add"), RowStatus(Run(TEXT("add"), AddParams), Tag), FString(TEXT("added")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("tag"), Tag);
	Params->SetStringField(TEXT("comment"), TEXT("after"));
	const FMonolithActionResult Result = Run(TEXT("update"), Params);
	if (!TestTrue(TEXT("update succeeds"), Result.bSuccess))
	{
		AddError(Result.ErrorMessage);
		return false;
	}
	TestEqual(TEXT("old comment is echoed"), Result.Result->GetStringField(TEXT("old_comment")), FString(TEXT("before")));
	const TSharedPtr<FGameplayTagNode> Node = UGameplayTagsManager::Get().FindTagNode(FName(*Tag));
	TestTrue(TEXT("tag still declared"), Node.IsValid());
	if (Node.IsValid())
	{
		TestEqual(TEXT("the tag tree carries the new comment"), Node->GetDevComment(), FString(TEXT("after")));
	}

	TSharedPtr<FJsonObject> Empty = MakeShared<FJsonObject>();
	Empty->SetStringField(TEXT("tag"), Tag);
	TestFalse(TEXT("update with nothing to change is refused"), Run(TEXT("update"), Empty).bSuccess);

	TSharedPtr<FJsonObject> Implicit = MakeShared<FJsonObject>();
	Implicit->SetStringField(TEXT("tag"), Sandbox.Root);
	Implicit->SetStringField(TEXT("comment"), TEXT("x"));
	TestFalse(TEXT("an implicit parent has no declaration to edit"), Run(TEXT("update"), Implicit).bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGameplayTagsRemoveTest,
	"Monolith.GameplayTags.Write.Remove",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGameplayTagsRemoveTest::RunTest(const FString& Parameters)
{
	using namespace MonolithGameplayTagsWriteTestDetail;
	FTagSandbox Sandbox;

	const FString Doomed = Sandbox.Tag(TEXT("Doomed"));
	const FString Child = Sandbox.Tag(TEXT("Branch.Child"));
	TSharedPtr<FJsonObject> AddParams = TagsParams({ Doomed, Child });
	AddParams->SetStringField(TEXT("source"), PrimarySource);
	Run(TEXT("add"), AddParams);
	TestTrue(TEXT("setup: both declared"), IsDeclared(Doomed) && IsDeclared(Child));

	TSharedPtr<FJsonObject> DryRun = TagsParams({ Doomed });
	DryRun->SetBoolField(TEXT("dry_run"), true);
	TestEqual(TEXT("dry run reports would_remove"), RowStatus(Run(TEXT("remove"), DryRun), Doomed), FString(TEXT("would_remove")));
	TestTrue(TEXT("dry run writes nothing"), IsDeclared(Doomed));

	TestEqual(TEXT("implicit parent is refused"),
		RowStatus(Run(TEXT("remove"), TagsParams({ Sandbox.Tag(TEXT("Branch")) })), Sandbox.Tag(TEXT("Branch"))),
		FString(TEXT("refused")));

	const FMonolithActionResult Result = Run(TEXT("remove"), TagsParams({ Doomed }));
	TestEqual(TEXT("unused tag is removed"), RowStatus(Result, Doomed), FString(TEXT("removed")));
	TestFalse(TEXT("tag is gone from the tree"), IsDeclared(Doomed));

	TestEqual(TEXT("unknown tag is refused"),
		RowStatus(Run(TEXT("remove"), TagsParams({ Sandbox.Tag(TEXT("Never")) })), Sandbox.Tag(TEXT("Never"))),
		FString(TEXT("refused")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGameplayTagsRenameTest,
	"Monolith.GameplayTags.Write.Rename",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGameplayTagsRenameTest::RunTest(const FString& Parameters)
{
	using namespace MonolithGameplayTagsWriteTestDetail;
	FTagSandbox Sandbox;

	const FString OldParent = Sandbox.Tag(TEXT("Old"));
	const FString OldLeaf = Sandbox.Tag(TEXT("Old.Leaf"));
	TSharedPtr<FJsonObject> AddParams = TagsParams({ OldParent, OldLeaf });
	AddParams->SetStringField(TEXT("source"), PrimarySource);
	Run(TEXT("add"), AddParams);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("old_tag"), OldParent);
	Params->SetStringField(TEXT("new_tag"), Sandbox.Tag(TEXT("New")));
	Params->SetBoolField(TEXT("dry_run"), true);
	const FMonolithActionResult Plan = Run(TEXT("rename"), Params);
	if (!TestTrue(TEXT("dry run succeeds"), Plan.bSuccess))
	{
		AddError(Plan.ErrorMessage);
		return false;
	}
	TestEqual(TEXT("dry run plans the whole subtree"), Plan.Result->GetArrayField(TEXT("renames")).Num(), 2);
	TestTrue(TEXT("dry run writes nothing"), IsDeclared(OldParent));

	Params->SetBoolField(TEXT("dry_run"), false);
	const FMonolithActionResult Result = Run(TEXT("rename"), Params);
	if (!TestTrue(TEXT("rename succeeds"), Result.bSuccess))
	{
		AddError(Result.ErrorMessage);
		return false;
	}
	TestTrue(TEXT("new parent declared"), IsDeclared(Sandbox.Tag(TEXT("New"))));
	TestTrue(TEXT("new leaf declared"), IsDeclared(Sandbox.Tag(TEXT("New.Leaf"))));
	TestFalse(TEXT("old leaf is gone"), IsListed(OldLeaf));
	TestFalse(TEXT("old parent is gone"), IsListed(OldParent));
	TestTrue(TEXT("response reports the parent re-declared"), Result.Result->HasField(TEXT("redeclared")));
	TestTrue(TEXT("response says the old declarations are gone"), Result.Result->GetBoolField(TEXT("old_removed")));
	TestTrue(TEXT("response says the new names are declared"), Result.Result->GetBoolField(TEXT("new_declared")));

	TSharedPtr<FJsonObject> IntoItself = MakeShared<FJsonObject>();
	IntoItself->SetStringField(TEXT("old_tag"), Sandbox.Tag(TEXT("New")));
	IntoItself->SetStringField(TEXT("new_tag"), Sandbox.Tag(TEXT("New.Inner")));
	TestFalse(TEXT("renaming below itself is refused"), Run(TEXT("rename"), IntoItself).bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGameplayTagsMoveTest,
	"Monolith.GameplayTags.Write.Move",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGameplayTagsMoveTest::RunTest(const FString& Parameters)
{
	using namespace MonolithGameplayTagsWriteTestDetail;
	FTagSandbox Sandbox;

	const FString Tag = Sandbox.Tag(TEXT("Mover"));
	TSharedPtr<FJsonObject> AddParams = TagsParams({ Tag });
	AddParams->SetStringField(TEXT("source"), PrimarySource);
	AddParams->SetStringField(TEXT("comment"), TEXT("travels with the tag"));
	Run(TEXT("add"), AddParams);

	TSharedPtr<FJsonObject> Params = TagsParams({ Tag });
	Params->SetStringField(TEXT("target_source"), SecondarySource);
	const FMonolithActionResult Result = Run(TEXT("move"), Params);
	if (!TestTrue(TEXT("move succeeds as a call"), Result.bSuccess))
	{
		AddError(Result.ErrorMessage);
		return false;
	}
	TestEqual(TEXT("tag is moved"), RowStatus(Result, Tag), FString(TEXT("moved")));
	TestTrue(TEXT("now declared in the target"), IsDeclaredIn(Tag, SecondarySource));
	TestFalse(TEXT("no longer declared in the origin"), IsDeclaredIn(Tag, PrimarySource));

	const TSharedPtr<FGameplayTagNode> Node = UGameplayTagsManager::Get().FindTagNode(FName(*Tag));
	if (Node.IsValid())
	{
		TestEqual(TEXT("comment survives the move"), Node->GetDevComment(), FString(TEXT("travels with the tag")));
	}

	TestEqual(TEXT("moving into the source it is in is refused"), RowStatus(Run(TEXT("move"), Params), Tag), FString(TEXT("refused")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
