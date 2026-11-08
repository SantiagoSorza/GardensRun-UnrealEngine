// The AutopilotCodex commandlet's text helpers: how names, types and property values are written out, and the
// properties that differ from a class's defaults (DiffProps).

#include "AutopilotCodexInternal.h"

#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/UserDefinedEnum.h"
#include "Internationalization/Regex.h"
#include "Misc/EngineVersionComparison.h"
#if UE_VERSION_OLDER_THAN(5, 5, 0)
#include "Engine/UserDefinedStruct.h"
#else
#include "StructUtils/UserDefinedStruct.h"
#endif
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"

namespace AutopilotCodex
{
	// ---------------------------------------------------------------- text helpers

	FString Cap(const FString& S, int32 Max)
	{
		if (S.Len() <= Max)
		{
			return S;
		}
		return S.Left(Max) + TEXT("...");
	}

	FString OneLine(const FString& S)
	{
		FString R = S;
		R.ReplaceInline(TEXT("\r\n"), TEXT(" "));
		R.ReplaceInline(TEXT("\n"), TEXT(" "));
		R.ReplaceInline(TEXT("\r"), TEXT(" "));
		R.ReplaceInline(TEXT("\t"), TEXT(" "));
		return R.TrimStartAndEnd();
	}

	/** 180.000000 -> 180, 0.500000 -> 0.5 */
	FString TrimFloats(const FString& S)
	{
		FString R;
		R.Reserve(S.Len());
		const int32 N = S.Len();
		int32 i = 0;
		while (i < N)
		{
			const TCHAR C = S[i];
			const bool bWordBefore = i > 0 && (FChar::IsAlpha(S[i - 1]) || S[i - 1] == TEXT('_'));
			if (FChar::IsDigit(C) && !bWordBefore)
			{
				int32 j = i;
				while (j < N && FChar::IsDigit(S[j]))
				{
					++j;
				}
				if (j < N && S[j] == TEXT('.') && j + 1 < N && FChar::IsDigit(S[j + 1]))
				{
					int32 k = j + 1;
					while (k < N && FChar::IsDigit(S[k]))
					{
						++k;
					}
					FString Int = S.Mid(i, j - i);
					FString Frac = S.Mid(j + 1, k - j - 1);
					while (Frac.Len() > 0 && Frac[Frac.Len() - 1] == TEXT('0'))
					{
						Frac.LeftChopInline(1);
					}
					R += Int;
					if (!Frac.IsEmpty())
					{
						R += TEXT(".");
						R += Frac;
					}
					i = k;
					continue;
				}
				R += S.Mid(i, j - i);
				i = j;
				continue;
			}
			R.AppendChar(C);
			++i;
		}
		return R;
	}

	FString RegexReplace(const FString& S, const FRegexPattern& Pattern, TFunctionRef<FString(FRegexMatcher&)> Repl)
	{
		FRegexMatcher M(Pattern, S);
		FString R;
		int32 Last = 0;
		while (M.FindNext())
		{
			const int32 B = M.GetMatchBeginning();
			const int32 E = M.GetMatchEnding();
			R += S.Mid(Last, B - Last);
			R += Repl(M);
			Last = E;
		}
		R += S.Mid(Last);
		return R;
	}

	/** Shortens object paths to object names and strips user-defined struct field suffixes. */
	FString Clean(const FString& In, int32 Max)
	{
		if (In.IsEmpty())
		{
			return In;
		}
		static const FRegexPattern UdsSuffix(TEXT("_[0-9]+_[0-9A-F]{32}"));
		static const FRegexPattern ClassPrefixed(TEXT("(?:/Script/[A-Za-z0-9_]+\\.)?[A-Za-z0-9_]*'\"?(/[^'\"]+)\"?'"));
		static const FRegexPattern ObjPath(TEXT("/[A-Za-z][A-Za-z0-9_]*(?:/[A-Za-z0-9_\\-]+)*\\.([A-Za-z0-9_\\-]+)(?:[:.]([A-Za-z0-9_\\-]+))*"));
		FString S = In;
		if (S.Contains(TEXT("_")))
		{
			S = RegexReplace(S, UdsSuffix, [](FRegexMatcher&) { return FString(); });
		}
		if (S.Contains(TEXT("/")))
		{
			S = RegexReplace(S, ClassPrefixed, [](FRegexMatcher& M) { return M.GetCaptureGroup(1); });
			S = RegexReplace(S, ObjPath, [](FRegexMatcher& M)
			{
				const FString A = M.GetCaptureGroup(1);
				const FString B = M.GetCaptureGroup(2);
				return B.IsEmpty() ? A : A + TEXT(":") + B;
			});
		}
		S = TrimFloats(OneLine(S));
		return Cap(S, Max);
	}

