// Copyright 2026 Nwiro. All Rights Reserved.
#pragma once

#include "UObject/StrongObjectPtr.h"
#include "Misc/ScopeExit.h"
#include "UObject/ObjectSaveContext.h"
#include "Editor/EditorEngine.h"

// TU-private state, scoped to one synchronous game-thread EditBlueprint invocation.
// Only staging owns a duplicate; no target replacement, editor undo, or disk rollback.
namespace NwiroBlueprintBatch
{
    struct FContext
    {
        bool bStaging = true;
        int32 RequestedUnits = 0;
        bool bRequestCountComplete = false;
        int32 AttemptedCommitUnits = 0;
        int32 InternalCompileAttempts = 0;
        int32 TargetCompileAttempts = 0;
        int32 TargetSaveAttempts = 0;
        int32 StagingCompileAttempts = 0;
        int32 CreationCompileAttempts = 0;
        bool bCreatingTarget = false;
        FString ExpectedTargetPath;
        FString ExpectedStagePath;
        TArray<TWeakObjectPtr<UBlueprint>> PendingLoadCompiles;
        TArray<TWeakObjectPtr<UPackage>> PendingLoadSaves;
        void BindTarget(UBlueprint* BP)
        {
            Target = BP;
            if (!BP) return;
            for (const auto& Item : PendingLoadCompiles) if (Item.Get() == BP) ++TargetCompileAttempts;
            for (const auto& Item : PendingLoadSaves) if (Item.Get() == BP->GetOutermost()) ++TargetSaveAttempts;
            PendingLoadCompiles.Reset(); PendingLoadSaves.Reset();
        }
        TWeakObjectPtr<UBlueprint> Target;
        TArray<FString> Errors;
        TSet<FGuid> ExistingNodeGuids;
    };
    static FContext* Active = nullptr;
#if WITH_DEV_AUTOMATION_TESTS
    // Actual-dispatch fault injection only; no production console variable or wire input.
    static TFunction<void(UBlueprint*, const FString&)> BeforeCommitOperation;
    static TFunction<void(UEdGraphNode*, const FGuid&)> AfterDefaultSetter;
    static TFunction<void(UEdGraphNode*, const FString&)> BeforeGeneratedPinLookup;
    static TFunction<void(UEdGraphNode*)> AfterWholeNodeBreak;
    static TFunction<void(UEdGraphNode*, const FGuid&, UEdGraphNode*, const FGuid&)> AfterConnectionSchema;
#endif
    struct FObserveTarget
    {
        FDelegateHandle CompileHandle, SaveHandle;
        explicit FObserveTarget(FContext& Context)
        {
            if (GEditor) CompileHandle = GEditor->OnBlueprintPreCompile().AddLambda([&Context](UBlueprint* BP)
            {
                if (!BP) return;
                if (!Context.ExpectedStagePath.IsEmpty() && BP->GetPathName().Equals(Context.ExpectedStagePath, ESearchCase::CaseSensitive))
                    ++Context.StagingCompileAttempts;
                if (Context.bCreatingTarget && BP->GetPathName().Equals(Context.ExpectedTargetPath, ESearchCase::CaseSensitive))
                { Context.Target = BP; ++Context.CreationCompileAttempts; }
                if (Context.Target.IsValid() && Context.Target.Get() == BP) ++Context.TargetCompileAttempts;
                else if (!Context.Target.IsValid()) Context.PendingLoadCompiles.Add(BP);
            });
            SaveHandle = UPackage::PreSavePackageWithContextEvent.AddLambda([&Context](UPackage* Package, FObjectPreSaveContext)
            {
                if (Context.Target.IsValid() && Context.Target->GetOutermost() == Package) ++Context.TargetSaveAttempts;
                else if (!Context.Target.IsValid()) Context.PendingLoadSaves.Add(Package);
            });
        }
        ~FObserveTarget()
        {
            if (GEditor) GEditor->OnBlueprintPreCompile().Remove(CompileHandle);
            UPackage::PreSavePackageWithContextEvent.Remove(SaveHandle);
        }
    };


