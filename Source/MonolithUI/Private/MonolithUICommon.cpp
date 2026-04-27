// Copyright tumourlove. All Rights Reserved.
#include "MonolithUICommon.h"

#include "MonolithToolRegistry.h"

// UMG widget classes — required for the WidgetClassFromName curated table.
#include "Blueprint/WidgetTree.h"
#include "Components/BackgroundBlur.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"   // FAnchors (transitive via Layout/Geometry.h)
#include "Components/CheckBox.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableText.h"
#include "Components/EditableTextBox.h"
#include "Components/GridPanel.h"
#include "Components/HorizontalBox.h"
#include "Components/Image.h"
#include "Components/InputKeySelector.h"
#include "Components/ListView.h"
#include "Components/NamedSlot.h"
#include "Components/Overlay.h"
#include "Components/ProgressBar.h"
#include "Components/RichTextBlock.h"
#include "Components/ScaleBox.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/Slider.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/TileView.h"
#include "Components/UniformGridPanel.h"
#include "Components/VerticalBox.h"
#include "Components/WidgetSwitcher.h"
#include "Components/WrapBox.h"

#include "Animation/WidgetAnimation.h"
#include "WidgetBlueprint.h"

// R3b -- optional EffectSurface provider probe. The UClass lookup scans loaded
// widget classes by public class token; no provider module name or header is
// baked into public Monolith source.
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/WeakObjectPtr.h"

DEFINE_LOG_CATEGORY(LogMonolithUISpec);

namespace MonolithUI
{
    FName MakeTokenFromClassName(const UClass* Class)
    {
        if (!Class)
        {
            return NAME_None;
        }

        const FString ClassName = Class->GetName();
        if (ClassName.Len() > 1 && ClassName[0] == TEXT('U') && FChar::IsUpper(ClassName[1]))
        {
            return FName(*ClassName.Mid(1));
        }
        return FName(*ClassName);
    }

    FLinearColor ParseColor(const FString& ColorStr)
    {
        FString S = ColorStr.TrimStartAndEnd();

        // Hex format — degamma'd via the FLinearColor(FColor) ctor.
        if (S.StartsWith(TEXT("#")))
        {
            const FColor C = FColor::FromHex(S);
            return FLinearColor(C);
        }

        // r,g,b,a float format.
        TArray<FString> Parts;
        S.ParseIntoArray(Parts, TEXT(","));
        if (Parts.Num() >= 3)
        {
            const float R = FCString::Atof(*Parts[0].TrimStartAndEnd());
            const float G = FCString::Atof(*Parts[1].TrimStartAndEnd());
            const float B = FCString::Atof(*Parts[2].TrimStartAndEnd());
            const float A = Parts.Num() >= 4 ? FCString::Atof(*Parts[3].TrimStartAndEnd()) : 1.f;
            return FLinearColor(R, G, B, A);
        }

        return FLinearColor::White;
    }

    bool TryParseColor(const FString& InStr, FLinearColor& OutColor)
    {
        const FString S = InStr.TrimStartAndEnd();
        if (S.IsEmpty())
        {
            return false;
        }

        if (S.StartsWith(TEXT("#")))
        {
            // FColor::FromHex returns zeroed FColor on failure — detect by
            // checking the input length is plausible for the supported forms.
            const int32 HexLen = S.Len() - 1;
            if (HexLen != 3 && HexLen != 6 && HexLen != 8)
            {
                return false;
            }
            const FColor C = FColor::FromHex(S);
            // Direct byte-to-float — keep sRGB values as-is. The Slate UI
            // shader applies its own gamma correction; the FLinearColor(FColor)
            // ctor would otherwise wash colors out.
            OutColor = FLinearColor(C.R / 255.0f, C.G / 255.0f, C.B / 255.0f, C.A / 255.0f);
            return true;
        }

        // Comma-delimited floats.
        TArray<FString> Parts;
        S.ParseIntoArray(Parts, TEXT(","));
        if (Parts.Num() >= 3)
        {
            const float R = FCString::Atof(*Parts[0].TrimStartAndEnd());
            const float G = FCString::Atof(*Parts[1].TrimStartAndEnd());
            const float B = FCString::Atof(*Parts[2].TrimStartAndEnd());
            const float A = Parts.Num() >= 4 ? FCString::Atof(*Parts[3].TrimStartAndEnd()) : 1.0f;
            OutColor = FLinearColor(R, G, B, A);
            return true;
        }

        return false;
    }

