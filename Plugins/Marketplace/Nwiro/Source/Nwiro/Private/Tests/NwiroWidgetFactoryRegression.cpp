// Artifact proposal: explicit owned-host native regression. No model/provider/Widget-CDO observation.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "../NwiroToolDispatch.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "WidgetBlueprint.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "EdGraphSchema_K2.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Serialization/Archive.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "UObject/Package.h"

namespace NwiroWidgetFactoryRegression
{
static const TCHAR* Target = TEXT("/Game/StackOBot/UI/_Shared/WBP_UI_Button.WBP_UI_Button");
static const TCHAR* Editable = TEXT("/Game/WidgetFactoryRegression/WBP_Editable.WBP_Editable");
static const TCHAR* Collision = TEXT("/Game/WidgetFactoryRegression/WBP_Collision.WBP_Collision");
static const TCHAR* Invalid = TEXT("/Game/WidgetFactoryRegression/WBP_Invalid.WBP_Invalid");
struct FRun
{
	FAutomationTestBase& Test;
	FString Output;
	TArray<TSharedPtr<FJsonValue>> Checks;
	bool EvidenceUsable = true;
	bool Check(const FString& Name, bool Passed)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("name"), Name); O->SetBoolField(TEXT("passed"), Passed);
		Checks.Add(MakeShared<FJsonValueObject>(O));
		if (!Passed) Test.AddError(Name);
		return Passed;
	}
	bool Row(const TSharedPtr<FJsonObject>& O)
	{
		if (!EvidenceUsable) return false;
		FString Text; auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		if (!FJsonSerializer::Serialize(O.ToSharedRef(), Writer))
		{
			EvidenceUsable = false; Test.AddError(TEXT("Cannot serialize native evidence")); return false;
		}
		Text += LINE_TERMINATOR;
		TUniquePtr<FArchive> File(IFileManager::Get().CreateFileWriter(*(Output / TEXT("native-actions.jsonl")), FILEWRITE_Append));
		if (!File) { EvidenceUsable = false; Test.AddError(TEXT("Cannot persist native evidence")); return false; }
		FTCHARToUTF8 Bytes(*Text);
		File->Serialize((void*)Bytes.Get(), Bytes.Length()); File->Flush();
		const bool Closed = File->Close();
		if (!Closed || File->IsError())
		{
			EvidenceUsable = false; Test.AddError(TEXT("Native evidence archive failed")); return false;
		}
		return true;
	}
	TSharedPtr<FJsonObject> Call(const TCHAR* Name, const FString& Args)
	{
		TSharedPtr<FJsonObject> Start = MakeShared<FJsonObject>();
		const FString Id = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
		Start->SetStringField(TEXT("id"), Id); Start->SetStringField(TEXT("phase"), TEXT("before"));
		Start->SetStringField(TEXT("at"), FDateTime::UtcNow().ToIso8601()); Start->SetStringField(TEXT("tool"), Name);
		Start->SetStringField(TEXT("args"), Args);
		if (!Row(Start)) return nullptr;
		const FString Raw = FNwiroToolDispatch::DispatchTool(Name, Args);
		TSharedPtr<FJsonObject> End = MakeShared<FJsonObject>();
		End->SetStringField(TEXT("id"), Id); End->SetStringField(TEXT("phase"), TEXT("after"));
		End->SetStringField(TEXT("at"), FDateTime::UtcNow().ToIso8601()); End->SetStringField(TEXT("tool"), Name);
		End->SetStringField(TEXT("raw"), Raw);
		if (!Row(End)) return nullptr;
		TSharedPtr<FJsonObject> Result;
		Check(FString(Name) + TEXT(" returned real JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Result) && Result.IsValid());
		return Result;
	}
	bool Success(const TSharedPtr<FJsonObject>& R) const
	{
		bool Value = false; return R.IsValid() && R->TryGetBoolField(TEXT("success"), Value) && Value;
	}
	bool Finish()
	{
		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("phase"), TEXT("complete")); R->SetStringField(TEXT("at"), FDateTime::UtcNow().ToIso8601());
		R->SetStringField(TEXT("product"), TEXT("pro")); R->SetStringField(TEXT("project"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
		R->SetArrayField(TEXT("checks"), Checks);
		return Row(R) && !Test.HasAnyErrors();
	}
};
static bool Guard(FRun& R, const TCHAR* Phase)
{
	FString ManifestPath, Raw; TSharedPtr<FJsonObject> M;
	if (!R.Check(TEXT("Explicit owned-host manifest required"), FParse::Value(FCommandLine::Get(), TEXT("NwiroWidgetRegressionManifest="), ManifestPath))) return false;
	if (!R.Check(TEXT("Readable manifest"), FFileHelper::LoadFileToString(Raw, *ManifestPath) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), M) && M.IsValid())) return false;
	FString Project, Product, Output, ExpectedPhase, Dll;
	M->TryGetStringField(TEXT("project"), Project); M->TryGetStringField(TEXT("product"), Product);
	M->TryGetStringField(TEXT("outputDirectory"), Output); M->TryGetStringField(TEXT("phase"), ExpectedPhase);
	M->TryGetStringField(TEXT("dll"), Dll);
	R.Output = Output;
	const FString ActualProject = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
	if (!R.Check(TEXT("Exact phase/product"), Product == TEXT("pro") && ExpectedPhase == Phase)) return false;
	if (!R.Check(TEXT("Exact owned project"), FPaths::IsSamePath(ActualProject, Project) && FPaths::FileExists(FPaths::ProjectDir() / TEXT("owned-host.json")))) return false;
	if (!R.Check(TEXT("Output is precreated and new"), IFileManager::Get().DirectoryExists(*Output) && !FPaths::FileExists(Output / TEXT("native-actions.jsonl")))) return false;
	if (!R.Check(TEXT("Loaded module path equals pinned child DLL"), FPaths::IsSamePath(FModuleManager::Get().GetModuleFilename(TEXT("Nwiro")), Dll) && FPaths::IsUnderDirectory(Dll, FPaths::ProjectDir()))) return false;
	const FEngineVersion Version = FEngineVersion::Current();
	return R.Check(TEXT("Installed UE5.8.2"), Version.GetMajor()==5 && Version.GetMinor()==8 && Version.GetPatch()==2);
}
static FString JsonTarget(const TCHAR* Asset)
{
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>(); O->SetStringField(TEXT("assetPath"), Asset); O->SetStringField(TEXT("blueprint"), Asset);
	FString S; FJsonSerializer::Serialize(O, TJsonWriterFactory<>::Create(&S)); return S;
}
static FString Filename(UObject* Object)
{
	return FPackageName::LongPackageNameToFilename(Object->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
}
static bool SaveFixture(FRun& R, UObject* Object)
{
	if (!Object) return R.Check(TEXT("Fixture object exists before explicit test save"), false);
	TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>(); E->SetStringField(TEXT("action"),TEXT("explicit-regression-fixture-save"));
	E->SetStringField(TEXT("asset"),Object->GetPathName()); E->SetStringField(TEXT("at"),FDateTime::UtcNow().ToIso8601());
	if (!R.Row(E)) return false;
	return R.Check(TEXT("Explicit requested fixture saved"), UEditorAssetLibrary::SaveLoadedAsset(Object, false));
}
static bool InspectWidget(FRun& R, const TCHAR* Asset, const TCHAR* Variable, bool RequireTree, bool RequireEditable)
{
	UWidgetBlueprint* BP = LoadObject<UWidgetBlueprint>(nullptr, Asset);
	if (!R.Check(FString(Asset)+TEXT(" exact WidgetBlueprint"), BP && BP->GetPathName()==Asset)) return false;
	R.Check(TEXT("Real Widget generated class"), Cast<UWidgetBlueprintGeneratedClass>(BP->GeneratedClass)!=nullptr);
	R.Check(TEXT("Separate generated/skeleton classes"), BP->GeneratedClass && BP->SkeletonGeneratedClass && BP->GeneratedClass!=BP->SkeletonGeneratedClass);
	R.Check(TEXT("Correct UserWidget parent"), BP->ParentClass==UUserWidget::StaticClass());
	if (RequireTree)
	{
		UButton* Button = BP->WidgetTree ? Cast<UButton>(BP->WidgetTree->FindWidget(TEXT("ButtonWidget"))) : nullptr;
		UTextBlock* Label = BP->WidgetTree ? Cast<UTextBlock>(BP->WidgetTree->FindWidget(TEXT("Label"))) : nullptr;
		R.Check(TEXT("ButtonWidget is Button"), Button!=nullptr);
		R.Check(TEXT("Label is TextBlock and direct ButtonWidget child"), Label && Button && Label->GetParent()==Button);
	}
	const FBPVariableDescription* V = BP->NewVariables.FindByPredicate([&](const FBPVariableDescription& X){return X.VarName==FName(Variable);});
	if (!R.Check(FString(Variable)+TEXT(" authored variable exists"), V!=nullptr)) return false;
	R.Check(FString(Variable)+TEXT(" real scalar Text"), V->VarType.PinCategory==UEdGraphSchema_K2::PC_Text && V->VarType.ContainerType==EPinContainerType::None);
	const bool EditableFlag = (V->PropertyFlags & CPF_Edit) && !(V->PropertyFlags & (CPF_DisableEditOnInstance|CPF_EditConst));
	TSharedPtr<FJsonObject> Observation=MakeShared<FJsonObject>();
	Observation->SetStringField(TEXT("action"),TEXT("authored-member-observation"));
	Observation->SetStringField(TEXT("asset"),Asset); Observation->SetStringField(TEXT("name"),Variable);
	Observation->SetStringField(TEXT("propertyFlags"),FString::Printf(TEXT("%llu"),(unsigned long long)V->PropertyFlags));
	Observation->SetBoolField(TEXT("instanceEditable"),EditableFlag);
	Observation->SetBoolField(TEXT("editableRequiredByThisRegressionCase"),RequireEditable);
	if (!R.Row(Observation)) return false;
	if (RequireEditable) R.Check(TEXT("Supported expose_on_spawn produces real editable member"),EditableFlag);
	// Deliberately no generated Widget CDO access, widget instantiation, rendering or event firing.
	return !R.Test.HasAnyErrors();
}
static void CompileNoSave(FRun& R, const TCHAR* Asset)
{
	const TSharedPtr<FJsonObject> Result=R.Call(TEXT("bp_get_compile_errors"),JsonTarget(Asset));
	R.Check(TEXT("Native compile diagnostic succeeded"),R.Success(Result));
	double ErrorCount=-1; bool SavePerformed=true, CompilePerformed=false;
	FString ActualAsset, VerificationStatus, CompilerStatus;
	if (Result)
	{
		Result->TryGetNumberField(TEXT("errorCount"),ErrorCount);
		Result->TryGetBoolField(TEXT("savePerformed"),SavePerformed);
		Result->TryGetBoolField(TEXT("compilePerformed"),CompilePerformed);
		Result->TryGetStringField(TEXT("assetPath"),ActualAsset);
		Result->TryGetStringField(TEXT("verificationStatus"),VerificationStatus);
		Result->TryGetStringField(TEXT("status"),CompilerStatus);
	}
	R.Check(TEXT("Native compile exact canonical asset"),ActualAsset==Asset);
	R.Check(TEXT("Native compile actually completed"),CompilePerformed && VerificationStatus==TEXT("completed"));
	R.Check(TEXT("Native compiler status is validated"),CompilerStatus==TEXT("UpToDate") || CompilerStatus==TEXT("UpToDateWithWarnings"));
	R.Check(TEXT("Native compile diagnostic has zero errors"),ErrorCount==0);
	R.Check(TEXT("No-save diagnostic receipt"),!SavePerformed);
}
static bool NoClobber(FRun& R, bool MustBeUnloaded)
{
	if (MustBeUnloaded && !R.Check(TEXT("Cold wrong-type fixture not preloaded"),FindObject<UObject>(nullptr,Collision)==nullptr)) return false;
	const FString Package=FPackageName::ObjectPathToPackageName(FString(Collision));
	const FString File=FPackageName::LongPackageNameToFilename(Package,FPackageName::GetAssetPackageExtension());
	TArray<uint8> Before,After; if (!R.Check(TEXT("Wrong-type saved fixture exists"),FFileHelper::LoadFileToArray(Before,*File))) return false;
	UObject* Prior=MustBeUnloaded ? nullptr : FindObject<UObject>(nullptr,Collision);
	const bool PriorDirty=Prior ? Prior->GetOutermost()->IsDirty() : false;
	const auto Result=R.Call(TEXT("create_widget_blueprint"),TEXT("{\"name\":\"WBP_Collision\",\"path\":\"/Game/WidgetFactoryRegression\"}"));
	R.Check(TEXT("Wrong-type collision explicitly refused"),Result.IsValid() && !R.Success(Result));
	UObject* Current=LoadObject<UObject>(nullptr,Collision);
	R.Check(TEXT("Wrong-type asset is still an Actor Blueprint"),Current && Current->GetClass()==UBlueprint::StaticClass() && Cast<UBlueprint>(Current)->ParentClass==AActor::StaticClass());
	if (Prior) R.Check(TEXT("Loaded wrong-type identity/dirtiness preserved"),Current==Prior && Current->GetOutermost()->IsDirty()==PriorDirty);
	R.Check(TEXT("Wrong-type saved bytes untouched"),FFileHelper::LoadFileToArray(After,*File) && Before==After);
	return !R.Test.HasAnyErrors();
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroWidgetFactoryRegressionCreateSave,"Nwiro.WidgetFactoryRegression.CreateSave",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FNwiroWidgetFactoryRegressionCreateSave::RunTest(const FString&)
{
	using namespace NwiroWidgetFactoryRegression; FRun R{*this}; if (!Guard(R,TEXT("create_save"))) return false;
	for (const TCHAR* Asset : {Target, Editable, Collision, Invalid})
	{
		if (!R.Check(FString(TEXT("Fresh fixture absent: "))+Asset,!UEditorAssetLibrary::DoesAssetExist(Asset) && FindObject<UObject>(nullptr,Asset)==nullptr)) return R.Finish();
	}
	const auto Created = R.Call(TEXT("create_widget_blueprint"),TEXT("{\"name\":\"UI_Button\",\"path\":\"/Game/StackOBot/UI/_Shared/\"}"));
	if (!R.Check(TEXT("Exact BP040 create returned"),R.Success(Created))) return R.Finish();
	bool SaveAttempted = false, SavedToDisk = false;
	Created->TryGetBoolField(TEXT("saveAttempted"),SaveAttempted);
	Created->TryGetBoolField(TEXT("savedToDisk"),SavedToDisk);
	TArray<uint8> FactorySavedBytes;
	const FString FactorySavedFile = FPackageName::LongPackageNameToFilename(FPackageName::ObjectPathToPackageName(FString(Target)),FPackageName::GetAssetPackageExtension());
	if (!R.Check(TEXT("Factory reported an actual successful save"),SaveAttempted && SavedToDisk) ||
		!R.Check(TEXT("Factory saved nonempty asset before any edit or fixture save"),FFileHelper::LoadFileToArray(FactorySavedBytes,*FactorySavedFile) && FactorySavedBytes.Num()>0)) return R.Finish();
	if (!R.Check(TEXT("Exact BP040 Button addition"),R.Success(R.Call(TEXT("add_widget"),TEXT("{\"blueprint\":\"/Game/StackOBot/UI/_Shared/WBP_UI_Button.WBP_UI_Button\",\"widgetClass\":\"Button\",\"name\":\"ButtonWidget\"}"))))) return R.Finish();
	if (!R.Check(TEXT("Exact BP040 Label addition"),R.Success(R.Call(TEXT("add_widget"),TEXT("{\"blueprint\":\"/Game/StackOBot/UI/_Shared/WBP_UI_Button.WBP_UI_Button\",\"name\":\"Label\",\"parent\":\"ButtonWidget\",\"widgetClass\":\"TextBlock\"}"))))) return R.Finish();
	// Preserve the corpus alias as a rejected/no-touch control before canonical fixture setup.
	UWidgetBlueprint* LegacyTarget = LoadObject<UWidgetBlueprint>(nullptr,Target);
	if (!R.Check(TEXT("Legacy control target exists"),LegacyTarget && LegacyTarget->WidgetTree)) return R.Finish();
	UWidgetTree* LegacyTree = LegacyTarget->WidgetTree;
	UWidget* LegacyRoot = LegacyTree->RootWidget;
	TArray<UWidget*> LegacyWidgets; LegacyTree->GetAllWidgets(LegacyWidgets);
	TArray<UPanelWidget*> LegacyParents; for (UWidget* Widget : LegacyWidgets) LegacyParents.Add(Widget->GetParent());
	const int32 LegacyMemberCount = LegacyTarget->NewVariables.Num();
	const bool LegacyDirty = LegacyTarget->GetOutermost()->IsDirty();
	const auto HasLabelString = [](const UWidgetBlueprint* BP) { return BP->NewVariables.ContainsByPredicate([](const FBPVariableDescription& V){ return V.VarName==FName(TEXT("Label String")); }); };
	TArray<uint8> LegacyBefore,LegacyAfter; const FString LegacyFile=Filename(LegacyTarget);
	if (!R.Check(TEXT("Legacy control member initially absent"),!HasLabelString(LegacyTarget)) ||
	    !R.Check(TEXT("Legacy control baseline disk bytes readable"),FFileHelper::LoadFileToArray(LegacyBefore,*LegacyFile))) return R.Finish();
	const auto Legacy = R.Call(TEXT("edit_blueprint"),TEXT("{\"blueprint\":\"/Game/StackOBot/UI/_Shared/WBP_UI_Button.WBP_UI_Button\",\"variables\":[{\"name\":\"Label String\",\"type\":\"Text\",\"isExposed\":true}]}"));
	if (!R.Check(TEXT("Exact BP040 ignored alias is rejected"),Legacy.IsValid()&&!R.Success(Legacy))) return R.Finish();
	TArray<UWidget*> LegacyWidgetsAfter; LegacyTree->GetAllWidgets(LegacyWidgetsAfter);
	TArray<UPanelWidget*> LegacyParentsAfter; for (UWidget* Widget : LegacyWidgetsAfter) LegacyParentsAfter.Add(Widget->GetParent());
	if (!R.Check(TEXT("Legacy rejection preserves object/member identity"),FindObject<UWidgetBlueprint>(nullptr,Target)==LegacyTarget && LegacyTarget->NewVariables.Num()==LegacyMemberCount && !HasLabelString(LegacyTarget)) ||
	    !R.Check(TEXT("Legacy rejection preserves widget tree/root/children"),LegacyTarget->WidgetTree==LegacyTree && LegacyTree->RootWidget==LegacyRoot && LegacyWidgetsAfter==LegacyWidgets && LegacyParentsAfter==LegacyParents) ||
	    !R.Check(TEXT("Legacy rejection preserves dirty bit and saved bytes"),LegacyTarget->GetOutermost()->IsDirty()==LegacyDirty && FFileHelper::LoadFileToArray(LegacyAfter,*LegacyFile) && LegacyAfter==LegacyBefore)) return R.Finish();
	if (!R.Check(TEXT("Canonical instance editable Label String fixture"),R.Success(R.Call(TEXT("edit_blueprint"),TEXT("{\"blueprint\":\"/Game/StackOBot/UI/_Shared/WBP_UI_Button.WBP_UI_Button\",\"add_variables\":[{\"name\":\"Label String\",\"type\":\"text\",\"instance_editable\":true}]}"))))) return R.Finish();
	if (!InspectWidget(R,Target,TEXT("Label String"),true,true)) return R.Finish();
	TSharedPtr<FJsonObject> Naming=MakeShared<FJsonObject>();Naming->SetStringField(TEXT("action"),TEXT("separate-corpus-naming-observation"));
	Naming->SetBoolField(TEXT("canonicalUI_ButtonExists"),UEditorAssetLibrary::DoesAssetExist(TEXT("/Game/StackOBot/UI/_Shared/UI_Button.UI_Button")));
	if (!R.Row(Naming)) return R.Finish();
	UWidgetBlueprint* Original=LoadObject<UWidgetBlueprint>(nullptr,Target);
	if (!SaveFixture(R,Original)) return R.Finish();
	TArray<uint8> Before,After; FString File=Filename(Original);if (!R.Check(TEXT("Saved target readable"),FFileHelper::LoadFileToArray(Before,*File))) return R.Finish();
	const bool Dirty=Original->GetOutermost()->IsDirty();
	const auto Again=R.Call(TEXT("create_widget_blueprint"),TEXT("{\"name\":\"UI_Button\",\"path\":\"/Game/StackOBot/UI/_Shared/\"}"));
	bool Existed=false;if(Again)Again->TryGetBoolField(TEXT("existed"),Existed);
	R.Check(TEXT("Existing Widget is idempotent"),R.Success(Again)&&Existed&&LoadObject<UWidgetBlueprint>(nullptr,Target)==Original&&Original->GetOutermost()->IsDirty()==Dirty);
	R.Check(TEXT("Existing Widget saved bytes untouched"),FFileHelper::LoadFileToArray(After,*File)&&Before==After);
	InspectWidget(R,Target,TEXT("Label String"),true,true);
	if (!R.Check(TEXT("Supported flag fixture creation"),R.Success(R.Call(TEXT("create_widget_blueprint"),TEXT("{\"name\":\"WBP_Editable\",\"path\":\"/Game/WidgetFactoryRegression\",\"rootWidget\":\"CanvasPanel\"}"))))) return R.Finish();
	if (!R.Check(TEXT("Supported editable Text mutation"),R.Success(R.Call(TEXT("edit_blueprint"),TEXT("{\"blueprint\":\"/Game/WidgetFactoryRegression/WBP_Editable.WBP_Editable\",\"add_variables\":[{\"name\":\"LabelText\",\"type\":\"text\",\"expose_on_spawn\":true}],\"compile\":true}"))))) return R.Finish();
	if (!InspectWidget(R,Editable,TEXT("LabelText"),false,true) || !SaveFixture(R,LoadObject<UWidgetBlueprint>(nullptr,Editable))) return R.Finish();
	if (!R.Check(TEXT("Wrong-type setup through native Blueprint tool"),R.Success(R.Call(TEXT("create_blueprint"),TEXT("{\"name\":\"WBP_Collision\",\"path\":\"/Game/WidgetFactoryRegression\",\"parentClass\":\"Actor\"}"))))) return R.Finish();
	if (!SaveFixture(R,LoadObject<UBlueprint>(nullptr,Collision)) || !NoClobber(R,false)) return R.Finish();
	R.Check(TEXT("Invalid root target absent before call"),!UEditorAssetLibrary::DoesAssetExist(Invalid));
	const auto Bad=R.Call(TEXT("create_widget_blueprint"),TEXT("{\"name\":\"WBP_Invalid\",\"path\":\"/Game/WidgetFactoryRegression\",\"rootWidget\":\"DefinitelyMissingWidgetClass_Regression\"}"));
	R.Check(TEXT("Invalid root rejected without asset"),Bad.IsValid()&&!R.Success(Bad)&&!UEditorAssetLibrary::DoesAssetExist(Invalid)&&FindObject<UObject>(nullptr,Invalid)==nullptr);
	return R.Finish();
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroWidgetFactoryRegressionColdReload,"Nwiro.WidgetFactoryRegression.ColdReload",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FNwiroWidgetFactoryRegressionColdReload::RunTest(const FString&)
{
	using namespace NwiroWidgetFactoryRegression; FRun R{*this};if(!Guard(R,TEXT("cold_reload")))return false;
	if (!R.Check(TEXT("Both cold widget fixtures initially unloaded"),FindObject<UObject>(nullptr,Target)==nullptr && FindObject<UObject>(nullptr,Editable)==nullptr)) return R.Finish();
	if (!NoClobber(R,true)) return R.Finish();
	if (!InspectWidget(R,Target,TEXT("Label String"),true,true) || !InspectWidget(R,Editable,TEXT("LabelText"),false,true)) return R.Finish();
	CompileNoSave(R,Target);CompileNoSave(R,Editable);
	InspectWidget(R,Target,TEXT("Label String"),true,true);
	InspectWidget(R,Editable,TEXT("LabelText"),false,true);
	return R.Finish();
}
#endif
