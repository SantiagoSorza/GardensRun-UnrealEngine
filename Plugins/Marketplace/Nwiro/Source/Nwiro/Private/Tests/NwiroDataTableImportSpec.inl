// Copyright 2026 Nwiro. All Rights Reserved.
// Public-handler regression: malformed imports must preserve the live table.
#include "UObject/ObjectSaveContext.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNwiroDataTableImportSpec,
	"Nwiro.Pro.DataTools.ImportJson.AtomicReplacement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNwiroDataTableImportSpec::RunTest(const FString& Parameters)
{
	using namespace NwiroDataToolsAcceptance;
	if (!TestTrue(TEXT("import fixture owns an idle editor transaction"),
		IsInGameThread() && GEditor && !GEditor->IsTransactionActive() && !GIsTransacting)) return false;
	FFixture Fixture(*this);
	const FString StructName = Fixture.NewName(TEXT("ImportRow"));
	if (!Fixture.Call(true, StructName, TEXT("[{\"name\":\"Count\",\"type\":\"int\"},{\"name\":\"Label\",\"type\":\"string\"},{\"name\":\"Enabled\",\"type\":\"bool\"},{\"name\":\"Scores\",\"type\":\"int\",\"container\":\"array\"}]"), true)) return false;
	UScriptStruct* RowStruct = Cast<UScriptStruct>(Fixture.Find(StructName));
	if (!TestNotNull(TEXT("row struct exists"), RowStruct)) return false;
	const FString TableName = Fixture.NewName(TEXT("ImportTable"));
	const FString TablePackagePath = Fixture.Root / TableName;
	int32 Saves = 0;
	const FDelegateHandle Saving = FCoreUObjectDelegates::OnObjectPreSave.AddLambda(
		[&](UObject* Object, FObjectPreSaveContext)
		{ if (Object && Object->GetOutermost()->GetName() == TablePackagePath) ++Saves; });
	ON_SCOPE_EXIT { FCoreUObjectDelegates::OnObjectPreSave.Remove(Saving); };
	TSharedRef<FJsonObject> CreateCommand = MakeShared<FJsonObject>();
	CreateCommand->SetStringField(TEXT("name"), TableName);
	CreateCommand->SetStringField(TEXT("path"), Fixture.Root);
	CreateCommand->SetStringField(TEXT("rowStruct"), RowStruct->GetPathName());
	FString CreateJson;
	FJsonSerializer::Serialize(CreateCommand, TJsonWriterFactory<>::Create(&CreateJson));
	const FGuid BeforeCreation = GEditor->Trans->GetUndoContext().TransactionId;
	// Exercise the real tool, without an outer transaction. Unexpected engine
	// LogDataTable errors fail automation; no expected-error suppression is used.
	const FString CreateRaw = FNwiroDataTools::CreateDataTable(CreateJson);
	TSharedPtr<FJsonObject> Created;
	bool CreatedSuccess = false, CreatedSaved = true;
	if (!TestTrue(TEXT("DataTable creation returns successful JSON"),
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(CreateRaw), Created) && Created.IsValid()
		&& Created->TryGetBoolField(TEXT("success"), CreatedSuccess) && CreatedSuccess))
	{
		AddError(CreateRaw);
		return false;
	}
	TStrongObjectPtr<UDataTable> Table(Cast<UDataTable>(Fixture.Find(TableName)));
	if (!TestTrue(TEXT("actual tool created the exact table"), Table.IsValid())) return false;
	UPackage* Package = Table->GetOutermost();
	TestEqual(TEXT("creation reports exact RowStruct path"), Created->GetStringField(TEXT("rowStructPath")), RowStruct->GetPathName());
	TestTrue(TEXT("creation reports no disk save"), Created->TryGetBoolField(TEXT("savedToDisk"), CreatedSaved) && !CreatedSaved);
	TestTrue(TEXT("factory bound actual row struct before publication"), Table->GetRowStruct() == RowStruct);
	TestEqual(TEXT("new table starts empty"), Table->GetRowMap().Num(), 0);
	TestTrue(TEXT("new table remains transactional and dirty"), Table->HasAnyFlags(RF_Transactional) && Package->IsDirty());
	Fixture.AssertNotSaved(TableName);
	const FTransactionContext Creation = GEditor->Trans->GetUndoContext();
	if (!TestTrue(TEXT("DataTable creation owns one undo transaction"), Creation.TransactionId.IsValid()
		&& Creation.TransactionId != BeforeCreation && Creation.Title.EqualTo(NSLOCTEXT("Nwiro", "CreateDataTable", "AI: Create DataTable")))) return false;
	if (!TestTrue(TEXT("DataTable creation can be undone"), GEditor->UndoTransaction())) return false;
	TestNull(TEXT("creation undo removes the asset at its published path"), Fixture.Find(TableName));
	TestTrue(TEXT("creation undo retains completed row struct in transient storage"),
		Table->GetOutermost() == GetTransientPackage() && Table->GetRowStruct() == RowStruct);
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TestFalse(TEXT("creation undo removes registry entry"), Registry.GetAssetByObjectPath(FSoftObjectPath(Fixture.ObjectPath(TableName))).IsValid());
	if (!TestTrue(TEXT("creation redo belongs to this tool"), GEditor->Trans->GetRedoContext().TransactionId == Creation.TransactionId)
		|| !TestTrue(TEXT("DataTable creation can be redone"), GEditor->RedoTransaction())) return false;
	TestTrue(TEXT("creation redo restores exact table identity and row struct"),
		Fixture.Find(TableName) == Table.Get() && Table->GetOutermost() == Package && Table->GetRowStruct() == RowStruct);
	TestTrue(TEXT("creation redo restores registry entry"), Registry.GetAssetByObjectPath(FSoftObjectPath(Fixture.ObjectPath(TableName))).IsValid());
	TestEqual(TEXT("creation and undo/redo never save"), Saves, 0);
	Fixture.AssertNotSaved(TableName);

	// An external transaction is rejected before allocation and is not canceled.
	const FString BusyName = Fixture.NewName(TEXT("BusyImportTable"));
	{
		FNwiroTransactionHelper Outer(NSLOCTEXT("NwiroTest", "BusyDataTableCreation", "Busy DataTable creation"));
		CreateCommand->SetStringField(TEXT("name"), BusyName);
		CreateJson.Reset(); FJsonSerializer::Serialize(CreateCommand, TJsonWriterFactory<>::Create(&CreateJson));
		TSharedPtr<FJsonObject> Rejected; bool Success = true;
		TestTrue(TEXT("active external transaction is rejected explicitly"),
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FNwiroDataTools::CreateDataTable(CreateJson)), Rejected)
			&& Rejected.IsValid() && Rejected->TryGetBoolField(TEXT("success"), Success) && !Success
			&& Rejected->GetStringField(TEXT("error")).Contains(TEXT("transaction")));
		TestTrue(TEXT("rejection preserves caller transaction"), GEditor->IsTransactionActive());
		Fixture.AssertAbsent(BusyName);
		Outer.Cancel();
	}
	int32 Changes = 0;
	const FDelegateHandle Changed = Table->OnDataTableChanged().AddLambda([&]() { ++Changes; });
	ON_SCOPE_EXIT { Table->OnDataTableChanged().Remove(Changed); };
	const auto Call = [&](const FString& Data, bool Expected)
	{
		TSharedRef<FJsonObject> Command = MakeShared<FJsonObject>();
		Command->SetStringField(TEXT("table"), Table->GetPathName());
		Command->SetStringField(TEXT("json"), Data);
		FString Json;
		FJsonSerializer::Serialize(Command, TJsonWriterFactory<>::Create(&Json));
		const FString Raw = FNwiroDataTools::ImportDataTableJson(Json);
		TSharedPtr<FJsonObject> Result;
		bool Success = false;
		if (!TestTrue(TEXT("import response parses as JSON"),
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Result) && Result.IsValid())) return Result;
		TestTrue(TEXT("import result has success"), Result->TryGetBoolField(TEXT("success"), Success));
		TestEqual(TEXT("import result matches expectation"), Success, Expected);
		if (Success != Expected) AddError(TEXT("Import response: ") + Raw);
		if (!Success)
		{
			FString Error;
			TestTrue(TEXT("rejected import explains the problem"), Result->TryGetStringField(TEXT("error"), Error) && !Error.IsEmpty());
		}
		double Count = -1;
		TestTrue(TEXT("result reports observed row count"), Result->TryGetNumberField(TEXT("rowCount"), Count));
		TestEqual(TEXT("reported row count is actual target state"), Count, static_cast<double>(Table->GetRowMap().Num()));
		bool Saved = true;
		TestTrue(TEXT("result reports explicit save outcome"), Result->TryGetBoolField(TEXT("savedToDisk"), Saved));
		TestFalse(TEXT("import never claims a save"), Saved);
		return Result;
	};
	const FString Baseline = TEXT("[{\"Name\":\"Row_A\",\"Count\":7,\"Label\":\"keep me\",\"Enabled\":true,\"Scores\":[1,2]},{\"Name\":\"Row_Keep\",\"Count\":11,\"Label\":\"unchanged\",\"Enabled\":false,\"Scores\":[3]}]");
	Call(Baseline, true);
	if (!TestEqual(TEXT("baseline contains both rows"), Table->GetRowMap().Num(), 2)) return false;
	const FString BeforeJson = Table->GetTableAsJSON();
	const TMap<FName, uint8*> BeforeRows = Table->GetRowMap();
	TArray<FString> Invalid = {
		TEXT("[42]"), TEXT("[null]"), TEXT("[]"), TEXT("{}"), TEXT("not json"),
		TEXT("[{\"Name\":7,\"Count\":1,\"Label\":\"x\",\"Enabled\":true,\"Scores\":[]}]"),
		TEXT("[{\"Count\":1,\"Label\":\"x\",\"Enabled\":true,\"Scores\":[]}]"),
		TEXT("[{\"Name\":\"None\",\"Count\":1,\"Label\":\"x\",\"Enabled\":true,\"Scores\":[]}]"),
		TEXT("[{\"Name\":\"Bad\",\"Count\":1,\"Label\":\"x\",\"Enabled\":true,\"Scores\":[],\"UnknownColumn\":3}]"),
		TEXT("[{\"Name\":\"Bad\",\"Count\":\"invalid\",\"Label\":\"x\",\"Enabled\":true,\"Scores\":[]}]"),
		TEXT("[{\"Name\":\"Bad\",\"Count\":\"7\",\"Label\":\"x\",\"Enabled\":true,\"Scores\":[]}]"),
		TEXT("[{\"Name\":\"Bad\",\"Count\":2.5,\"Label\":\"x\",\"Enabled\":true,\"Scores\":[]}]"),
		TEXT("[{\"Name\":\"Bad\",\"Count\":2147483648,\"Label\":\"x\",\"Enabled\":true,\"Scores\":[]}]"),
		TEXT("[{\"Name\":\"Bad\",\"Count\":-2147483649,\"Label\":\"x\",\"Enabled\":true,\"Scores\":[]}]"),
		TEXT("[{\"Name\":\"Bad\",\"Count\":true,\"Label\":\"x\",\"Enabled\":true,\"Scores\":[]}]"),
		TEXT("[{\"Name\":\"Bad\",\"Count\":1,\"Label\":9,\"Enabled\":true,\"Scores\":[]}]"),
		TEXT("[{\"Name\":\"Bad\",\"Count\":1,\"Label\":\"x\",\"Enabled\":\"invalid\",\"Scores\":[]}]"),
		TEXT("[{\"Name\":\"Bad\",\"Count\":1,\"Label\":\"x\",\"Enabled\":true,\"Scores\":[\"invalid\"]}]"),
		TEXT("[{\"Name\":\"Bad\",\"Count\":1,\"Label\":\"x\",\"Enabled\":true,\"Scores\":[2.5]}]"),
		TEXT("[{\"Name\":\"Bad\",\"Count\":{\"wrong\":2},\"Label\":\"x\",\"Enabled\":true,\"Scores\":[]}]"),
		TEXT("[{\"Name\":\"Bad\",\"Count\":1,\"Label\":{},\"Enabled\":true,\"Scores\":[]}]"),
		TEXT("[{\"Name\":\"Bad\",\"Count\":1,\"Label\":\"x\",\"Enabled\":[],\"Scores\":[]}]"),
		TEXT("[{\"Name\":\"Bad\",\"Count\":1,\"Label\":\"x\",\"Enabled\":true,\"Scores\":[{}]}]"),
		TEXT("[{\"Name\":\"Bad\",\"Count\":1,\"Enabled\":true,\"Scores\":[]}]"),
		TEXT("[{\"Name\":\"Good\",\"Count\":1,\"Label\":\"x\",\"Enabled\":true,\"Scores\":[]},42]"),
		TEXT("[{\"Name\":\"Good\",\"Count\":1,\"Label\":\"x\",\"Enabled\":true,\"Scores\":[]},{\"Name\":\"LateBad\",\"Count\":{},\"Label\":\"x\",\"Enabled\":true,\"Scores\":[]}]"),
		TEXT("[{\"Name\":\"Duplicate\",\"Count\":1,\"Label\":\"x\",\"Enabled\":true,\"Scores\":[]},{\"Name\":\"duplicate\",\"Count\":2,\"Label\":\"y\",\"Enabled\":false,\"Scores\":[]}]")
	};
	// Both UDS display aliases and exact generated native property names use the same gate.
	FProperty* CountProperty = nullptr;
	for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
		if (DataTableUtils::GetPropertyExportName(*It) == TEXT("Count")) CountProperty = *It;
	if (!TestNotNull(TEXT("native Count field is resolved through its export alias"), CountProperty)) return false;
	Invalid.Add(FString::Printf(TEXT("[{\"Name\":\"BadAlias\",\"%s\":\"invalid\",\"Label\":\"x\",\"Enabled\":true,\"Scores\":[]}]"), *CountProperty->GetName()));
	for (bool WasDirty : { false, true })
	{
		Package->SetDirtyFlag(WasDirty);
		for (const FString& Bad : Invalid)
		{
			const int32 ChangesBefore = Changes;
			const FGuid UndoBefore = GEditor->Trans->GetUndoContext().TransactionId;
			const auto Result = Call(Bad, false);
			TestEqual(TEXT("rejected import preserves exact exported contents"), Table->GetTableAsJSON(), BeforeJson);
			TestEqual(TEXT("rejected import preserves target row count"), Table->GetRowMap().Num(), BeforeRows.Num());
			for (const auto& Row : BeforeRows)
			{
				uint8* const* Current = Table->GetRowMap().Find(Row.Key);
				TestTrue(TEXT("rejected import preserves existing row storage"), Current && *Current == Row.Value);
			}
			TestTrue(TEXT("rejected import retains exact row struct"), Table->GetRowStruct() == RowStruct);
			TestEqual(TEXT("rejected import preserves prior dirty state"), Package->IsDirty(), WasDirty);
			TestEqual(TEXT("rejected import does not notify target changes"), Changes, ChangesBefore);
			TestEqual(TEXT("rejected import leaves undo history unchanged"), GEditor->Trans->GetUndoContext().TransactionId, UndoBefore);
			if (Result)
			{
				bool ReportedBefore = !WasDirty, ReportedAfter = !WasDirty;
				TestTrue(TEXT("rejection reports dirtyBefore"), Result->TryGetBoolField(TEXT("dirtyBefore"), ReportedBefore));
				TestTrue(TEXT("rejection reports dirtyAfter"), Result->TryGetBoolField(TEXT("dirtyAfter"), ReportedAfter));
				TestEqual(TEXT("dirtyBefore is observed"), ReportedBefore, WasDirty);
				TestEqual(TEXT("dirtyAfter is observed"), ReportedAfter, WasDirty);
			}
		}
	}

	Package->SetDirtyFlag(false);
	const int32 ChangesBefore = Changes;
	const FGuid UndoBefore = GEditor->Trans->GetUndoContext().TransactionId;
	Call(TEXT("[{\"Name\":\"Replacement\",\"Count\":29,\"Label\":\"new value\",\"Enabled\":false,\"Scores\":[8,9]}]"), true);
	TestEqual(TEXT("valid import replaces rather than appends"), Table->GetRowMap().Num(), 1);
	TestFalse(TEXT("old Row_A removed only after successful validation"), Table->GetRowMap().Contains(TEXT("Row_A")));
	TestTrue(TEXT("replacement row exists"), Table->GetRowMap().Contains(TEXT("Replacement")));
	TestTrue(TEXT("valid import marks target dirty"), Package->IsDirty());
	TestEqual(TEXT("valid replacement notifies once after copying all rows"), Changes, ChangesBefore + 1);
	const FString ReplacedJson = Table->GetTableAsJSON();
	TestTrue(TEXT("replacement retains imported scalar and array contents"), ReplacedJson.Contains(TEXT("new value")) && ReplacedJson.Contains(TEXT("29")) && ReplacedJson.Contains(TEXT("9")));
	const FTransactionContext Imported = GEditor->Trans->GetUndoContext();
	if (TestTrue(TEXT("valid import owns one new undo transaction"), Imported.TransactionId.IsValid() && Imported.TransactionId != UndoBefore
		&& Imported.Title.EqualTo(NSLOCTEXT("Nwiro", "ImportDataTableJson", "AI: Import DataTable JSON"))))
	{
		if (TestTrue(TEXT("replacement can be undone"), GEditor->UndoTransaction()))
		{
			TestEqual(TEXT("undo restores all old rows and values"), Table->GetTableAsJSON(), BeforeJson);
			if (TestTrue(TEXT("redo still belongs to this import"), GEditor->Trans->GetRedoContext().TransactionId == Imported.TransactionId)
				&& TestTrue(TEXT("replacement can be redone"), GEditor->RedoTransaction()))
				TestEqual(TEXT("redo restores validated replacement"), Table->GetTableAsJSON(), ReplacedJson);
		}
	}

	// Existing explicit import options still govern accepted column/key shapes.
	Table->ImportKeyField = TEXT("RowKey");
	Table->bIgnoreExtraFields = true;
	Call(TEXT("[{\"RowKey\":\"CustomKey\",\"Count\":5,\"Label\":\"options kept\",\"Enabled\":true,\"Scores\":[],\"IgnoredColumn\":3}]"), true);
	TestTrue(TEXT("custom key is honored"), Table->GetRowMap().Contains(TEXT("CustomKey")));
	TestTrue(TEXT("target extra-field option is preserved"), Table->bIgnoreExtraFields);
	TestEqual(TEXT("target import key option is preserved"), Table->ImportKeyField, FString(TEXT("RowKey")));
	Call(FString::Printf(TEXT("[{\"RowKey\":\"NativeAlias\",\"%s\":2147483647,\"Label\":\"native name\",\"Enabled\":true,\"Scores\":[]},{\"RowKey\":\"LowerBound\",\"Count\":-2147483648,\"Label\":\"friendly name\",\"Enabled\":false,\"Scores\":[]}]"), *CountProperty->GetName()), true);
	const FNumericProperty* CountNumber = CastField<FNumericProperty>(CountProperty);
	uint8* UpperRow = Table->FindRowUnchecked(TEXT("NativeAlias"));
	uint8* LowerRow = Table->FindRowUnchecked(TEXT("LowerBound"));
	if (TestNotNull(TEXT("native integer field exists"), CountNumber) && TestNotNull(TEXT("native-name row imported"), UpperRow) && TestNotNull(TEXT("display-name row imported"), LowerRow))
	{
		TestEqual(TEXT("upper int32 boundary preserved through native UDS name"), CountNumber->GetSignedIntPropertyValue(CountNumber->ContainerPtrToValuePtr<void>(UpperRow)), static_cast<int64>(2147483647));
		TestEqual(TEXT("lower int32 boundary preserved through display UDS name"), CountNumber->GetSignedIntPropertyValue(CountNumber->ContainerPtrToValuePtr<void>(LowerRow)), static_cast<int64>(-2147483647) - 1);
	}
	TestEqual(TEXT("no import or validation attempts a save"), Saves, 0);
	Fixture.AssertNotSaved(TableName);
	Fixture.AssertNotSaved(StructName);
	return true;
}