    UWidgetBlueprint* LoadWidgetBlueprint(const FString& AssetPath, FMonolithActionResult& OutError)
    {
        UObject* Loaded = StaticLoadObject(UWidgetBlueprint::StaticClass(), nullptr, *AssetPath);
        if (!Loaded)
        {
            FString CleanPath = AssetPath;
            if (!CleanPath.StartsWith(TEXT("/")))
            {
                CleanPath = TEXT("/Game/") + CleanPath;
            }
            Loaded = StaticLoadObject(UWidgetBlueprint::StaticClass(), nullptr, *CleanPath);
        }

        UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Loaded);
        if (!WBP)
        {
            OutError = FMonolithActionResult::Error(
                FString::Printf(TEXT("Widget Blueprint not found: %s"), *AssetPath));
            return nullptr;
        }
        return WBP;
    }

    UClass* WidgetClassFromName(const FString& ClassName)
    {
        static TMap<FString, UClass*> ClassMap;
        if (ClassMap.Num() == 0)
        {
            ClassMap.Add(TEXT("CanvasPanel"),       UCanvasPanel::StaticClass());
            ClassMap.Add(TEXT("VerticalBox"),       UVerticalBox::StaticClass());
            ClassMap.Add(TEXT("HorizontalBox"),     UHorizontalBox::StaticClass());
            ClassMap.Add(TEXT("Overlay"),           UOverlay::StaticClass());
            ClassMap.Add(TEXT("ScrollBox"),         UScrollBox::StaticClass());
            ClassMap.Add(TEXT("SizeBox"),           USizeBox::StaticClass());
            ClassMap.Add(TEXT("ScaleBox"),          UScaleBox::StaticClass());
            ClassMap.Add(TEXT("Border"),            UBorder::StaticClass());
            ClassMap.Add(TEXT("WrapBox"),           UWrapBox::StaticClass());
            ClassMap.Add(TEXT("UniformGridPanel"),  UUniformGridPanel::StaticClass());
            ClassMap.Add(TEXT("GridPanel"),         UGridPanel::StaticClass());
            ClassMap.Add(TEXT("WidgetSwitcher"),    UWidgetSwitcher::StaticClass());
            ClassMap.Add(TEXT("BackgroundBlur"),    UBackgroundBlur::StaticClass());
            ClassMap.Add(TEXT("NamedSlot"),         UNamedSlot::StaticClass());
            ClassMap.Add(TEXT("TextBlock"),         UTextBlock::StaticClass());
            ClassMap.Add(TEXT("RichTextBlock"),     URichTextBlock::StaticClass());
            ClassMap.Add(TEXT("Image"),             UImage::StaticClass());
            ClassMap.Add(TEXT("Button"),            UButton::StaticClass());
            ClassMap.Add(TEXT("CheckBox"),          UCheckBox::StaticClass());
            ClassMap.Add(TEXT("ProgressBar"),       UProgressBar::StaticClass());
            ClassMap.Add(TEXT("Slider"),            USlider::StaticClass());
            ClassMap.Add(TEXT("Spacer"),            USpacer::StaticClass());
            ClassMap.Add(TEXT("EditableText"),      UEditableText::StaticClass());
            ClassMap.Add(TEXT("EditableTextBox"),   UEditableTextBox::StaticClass());
            ClassMap.Add(TEXT("ComboBoxString"),    UComboBoxString::StaticClass());
            ClassMap.Add(TEXT("InputKeySelector"),  UInputKeySelector::StaticClass());
            ClassMap.Add(TEXT("ListView"),          UListView::StaticClass());
            ClassMap.Add(TEXT("TileView"),          UTileView::StaticClass());
        }

        if (UClass** Found = ClassMap.Find(ClassName))
        {
            return *Found;
        }

        // Fall back to FindFirstObject for fully qualified class paths.
        return FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
    }

    FAnchors GetAnchorPreset(const FString& PresetName)
    {
        static TMap<FString, FAnchors> Presets;
        if (Presets.Num() == 0)
        {
            Presets.Add(TEXT("top_left"),            FAnchors(0.f, 0.f, 0.f, 0.f));
            Presets.Add(TEXT("top_center"),          FAnchors(0.5f, 0.f, 0.5f, 0.f));
            Presets.Add(TEXT("top_right"),           FAnchors(1.f, 0.f, 1.f, 0.f));
            Presets.Add(TEXT("center_left"),         FAnchors(0.f, 0.5f, 0.f, 0.5f));
            Presets.Add(TEXT("center"),              FAnchors(0.5f, 0.5f, 0.5f, 0.5f));
            Presets.Add(TEXT("center_right"),        FAnchors(1.f, 0.5f, 1.f, 0.5f));
            Presets.Add(TEXT("bottom_left"),         FAnchors(0.f, 1.f, 0.f, 1.f));
            Presets.Add(TEXT("bottom_center"),       FAnchors(0.5f, 1.f, 0.5f, 1.f));
            Presets.Add(TEXT("bottom_right"),        FAnchors(1.f, 1.f, 1.f, 1.f));
            Presets.Add(TEXT("stretch_top"),         FAnchors(0.f, 0.f, 1.f, 0.f));
            Presets.Add(TEXT("stretch_bottom"),      FAnchors(0.f, 1.f, 1.f, 1.f));
            Presets.Add(TEXT("stretch_left"),        FAnchors(0.f, 0.f, 0.f, 1.f));
            Presets.Add(TEXT("stretch_right"),       FAnchors(1.f, 0.f, 1.f, 1.f));
            Presets.Add(TEXT("stretch_horizontal"),  FAnchors(0.f, 0.5f, 1.f, 0.5f));
            Presets.Add(TEXT("stretch_vertical"),    FAnchors(0.5f, 0.f, 0.5f, 1.f));
            Presets.Add(TEXT("stretch_fill"),        FAnchors(0.f, 0.f, 1.f, 1.f));
        }

        if (const FAnchors* Found = Presets.Find(PresetName))
        {
            return *Found;
        }
        return FAnchors(0.f, 0.f, 0.f, 0.f);
    }

    EHorizontalAlignment ParseHAlign(const FString& S)
    {
        if (S == TEXT("Left"))   return HAlign_Left;
        if (S == TEXT("Center")) return HAlign_Center;
        if (S == TEXT("Right"))  return HAlign_Right;
        return HAlign_Fill;
    }

    EVerticalAlignment ParseVAlign(const FString& S)
    {
        if (S == TEXT("Top"))    return VAlign_Top;
        if (S == TEXT("Center")) return VAlign_Center;
        if (S == TEXT("Bottom")) return VAlign_Bottom;
        return VAlign_Fill;
    }

    void RegisterVariableName(UWidgetBlueprint* WBP, const FName& VariableName)
    {
        if (!WBP || VariableName.IsNone())
        {
            return;
        }

        if (!WBP->WidgetVariableNameToGuidMap.Contains(VariableName))
        {
            WBP->OnVariableAdded(VariableName);
        }
    }

    void RegisterCreatedWidget(UWidgetBlueprint* WBP, UWidget* Widget)
    {
        if (!Widget)
        {
            return;
        }
        RegisterVariableName(WBP, Widget->GetFName());
    }

    void ReconcileWidgetVariableGuids(UWidgetBlueprint* WBP)
    {
        if (!WBP)
        {
            return;
        }

        TSet<FName> LiveVariableNames;

        WBP->ForEachSourceWidget([WBP, &LiveVariableNames](UWidget* Widget)
        {
            if (Widget)
            {
                const FName WidgetName = Widget->GetFName();
                LiveVariableNames.Add(WidgetName);
                RegisterVariableName(WBP, WidgetName);
            }
        });

        for (UWidgetAnimation* Animation : WBP->Animations)
        {
            if (Animation)
            {
                const FName AnimationName = Animation->GetFName();
                LiveVariableNames.Add(AnimationName);
                RegisterVariableName(WBP, AnimationName);
            }
        }

        for (auto It = WBP->WidgetVariableNameToGuidMap.CreateIterator(); It; ++It)
        {
            if (!LiveVariableNames.Contains(It.Key()))
            {
                It.RemoveCurrent();
            }
        }
    }

    // -------------------------------------------------------------------------
    // EffectSurface probe (R3b — §5.5 contract single source of truth)
    // -------------------------------------------------------------------------

    namespace
    {
        // Function-static cache shared by both probe entry points. Weak ptr so
        // a hot-reload-destroyed UClass auto-clears via GC observer semantics
        // even before the explicit delegate hook fires.
        TWeakObjectPtr<UClass>& GetEffectSurfaceCache()
        {
            static TWeakObjectPtr<UClass> Cached;
            return Cached;
        }

        // Lazy ReloadComplete subscription — wired on the first probe call,
        // not at module startup. Two reasons:
        //   1) Keeps the probe API self-contained in this .cpp; no edits to
        //      MonolithUIModule.cpp required for the R3b landing.
        //   2) Modules that never call the probe (release builds where no
        //      EffectSurface action handler executes) pay zero subscription
        //      cost. The UE 5.7 ReloadCompleteDelegate is a multicast — every
        //      added handler runs on every reload, so honest pruning matters.
        //
        // The subscription is one-shot per process. The handle leaks at module
        // unload — acceptable because (a) the only call site here is a static
        // lambda that just clears the cache, no captured state to dangle, and
        // (b) the editor process tears down all delegate maps on exit. This
        // matches the "leak is fine, exit clears it" pattern used by a number
        // of engine-side caches (e.g., FAutocastFunctionMap at
        // `EdGraphSchema_K2.cpp:2531`). If a future hygiene pass needs clean
        // shutdown, hoist the handle into a module-init/shutdown pair.
        void EnsureReloadHookInstalled()
        {
            static bool bHookInstalled = false;
            if (bHookInstalled)
            {
                return;
            }
            bHookInstalled = true;
            FCoreUObjectDelegates::ReloadCompleteDelegate.AddStatic(
                [](EReloadCompleteReason /*Reason*/)
                {
                    GetEffectSurfaceCache().Reset();
                });
        }

        // FindObject on the transient package does not find plugin-defined
        // UClasses in script packages. Iterate loaded UClasses instead so the
        // public probe stays provider-agnostic.
        static UClass* FindLoadedEffectSurfaceClass()
        {
            static const FName EffectSurfaceName(TEXT("EffectSurface"));

            for (TObjectIterator<UClass> It; It; ++It)
            {
                UClass* Candidate = *It;
                if (!Candidate
                    || Candidate->GetFName() != EffectSurfaceName
                    || !Candidate->IsChildOf(UWidget::StaticClass()))
                {
                    continue;
                }

                return Candidate;
            }

            return nullptr;
        }
    }

    UClass* GetEffectSurfaceClass()
    {
        TWeakObjectPtr<UClass>& Cache = GetEffectSurfaceCache();

        // Fast path — cached pointer is still valid. Get() returns nullptr if
        // the class was hot-reload-destroyed even before the delegate hook
        // fires, so this branch is intrinsically safe under GC.
        if (UClass* Cached = Cache.Get())
        {
            return Cached;
        }

        EnsureReloadHookInstalled();

        UClass* Resolved = FindLoadedEffectSurfaceClass();
        if (Resolved)
        {
            Cache = Resolved;
        }
        return Resolved;
    }

    bool IsEffectSurfaceAvailable()
    {
        return GetEffectSurfaceClass() != nullptr;
    }
}
