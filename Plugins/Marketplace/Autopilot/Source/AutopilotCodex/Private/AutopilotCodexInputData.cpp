// The AutopilotCodex commandlet's input assets (input/*.txt: input actions and mapping contexts) and data (data/*.txt:
// user-defined enums and structs, curves and data assets; Main writes data tables and curve tables itself).

#include "AutopilotCodexInternal.h"

#include "Engine/UserDefinedEnum.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Misc/EngineVersionComparison.h"
#if UE_VERSION_OLDER_THAN(5, 5, 0)
#include "Engine/UserDefinedStruct.h"
#else
#include "StructUtils/UserDefinedStruct.h"
#endif
#include "UObject/UnrealType.h"

namespace AutopilotCodex
{
	// ---------------------------------------------------------------- input and data

	FString InstancedList(const TArray<UObject*>& Objects, const TCHAR* StripPrefix)
	{
		TArray<FString> Items;
		for (const UObject* O : Objects)
		{
			if (!O)
			{
				continue;
			}
			FString Name = O->GetClass()->GetName();
			Name.RemoveFromStart(StripPrefix);
			TArray<FString> Props;
			FDiffOptions Opt;
			Opt.Max = 6;
			Opt.MaxLen = 80;
			DiffProps(O, O->GetClass()->GetDefaultObject(), Props, Opt);
			Items.Add(Props.Num() ? Name + TEXT("(") + JoinProps(Props) + TEXT(")") : Name);
		}
		return FString::Join(Items, TEXT(", "));
	}

	template <typename T>
	TArray<UObject*> ToObjects(const TArray<TObjectPtr<T>>& In)
	{
		TArray<UObject*> R;
		for (const TObjectPtr<T>& O : In)
		{
			R.Add(const_cast<UObject*>(static_cast<const UObject*>(O.Get())));
		}
		return R;
	}

	FString ExportInputAction(UInputAction* IA, const FString& Path)
	{
		FOut Out;
		Out.Add(0, TEXT("InputAction ") + Path);
		Out.Add(1, TEXT("ValueType: ") + EnumValueName(StaticEnum<EInputActionValueType>(), (int64)IA->ValueType));
		if (IA->Triggers.Num() > 0)
		{
			Out.Add(1, TEXT("Triggers: ") + InstancedList(ToObjects(IA->Triggers), TEXT("InputTrigger")));
		}
		if (IA->Modifiers.Num() > 0)
		{
			Out.Add(1, TEXT("Modifiers: ") + InstancedList(ToObjects(IA->Modifiers), TEXT("InputModifier")));
		}
		TArray<FString> Props;
		FDiffOptions O;
		O.Max = 20;
		static const TSet<FName> Skip = { TEXT("ValueType") };
		O.Skip = &Skip;
		DiffProps(IA, IA->GetClass()->GetDefaultObject(), Props, O);
		for (const FString& P : Props)
		{
			Out.Add(1, P);
		}
		return Out.Join();
	}

	FString ExportMappingContext(UInputMappingContext* IMC, const FString& Path)
	{
		FOut Out;
		Out.Add(0, TEXT("InputMappingContext ") + Path);
		TArray<FString> Lines;
		for (const FEnhancedActionKeyMapping& M : IMC->GetMappings())
		{
			FString Line = M.Key.GetFName().ToString() + TEXT(" -> ") + (M.Action ? M.Action->GetName() : FString(TEXT("(none)")));
			if (M.Triggers.Num() > 0)
			{
				Line += TEXT("  triggers: ") + InstancedList(ToObjects(M.Triggers), TEXT("InputTrigger"));
			}
			if (M.Modifiers.Num() > 0)
			{
				Line += TEXT("  modifiers: ") + InstancedList(ToObjects(M.Modifiers), TEXT("InputModifier"));
			}
			Lines.Add(Line);
		}
		for (const FString& L : Lines)
		{
			Out.Add(1, L);
		}
		TArray<FString> Props;
		FDiffOptions O;
		O.Max = 10;
		static const TSet<FName> Skip = { TEXT("Mappings"), TEXT("DefaultKeyMappings"), TEXT("MappingProfileOverrides") };
		O.Skip = &Skip;
		DiffProps(IMC, IMC->GetClass()->GetDefaultObject(), Props, O);
		for (const FString& P : Props)
		{
			Out.Add(1, P);
		}
		return Out.Join();
	}

	FString ExportUserEnum(UUserDefinedEnum* E, const FString& Path)
	{
		FOut Out;
		Out.Add(0, TEXT("Enum ") + Path);
		const int32 N = E->NumEnums() - (E->ContainsExistingMax() ? 1 : 0);
		for (int32 i = 0; i < N; ++i)
		{
			Out.Add(1, FString::Printf(TEXT("%lld: %s = \"%s\""), E->GetValueByIndex(i), *E->GetNameStringByIndex(i), *E->GetDisplayNameTextByIndex(i).ToString()));
		}
		return Out.Join();
	}

	FString ExportUserStruct(UUserDefinedStruct* S, const FString& Path)
	{
		FOut Out;
		Out.Add(0, TEXT("Struct ") + Path);
		const uint8* Def = S->GetDefaultInstance();
		for (TFieldIterator<FProperty> It(S); It; ++It)
		{
			FString Line = It->GetAuthoredName() + TEXT(": ") + PropTypeStr(*It);
			if (Def)
			{
				const FString V = ExportValue(*It, It->ContainerPtrToValuePtr<void>(Def));
				if (!V.IsEmpty() && V != TEXT("None") && V != TEXT("\"\""))
				{
					Line += TEXT(" = ") + V;
				}
			}
			const FString Tip = OneLine(It->GetToolTipText().ToString());
			if (!Tip.IsEmpty() && Tip != It->GetAuthoredName())
			{
				Line += TEXT("  // ") + Cap(Tip, 120);
			}
			Out.Add(1, Line);
		}
		return Out.Join();
	}

	FString ExportGenericObject(UObject* O, const FString& Kind, const FString& Path)
	{
		FOut Out;
		Out.Add(0, Kind + TEXT(" ") + Path + TEXT("  (class ") + O->GetClass()->GetName() + TEXT(")"));
		TArray<FString> Props;
		FDiffOptions Opt;
		Opt.Max = 200;
		Opt.MaxLen = 2000;
		Opt.bExpandStructs = false;
		Opt.bRequireEdit = true;  // what a designer set; compiled runtime data (e.g. state trees) is not readable
		DiffProps(O, O->GetClass()->GetDefaultObject(), Props, Opt);
		for (const FString& P : Props)
		{
			Out.Add(1, P);
		}
		return Out.Join();
	}
}
