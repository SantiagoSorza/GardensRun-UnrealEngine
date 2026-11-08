// Copyright 2026 Nwiro. All Rights Reserved.

#include "NwiroWidgetTools.h"
#include "NwiroAssetGuard.h"
#include "NwiroTransactionHelper.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "WidgetBlueprintFactory.h"
#include "WidgetBlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "KismetCompilerModule.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/PackageName.h"
#include "UObject/StrongObjectPtr.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/Image.h"
#include "Components/CanvasPanel.h"
#include "Components/VerticalBox.h"
#include "Components/HorizontalBox.h"
#include "Components/Overlay.h"
#include "Components/ScrollBox.h"
#include "Components/Slider.h"
#include "Components/CheckBox.h"
#include "Components/ProgressBar.h"
#include "Components/EditableTextBox.h"
#include "Components/Border.h"
#include "Components/Spacer.h"
#include "Components/SizeBox.h"
#include "Components/GridPanel.h"
#include "Components/WrapBox.h"
#include "Components/UniformGridPanel.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableText.h"
#include "Components/MultiLineEditableText.h"
#include "Components/MultiLineEditableTextBox.h"
#include "Components/WidgetSwitcher.h"
#include "Components/ScaleBox.h"
#include "Components/RetainerBox.h"
#include "Components/InvalidationBox.h"
#include "Components/Throbber.h"
#include "Components/CircularThrobber.h"
#include "Components/SpinBox.h"
#include "Components/ListView.h"
#include "Components/TileView.h"
#include "Components/TreeView.h"
#include "Components/ExpandableArea.h"
#include "Components/MenuAnchor.h"
#include "Components/BackgroundBlur.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/OverlaySlot.h"
#include "Components/SizeBoxSlot.h"
#include "Components/BorderSlot.h"
#include "Layout/Margin.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "BlueprintCompilationManager.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Async/Async.h"
#include "Async/Future.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Json.h"
// Render deps
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Slate/WidgetRenderer.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "RenderingThread.h"

DEFINE_LOG_CATEGORY_STATIC(LogNwiroWidget, Log, All);

// Shared root/compile lifecycle for the public factory path and no-save fixtures.
// The engine owns distinct skeleton/generated classes and their regeneration flags.
static bool InitializeWidgetBlueprintRoot(UWidgetBlueprint* WBP, UClass* RootClass,
	FCompilerResultsLog& CompileLog)
{
	if (!WBP || !WBP->WidgetTree || !RootClass) return false;
	WBP->WidgetTree->Modify();
	UWidget* RootWidget = WBP->WidgetTree->ConstructWidget<UWidget>(RootClass, TEXT("RootPanel"));
	if (!RootWidget) return false;
	WBP->WidgetTree->RootWidget = RootWidget;
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
	WBP->OnVariableAdded(RootWidget->GetFName());
#endif
	FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
	// Creation needs a complete class. Later structural edits regenerate only
	// the skeleton; property edits remain dirty until explicit compilation/save.
	FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::SkipSave, &CompileLog);
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS
// Keep lifecycle fixtures unsaved while exercising the same root/compile path
// as public factory creation. The public dispatch is covered by factory regressions.
static UWidgetBlueprint* CreateWidgetBlueprintAsset(UPackage* Package, FName Name,
	UClass* RootClass, FCompilerResultsLog& CompileLog)
{
	FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("KismetCompiler"));
	FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("UMGEditor"));
	UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		UUserWidget::StaticClass(), Package, Name, BPTYPE_Normal,
		UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass(),
		TEXT("NwiroWidgetTools")));
	InitializeWidgetBlueprintRoot(WBP, RootClass, CompileLog);
	return WBP;
}
#endif

static bool IsWidgetBlueprintCompiled(const UWidgetBlueprint* WBP)
{
	return WBP && (WBP->Status == BS_UpToDate || WBP->Status == BS_UpToDateWithWarnings)
		&& WBP->WidgetTree && WBP->WidgetTree->RootWidget
		&& Cast<UWidgetBlueprintGeneratedClass>(WBP->GeneratedClass)
		&& WBP->SkeletonGeneratedClass && WBP->GeneratedClass != WBP->SkeletonGeneratedClass
		&& WBP->GeneratedClass->GetDefaultObject(false);
}

static FString WidgetCreateResult(UWidgetBlueprint* WBP, bool bSuccess, bool bSaved,
	bool bExisted, const FString& RootType, const FString& Error = FString())
{
	const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), bSuccess);
	Result->SetBoolField(TEXT("savedToDisk"), bSaved);
	Result->SetBoolField(TEXT("existed"), bExisted);
	if (!Error.IsEmpty()) Result->SetStringField(TEXT("error"), Error);
	TArray<TSharedPtr<FJsonValue>> Dirty;
	if (WBP)
	{
		Result->SetStringField(TEXT("name"), WBP->GetName());
		Result->SetStringField(TEXT("path"), WBP->GetPathName());
		Result->SetStringField(TEXT("rootWidget"), WBP->WidgetTree && WBP->WidgetTree->RootWidget
			? WBP->WidgetTree->RootWidget->GetClass()->GetName() : RootType);
		Result->SetBoolField(TEXT("compiled"), IsWidgetBlueprintCompiled(WBP));
		Result->SetStringField(TEXT("compileStatus"), UEnum::GetValueAsString(WBP->Status.GetValue()));
		if (WBP->GetOutermost()->IsDirty())
			Dirty.Add(MakeShared<FJsonValueString>(WBP->GetOutermost()->GetName()));
	}
	Result->SetArrayField(TEXT("dirtyPackages"), Dirty);
	FString Json;
	FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Json));
	return Json;
}

static UWidgetBlueprint* FindWidgetBP(const FString& PathOrName)
{
	UObject* Asset = UEditorAssetLibrary::LoadAsset(PathOrName);
	if (UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Asset)) return WBP;

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	FARFilter Filter;
	Filter.ClassPaths.Add(UWidgetBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = true;
	Filter.PackagePaths.Add(TEXT("/Game"));
	TArray<FAssetData> Assets;
	ARM.Get().GetAssets(Filter, Assets);
	for (const FAssetData& A : Assets)
	{
		if (A.AssetName.ToString().Contains(PathOrName, ESearchCase::IgnoreCase))
		{
			return Cast<UWidgetBlueprint>(NwiroSafeRegistryLoad(A));
		}
	}
	return nullptr;
}