	/** /Game/Foo/Bar -> Foo/Bar; /Plugin/Foo -> Plugin/Foo */
	FString RelPath(const FString& PackageName)
	{
		FString P = PackageName;
		P.RemoveFromStart(TEXT("/"));
		P.RemoveFromStart(TEXT("Game/"));
		return P;
	}

	// ---------------------------------------------------------------- types

	FString ObjName(const UObject* O)
	{
		return O ? O->GetName() : FString(TEXT("?"));
	}

	FString TerminalStr(const FName& Cat, const FName& SubCat, const UObject* Obj)
	{
		if (Cat == UEdGraphSchema_K2::PC_Boolean) return TEXT("bool");
		if (Cat == UEdGraphSchema_K2::PC_Byte) return Obj ? ObjName(Obj) : FString(TEXT("byte"));
		if (Cat == UEdGraphSchema_K2::PC_Int) return TEXT("int");
		if (Cat == UEdGraphSchema_K2::PC_Int64) return TEXT("int64");
		if (Cat == UEdGraphSchema_K2::PC_Real || Cat == UEdGraphSchema_K2::PC_Float || Cat == UEdGraphSchema_K2::PC_Double) return TEXT("float");
		if (Cat == UEdGraphSchema_K2::PC_Name) return TEXT("name");
		if (Cat == UEdGraphSchema_K2::PC_String) return TEXT("string");
		if (Cat == UEdGraphSchema_K2::PC_Text) return TEXT("text");
		if (Cat == UEdGraphSchema_K2::PC_Enum) return ObjName(Obj);
		if (Cat == UEdGraphSchema_K2::PC_Struct) return ObjName(Obj);
		if (Cat == UEdGraphSchema_K2::PC_Object) return SubCat == UEdGraphSchema_K2::PSC_Self ? FString(TEXT("self object")) : ObjName(Obj) + TEXT(" object");
		if (Cat == UEdGraphSchema_K2::PC_Class) return TEXT("class of ") + (SubCat == UEdGraphSchema_K2::PSC_Self ? FString(TEXT("self")) : ObjName(Obj));
		if (Cat == UEdGraphSchema_K2::PC_SoftObject) return TEXT("soft ") + ObjName(Obj) + TEXT(" object");
		if (Cat == UEdGraphSchema_K2::PC_SoftClass) return TEXT("soft class of ") + ObjName(Obj);
		if (Cat == UEdGraphSchema_K2::PC_Interface) return ObjName(Obj) + TEXT(" interface");
		if (Cat == UEdGraphSchema_K2::PC_Delegate) return TEXT("delegate");
		if (Cat == UEdGraphSchema_K2::PC_MCDelegate) return TEXT("event dispatcher");
		if (Cat == UEdGraphSchema_K2::PC_Wildcard) return TEXT("wildcard");
		if (Cat == UEdGraphSchema_K2::PC_Exec) return TEXT("exec");
		return Cat.ToString();
	}

	FString PinTypeStr(const FEdGraphPinType& T)
	{
		const FString Base = TerminalStr(T.PinCategory, T.PinSubCategory, T.PinSubCategoryObject.Get());
		if (T.IsArray()) return TEXT("array of ") + Base;
		if (T.IsSet()) return TEXT("set of ") + Base;
		if (T.IsMap())
		{
			return TEXT("map of ") + Base + TEXT(" to ") + TerminalStr(T.PinValueType.TerminalCategory, T.PinValueType.TerminalSubCategory, T.PinValueType.TerminalSubCategoryObject.Get());
		}
		return Base;
	}

	FString PropTypeStr(const FProperty* P)
	{
		FEdGraphPinType T;
		if (GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(P, T))
		{
			return PinTypeStr(T);
		}
		return P->GetCPPType();
	}

	// ---------------------------------------------------------------- values

	FString EnumValueName(const UEnum* E, int64 V)
	{
		const int32 Idx = E->GetIndexByValue(V);
		if (Idx == INDEX_NONE)
		{
			return FString::Printf(TEXT("%lld"), V);
		}
		if (Cast<UUserDefinedEnum>(E))
		{
			return E->GetDisplayNameTextByIndex(Idx).ToString();
		}
		FString Name = E->GetNameStringByIndex(Idx);
		return Name;
	}

	FString EnumNameToDisplay(const UEnum* E, const FString& Name)
	{
		if (!E)
		{
			return Name;
		}
		int32 Idx = E->GetIndexByNameString(Name);
		if (Idx == INDEX_NONE)
		{
			return Name;
		}
		return Cast<UUserDefinedEnum>(E) ? E->GetDisplayNameTextByIndex(Idx).ToString() : E->GetNameStringByIndex(Idx);
	}