    static void Reject(const FString& Error)
    {
        if (Active) Active->Errors.AddUnique(Error);
    }
    static void RecordErrors(const TArray<FString>& Errors)
    {
        for (const FString& Error : Errors) Reject(Error);
    }
    static bool StopCommit(const TArray<FString>& Errors)
    {
        if (!Active || Active->bStaging) return false;
        RecordErrors(Errors);
        return Active->Errors.Num() != 0;
    }
    static const TArray<FString>& OperationKeys()
    {
        static const TArray<FString> Keys = {
            TEXT("add_variables"), TEXT("remove_variables"), TEXT("rename_variables"),
            TEXT("add_components"), TEXT("remove_components"), TEXT("set_component_properties"),
            TEXT("add_functions"), TEXT("remove_functions"), TEXT("add_custom_events"),
            TEXT("add_event_dispatchers"), TEXT("add_interfaces"), TEXT("remove_interfaces"),
            TEXT("add_nodes"), TEXT("remove_nodes"), TEXT("connect_pins"),
            TEXT("break_connections"), TEXT("set_pin_defaults")
        };
        return Keys;
    }
    static bool NeedsAdmission(const TSharedPtr<FJsonObject>& Cmd)
    {
        return Cmd->HasField(TEXT("add_nodes")) || Cmd->HasField(TEXT("connect_pins"))
            || Cmd->HasField(TEXT("break_connections")) || Cmd->HasField(TEXT("set_pin_defaults"));
    }
    static FString Json(const TSharedRef<FJsonObject>& Object)
    {
        FString Out;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
        FJsonSerializer::Serialize(Object, Writer);
        return Out;
    }
    static FString AdmissionFailure(const TArray<FString>& Errors, int32 RequestedUnits)
    {
        const bool IsolationFailed = Active && (Active->TargetCompileAttempts || Active->TargetSaveAttempts);
        const bool CountKnown = Active && Active->bRequestCountComplete;
        const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("success"), false);
        Result->SetStringField(TEXT("errorCode"), TEXT("BLUEPRINT_BATCH_NOT_ADMITTED"));
        Result->SetStringField(TEXT("error"), FString::Join(Errors, TEXT("; ")));
        Result->SetBoolField(TEXT("compiled"), Active && Active->TargetCompileAttempts > 0);
        Result->SetBoolField(TEXT("saveAttempted"), Active && Active->TargetSaveAttempts > 0);
        if (IsolationFailed) Result->SetField(TEXT("savedToDisk"), MakeShared<FJsonValueNull>());
        else Result->SetBoolField(TEXT("savedToDisk"), false);
        const TSharedRef<FJsonObject> Batch = MakeShared<FJsonObject>();
        Batch->SetStringField(TEXT("phase"), IsolationFailed ? TEXT("admission_isolation_failed") : TEXT("rejected_before_target_mutation"));
        if (CountKnown) Batch->SetNumberField(TEXT("requestedUnits"), RequestedUnits);
        else Batch->SetField(TEXT("requestedUnits"), MakeShared<FJsonValueNull>());
        Batch->SetBoolField(TEXT("countsComplete"), CountKnown && !IsolationFailed);
        if (IsolationFailed) Batch->SetField(TEXT("appliedUnits"), MakeShared<FJsonValueNull>());
        else Batch->SetNumberField(TEXT("appliedUnits"), 0);
        if (IsolationFailed || !CountKnown) Batch->SetField(TEXT("rejectedUnits"), MakeShared<FJsonValueNull>());
        else Batch->SetNumberField(TEXT("rejectedUnits"), RequestedUnits);
        if (IsolationFailed || !CountKnown) Batch->SetField(TEXT("skippedUnits"), MakeShared<FJsonValueNull>());
        else Batch->SetNumberField(TEXT("skippedUnits"), 0);
        Batch->SetNumberField(TEXT("stagingCompileAttempts"), Active ? Active->StagingCompileAttempts : 0);
        Batch->SetNumberField(TEXT("targetCompileAttempts"), Active ? Active->TargetCompileAttempts : 0);
        Batch->SetNumberField(TEXT("targetSaveAttempts"), Active ? Active->TargetSaveAttempts : 0);
        Batch->SetStringField(TEXT("currentRequestMutation"), Active && (Active->TargetCompileAttempts || Active->TargetSaveAttempts) ? TEXT("unknown_isolation_failure") : TEXT("none"));
        Result->SetObjectField(TEXT("batchAdmission"), Batch);
        return Json(Result);
    }
    static FString PlannedNodeRef(UEdGraphNode* Node, const FString& Requested)
    {
        return Active && Active->ExistingNodeGuids.Contains(Node->NodeGuid)
            ? Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : Requested;
    }
    static void RecordInternalCompile()
    {
        if (Active && !Active->bStaging) ++Active->InternalCompileAttempts;
    }

    // Normal UE duplication owns independent generated/skeleton classes and derived
    // Blueprint subobjects. PIE duplication or assigning the source generated class
    // would leave staging mutations able to reach authored target state.
    static UBlueprint* DuplicateForValidation(UBlueprint* Source)
    {
        FBlueprintDuplicationScopeFlags Flags(
            FBlueprintDuplicationScopeFlags::NoExtraCompilation |
            FBlueprintDuplicationScopeFlags::TheSameNodeGuid |
            FBlueprintDuplicationScopeFlags::TheSameTimelineGuid);
        FObjectDuplicationParameters Params(Source, GetTransientPackage());
        Params.DestName = MakeUniqueObjectName(GetTransientPackage(), Source->GetClass(), TEXT("NwiroBatchStage"));
        if (Active) Active->ExpectedStagePath = GetTransientPackage()->GetPathName() + TEXT(".") + Params.DestName.ToString();
        Params.ApplyFlags = RF_Transient;
        Params.FlagMask = RF_AllFlags & ~(RF_Public | RF_Standalone | RF_Transactional);
        UBlueprint* Stage = Cast<UBlueprint>(StaticDuplicateObjectEx(Params));
        if (!Stage || Stage == Source || Stage->GetOutermost() != GetTransientPackage()
            || !Stage->GeneratedClass || !Stage->SkeletonGeneratedClass
            || Stage->GeneratedClass == Source->GeneratedClass
            || Stage->SkeletonGeneratedClass == Source->SkeletonGeneratedClass
            || Stage->GeneratedClass->ClassGeneratedBy != Stage
            || Stage->SkeletonGeneratedClass->ClassGeneratedBy != Stage)
        {
            Reject(TEXT("Staging Blueprint does not own independent generated classes"));
            return nullptr;
        }
        return Stage;
    }

    // K2 may insert a conversion node. Check the actual surviving route using only
    // direct links and nodes introduced by this schema call; unrelated old graph
    // paths cannot turn a removed requested edge into an as-requested success.
    static bool Connect(UEdGraphPin* A, UEdGraphPin* B, bool bRetryReverse, FString& Error)
    {
        if (!A || !B) { Error = TEXT("Missing connection endpoint"); return false; }
        TStrongObjectPtr<UEdGraphNode> KeepA(A->GetOwningNode()), KeepB(B->GetOwningNode());
        TStrongObjectPtr<UEdGraph> Graph(KeepA->GetGraph());
        if (!Graph.IsValid() || KeepB->GetGraph() != Graph.Get())
        { Error = TEXT("Connection endpoints must belong to the same graph"); return false; }
        const FGuid AId = A->PinId, BId = B->PinId;
        const FString AName = A->PinName.ToString(), BName = B->PinName.ToString();
        const EEdGraphPinDirection ADir = A->Direction, BDir = B->Direction;
        TSet<UEdGraphNode*> Existing;
        for (UEdGraphNode* Node : Graph->Nodes) Existing.Add(Node);
        auto Reacquire = [&]()
        {
            A = IsValid(KeepA.Get()) ? KeepA->FindPinById(AId) : nullptr;
            B = IsValid(KeepB.Get()) ? KeepB->FindPinById(BId) : nullptr;
            return A && B && Graph->Nodes.Contains(KeepA.Get()) && Graph->Nodes.Contains(KeepB.Get())
                && A->Direction == ADir && B->Direction == BDir
                && A->PinName.ToString().Equals(AName, ESearchCase::CaseSensitive)
                && B->PinName.ToString().Equals(BName, ESearchCase::CaseSensitive);
        };
        const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
        bool Connected = Schema->TryCreateConnection(A, B);
#if WITH_DEV_AUTOMATION_TESTS
        if (AfterConnectionSchema) AfterConnectionSchema(KeepA.Get(), AId, KeepB.Get(), BId);
#endif
        if (!Reacquire()) { Error = TEXT("Connection callback changed endpoint identity; commit may be partial"); return false; }
        if (!Connected && bRetryReverse)
        {
            Connected = Schema->TryCreateConnection(B, A);
#if WITH_DEV_AUTOMATION_TESTS
            if (AfterConnectionSchema) AfterConnectionSchema(KeepA.Get(), AId, KeepB.Get(), BId);
#endif
            if (!Reacquire()) { Error = TEXT("Connection callback changed endpoint identity; commit may be partial"); return false; }
        }
        if (!Connected) { Error = TEXT("Schema refused the connection"); return false; }
        UEdGraphPin* Start = ADir == EGPD_Output ? A : B;
        UEdGraphPin* End = ADir == EGPD_Output ? B : A;
        TArray<UEdGraphPin*> Pending { Start };
        TSet<UEdGraphPin*> Visited;
        bool Found = false;
        while (Pending.Num() && !Found)
        {
            UEdGraphPin* Current = Pending.Pop(EAllowShrinking::No);
            if (!Current || Visited.Contains(Current)) continue;
            Visited.Add(Current);
            for (UEdGraphPin* Peer : Current->LinkedTo)
            {
                if (Peer == End) { Found = true; break; }
                UEdGraphNode* Via = Peer ? Peer->GetOwningNode() : nullptr;
                if (!Via || Existing.Contains(Via) || !Graph->Nodes.Contains(Via)) continue;
                for (UEdGraphPin* Output : Via->Pins)
                    if (Output && Output->Direction == EGPD_Output) Pending.Add(Output);
            }
        }
        if (!Found) Error = TEXT("Connection callback did not preserve the requested direct/conversion route; commit may be partial");
        return Found;
    }

    // Validate the complete schema tuple before any setter, then read back through
    // stable node/pin identity: callbacks may reconstruct the node and invalidate Pin.
    template <typename FClassResolver>
    static bool SetDefault(UEdGraphPin* Pin, const FString& Requested,
        FClassResolver ResolveClass, FString& OutCanonical, FString& Error)
    {
        if (!Pin || Pin->Direction != EGPD_Input || Pin->bDefaultValueIsReadOnly
            || Pin->bDefaultValueIsIgnored || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            Error = TEXT("Pin has no writable, valid input default");
            return false;
        }
        TStrongObjectPtr<UEdGraphNode> KeepNode(Pin->GetOwningNode());
        TWeakObjectPtr<UEdGraphNode> Node(KeepNode.Get());
        TStrongObjectPtr<UEdGraph> KeepGraph(KeepNode->GetGraph());
        const FGuid PinId = Pin->PinId;
        const FEdGraphPinType IntendedType = Pin->PinType;
        const FString IntendedName = Pin->PinName.ToString();
        TStrongObjectPtr<UObject> KeepSubtype(IntendedType.PinSubCategoryObject.Get());
        if (!FBlueprintEditorUtils::IsPinTypeValid(IntendedType))
        { Error = TEXT("Pin has an unresolved or invalid type"); return false; }
        const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
        FString Value = Requested;
        const FName Category = IntendedType.PinCategory;
        const bool bNull = Value.IsEmpty() || Value.Equals(TEXT("None"), ESearchCase::IgnoreCase);
        UObject* Resolved = nullptr;
        const bool bClass = Category == UEdGraphSchema_K2::PC_Class;
        const bool bObject = Category == UEdGraphSchema_K2::PC_Object
            || Category == UEdGraphSchema_K2::PC_Interface;
        const bool bSoft = Category == UEdGraphSchema_K2::PC_SoftObject
            || Category == UEdGraphSchema_K2::PC_SoftClass;
        if ((bClass || bObject || bSoft) && !bNull)
        {
            Resolved = bClass || Category == UEdGraphSchema_K2::PC_SoftClass
                ? static_cast<UObject*>(ResolveClass(Value)) : LoadObject<UObject>(nullptr, *Value);
            if (!Resolved)
            {
                Error = FString::Printf(TEXT("Unresolved default object/class '%s'"), *Value);
                return false; // never fall back to a string/default/null for a requested object
            }
            Value = Resolved->GetPathName();
        }
        const UScriptStruct* ValueStruct = Category == UEdGraphSchema_K2::PC_Struct
            ? Cast<UScriptStruct>(KeepSubtype.Get()) : nullptr;
        const bool bK2MathLiteral = ValueStruct == TBaseStructure<FVector>::Get()
            || ValueStruct == TBaseStructure<FRotator>::Get() || ValueStruct == TBaseStructure<FLinearColor>::Get();
        if (Category == UEdGraphSchema_K2::PC_Struct && !bK2MathLiteral && !Value.StartsWith(TEXT("(")))
        {
            const UScriptStruct* Struct = Cast<UScriptStruct>(IntendedType.PinSubCategoryObject.Get());
            if (!Struct) { Error = TEXT("Unresolved struct subtype"); return false; }
            if (Value.Contains(TEXT("="))) Value = TEXT("(") + Value + TEXT(")");
            else if (Value.Contains(TEXT(",")))
            {
                TArray<FString> Parts, Pairs;
                Value.ParseIntoArray(Parts, TEXT(","), false);
                int32 Index = 0;
                for (TFieldIterator<FProperty> It(Struct); It; ++It, ++Index)
                {
                    if (!Parts.IsValidIndex(Index)) { Error = TEXT("Incomplete struct default"); return false; }
                    Pairs.Add(It->GetName() + TEXT("=") + Parts[Index].TrimStartAndEnd());
                }
                if (Index != Parts.Num()) { Error = TEXT("Extra struct default fields"); return false; }
                Value = TEXT("(") + FString::Join(Pairs, TEXT(",")) + TEXT(")");
            }
        }
        FString DefaultValue;
        TObjectPtr<UObject> DefaultObject = nullptr;
        FText DefaultText;
        TStrongObjectPtr<UObject> KeepResolved(Resolved);
        Schema->GetPinDefaultValuesFromString(IntendedType, KeepNode.Get(), Value,
            DefaultValue, DefaultObject, DefaultText, false);
        if (bClass || bObject) { DefaultValue.Empty(); DefaultObject = Resolved; DefaultText = FText::GetEmpty(); }
        // Asset/class resolution above may compile a Blueprint and reconstruct pins.
        // Never dereference the old raw Pin after a load-capable call.
        Pin = Node.IsValid() ? Node->FindPinById(PinId) : nullptr;
        if (!Pin || !KeepGraph.IsValid() || !KeepGraph->Nodes.Contains(KeepNode.Get()) || !(Pin->PinType == IntendedType)
            || !Pin->PinName.ToString().Equals(IntendedName, ESearchCase::CaseSensitive)
            || Pin->Direction != EGPD_Input || Pin->bDefaultValueIsReadOnly || Pin->bDefaultValueIsIgnored)
        {
            Error = TEXT("Pin identity/type changed while resolving its default");
            return false;
        }
        Error = Schema->IsPinDefaultValid(Pin, DefaultValue, DefaultObject, DefaultText);
        if (!Error.IsEmpty()) return false;
        TStrongObjectPtr<UObject> KeepDefault(DefaultObject.Get());
        if (bClass || bObject) Schema->TrySetDefaultObject(*Pin, DefaultObject.Get());
        else Schema->TrySetDefaultValue(*Pin, Value);
#if WITH_DEV_AUTOMATION_TESTS
        if (AfterDefaultSetter && Node.IsValid()) AfterDefaultSetter(Node.Get(), PinId);
#endif
        UEdGraphPin* Current = Node.IsValid() ? Node->FindPinById(PinId) : nullptr;
        if (!Current || !KeepGraph.IsValid() || !KeepGraph->Nodes.Contains(KeepNode.Get())
            || !(Current->PinType == IntendedType)
            || !Current->PinName.ToString().Equals(IntendedName, ESearchCase::CaseSensitive)
            || Current->Direction != EGPD_Input || Current->bDefaultValueIsReadOnly || Current->bDefaultValueIsIgnored
            || !Current->DefaultValue.Equals(DefaultValue, ESearchCase::CaseSensitive)
            || Current->DefaultObject != DefaultObject || !Current->DefaultTextValue.EqualTo(DefaultText)
            || !Current->DefaultTextValue.ToString().Equals(DefaultText.ToString(), ESearchCase::CaseSensitive))
        {
            Error = TEXT("Default setter did not preserve the validated pin identity/value; commit may be partial");
            return false;
        }
        OutCanonical = Value;
        return true;
    }
}