static UClass* ResolveWidgetClass(const FString& ClassName)
{
	FString Lower = ClassName.ToLower();
	if (Lower == TEXT("button")) return UButton::StaticClass();
	if (Lower == TEXT("textblock") || Lower == TEXT("text")) return UTextBlock::StaticClass();
	if (Lower == TEXT("image")) return UImage::StaticClass();
	if (Lower == TEXT("canvaspanel") || Lower == TEXT("canvas")) return UCanvasPanel::StaticClass();
	if (Lower == TEXT("verticalbox") || Lower == TEXT("vbox")) return UVerticalBox::StaticClass();
	if (Lower == TEXT("horizontalbox") || Lower == TEXT("hbox")) return UHorizontalBox::StaticClass();
	if (Lower == TEXT("overlay")) return UOverlay::StaticClass();
	if (Lower == TEXT("scrollbox")) return UScrollBox::StaticClass();
	if (Lower == TEXT("slider")) return USlider::StaticClass();
	if (Lower == TEXT("checkbox")) return UCheckBox::StaticClass();
	if (Lower == TEXT("progressbar")) return UProgressBar::StaticClass();
	if (Lower == TEXT("editabletextbox") || Lower == TEXT("textbox") || Lower == TEXT("input")) return UEditableTextBox::StaticClass();
	if (Lower == TEXT("border")) return UBorder::StaticClass();
	if (Lower == TEXT("spacer")) return USpacer::StaticClass();
	if (Lower == TEXT("sizebox")) return USizeBox::StaticClass();
	if (Lower == TEXT("gridpanel") || Lower == TEXT("grid")) return UGridPanel::StaticClass();
	if (Lower == TEXT("wrapbox")) return UWrapBox::StaticClass();
	if (Lower == TEXT("uniformgridpanel") || Lower == TEXT("uniformgrid")) return UUniformGridPanel::StaticClass();
	if (Lower == TEXT("comboboxstring") || Lower == TEXT("combobox") || Lower == TEXT("dropdown")) return UComboBoxString::StaticClass();
	if (Lower == TEXT("editabletext")) return UEditableText::StaticClass();
	if (Lower == TEXT("multilineeditabletext") || Lower == TEXT("multilinetext")) return UMultiLineEditableText::StaticClass();
	if (Lower == TEXT("multilineeditabletextbox") || Lower == TEXT("multilinetextbox")) return UMultiLineEditableTextBox::StaticClass();
	if (Lower == TEXT("widgetswitcher") || Lower == TEXT("switcher")) return UWidgetSwitcher::StaticClass();
	if (Lower == TEXT("scalebox")) return UScaleBox::StaticClass();
	if (Lower == TEXT("retainerbox")) return URetainerBox::StaticClass();
	if (Lower == TEXT("invalidationbox")) return UInvalidationBox::StaticClass();
	if (Lower == TEXT("throbber")) return UThrobber::StaticClass();
	if (Lower == TEXT("circularthrobber")) return UCircularThrobber::StaticClass();
	if (Lower == TEXT("spinbox") || Lower == TEXT("numericinput")) return USpinBox::StaticClass();
	if (Lower == TEXT("listview")) return UListView::StaticClass();
	if (Lower == TEXT("tileview")) return UTileView::StaticClass();
	if (Lower == TEXT("treeview")) return UTreeView::StaticClass();
	if (Lower == TEXT("expandablearea")) return UExpandableArea::StaticClass();
	if (Lower == TEXT("menuanchor")) return UMenuAnchor::StaticClass();
	if (Lower == TEXT("backgroundblur")) return UBackgroundBlur::StaticClass();
	return nullptr;
}

// ============================================================
// JSON -> UProperty reflection helpers (used by AddWidget for
// `properties` and `slot` blocks). Hallucination-tolerant: accepts
// JSON numbers/bools/strings as natural types, plus array/object
// forms for FVector2D / FLinearColor / FMargin.
// ============================================================

static bool ReadVec2FromJson(const TSharedPtr<FJsonValue>& V, FVector2D& Out)
{
	if (!V.IsValid()) return false;
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	const TSharedPtr<FJsonObject>* Obj = nullptr;
	if (V->TryGetArray(Arr) && Arr->Num() >= 2)
	{
		Out.X = (*Arr)[0]->AsNumber(); Out.Y = (*Arr)[1]->AsNumber(); return true;
	}
	if (V->TryGetObject(Obj))
	{
		double X = Out.X, Y = Out.Y;
		(*Obj)->TryGetNumberField(TEXT("x"), X);
		(*Obj)->TryGetNumberField(TEXT("y"), Y);
		Out.X = X; Out.Y = Y;
		return true;
	}
	return false;
}

static bool ReadColorFromJson(const TSharedPtr<FJsonValue>& V, FLinearColor& Out)
{
	if (!V.IsValid()) return false;
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	const TSharedPtr<FJsonObject>* Obj = nullptr;
	if (V->TryGetArray(Arr) && Arr->Num() >= 3)
	{
		Out.R = (float)(*Arr)[0]->AsNumber();
		Out.G = (float)(*Arr)[1]->AsNumber();
		Out.B = (float)(*Arr)[2]->AsNumber();
		Out.A = Arr->Num() >= 4 ? (float)(*Arr)[3]->AsNumber() : 1.f;
		return true;
	}
	if (V->TryGetObject(Obj))
	{
		double R = Out.R, G = Out.G, B = Out.B, A = Out.A;
		(*Obj)->TryGetNumberField(TEXT("r"), R);
		(*Obj)->TryGetNumberField(TEXT("g"), G);
		(*Obj)->TryGetNumberField(TEXT("b"), B);
		(*Obj)->TryGetNumberField(TEXT("a"), A);
		Out.R = R; Out.G = G; Out.B = B; Out.A = A;
		return true;
	}
	FString S;
	if (V->TryGetString(S))
	{
		FColor C = FColor::FromHex(S);
		Out = FLinearColor(C);
		return true;
	}
	return false;
}

static bool ApplyJsonToProperty(UObject* Owner, FProperty* Prop, const TSharedPtr<FJsonValue>& Val)
{
	if (!Owner || !Prop || !Val.IsValid()) return false;
	void* Ptr = Prop->ContainerPtrToValuePtr<void>(Owner);

	if (FBoolProperty* B = CastField<FBoolProperty>(Prop))
	{
		bool b = false; if (Val->TryGetBool(b)) { B->SetPropertyValue(Ptr, b); return true; }
	}
	if (FFloatProperty* F = CastField<FFloatProperty>(Prop))
	{
		double d = 0; if (Val->TryGetNumber(d)) { F->SetPropertyValue(Ptr, (float)d); return true; }
	}
	if (FDoubleProperty* D = CastField<FDoubleProperty>(Prop))
	{
		double d = 0; if (Val->TryGetNumber(d)) { D->SetPropertyValue(Ptr, d); return true; }
	}
	if (FIntProperty* I = CastField<FIntProperty>(Prop))
	{
		int32 n = 0; if (Val->TryGetNumber(n)) { I->SetPropertyValue(Ptr, n); return true; }
	}
	if (FInt64Property* I64 = CastField<FInt64Property>(Prop))
	{
		int64 n = 0; if (Val->TryGetNumber(n)) { I64->SetPropertyValue(Ptr, n); return true; }
	}
	if (FByteProperty* By = CastField<FByteProperty>(Prop))
	{
		if (By->Enum)
		{
			FString s; if (Val->TryGetString(s))
			{
				int64 EV = By->Enum->GetValueByNameString(s);
				if (EV != INDEX_NONE) { By->SetPropertyValue(Ptr, (uint8)EV); return true; }
			}
		}
		else
		{
			double d = 0; if (Val->TryGetNumber(d)) { By->SetPropertyValue(Ptr, (uint8)d); return true; }
		}
	}
	if (FEnumProperty* E = CastField<FEnumProperty>(Prop))
	{
		FString s; if (Val->TryGetString(s))
		{
			int64 EV = E->GetEnum()->GetValueByNameString(s);
			if (EV != INDEX_NONE)
			{
				E->GetUnderlyingProperty()->SetIntPropertyValue(Ptr, EV);
				return true;
			}
		}
	}
	if (FStrProperty* S = CastField<FStrProperty>(Prop))
	{
		FString s; if (Val->TryGetString(s)) { S->SetPropertyValue(Ptr, s); return true; }
	}
	if (FNameProperty* N = CastField<FNameProperty>(Prop))
	{
		FString s; if (Val->TryGetString(s)) { N->SetPropertyValue(Ptr, FName(*s)); return true; }
	}
	if (FTextProperty* T = CastField<FTextProperty>(Prop))
	{
		FString s; if (Val->TryGetString(s)) { T->SetPropertyValue(Ptr, FText::FromString(s)); return true; }
	}
	if (FStructProperty* Sp = CastField<FStructProperty>(Prop))
	{
		if (Sp->Struct == TBaseStructure<FLinearColor>::Get())
		{
			FLinearColor C = *((FLinearColor*)Ptr);
			if (ReadColorFromJson(Val, C)) { *((FLinearColor*)Ptr) = C; return true; }
		}
		if (Sp->Struct == TBaseStructure<FVector2D>::Get())
		{
			FVector2D V = *((FVector2D*)Ptr);
			if (ReadVec2FromJson(Val, V)) { *((FVector2D*)Ptr) = V; return true; }
		}
	}

	FString AsStr;
	if (Val->TryGetString(AsStr))
	{
		return Prop->ImportText_Direct(*AsStr, Ptr, Owner, PPF_None) != nullptr;
	}
	double AsNum = 0;
	if (Val->TryGetNumber(AsNum))
	{
		FString S = FString::SanitizeFloat(AsNum);
		return Prop->ImportText_Direct(*S, Ptr, Owner, PPF_None) != nullptr;
	}
	return false;
}