	/** How an unset pin of this type reads. */
	FString EmptyValue(const FEdGraphPinType& T)
	{
		const FName Cat = T.PinCategory;
		if (T.IsContainer()) return TEXT("[]");
		if (Cat == UEdGraphSchema_K2::PC_Object || Cat == UEdGraphSchema_K2::PC_Class || Cat == UEdGraphSchema_K2::PC_Interface
			|| Cat == UEdGraphSchema_K2::PC_SoftObject || Cat == UEdGraphSchema_K2::PC_SoftClass) return TEXT("None");
		if (Cat == UEdGraphSchema_K2::PC_Boolean) return TEXT("false");
		if (Cat == UEdGraphSchema_K2::PC_String || Cat == UEdGraphSchema_K2::PC_Text) return TEXT("\"\"");
		if (Cat == UEdGraphSchema_K2::PC_Name) return TEXT("None");
		if (Cat == UEdGraphSchema_K2::PC_Struct) return TEXT("(default ") + TerminalStr(Cat, T.PinSubCategory, T.PinSubCategoryObject.Get()) + TEXT(")");
		return TEXT("0");
	}

	FString ExportValue(const FProperty* P, const void* V, int32 Max)
	{
		if (const FEnumProperty* EP = CastField<FEnumProperty>(P))
		{
			return EnumValueName(EP->GetEnum(), EP->GetUnderlyingProperty()->GetSignedIntPropertyValue(V));
		}
		if (const FByteProperty* BP = CastField<FByteProperty>(P))
		{
			if (BP->Enum)
			{
				return EnumValueName(BP->Enum, BP->GetPropertyValue(V));
			}
		}
		if (const FBoolProperty* B = CastField<FBoolProperty>(P))
		{
			return B->GetPropertyValue(V) ? TEXT("true") : TEXT("false");
		}
		if (const FTextProperty* T = CastField<FTextProperty>(P))
		{
			return TEXT("\"") + Cap(OneLine(T->GetPropertyValue(V).ToString()), Max) + TEXT("\"");
		}
		if (const FStrProperty* SP = CastField<FStrProperty>(P))
		{
			return TEXT("\"") + Cap(OneLine(SP->GetPropertyValue(V)), Max) + TEXT("\"");
		}
		FString S;
		P->ExportTextItem_Direct(S, V, nullptr, nullptr, PPF_None);
		return Clean(S, Max);
	}

	bool IsSimpleProp(const FProperty* P)
	{
		return P->IsA<FNumericProperty>() || P->IsA<FBoolProperty>() || P->IsA<FEnumProperty>() || P->IsA<FStrProperty>()
			|| P->IsA<FNameProperty>() || P->IsA<FTextProperty>();
	}

	const TSet<FName>& NoisyProps()
	{
		static TSet<FName> S = {
			TEXT("UCSSerializationIndex"), TEXT("CreationMethod"), TEXT("bNetAddressable"), TEXT("AttachParent"),
			TEXT("UberGraphFrame"), TEXT("UCSModifiedProperties"), TEXT("BlueprintCreatedComponents"),
			TEXT("InstanceComponents"), TEXT("bAutoRegister"), TEXT("ActorLabel"), TEXT("FolderPath"),
			TEXT("bIsEditorOnlyActor"), TEXT("RootComponent"), TEXT("ReplicatedComponents"),
			TEXT("bHasFinishedSpawning"), TEXT("ActorGuid"), TEXT("ActorInstanceGuid"), TEXT("ContentBundleGuid"),
			TEXT("ExternalDataLayerAsset"), TEXT("bNetLoadOnClient"), TEXT("SpawnCollisionHandlingMethod"),
			TEXT("Slot"), TEXT("bIsVariable"), TEXT("PrimaryComponentTick"), TEXT("PrimaryActorTick"),
			TEXT("bUseSystemDefaultObstacleAreaClass"), TEXT("AreaClassOverride"), TEXT("bHasScriptImplementedPaint"), TEXT("bHasScriptImplementedTick"), TEXT("bCreatedByConstructionScript"), TEXT("bHiddenEdLevel"),
		};
		return S;
	}