static int32 ApplyJsonProperties(UObject* Target, const TSharedPtr<FJsonObject>& Props, TArray<FString>* OutSkipped = nullptr)
{
	if (!Target || !Props.IsValid()) return 0;
	int32 Applied = 0;
	for (const auto& KV : Props->Values)
	{
		const FString Key(*KV.Key);
		FProperty* Prop = Target->GetClass()->FindPropertyByName(FName(*Key));
		if (!Prop)
		{
			if (OutSkipped) OutSkipped->Add(Key + TEXT("(unknown)"));
			continue;
		}
		if (ApplyJsonToProperty(Target, Prop, KV.Value)) ++Applied;
		else if (OutSkipped) OutSkipped->Add(Key + TEXT("(typefail)"));
	}
	return Applied;
}

static void ApplyCanvasSlotConfig(UCanvasPanelSlot* Slot, const TSharedPtr<FJsonObject>& Cfg)
{
	if (!Slot || !Cfg.IsValid()) return;

	auto GetVec2Key = [&](const TCHAR* Key, FVector2D& Out) -> bool {
		const TSharedPtr<FJsonValue> V = Cfg->Values.FindRef(Key);
		return V.IsValid() && ReadVec2FromJson(V, Out);
	};

	FVector2D Tmp;
	Tmp = Slot->GetPosition();   if (GetVec2Key(TEXT("position"), Tmp) || GetVec2Key(TEXT("pos"), Tmp)) Slot->SetPosition(Tmp);
	Tmp = Slot->GetSize();       if (GetVec2Key(TEXT("size"), Tmp))         Slot->SetSize(Tmp);
	Tmp = Slot->GetAlignment();  if (GetVec2Key(TEXT("alignment"), Tmp) || GetVec2Key(TEXT("align"), Tmp)) Slot->SetAlignment(Tmp);

	{
		FVector2D P = Slot->GetPosition();
		double X = P.X, Y = P.Y;
		const bool bX = Cfg->TryGetNumberField(TEXT("x"), X);
		const bool bY = Cfg->TryGetNumberField(TEXT("y"), Y);
		if (bX || bY) Slot->SetPosition(FVector2D(X, Y));
	}
	{
		FVector2D Sz = Slot->GetSize();
		double W = Sz.X, H = Sz.Y;
		bool bW = Cfg->TryGetNumberField(TEXT("width"), W);  if (!bW) bW = Cfg->TryGetNumberField(TEXT("w"), W);
		bool bH = Cfg->TryGetNumberField(TEXT("height"), H); if (!bH) bH = Cfg->TryGetNumberField(TEXT("h"), H);
		if (bW || bH) Slot->SetSize(FVector2D(W, H));
	}

	const TSharedPtr<FJsonValue> AnchorsVal = Cfg->Values.FindRef(TEXT("anchors"));
	if (AnchorsVal.IsValid())
	{
		FAnchors A = Slot->GetAnchors();
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		FString PresetStr;
		if (AnchorsVal->TryGetArray(Arr) && Arr->Num() >= 4)
		{
			A.Minimum = FVector2D((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber());
			A.Maximum = FVector2D((*Arr)[2]->AsNumber(), (*Arr)[3]->AsNumber());
			Slot->SetAnchors(A);
		}
		else if (AnchorsVal->TryGetObject(Obj))
		{
			FVector2D MinV = A.Minimum, MaxV = A.Maximum;
			const TSharedPtr<FJsonValue> Mn = (*Obj)->Values.FindRef(TEXT("min"));
			const TSharedPtr<FJsonValue> Mx = (*Obj)->Values.FindRef(TEXT("max"));
			if (Mn.IsValid()) ReadVec2FromJson(Mn, MinV);
			if (Mx.IsValid()) ReadVec2FromJson(Mx, MaxV);
			A.Minimum = MinV; A.Maximum = MaxV;
			Slot->SetAnchors(A);
		}
		else if (AnchorsVal->TryGetString(PresetStr))
		{
			FString P = PresetStr.ToLower();
			if      (P == TEXT("topleft"))     A = FAnchors(0, 0, 0, 0);
			else if (P == TEXT("topcenter") || P == TEXT("topcentre")) A = FAnchors(0.5f, 0, 0.5f, 0);
			else if (P == TEXT("topright"))    A = FAnchors(1, 0, 1, 0);
			else if (P == TEXT("centerleft"))  A = FAnchors(0, 0.5f, 0, 0.5f);
			else if (P == TEXT("center") || P == TEXT("centre")) A = FAnchors(0.5f, 0.5f, 0.5f, 0.5f);
			else if (P == TEXT("centerright")) A = FAnchors(1, 0.5f, 1, 0.5f);
			else if (P == TEXT("bottomleft"))  A = FAnchors(0, 1, 0, 1);
			else if (P == TEXT("bottomcenter")) A = FAnchors(0.5f, 1, 0.5f, 1);
			else if (P == TEXT("bottomright")) A = FAnchors(1, 1, 1, 1);
			else if (P == TEXT("fill") || P == TEXT("stretch")) A = FAnchors(0, 0, 1, 1);
			else if (P == TEXT("fillhorizontal") || P == TEXT("hstretch")) A = FAnchors(0, 0, 1, 0);
			else if (P == TEXT("fillvertical")   || P == TEXT("vstretch")) A = FAnchors(0, 0, 0, 1);
			Slot->SetAnchors(A);
		}
	}

	int32 ZOrder = 0;
	if (Cfg->TryGetNumberField(TEXT("zOrder"), ZOrder) || Cfg->TryGetNumberField(TEXT("zorder"), ZOrder) || Cfg->TryGetNumberField(TEXT("z"), ZOrder))
		Slot->SetZOrder(ZOrder);

	bool bAutoSize = false;
	if (Cfg->TryGetBoolField(TEXT("autoSize"), bAutoSize) || Cfg->TryGetBoolField(TEXT("autosize"), bAutoSize) || Cfg->TryGetBoolField(TEXT("auto"), bAutoSize))
		Slot->SetAutoSize(bAutoSize);
}

// ============================================================
// CREATE WIDGET BLUEPRINT
// ============================================================

FString FNwiroWidgetTools::CreateWidgetBlueprint(const FString& JsonCommand)
{
	if (!IsInGameThread())
	{
		TSharedRef<TPromise<FString>, ESPMode::ThreadSafe> Promise = MakeShared<TPromise<FString>, ESPMode::ThreadSafe>();
		TFuture<FString> Future = Promise->GetFuture();
		AsyncTask(ENamedThreads::GameThread, [JsonCommand, Promise]()
		{
			Promise->SetValue(FNwiroWidgetTools::CreateWidgetBlueprint(JsonCommand));
		});
		return Future.Get();
	}

	TSharedPtr<FJsonObject> Cmd;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonCommand), Cmd) || !Cmd.IsValid())
		return WidgetCreateResult(nullptr, false, false, false, FString(), TEXT("Invalid JSON"));

	FString Name, Path, RootType;
	Cmd->TryGetStringField(TEXT("name"), Name);
	Cmd->TryGetStringField(TEXT("path"), Path);
	Cmd->TryGetStringField(TEXT("rootWidget"), RootType);
	if (Name.IsEmpty()) return WidgetCreateResult(nullptr, false, false, false, RootType, TEXT("Missing 'name'"));
	if (Path.IsEmpty()) Path = TEXT("/Game/UI");
	if (RootType.IsEmpty()) RootType = TEXT("CanvasPanel");
	if (!Name.StartsWith(TEXT("WBP_")) && !Name.StartsWith(TEXT("W_")))
		Name = TEXT("WBP_") + Name;
	const FString FullPath = Path / Name;
	const FString ObjectPath = FullPath + TEXT(".") + Name;
	bool bCreated = false;
	bool bRootConstructed = false;
	bool bSaveAttempted = false;
	bool bSaved = false;
	UWidgetBlueprint* ResultWBP = nullptr;
	TArray<TSharedPtr<FJsonValue>> CompileErrors;
	auto Reply = [&](bool bSuccess, const FString& Error, bool bExisted = false)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), bSuccess);
		Result->SetStringField(TEXT("name"), Name);
		Result->SetStringField(TEXT("path"), ObjectPath);
		if (ResultWBP && ResultWBP->WidgetTree && ResultWBP->WidgetTree->RootWidget)
			Result->SetStringField(TEXT("rootWidget"), ResultWBP->WidgetTree->RootWidget->GetClass()->GetName());
		else if (bRootConstructed) Result->SetStringField(TEXT("rootWidget"), RootType);
		Result->SetBoolField(TEXT("created"), bCreated);
		Result->SetBoolField(TEXT("saveAttempted"), bSaveAttempted);
		Result->SetBoolField(TEXT("savePerformed"), bSaveAttempted);
		Result->SetBoolField(TEXT("savedToDisk"), bSaved);
		Result->SetBoolField(TEXT("existed"), bExisted);
		TArray<TSharedPtr<FJsonValue>> Dirty;
		if (ResultWBP)
		{
			Result->SetBoolField(TEXT("compiled"), IsWidgetBlueprintCompiled(ResultWBP));
			Result->SetStringField(TEXT("compileStatus"), UEnum::GetValueAsString(ResultWBP->Status.GetValue()));
			if (ResultWBP->GetOutermost()->IsDirty())
				Dirty.Add(MakeShared<FJsonValueString>(ResultWBP->GetOutermost()->GetName()));
		}
		Result->SetArrayField(TEXT("dirtyPackages"), Dirty);
		if (!Error.IsEmpty()) Result->SetStringField(TEXT("error"), Error);
		if (!CompileErrors.IsEmpty()) Result->SetArrayField(TEXT("compileErrors"), CompileErrors);
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Result, Writer);
		return Out;
	};

	// Validate before creating or replacing anything at the destination.
	FText PathError;
	if (!FPackageName::IsValidLongPackageName(FullPath, false, &PathError))
		return Reply(false, PathError.ToString());
	if (!FPackageName::IsValidObjectPath(ObjectPath, &PathError))
		return Reply(false, PathError.ToString());
	UClass* RootClass = ResolveWidgetClass(RootType);
	if (!RootClass || !RootClass->IsChildOf(UWidget::StaticClass()) || RootClass->HasAnyClassFlags(CLASS_Abstract))
		return Reply(false, TEXT("Unknown or non-constructible rootWidget type"));

	// Exact loaded assets include unsaved creations that have no registry entry.
	// Preserve their identity and dirty state; never purge destination collisions.
	FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	UObject* ExistingObject = StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath);
	if (!ExistingObject)
	{
		const FAssetData ExistingData = Registry.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
		if (ExistingData.IsValid())
		{
			ExistingObject = ExistingData.GetAsset();
			if (!ExistingObject) return Reply(false, TEXT("Existing asset could not be loaded"));
		}
	}
	if (ExistingObject)
	{
		ResultWBP = Cast<UWidgetBlueprint>(ExistingObject);
		if (!ResultWBP) return Reply(false, TEXT("Destination already contains a different asset type"));
		if (!ResultWBP->WidgetTree || !Cast<UWidgetBlueprintGeneratedClass>(ResultWBP->GeneratedClass)
			|| !ResultWBP->SkeletonGeneratedClass || ResultWBP->GeneratedClass == ResultWBP->SkeletonGeneratedClass)
			return Reply(false, TEXT("Existing widget has invalid generated/skeleton class state; asset was not modified"), true);
		return Reply(true, FString(), true);
	}
	if (FPackageName::DoesPackageExist(FullPath))
		return Reply(false, TEXT("Destination already exists on disk but was not resolved by the asset registry; asset was not modified"));
	// A failed earlier operation may have left named objects without registry data.
	// Refuse that collision rather than purge or overwrite them.
	if (UPackage* ExistingPackage = FindPackage(nullptr, *FullPath))
	{
		for (const FString& CandidateName : { Name, Name + TEXT("_C"), TEXT("SKEL_") + Name + TEXT("_C") })
		{
			if (StaticFindObjectFast(UObject::StaticClass(), ExistingPackage, FName(*CandidateName)))
				return Reply(false, TEXT("Destination contains an unregistered object; asset was not modified"));
		}
	}

	FModuleManager::LoadModuleChecked<IKismetCompilerInterface>("KismetCompiler");
	FModuleManager::Get().LoadModuleChecked(FName("UMGEditor"));
	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "CreateWidgetBlueprint", "AI: Create Widget Blueprint"));
	TStrongObjectPtr<UWidgetBlueprintFactory> Factory(NewObject<UWidgetBlueprintFactory>());
	Factory->ParentClass = UUserWidget::StaticClass();
	Factory->BlueprintType = BPTYPE_Normal;
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(AssetTools.CreateAsset(
		Name, Path, UWidgetBlueprint::StaticClass(), Factory.Get(), FName(TEXT("Nwiro"))
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
		, false
#endif
	));
	if (!WBP) { Tx.Cancel(); return Reply(false, TEXT("Widget Blueprint factory failed")); }
	bCreated = true;
	ResultWBP = WBP;
	TStrongObjectPtr<UWidgetBlueprint> KeepAlive(WBP);
	Tx.AlsoModify(WBP);
	if (WBP->GetPathName() != ObjectPath || !WBP->WidgetTree)
		return Reply(false, TEXT("Factory returned an unexpected widget asset or missing widget tree; new asset was not saved"));

	// Factory project settings may supply a default root. Replace only that new
	// widget instance through the editor API, preserving the tool's RootPanel name.
	if (UWidget* FactoryRoot = WBP->WidgetTree->RootWidget)
	{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
		FWidgetBlueprintEditorUtils::DeleteWidgets(WBP, { FactoryRoot },
			FWidgetBlueprintEditorUtils::EDeleteWidgetWarningType::DeleteSilently);
#else
		// UE 5.5 has only an editor-instance DeleteWidgets overload and no widget
		// GUID map. Detach and retire only this new factory root and its children.
		TArray<UWidget*> FactoryWidgets;
		UWidgetTree::GetChildWidgets(FactoryRoot, FactoryWidgets);
		FactoryWidgets.Add(FactoryRoot);
		if (!WBP->WidgetTree->RemoveWidget(FactoryRoot))
			return Reply(false, TEXT("Could not detach the factory root; new asset was not saved"));
		for (UWidget* FactoryWidget : FactoryWidgets)
			if (!FactoryWidget->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional))
				return Reply(false, TEXT("Could not retire a factory widget; new asset was not saved"));
#endif
		if (WBP->WidgetTree->RootWidget)
			return Reply(false, TEXT("Could not replace the factory root; new asset was not saved"));
	}
	// Use the same checked, no-save root/compile lifecycle as the test fixtures.
	FCompilerResultsLog CompileResults;
	if (!InitializeWidgetBlueprintRoot(WBP, RootClass, CompileResults))
		return Reply(false, TEXT("Could not construct the requested root; new asset was not saved"));
	bRootConstructed = true;
	for (const TSharedRef<FTokenizedMessage>& Message : CompileResults.Messages)
		if (Message->GetSeverity() == EMessageSeverity::Error)
			CompileErrors.Add(MakeShared<FJsonValueString>(Message->ToText().ToString()));
	UWidgetBlueprintGeneratedClass* Generated = Cast<UWidgetBlueprintGeneratedClass>(WBP->GeneratedClass);
	if (CompileResults.NumErrors > 0 || !IsWidgetBlueprintCompiled(WBP) || !Generated
		|| !WBP->SkeletonGeneratedClass || Generated == WBP->SkeletonGeneratedClass
		|| Generated->ClassGeneratedBy != WBP || Generated->GetSuperClass() != WBP->ParentClass)
		return Reply(false, TEXT("Widget compilation or generated-class validation failed; new asset was not saved"));
	WBP->MarkPackageDirty();
	bSaveAttempted = true;
	bSaved = UEditorAssetLibrary::SaveLoadedAsset(WBP, false);
	if (!bSaved) return Reply(false, TEXT("Widget was created and compiled, but saving failed"));
	return Reply(true, FString());
}