	/** Lines "Name=Value" for each property of Obj that differs from Def. */
	int32 DiffProps(const UObject* Obj, const UObject* Def, TArray<FString>& Out, const FDiffOptions& Opt)
	{
		if (!Obj || !Def)
		{
			return 0;
		}
		int32 Count = 0;
		int32 Hidden = 0;
		const UClass* DefClass = Def->GetClass();
		for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
		{
			const FProperty* P = *It;
			if (P->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditorOnly | CPF_Parm))
			{
				continue;
			}
			const UClass* Owner = P->GetOwnerClass();
			if (!Owner || !DefClass->IsChildOf(Owner))
			{
				continue;
			}
			if (Opt.bRequireEdit && !P->HasAnyPropertyFlags(CPF_Edit))
			{
				continue;
			}
			if (Opt.bInstanceEditableOnly && (!P->HasAnyPropertyFlags(CPF_Edit) || P->HasAnyPropertyFlags(CPF_DisableEditOnInstance)))
			{
				continue;
			}
			if (Opt.bBlueprintDeclaredOnly && !Cast<UBlueprintGeneratedClass>(Owner))
			{
				continue;
			}
			if (Opt.bSkipOwnClass && Owner == Obj->GetClass())
			{
				continue;
			}
			if (NoisyProps().Contains(P->GetFName()) || (Opt.Skip && Opt.Skip->Contains(P->GetFName())))
			{
				continue;
			}
			if (Opt.bSimpleOnly && !IsSimpleProp(P))
			{
				continue;
			}
			for (int32 Idx = 0; Idx < P->ArrayDim; ++Idx)
			{
				const void* A = P->ContainerPtrToValuePtr<void>(Obj, Idx);
				const void* B = P->ContainerPtrToValuePtr<void>(Def, Idx);
				if (P->Identical(A, B, PPF_None))
				{
					continue;
				}
				if (const FObjectPropertyBase* OP = CastField<FObjectPropertyBase>(P))
				{
					const UObject* VA = OP->GetObjectPropertyValue(A);
					const UObject* VB = OP->GetObjectPropertyValue(B);
					// Instanced subobjects (components, triggers...) differ by identity only.
					if ((VA && VA->IsIn(Obj)) || (VB && VB->IsIn(Def) && !VA))
					{
						continue;
					}
				}
				else if (P->HasAnyPropertyFlags(CPF_ContainsInstancedReference))
				{
					continue;
				}
				FString Name = P->GetAuthoredName();
				if (P->ArrayDim > 1)
				{
					Name += FString::Printf(TEXT("[%d]"), Idx);
				}
				const FStructProperty* SP = CastField<FStructProperty>(P);
				if (SP && Opt.bExpandStructs && SP->Struct && !SP->Struct->IsA<UUserDefinedStruct>()
					&& SP->Struct->GetFName() != NAME_Vector && SP->Struct->GetFName() != NAME_Rotator && SP->Struct->GetFName() != NAME_Color
					&& SP->Struct->GetFName() != NAME_LinearColor && SP->Struct->GetFName() != NAME_Vector2D && SP->Struct->GetFName() != NAME_Transform)
				{
					int32 Fields = 0;
					for (TFieldIterator<FProperty> F(SP->Struct); F; ++F)
					{
						const FProperty* FP = *F;
						if (FP->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditorOnly))
						{
							continue;
						}
						const void* FA = FP->ContainerPtrToValuePtr<void>(A);
						const void* FB = FP->ContainerPtrToValuePtr<void>(B);
						if (FP->Identical(FA, FB, PPF_None))
						{
							continue;
						}
						if (Count >= Opt.Max)
						{
							++Hidden;
							continue;
						}
						const FString FV = ExportValue(FP, FA, Opt.SkipLongerThan > 0 ? Opt.SkipLongerThan + 1 : Opt.MaxLen);
						if (Opt.SkipLongerThan > 0 && FV.Len() > Opt.SkipLongerThan)
						{
							continue;
						}
						Out.Add(Name + TEXT(".") + FP->GetAuthoredName() + TEXT("=") + Cap(FV, Opt.MaxLen));
						++Count;
						if (++Fields >= 6)
						{
							break;
						}
					}
					continue;
				}
				if (Count >= Opt.Max)
				{
					++Hidden;
					continue;
				}
				const FString V = ExportValue(P, A, Opt.SkipLongerThan > 0 ? Opt.SkipLongerThan + 1 : Opt.MaxLen);
				if (Opt.SkipLongerThan > 0 && V.Len() > Opt.SkipLongerThan)
				{
					continue;
				}
				Out.Add(Name + TEXT("=") + Cap(V, Opt.MaxLen));
				++Count;
			}
		}
		if (Hidden > 0)
		{
			Out.Add(FString::Printf(TEXT("(+%d more)"), Hidden));
		}
		return Count;
	}

	FString JoinProps(const TArray<FString>& Props)
	{
		return FString::Join(Props, TEXT(", "));
	}
}