// ============================================================
// READ WIDGET BLUEPRINT
// ============================================================

FString FNwiroWidgetTools::ReadWidgetBlueprint(const FString& JsonCommand)
{
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path;
	for (const TCHAR* K : { TEXT("path"), TEXT("assetPath"), TEXT("widgetBlueprint"), TEXT("blueprint") }) {
		if (Cmd->TryGetStringField(K, Path) && !Path.IsEmpty()) break;
	}
	UWidgetBlueprint* WBP = FindWidgetBP(Path);
	if (!WBP)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"WidgetBlueprint not found: %s\"}"), *Path);

	TSharedRef<FJsonObject> Result = MakeShareable(new FJsonObject());
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), WBP->GetName());
	Result->SetStringField(TEXT("path"), WBP->GetPathName());

	// Serialize widget tree (+ key properties + slot info — needed for
	// acceptance assertions like "ProgressBar.Percent > 0").
	auto SerializeColor = [](const FLinearColor& C) -> TSharedPtr<FJsonObject> {
		TSharedPtr<FJsonObject> O = MakeShareable(new FJsonObject());
		O->SetNumberField(TEXT("r"), C.R); O->SetNumberField(TEXT("g"), C.G);
		O->SetNumberField(TEXT("b"), C.B); O->SetNumberField(TEXT("a"), C.A);
		return O;
	};
	auto SerializeVec2 = [](const FVector2D& V) -> TSharedPtr<FJsonObject> {
		TSharedPtr<FJsonObject> O = MakeShareable(new FJsonObject());
		O->SetNumberField(TEXT("x"), V.X); O->SetNumberField(TEXT("y"), V.Y);
		return O;
	};

	TFunction<TSharedPtr<FJsonObject>(UWidget*)> SerializeWidget;
	SerializeWidget = [&](UWidget* W) -> TSharedPtr<FJsonObject>
	{
		if (!W) return nullptr;

		TSharedPtr<FJsonObject> WidgetObj = MakeShareable(new FJsonObject());
		WidgetObj->SetStringField(TEXT("name"), W->GetName());
		WidgetObj->SetStringField(TEXT("class"), W->GetClass()->GetName());
		WidgetObj->SetBoolField(TEXT("isVisible"), W->GetVisibility() == ESlateVisibility::Visible || W->GetVisibility() == ESlateVisibility::SelfHitTestInvisible);

		TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject());
		if (UTextBlock* T = Cast<UTextBlock>(W))
		{
			Props->SetStringField(TEXT("Text"), T->GetText().ToString());
			Props->SetObjectField(TEXT("ColorAndOpacity"), SerializeColor(T->GetColorAndOpacity().GetSpecifiedColor()));
		}
		if (UProgressBar* PB = Cast<UProgressBar>(W))
		{
			Props->SetNumberField(TEXT("Percent"), PB->GetPercent());
			Props->SetObjectField(TEXT("FillColorAndOpacity"), SerializeColor(PB->GetFillColorAndOpacity()));
		}
		if (UButton* B = Cast<UButton>(W))
		{
			Props->SetObjectField(TEXT("ColorAndOpacity"), SerializeColor(B->GetColorAndOpacity()));
		}
		if (UBorder* Br = Cast<UBorder>(W))
		{
			Props->SetObjectField(TEXT("BrushColor"), SerializeColor(Br->GetBrushColor()));
		}
		if (USlider* Sl = Cast<USlider>(W))
		{
			Props->SetNumberField(TEXT("Value"), Sl->GetValue());
		}
		if (UCheckBox* Cb = Cast<UCheckBox>(W))
		{
			Props->SetBoolField(TEXT("IsChecked"), Cb->IsChecked());
		}
		if (USpinBox* Sp = Cast<USpinBox>(W))
		{
			Props->SetNumberField(TEXT("Value"), Sp->GetValue());
		}
		if (UEditableText* Et = Cast<UEditableText>(W))
		{
			Props->SetStringField(TEXT("Text"), Et->GetText().ToString());
		}
		if (UEditableTextBox* Etb = Cast<UEditableTextBox>(W))
		{
			Props->SetStringField(TEXT("Text"), Etb->GetText().ToString());
		}
		if (Props->Values.Num() > 0)
			WidgetObj->SetObjectField(TEXT("properties"), Props);

		if (UCanvasPanelSlot* CSlot = Cast<UCanvasPanelSlot>(W->Slot))
		{
			TSharedPtr<FJsonObject> SlotObj = MakeShareable(new FJsonObject());
			SlotObj->SetStringField(TEXT("type"), TEXT("CanvasPanelSlot"));
			SlotObj->SetObjectField(TEXT("position"), SerializeVec2(CSlot->GetPosition()));
			SlotObj->SetObjectField(TEXT("size"), SerializeVec2(CSlot->GetSize()));
			SlotObj->SetObjectField(TEXT("alignment"), SerializeVec2(CSlot->GetAlignment()));
			FAnchors An = CSlot->GetAnchors();
			TSharedPtr<FJsonObject> Anchors = MakeShareable(new FJsonObject());
			Anchors->SetObjectField(TEXT("min"), SerializeVec2(An.Minimum));
			Anchors->SetObjectField(TEXT("max"), SerializeVec2(An.Maximum));
			SlotObj->SetObjectField(TEXT("anchors"), Anchors);
			SlotObj->SetNumberField(TEXT("zOrder"), CSlot->GetZOrder());
			SlotObj->SetBoolField(TEXT("autoSize"), CSlot->GetAutoSize());
			WidgetObj->SetObjectField(TEXT("slot"), SlotObj);
		}
		else if (W->Slot)
		{
			TSharedPtr<FJsonObject> SlotObj = MakeShareable(new FJsonObject());
			SlotObj->SetStringField(TEXT("type"), W->Slot->GetClass()->GetName());
			WidgetObj->SetObjectField(TEXT("slot"), SlotObj);
		}

		if (UPanelWidget* Panel = Cast<UPanelWidget>(W))
		{
			TArray<TSharedPtr<FJsonValue>> Children;
			for (int32 i = 0; i < Panel->GetChildrenCount(); i++)
			{
				UWidget* Child = Panel->GetChildAt(i);
				TSharedPtr<FJsonObject> ChildObj = SerializeWidget(Child);
				if (ChildObj.IsValid())
					Children.Add(MakeShareable(new FJsonValueObject(ChildObj)));
			}
			if (Children.Num() > 0)
				WidgetObj->SetArrayField(TEXT("children"), Children);
		}

		return WidgetObj;
	};

	if (WBP->WidgetTree && WBP->WidgetTree->RootWidget)
	{
		TSharedPtr<FJsonObject> TreeObj = SerializeWidget(WBP->WidgetTree->RootWidget);
		if (TreeObj.IsValid())
			Result->SetObjectField(TEXT("widgetTree"), TreeObj);
	}

	FString Out;
	TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Result, W);
	return Out;
}

// ============================================================
// ADD WIDGET
// ============================================================

FString FNwiroWidgetTools::AddWidget(const FString& JsonCommand)
{
	// Game-thread marshal: Slate/UObject mutation here is game-thread-only, but tool
	// dispatch can arrive on the MCP HTTP worker thread. Mirrors CreateWidgetBlueprint's guard.
	if (!IsInGameThread())
	{
		TSharedRef<TPromise<FString>, ESPMode::ThreadSafe> Promise = MakeShared<TPromise<FString>, ESPMode::ThreadSafe>();
		TFuture<FString> Future = Promise->GetFuture();
		AsyncTask(ENamedThreads::GameThread, [JsonCommand, Promise]()
		{
			Promise->SetValue(FNwiroWidgetTools::AddWidget(JsonCommand));
		});
		return Future.Get();
	}
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	auto Pick = [&](std::initializer_list<const TCHAR*> Names) -> FString {
		for (const TCHAR* N : Names) { FString V; if (Cmd->TryGetStringField(N, V) && !V.IsEmpty()) return V; }
		return FString();
	};
	FString BPPath      = Pick({ TEXT("blueprint"), TEXT("widgetBlueprint"), TEXT("assetPath"), TEXT("path") });
	FString WidgetClass = Pick({ TEXT("widgetClass"), TEXT("widgetType"), TEXT("type"), TEXT("class"), TEXT("widget_type") });
	FString WidgetName  = Pick({ TEXT("name"), TEXT("widgetName"), TEXT("label") });
	FString ParentName  = Pick({ TEXT("parent"), TEXT("parentName"), TEXT("parentWidget") });

	const TSharedPtr<FJsonObject>* PropsObjPtr = nullptr;
	const TSharedPtr<FJsonObject>* SlotObjPtr  = nullptr;
	for (const TCHAR* K : { TEXT("properties"), TEXT("props"), TEXT("settings") })
		if (!PropsObjPtr && Cmd->TryGetObjectField(K, PropsObjPtr)) {}
	for (const TCHAR* K : { TEXT("slot"), TEXT("slotConfig"), TEXT("layout") })
		if (!SlotObjPtr && Cmd->TryGetObjectField(K, SlotObjPtr)) {}

	UWidgetBlueprint* WBP = FindWidgetBP(BPPath);
	if (!WBP)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"WidgetBlueprint not found: %s\"}"), *BPPath);

	UClass* WClass = ResolveWidgetClass(WidgetClass);
	if (!WClass)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Unknown widget class: %s. Use: Button, TextBlock, Image, CanvasPanel, VerticalBox, HorizontalBox, Overlay, ScrollBox, Slider, CheckBox, ProgressBar, EditableTextBox, Border, Spacer, SizeBox\"}"), *WidgetClass);

	if (WidgetName.IsEmpty())
		WidgetName = WidgetClass + FString::FromInt(FMath::Rand() % 1000);

	UWidgetTree* Tree = WBP->WidgetTree;
	if (!Tree) return TEXT("{\"success\":false,\"error\":\"No widget tree\"}");

	if (StaticFindObjectFast(UObject::StaticClass(), Tree, FName(*WidgetName)))
		return TEXT("{\"success\":false,\"error\":\"A widget with this name already exists\"}");
	UWidget* ParentWidget = ParentName.IsEmpty() ? Tree->RootWidget.Get() : Tree->FindWidget(FName(*ParentName));
	if (!ParentName.IsEmpty() && !ParentWidget)
		return TEXT("{\"success\":false,\"error\":\"Parent widget not found\"}");
	UPanelWidget* ParentPanel = Cast<UPanelWidget>(ParentWidget);
	if (ParentWidget && !ParentPanel)
		return TEXT("{\"success\":false,\"error\":\"Parent widget cannot contain children\"}");
	if (ParentPanel && !ParentPanel->CanAddMoreChildren())
		return TEXT("{\"success\":false,\"error\":\"Parent panel cannot accept more children\"}");

	FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "AddWidget", "AI: Add Widget"), Tree);
	Tx.AlsoModify(WBP);
	if (ParentPanel) Tx.AlsoModify(ParentPanel);

	// Create the widget
	UWidget* NewWidget = Tree->ConstructWidget<UWidget>(WClass, FName(*WidgetName));
	if (!NewWidget)
		return TEXT("{\"success\":false,\"error\":\"Failed to construct widget\"}");
	NewWidget->bIsVariable = true;

	// Register variable identity through the same hook as the UMG editor.
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6)
	WBP->OnVariableAdded(NewWidget->GetFName());
#endif

	TArray<FString> SkippedProps;
	int32 PropsApplied = 0;
	if (PropsObjPtr && (*PropsObjPtr).IsValid())
	{
		PropsApplied = ApplyJsonProperties(NewWidget, *PropsObjPtr, &SkippedProps);
	}

	UPanelSlot* PanelSlot = nullptr;
	if (ParentPanel)
	{
		PanelSlot = ParentPanel->AddChild(NewWidget);
		if (!PanelSlot)
		{
			return TEXT("{\"success\":false,\"error\":\"Parent panel cannot accept children\"}");
		}
	}
	else
	{
		// No root, set as root
		Tree->RootWidget = NewWidget;
	}

	FString SlotApplied = TEXT("none");
	if (PanelSlot && SlotObjPtr && (*SlotObjPtr).IsValid())
	{
		if (UCanvasPanelSlot* CSlot = Cast<UCanvasPanelSlot>(PanelSlot))
		{
			ApplyCanvasSlotConfig(CSlot, *SlotObjPtr);
			SlotApplied = TEXT("canvas");
		}
		else
		{
			ApplyJsonProperties(PanelSlot, *SlotObjPtr);
			SlotApplied = TEXT("generic");
		}
	}

	// Adding a variable widget changes the generated member layout. Match the
	// editor's skeleton regeneration after the tree/slot mutation is complete;
	// a full compile and persistence remain separate operations.
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

	FString SkippedJoined = FString::Join(SkippedProps, TEXT(","));
	return FString::Printf(TEXT("{\"success\":true,\"widget\":\"%s\",\"class\":\"%s\",\"parent\":\"%s\",\"propsApplied\":%d,\"propsSkipped\":\"%s\",\"slotApplied\":\"%s\"}"),
		*WidgetName, *WClass->GetName(), ParentPanel ? *ParentPanel->GetName() : TEXT("root"),
		PropsApplied, *SkippedJoined, *SlotApplied);
}

// ============================================================
// SET WIDGET PROPERTY
// ============================================================

FString FNwiroWidgetTools::SetWidgetProperty(const FString& JsonCommand)
{
	// Game-thread marshal: Slate/UObject mutation here is game-thread-only, but tool
	// dispatch can arrive on the MCP HTTP worker thread. Mirrors CreateWidgetBlueprint's guard.
	if (!IsInGameThread())
	{
		TSharedRef<TPromise<FString>, ESPMode::ThreadSafe> Promise = MakeShared<TPromise<FString>, ESPMode::ThreadSafe>();
		TFuture<FString> Future = Promise->GetFuture();
		AsyncTask(ENamedThreads::GameThread, [JsonCommand, Promise]()
		{
			Promise->SetValue(FNwiroWidgetTools::SetWidgetProperty(JsonCommand));
		});
		return Future.Get();
	}
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	auto Pick = [&](std::initializer_list<const TCHAR*> Names) -> FString {
		for (const TCHAR* N : Names) { FString V; if (Cmd->TryGetStringField(N, V) && !V.IsEmpty()) return V; }
		return FString();
	};
	FString BPPath       = Pick({ TEXT("blueprint"), TEXT("widgetBlueprint"), TEXT("assetPath"), TEXT("path") });
	FString WidgetName   = Pick({ TEXT("widget"), TEXT("widgetName"), TEXT("name") });
	FString PropertyName = Pick({ TEXT("property"), TEXT("propertyName"), TEXT("prop") });
	FString Value        = Pick({ TEXT("value"), TEXT("val") });

	UWidgetBlueprint* WBP = FindWidgetBP(BPPath);
	if (!WBP)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"WidgetBlueprint not found: %s\"}"), *BPPath);

	UWidget* Widget = WBP->WidgetTree ? WBP->WidgetTree->FindWidget(FName(*WidgetName)) : nullptr;
	if (!Widget && WBP->WidgetTree)
	{
		TArray<UWidget*> AllW;
		WBP->WidgetTree->GetAllWidgets(AllW);
		for (UWidget* W : AllW)
		{
			if (W && W->GetName().Equals(WidgetName, ESearchCase::IgnoreCase)) { Widget = W; break; }
		}
	}
	if (!Widget)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Widget not found: %s\"}"), *WidgetName);

	// Resolve which object/struct owns the property. The agent commonly uses
	// names visible in the blueprint editor — for slots these don't always
	// match the C++ field names. UCanvasPanelSlot exposes Anchors/Alignment/
	// Offsets in the editor but stores them nested inside `LayoutData`
	// (FAnchorData). So a single property name may need to walk through:
	//   Widget → Slot → LayoutData → Anchors
	// We accept any dotted path AND a list of editor-name → C++-path
	// aliases so the agent's mental model "just works".
	UObject* RootTarget = Widget;
	FString ResolvedPropName = PropertyName;

	TArray<TPair<UObject*, FString>> Candidates;
	Candidates.Emplace(Widget, PropertyName);
	if (Widget->Slot)
	{
		FString OnSlot = PropertyName;
		if (OnSlot.StartsWith(TEXT("Slot.")) || OnSlot.StartsWith(TEXT("slot.")))
			OnSlot = OnSlot.Mid(5);
		Candidates.Emplace(Widget->Slot, OnSlot);
		static const TMap<FString, FString> SlotAliases = {
			{ TEXT("anchors"),    TEXT("LayoutData.Anchors") },
			{ TEXT("alignment"),  TEXT("LayoutData.Alignment") },
			{ TEXT("offsets"),    TEXT("LayoutData.Offsets") },
			{ TEXT("offset"),     TEXT("LayoutData.Offsets") },
			{ TEXT("autosize"),   TEXT("bAutoSize") },
			{ TEXT("sizetocontent"), TEXT("bAutoSize") },
		};
		if (const FString* Alias = SlotAliases.Find(OnSlot.ToLower()))
		{
			Candidates.Emplace(Widget->Slot, *Alias);
		}
	}

	// Widget-level size aliases. The editor shows "size" on an Image via its
	// Brush's ImageSize, but agents naturally reach for WidthOverride /
	// HeightOverride / Size / DesiredSize (those are SizeBox-only). Map them
	// onto Brush.ImageSize so the natural call works on Image/Border/etc.
	{
		static const TMap<FString, FString> WidgetSizeAliases = {
			{ TEXT("widthoverride"),  TEXT("Brush.ImageSize.X") },
			{ TEXT("heightoverride"), TEXT("Brush.ImageSize.Y") },
			{ TEXT("width"),          TEXT("Brush.ImageSize.X") },
			{ TEXT("height"),         TEXT("Brush.ImageSize.Y") },
			{ TEXT("size"),           TEXT("Brush.ImageSize") },
			{ TEXT("desiredsize"),    TEXT("Brush.ImageSize") },
			{ TEXT("imagesize"),      TEXT("Brush.ImageSize") },
		};
		if (const FString* Alias = WidgetSizeAliases.Find(PropertyName.ToLower()))
			Candidates.Emplace(Widget, *Alias);
	}

	auto Resolve = [](UObject* Base, const FString& DottedPath, FProperty*& OutProp, void*& OutValPtr) -> bool
	{
		if (!Base) return false;
		TArray<FString> Parts; DottedPath.ParseIntoArray(Parts, TEXT("."));
		if (Parts.Num() == 0) return false;
		UStruct* CurStruct = Base->GetClass();
		void* CurPtr = (void*)Base;
		FProperty* CurProp = nullptr;
		for (int32 i = 0; i < Parts.Num(); i++)
		{
			CurProp = CurStruct->FindPropertyByName(FName(*Parts[i]));
			if (!CurProp) return false;
			void* NewPtr = CurProp->ContainerPtrToValuePtr<void>(CurPtr);
			if (i == Parts.Num() - 1) { OutProp = CurProp; OutValPtr = NewPtr; return true; }
			if (FStructProperty* SP = CastField<FStructProperty>(CurProp))
			{
				CurStruct = SP->Struct;
				CurPtr = NewPtr;
				continue;
			}
			return false;
		}
		return false;
	};

	UObject* Target = nullptr;
	FProperty* Prop = nullptr;
	void* ValuePtr = nullptr;
	for (const auto& Cand : Candidates)
	{
		FProperty* TryProp = nullptr; void* TryPtr = nullptr;
		if (Resolve(Cand.Key, Cand.Value, TryProp, TryPtr))
		{
			Target = Cand.Key;
			Prop = TryProp;
			ValuePtr = TryPtr;
			ResolvedPropName = Cand.Value;
			break;
		}
	}

	if (!Prop)
	{
		TArray<FString> WidgetProps;
		for (TFieldIterator<FProperty> It(Widget->GetClass()); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
				WidgetProps.Add(It->GetName());
		}
		TArray<FString> SlotProps;
		if (Widget->Slot)
		{
			for (TFieldIterator<FProperty> It(Widget->Slot->GetClass()); It; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
					SlotProps.Add(It->GetName());
			}
		}
		const int32 MaxList = 12;
		FString WidgetList = FString::Join(MakeArrayView(WidgetProps).Slice(0, FMath::Min(MaxList, WidgetProps.Num())), TEXT(", "));
		FString SlotList = Widget->Slot
			? FString::Join(MakeArrayView(SlotProps).Slice(0, FMath::Min(MaxList, SlotProps.Num())), TEXT(", "))
			: FString(TEXT("(widget has no slot — not in a panel)"));
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Property not found: %s. Aliases tried (Anchors/Alignment/Offsets → LayoutData.X, AutoSize → bAutoSize). Widget props (first %d): %s. Slot props (use bare name, 'Slot.X', or 'Slot.LayoutData.X'): %s\"}"),
			*PropertyName, MaxList, *WidgetList, *SlotList);
	}

	// Coerce friendly shorthand into the exact struct text ImportText wants, so
	// the agent does not burn retries guessing the literal syntax.
	//   FSlateChildSize (VerticalBox/HorizontalBox child Slot.Size):
	//     "auto" -> (SizeRule=Auto,Value=1.000000)
	//     "fill" / "fill:2" / "2" -> (SizeRule=Fill,Value=2.000000)
	//   FVector2D (e.g. Brush.ImageSize):
	//     "32" -> (X=32,Y=32) ; "32,48" / "32 48" -> (X=32,Y=48)
	// Only runs when the resolved prop is that struct AND the value is not
	// already a struct literal, so explicit "(SizeRule=...)" calls pass through.
	FString ImportVal = Value;
	if (FStructProperty* SP = CastField<FStructProperty>(Prop))
	{
		const FString StructName = SP->Struct->GetName();
		const FString Trimmed = Value.TrimStartAndEnd();
		if (!Trimmed.StartsWith(TEXT("(")))
		{
			if (StructName == TEXT("SlateChildSize"))
			{
				const FString Low = Trimmed.ToLower();
				if (Low.StartsWith(TEXT("auto")))
				{
					ImportVal = TEXT("(SizeRule=Auto,Value=1.000000)");
				}
				else
				{
					FString NumPart = Low;
					NumPart.ReplaceInline(TEXT("fill"), TEXT(""));
					NumPart.ReplaceInline(TEXT(":"), TEXT(" "));
					NumPart.ReplaceInline(TEXT(","), TEXT(" "));
					NumPart = NumPart.TrimStartAndEnd();
					const float Weight = NumPart.IsEmpty() ? 1.0f : FCString::Atof(*NumPart);
					ImportVal = FString::Printf(TEXT("(SizeRule=Fill,Value=%f)"), Weight);
				}
			}
			else if (StructName == TEXT("Vector2D") || StructName == TEXT("Vector2f"))
			{
				FString N = Trimmed;
				N.ReplaceInline(TEXT(","), TEXT(" "));
				TArray<FString> Toks;
				N.ParseIntoArray(Toks, TEXT(" "), true);
				if (Toks.Num() == 1)
					ImportVal = FString::Printf(TEXT("(X=%s,Y=%s)"), *Toks[0], *Toks[0]);
				else if (Toks.Num() >= 2)
					ImportVal = FString::Printf(TEXT("(X=%s,Y=%s)"), *Toks[0], *Toks[1]);
			}
		}
	}

	{
		FNwiroTransactionHelper Tx(NSLOCTEXT("Nwiro", "SetWidgetProperty", "AI: Set Widget Property"), Target);
		Tx.AlsoModify(WBP);

		if (Prop->ImportText_Direct(*ImportVal, ValuePtr, Target, PPF_None))
		{
			// A property default does not change the member layout. Preserve the
			// live edit and mark it dirty without compiling or saving here.
			FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

			const TCHAR* On = (Target == Widget) ? TEXT("widget") : TEXT("slot");
			return FString::Printf(TEXT("{\"success\":true,\"widget\":\"%s\",\"property\":\"%s\",\"value\":\"%s\",\"setOn\":\"%s\"}"),
				*WidgetName, *ResolvedPropName, *Value, On);
		}
		Tx.Cancel();
	}

	return FString::Printf(TEXT("{\"success\":false,\"error\":\"Failed to set %s on %s (ImportText rejected value '%s')\"}"), *PropertyName, *WidgetName, *Value);
}

// Render WidgetBlueprint to PNG via WidgetTree-direct (skips BPGC).
FString FNwiroWidgetTools::RenderWidgetBlueprint(const FString& JsonCommand)
{
	// Game-thread marshal: Slate construction + render commands here are game-thread-only, but
	// tool dispatch can arrive on the MCP HTTP worker thread. Mirrors CreateWidgetBlueprint's guard.
	if (!IsInGameThread())
	{
		TSharedRef<TPromise<FString>, ESPMode::ThreadSafe> Promise = MakeShared<TPromise<FString>, ESPMode::ThreadSafe>();
		TFuture<FString> Future = Promise->GetFuture();
		AsyncTask(ENamedThreads::GameThread, [JsonCommand, Promise]()
		{
			Promise->SetValue(FNwiroWidgetTools::RenderWidgetBlueprint(JsonCommand));
		});
		return Future.Get();
	}
	TSharedPtr<FJsonObject> Cmd;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonCommand);
	if (!FJsonSerializer::Deserialize(Reader, Cmd) || !Cmd.IsValid())
		return TEXT("{\"success\":false,\"error\":\"Invalid JSON\"}");

	FString Path;
	for (const TCHAR* K : { TEXT("path"), TEXT("blueprint"), TEXT("widget"), TEXT("name"), TEXT("assetPath") })
		if (Cmd->TryGetStringField(K, Path) && !Path.IsEmpty()) break;
	if (Path.IsEmpty())
		return TEXT("{\"success\":false,\"error\":\"Missing 'path'\"}");

	int32 Width = Cmd->HasField(TEXT("width")) ? (int32)Cmd->GetNumberField(TEXT("width")) : 1280;
	int32 Height = Cmd->HasField(TEXT("height")) ? (int32)Cmd->GetNumberField(TEXT("height")) : 720;
	Width = FMath::Clamp(Width, 64, 4096);
	Height = FMath::Clamp(Height, 64, 4096);

	UWidgetBlueprint* WBP = FindWidgetBP(Path);
	if (!WBP)
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Widget blueprint not found: %s\"}"), *Path);
	if (!WBP->WidgetTree || !WBP->WidgetTree->RootWidget)
		return TEXT("{\"success\":false,\"error\":\"WidgetTree or RootWidget is null\"}");

	TSharedRef<SWidget> SlateWidget = WBP->WidgetTree->RootWidget->TakeWidget();
	const FVector2D DrawSize((float)Width, (float)Height);

	UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>();
	RT->RenderTargetFormat = RTF_RGBA8;
	RT->ClearColor = FLinearColor(0.1f, 0.1f, 0.12f, 1.0f);
	RT->InitAutoFormat(Width, Height);
	RT->UpdateResourceImmediate(true);

	FWidgetRenderer* Renderer = new FWidgetRenderer(true);
	Renderer->SetIsPrepassNeeded(true);
	Renderer->DrawWidget(RT, SlateWidget, DrawSize, 0.0f);
	FlushRenderingCommands();
	BeginCleanup(Renderer);

	FTextureRenderTargetResource* RTRes = RT->GameThread_GetRenderTargetResource();
	if (!RTRes) return TEXT("{\"success\":false,\"error\":\"No render target resource\"}");

	TArray<FColor> Pixels;
	Pixels.SetNumUninitialized(Width * Height);
	FReadSurfaceDataFlags Flags;
	Flags.SetLinearToGamma(false);
	RTRes->ReadPixels(Pixels, Flags);

	IImageWrapperModule& ImageModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> Png = ImageModule.CreateImageWrapper(EImageFormat::PNG);
	if (!Png.IsValid() || !Png->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
		return TEXT("{\"success\":false,\"error\":\"PNG encode failed\"}");
	const TArray64<uint8> PngData = Png->GetCompressed();

	FString SavePath;
	Cmd->TryGetStringField(TEXT("saveTo"), SavePath);
	if (SavePath.IsEmpty())
	{
		const FString BaseName = FPaths::GetBaseFilename(WBP->GetPathName());
		SavePath = FPaths::ProjectSavedDir() / TEXT("NwiroWidgetRenders") / (BaseName + TEXT("_render.png"));
	}
	SavePath = FPaths::ConvertRelativePathToFull(SavePath);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(SavePath), true);
	if (!FFileHelper::SaveArrayToFile(PngData, *SavePath))
		return FString::Printf(TEXT("{\"success\":false,\"error\":\"Write PNG failed: %s\"}"), *SavePath);

	return FString::Printf(TEXT("{\"success\":true,\"path\":\"%s\",\"width\":%d,\"height\":%d,\"widget\":\"%s\"}"),
		*SavePath.Replace(TEXT("\\"), TEXT("/")), Width, Height, *WBP->GetName());
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Tests/NwiroWidgetLifecycleSpec.inl"
#endif
